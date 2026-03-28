#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>
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
// IDevice — SDR-агностичный интерфейс устройства.
//
// Правила реализации:
//   • Все методы конфигурации (setSampleRate, setFrequency, setGain)
//     могут бросить std::exception — caller обязан ловить.
//   • readBlock() блокирует вызывающий поток до прихода данных или таймаута.
//     Возвращает количество принятых I/Q пар (0 = таймаут, < 0 = ошибка).
//   • startStream() / stopStream() вызываются из потока StreamWorker.
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
    virtual void init()          = 0;   // инициализация железа
    virtual void calibrate()     {}     // опционально — пустая реализация по умолчанию

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
    // Вызываются из потока StreamWorker (не из UI-потока).
    virtual void startStream() = 0;
    virtual void stopStream()  = 0;

    // Блокирующее чтение одного I/Q блока.
    // buffer: interleaved int16 [I0, Q0, I1, Q1, ...], размер buffer >= count*2
    // Возвращает число принятых I/Q пар, 0 = таймаут, < 0 = ошибка.
    virtual int readBlock(int16_t* buffer, int count, int timeoutMs) = 0;

    // ── Состояние ─────────────────────────────────────────────────────────────
    [[nodiscard]] virtual DeviceState state() const = 0;

    // ── Опциональный UI с аппаратно-специфичными настройками ─────────────────
    // Возвращает виджет (LNA/TIA/PGA для LimeSDR) или nullptr.
    // DeviceDetailWindow вставляет его в layout если не nullptr.
    virtual QWidget* createAdvancedWidget(QWidget* /*parent*/) { return nullptr; }

signals:
    void stateChanged(DeviceState newState);
    void errorOccurred(const QString& message);
    void sampleRateChanged(double hz);
};
