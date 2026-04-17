#pragma once

#include "ChannelDescriptor.h"
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// DeviceState — жизненный цикл любого SDR-устройства.
// IDevice эмитирует stateChanged() при каждом переходе.
// ---------------------------------------------------------------------------
enum class DeviceState {
    Disconnected,   // устройство пропало из системы
    Connected,      // найдено, но не инициализировано
    Ready,          // инициализировано, можно стримить
    Streaming,      // стрим идёт
    Error           // аппаратная или программная ошибка
};

// ---------------------------------------------------------------------------
// ChannelInfo — describes one available stream endpoint on a device.
// Returned by IDevice::availableChannels().
// ---------------------------------------------------------------------------
struct ChannelInfo {
    ChannelDescriptor descriptor;
    QString           displayName;  // "RX0", "RX1", "File", etc.
};

// ---------------------------------------------------------------------------
// IDevice — SDR-агностичный интерфейс устройства.
//
// Правила реализации:
//   • Все методы конфигурации (setSampleRate, setFrequency, setGain)
//     могут бросить std::exception — caller обязан ловить.
//   • readBlock() блокирует вызывающий поток до прихода данных или таймаута.
//     Возвращает количество принятых I/Q пар (0 = таймаут, < 0 = ошибка).
//   • startStream() / stopStream() вызываются из потока RxWorker.
//   • createAdvancedWidget() опционален — возвращает nullptr по умолчанию.
// ---------------------------------------------------------------------------
class IDevice : public QObject {
    Q_OBJECT

public:
    explicit IDevice(QObject* parent = nullptr) : QObject(parent) {}
    ~IDevice() override = default;

    // ── Идентификация ─────────────────────────────────────────────────────────
    [[nodiscard]] virtual QString id()   const = 0;   // уникальный строковый ID
    [[nodiscard]] virtual QString name() const = 0;   // человекочитаемое название

    // ── Жизненный цикл ───────────────────────────────────────────────────────
    // Инициализация железа. Если channels пуст — включаются все доступные RX.
    // Иначе включаются только указанные каналы (TX0 всегда включается на
    // железе с калибровочным лупбеком).
    virtual void init(const QList<ChannelDescriptor>& channels = {}) = 0;
    virtual void calibrate(const QList<ChannelDescriptor>& channels = {}) {}  // опционально
    // Останавливает все стримы, закрывает хэндл и сбрасывает состояние в Connected.
    // Вызывается из UI-потока при закрытии DeviceDetailWindow.
    virtual void close() {}

    // ── Параметры ─────────────────────────────────────────────────────────────
    virtual void   setSampleRate(double hz)               = 0;
    [[nodiscard]] virtual double sampleRate()       const = 0;
    [[nodiscard]] virtual QList<double> supportedSampleRates() const = 0;

    virtual void   setFrequency(double hz)                = 0;
    [[nodiscard]] virtual double frequency()        const = 0;

    // Единое усиление в дБ. Диапазон: [0, maxGain()].
    virtual void   setGain(double dB)                     = 0;
    [[nodiscard]] virtual double gain()             const = 0;
    [[nodiscard]] virtual double maxGain()          const = 0;

    // ── Стрим ────────────────────────────────────────────────────────────────
    // Вызываются из потока RxWorker (не из UI-потока).
    virtual void startStream() = 0;
    virtual void stopStream()  = 0;

    // Блокирующее чтение одного I/Q блока.
    // buffer: interleaved int16 [I0, Q0, I1, Q1, ...], размер buffer >= count*2
    // Возвращает число принятых I/Q пар, 0 = таймаут, < 0 = ошибка.
    virtual int readBlock(int16_t* buffer, int count, int timeoutMs) = 0;

    // ── Channel-aware стрим (Phase 1+) ───────────────────────────────────────
    // Дефолтные реализации делегируют в однокнальные методы выше ({RX,0}).
    // LimeDevice переопределит их в Phase 1 для поддержки нескольких каналов.

    // prepareStream — вызывается из UI-потока до запуска воркеров при dual RX.
    // Выполняет только LMS_SetupStream без LMS_StartStream.
    // Это предотвращает остановку уже работающего канала когда второй поток
    // вызывает LMS_SetupStream из воркер-потока (LimeSuite прерывает все стримы).
    virtual void prepareStream(ChannelDescriptor /*ch*/) {}

