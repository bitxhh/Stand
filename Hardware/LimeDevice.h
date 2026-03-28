#pragma once

#include "../Core/IDevice.h"
#include "lime/LimeSuite.h"

#include <atomic>
#include <string>

// ---------------------------------------------------------------------------
// LimeDevice — реализация IDevice для LimeSDR (LimeSuite API).
//
// Инкапсулирует lms_device_t и lms_stream_t.
// StreamWorker взаимодействует только через IDevice API:
//   startStream() → readBlock() в цикле → stopStream().
//
// Поток вызовов:
//   UI thread : init(), calibrate(), setSampleRate(), setFrequency(), setGain()
//   Worker thread : startStream(), readBlock(), stopStream()
// ---------------------------------------------------------------------------
class LimeDevice : public IDevice {
    Q_OBJECT

public:
    explicit LimeDevice(const lms_info_str_t& id, QObject* parent = nullptr);
    ~LimeDevice() override;

    LimeDevice(const LimeDevice&)            = delete;
    LimeDevice& operator=(const LimeDevice&) = delete;

    // ── IDevice: идентификация ────────────────────────────────────────────────
    [[nodiscard]] QString id()   const override;
    [[nodiscard]] QString name() const override;

    // ── IDevice: жизненный цикл ───────────────────────────────────────────────
    void init()      override;
    void calibrate() override;   // читает currentSampleRate_ внутри

    // ── IDevice: параметры ────────────────────────────────────────────────────
    void   setSampleRate(double hz)                      override;
    [[nodiscard]] double sampleRate()              const override;
    [[nodiscard]] QList<double> supportedSampleRates()   const override;

    void   setFrequency(double hz)                       override;
    [[nodiscard]] double frequency()               const override;

    // Единое усиление [0, maxGain()]. Внутри — LMS_SetGaindB.
    void   setGain(double dB)                            override;
    [[nodiscard]] double gain()                    const override;
    [[nodiscard]] double maxGain()                 const override { return kMaxGainDb; }

    // ── IDevice: стрим ────────────────────────────────────────────────────────
    void startStream() override;
    void stopStream()  override;

    // Блокирует до прихода данных или таймаута.
    // Возвращает число I/Q пар, 0 = таймаут, < 0 = ошибка.
    int readBlock(int16_t* buffer, int count, int timeoutMs) override;

    // ── IDevice: состояние ────────────────────────────────────────────────────
    [[nodiscard]] DeviceState state() const override { return state_; }

    // ── IDevice: аппаратно-специфичный виджет (LNA / TIA / PGA) ──────────────
    QWidget* createAdvancedWidget(QWidget* parent) override;

    // ── LimeSDR-специфичное: прямое управление ступенями усиления ─────────────
    // Используется только внутри LimeAdvancedWidget, возвращаемого createAdvancedWidget().
    // lna : 0–5  (0/5/10/15/20/25.5 dB)
    // tia : 0–2  (0/9/12 dB)
    // pga : 0–31 (0–31 dB, 1 dB/шаг)
    void setLimeGain(int lna, int tia, int pga);

    static const QList<double> kSupportedRates;

    // Доступ к LimeSuite info string — используется в LimeDeviceManager для сравнения устройств.
    [[nodiscard]] const lms_info_str_t& limeInfo() const { return deviceId_; }

private:
    void setState(DeviceState s);
    void setupStream();
    void teardownStream();

    lms_device_t*     handle_{nullptr};
    lms_info_str_t    deviceId_{};
    std::string       serial_;

    double            currentSampleRate_{2'000'000.0};
    double            currentFrequency_ {102e6};
    double            currentGainDb_    {0.0};

    DeviceState       state_{DeviceState::Connected};
    lms_stream_t      streamId_{};
    bool              streamReady_{false};

    static constexpr double kMaxGainDb = 68.5;
};
