# План v2.1 — фиксы после Phase 6 + рефакторинг логгера и окна выбора устройства

Порядок выполнения: **A → B → D → E → C**.
D и E идут перед C, т.к. логгер нужен для диагностики C, а пересмотр lifecycle окна выбора упрощает отладку аудио-регрессий (меньше «липких» состояний).

---

## Phase A — Выбор каналов без блокировки UI

**Проблема.** После `onDeviceInitialized` (Application.cpp:729–730) `channelCountSpin_` и `channelAssignCombo_` безусловно отключаются. Auto-open в конце конструктора (Application.cpp:200) не даёт пользователю шанс поменять выбор до инициализации. Единственный выход — тяжёлый Reset.

**Решение.**
1. Убрать `setEnabled(false)` в `onDeviceInitialized` для обоих виджетов.
2. Гейтить только `isStreaming()`: пока стрима нет — можно менять; как только `startStream` — блокируем; после `stopStream` — снова разблокируем.
3. Для применения нового выбора без полного Reset: `stopStream` (если был) → diff по `LMS_EnableChannel` → `LMS_SetupStream` с новой конфигурацией → `calibrate` только для новых каналов. `LMS_Close`/`LMS_Init` не вызывать.
4. Ряд `channelAssignRow_` продолжает скрываться/показываться по текущей логике (`rxChannels.size() > 1 && channelCountSpin_->value() == 1`).

**Затронутые файлы.** `Application/Application.cpp`, `Hardware/DeviceController.{h,cpp}` (новый slot `reconfigureChannels`), возможно `Hardware/LimeDevice.cpp` (lightweight reconfigure).

**Критерии приёмки.**
- Выбор 1/2 каналов и rx0/rx1 меняется без Reset.
- Во время стрима виджеты заблокированы.
- При смене выбора пересоздаётся только стрим, без переинициализации устройства.

**Модель / эффорт.** Sonnet + medium.

---

## Phase B — Прогресс-бар и блокировка UI во время инициализации

**Проблема.** `autoOpen` (DeviceController.cpp:85–105) эмитит только строковый `statusChanged`. Нет процентов, нет явной блокировки остальных вкладок. Инициализация + калибровка 2 каналов занимает дольше, чем у HDSDR.

**Решение.**
1. Добавить сигнал `progressChanged(int percent, QString stage)` в `DeviceController`.
2. Разбить `autoOpen` на фазы с явными чекпоинтами:
   - 0%  — `Initializing…`
   - 15% — после `device_->init(channels)`
   - 25% — после `setSampleRate`
   - 30% — `Calibrating RX0…` (маркер-маркиза на время `LMS_Calibrate`, т.к. прогресса внутри нет)
   - 65% — `Calibrating RX1…` (если 2 канала)
   - 100% — `Ready — N Hz`
3. На Device control page — `QProgressBar` + текущий этап рядом.
4. На время инициализации: `QTabWidget::setTabEnabled(i, false)` для всех вкладок кроме Device control. После `deviceInitialized` (или ошибки) — разблокировать.
5. Reset использует тот же прогресс-канал.

**Затронутые файлы.** `Hardware/DeviceController.{h,cpp}`, `Application/Application.cpp`, соответствующий UI-файл.

**Критерии приёмки.**
- Во время auto-open/reset виден прогресс с этапами.
- Остальные вкладки недоступны до готовности.
- Ошибки корректно сбрасывают прогресс и разблокируют UI.

**Модель / эффорт.** Sonnet + medium.

---

## Phase D — Рефакторинг логгера (унифицированный + меню параметров)

**Проблема.** Текущий `Logger` не абстрагирован от устройства; при постоянном мониторинге качества сигнала логи раздуваются до 1000+ строк. Нет выборочного логирования параметров.

**Решение.**
1. Интерфейс `ILogger`:
   - `log(Level, const QString& category, const QString& msg)`
   - `logParam(const QString& paramKey, double value)` — канал параметров.
2. Device-specific реализации (например, `LimeLogger : ILogger`) — для вывода hardware-specific контекста (серийник, активные каналы).
3. `LoggerConfig` — набор чекбоксов «логировать параметр X»:
   - signal quality, gain RX0/RX1, frequency RX0/RX1, sample rate, calibration events, pipeline handler timings, audio underruns, etc.
   - Сохранять выбор в `DeviceSettings` (или отдельный `logger_settings.json`).
4. Меню опций (отдельный диалог или side panel) — таблица «Параметр | Чекбокс», привязана к `LoggerConfig`.
5. Runtime-фильтр в `logParam`: если чекбокс выключен — запись отбрасывается до форматирования (zero-cost).

**Затронутые файлы.** `Core/Logger.{h,cpp}` → `ILogger` + реализации; новый `Core/LoggerConfig.{h,cpp}`; новый UI-диалог в `Application/`; интеграция в `Application.cpp` (меню).

**Критерии приёмки.**
- Можно точечно включать/выключать параметры логирования без перезапуска.
- Интерфейс `ILogger` позволяет добавить второй тип устройства без правки существующих логов.
- Отключённые параметры не попадают в лог и не тратят CPU.