    virtual void startStream(ChannelDescriptor /*ch*/) { startStream(); }
    virtual void stopStream(ChannelDescriptor /*ch*/)  { stopStream(); }
    virtual int  readBlock(ChannelDescriptor /*ch*/, int16_t* buffer, int count, int timeoutMs) {
        return readBlock(buffer, count, timeoutMs);
    }

    // Worker-side pause point for retune handshake. Called at the top of the
    // RxWorker loop. Default no-op; LimeDevice blocks here while UI thread
    // performs an LO retune, then resumes.
    virtual void checkPauseForRetune(ChannelDescriptor /*ch*/) {}
    virtual void setFrequency(ChannelDescriptor /*ch*/, double hz) { setFrequency(hz); }
    virtual void setGain(ChannelDescriptor /*ch*/, double dB)      { setGain(dB); }

    [[nodiscard]] virtual double frequency(ChannelDescriptor /*ch*/) const { return frequency(); }
    [[nodiscard]] virtual double gain(ChannelDescriptor /*ch*/)      const { return gain(); }
    [[nodiscard]] virtual double maxGain(ChannelDescriptor /*ch*/)   const { return maxGain(); }

    // ── Hardware timestamps (Phase 6: sync) ────────────────────────────────
    // Последний аппаратный timestamp из readBlock() для данного канала.
    // Монотонный счётчик семплов. 0 = стрим не запущен или реализация не поддерживает.
    [[nodiscard]] virtual uint64_t lastReadTimestamp(ChannelDescriptor /*ch*/) const { return 0; }

    // TX write block — отправляет один блок I/Q в TX поток.
    // Вызывается из TxWorker thread. Возвращает число отправленных пар, <0 = ошибка.
    // Дефолт: -1 (TX не поддерживается).
    virtual int writeBlock(ChannelDescriptor /*ch*/, const int16_t* /*buffer*/,
                           int /*count*/, int /*timeoutMs*/) { return -1; }

    // ── Состояние ─────────────────────────────────────────────────────────────
    [[nodiscard]] virtual DeviceState state() const = 0;

    // ── Мониторинг ────────────────────────────────────────────────────────────
    // Температура чипа в °C. NaN если не поддерживается или устройство не готово.
    [[nodiscard]] virtual double temperature() const { return std::nan(""); }

    // ── Persistence (chip register state) ────────────────────────────────────
    // Сохранить/загрузить аппаратную конфигурацию (регистры чипа) в/из файла.
    // Дефолт: no-op/false. LimeDevice проксирует в LMS_SaveConfig/LMS_LoadConfig.
    // Вызывать после init() — иначе хэндл не готов.
    virtual bool saveConfig(const QString& /*path*/) const { return false; }
    virtual bool loadConfig(const QString& /*path*/)       { return false; }

    // ── Возможности устройства ────────────────────────────────────────────────
    // Возвращает список доступных каналов устройства.
    // Дефолт: один RX0 (для backward compat / тестовых заглушек).
    [[nodiscard]] virtual QList<ChannelInfo> availableChannels() const {
        return { {{ChannelDescriptor::RX, 0}, QStringLiteral("RX0")} };
    }

    // ── Опциональный UI с аппаратно-специфичными настройками ─────────────────
    // Возвращает виджет (LNA/TIA/PGA для LimeSDR) или nullptr.
    // DeviceDetailWindow вставляет его в layout если не nullptr.
    virtual QWidget* createAdvancedWidget(QWidget* /*parent*/) { return nullptr; }

signals:
    void stateChanged(DeviceState newState);
    void errorOccurred(const QString& message);
    void sampleRateChanged(double hz);

    // Emitted after an LO retune has completed on worker-safe boundary
    // (worker parked, LMS_SetLOFrequency applied, stream restarted).
    // Handlers with accumulated DSP state (FIR delay line, NCO phase, WAV
    // decimation) must reset on this signal. Connected with DirectConnection
    // from RxController so the slot runs before the worker is unparked.
    void retuned(ChannelDescriptor ch, double hz);
};
