#pragma once

#include <QList>
#include <QString>
#include <cstdint>

class IDevice;

// ---------------------------------------------------------------------------
// ISyncController — абстракция синхронизации нескольких SDR-устройств.
//
// Три уровня синхронизации:
//
//   1. Частотная: общий reference clock (10 MHz ext) → одинаковый LO.
//      setReferenceSource() вызывается ДО init().
//
//   2. Временна́я: аппаратные timestamps из readBlock() привязаны
//      к одному и тому же счётчику семплов (при общем reference).
//      getHardwareTimestamp() / timestampOffset() — для измерения сдвига
//      между устройствами.
//
//   3. Триггерная: одновременный старт нескольких потоков.
//      armTrigger() → fireTrigger() (two-phase commit).
//      Реализация через GPIO или timestamp-based start.
//
// Стабы: Phase 6 — только интерфейс + минимальная реализация для LimeSDR.
// Реальная синхронизация требует аппаратного тестирования и калибровки.
// ---------------------------------------------------------------------------
class ISyncController {
public:
    enum class ReferenceSource {
        Internal,        // внутренний VCTCXO (default)
        External10MHz    // внешний 10 MHz через ext_clk SMA
    };

    virtual ~ISyncController() = default;

    // ── Reference clock ──────────────────────────────────────────────────
    // Должен вызываться ПЕРЕД device->init().
    // При external source PLL перестраивается на внешний клок.
    virtual void setReferenceSource(IDevice* device, ReferenceSource src) = 0;
    [[nodiscard]] virtual ReferenceSource referenceSource(const IDevice* device) const = 0;

    // Проверяет, обнаружен ли внешний reference на физическом разъёме.
    // Результат валиден только после init().
    [[nodiscard]] virtual bool isExternalReferenceDetected(const IDevice* device) const = 0;

    // ── Hardware timestamps ──────────────────────────────────────────────
    // Последний timestamp, полученный из readBlock().
    // Смысл: монотонный счётчик семплов, привязанный к sample clock устройства.
    // При общем reference 10 MHz — тактирование одинаковое для всех устройств,
    // но начальная фаза (offset) разная → нужен timestampOffset().
    [[nodiscard]] virtual uint64_t getHardwareTimestamp(const IDevice* device) const = 0;

    // Вычисляет разницу timestamps (master − slave) в семплах.
    // Положительное значение: slave опережает master.
    // Точность: ±1 блок (зависит от readBlock() rate).
    [[nodiscard]] virtual int64_t timestampOffset(const IDevice* master,
                                                   const IDevice* slave) const = 0;

    // ── Synchronised start (two-phase commit) ────────────────────────────
    //
    // Phase 1 — arm:
    //   Подготовить все устройства к одновременному старту.
    //   LimeSDR: GPIO настроен как output (master) или input (slave);
    //   потоки сетаплены, но не начаты.
    //
    // Phase 2 — fire:
    //   Начать передачу данных.
    //   Если targetTimestamp > 0, потоки стартуют когда аппаратный счётчик
    //   достигнет этого значения (LimeSuite timed TX/RX).
    //   Если targetTimestamp == 0, используется GPIO trigger (ASAP).
    //
    virtual void armTrigger(const QList<IDevice*>& devices) = 0;
    virtual void fireTrigger(uint64_t targetTimestamp = 0) = 0;
    [[nodiscard]] virtual bool isArmed() const = 0;

    // ── Status / errors ──────────────────────────────────────────────────
    // Реализация может эмитировать сигналы через QObject-наследник.
    // ISyncController намеренно не наследует QObject — мок/тест проще.
};