**Модель / эффорт.** Opus + medium — проектирование интерфейса под несколько типов устройств требует продуманной абстракции; сама реализация не объёмная.

---

## Phase E — Lifecycle окна выбора устройства + «+» для повторного открытия

**Проблема.** Окно поиска устройств продолжает висеть/держать поток после того, как устройство выбрано. Нет способа добавить второе устройство.

**Решение.**
1. После `onDeviceOpened` скрывать `DeviceSelectionWindow` и корректно завершать его фоновый поток сканирования (`QThread::quit` + `wait`).
2. Добавить top-bar в главное окно с кнопкой «+» (QToolBar с `QAction`).
3. Клик по «+» создаёт новый экземпляр `DeviceSelectionWindow` (или переиспользует закэшированный) и открывает его модально/немодально — TBD в ходе работы.
4. Меню опций на `DeviceSelectionWindow`: как минимум «период опроса», «фильтр по hint», «автооткрытие единственного устройства». Дизайн — минимальный `QMenu` в верхнем углу окна.
5. Если одновременно открыты несколько устройств — each gets its own `DeviceDetailWindow`; shared state не трогаем (выходит за рамки фазы).

**Затронутые файлы.** `Application/DeviceSelectionWindow.{h,cpp}`, `Application/Application.cpp` (top bar, plus-кнопка), возможно новый `Application/DeviceSelectionOptionsDialog.{h,cpp}`.

**Критерии приёмки.**
- После открытия устройства окно поиска скрыто, поток освобождён.
- Кнопка «+» снова открывает окно поиска.
- Меню опций работает (минимальный набор).

**Модель / эффорт.** Sonnet + medium.

---

## Phase C — Аудио-артефакты (регрессия + историческая нагрузка)

**Предпосылка.** К моменту старта этой фазы логгер из Phase D уже позволяет выборочно снимать тайминги per-handler и события underrun — это критично для диагностики.

### C1 — Регрессия: клики/дрожь даже на одном демодуляторе

**Гипотезы (в порядке вероятности).**
1. `restoreDemodPanels` в конструкторе `Application` вызывается до старта стрима; `FmAudioOutput` внутри панели создаётся с `sampleRateHz = 0` или дефолтом, а `setSampleRateHz` в `onDeviceReady` не пересоздаёт ресемплер — остаётся некорректное состояние.
2. Порядок в `RadioMonitorPage::startStream`: `setSampleRateHz` → `ctrl_->startStream` → `onStreamStarted`. Гонка: панели могут начать эмиттить до готовности пайплайна.
3. Линейный ресемплер в `FmAudioOutput` — дрейф при несовпадении ожидаемого и фактического audio SR.

**Действия.**
- Инструментация через `ILogger::logParam`: input/output SR каждого ресемплера, underrun count `QAudioSink`, drop count в `IqCombiner`.
- Исправить порядок: панель узнаёт SR только после `onStreamStarted`; до этого — заморожена.
- Убедиться, что `setSampleRateHz` пересоздаёт ресемплер, если SR реально изменился, а не только переприсваивает число.

### C2 — Историческая: распределение нагрузки на множестве демодуляторов

**Проблема.** `Pipeline::dispatchBlock` (Core/Pipeline.cpp) ставит `QtConcurrent::run` на каждый handler и ждёт барьер `waitForFinished`. При 4 демодах + FFT + recording ≈ 14 задач; если `dspPool_` меньше — параллелизм ломается. `FmAudioOutput`, блокирующийся на WASAPI backpressure, пинит поток пула на всё время блока.

**Действия.**
1. Размер `dspPool_` — задать `QThread::idealThreadCount() - 1` (не меньше 4), зафиксировать в одном месте.
2. Вывести `FmAudioOutput` из пайплайна: `BaseDemodHandler` пушит аудио в SPSC-очередь, отдельный поток (per-demod) вытаскивает и отдаёт в `QAudioSink`. Пайплайн освобождается немедленно.
3. Опциональная группировка: несколько лёгких handlers (DC-blocker, envelope) в один `QtConcurrent::run`-таск — уменьшает overhead scheduling.
4. Проверить, что recording-пути (WAV + .cf32) используют неблокирующую запись (отдельный writer-thread или `QFile` с буфером).

**Критерии приёмки.**
- Один демод без кликов (проверка C1 закрыта).
- 4+ демода + FFT + recording не дают underrun на Release-билде при Fs=15 MS/s.
- Pipeline-тайминги в логе: ни один handler не доминирует.

**Модель / эффорт.** Opus + high — нетривиальная DSP+threading диагностика, требует аккуратного рассуждения о гонках и балансировке.

---

## Сводная таблица

| Фаза | Описание | Модель | Эффорт |
|------|----------|--------|--------|
| A | Выбор каналов без блокировки | Sonnet | medium |
| B | Прогресс-бар + блокировка вкладок | Sonnet | medium |
| D | Рефакторинг логгера | Opus | medium |
| E | Lifecycle окна выбора + «+» | Sonnet | medium |
| C | Аудио-артефакты (C1 регрессия + C2 нагрузка) | Opus | high |
