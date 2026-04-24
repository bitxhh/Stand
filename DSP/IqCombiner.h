#pragma once

#include "../Core/IPipelineHandler.h"

#include <QObject>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

class Pipeline;

// ---------------------------------------------------------------------------
// IqCombiner — merges I/Q blocks from N coherent RX channels into one output.
//
// Registered as an IPipelineHandler in each per-channel PrePipeline.
// When all N channels have delivered a block with the same timestamp,
// the combiner gain-normalises each channel (÷ linear gain), averages
// the I/Q samples, and dispatches the result to the output Pipeline.
//
// Также считает межканальную фазовую/когерентную метрику (ch0 vs ch1) и
// эмитит phaseMetric() с троттлингом. Фазовая калибровка хранится здесь:
// setPhaseCalibrationDeg() / calibrateNow() вычитают константный offset из
// сырой фазы (физически задержка между каналами постоянна при одном LO).
//
// Threading: processBlock() is called from different RxWorker threads
// (one per channel). Internal mutex serialises access; the thread that
// fills the last slot performs the combine+dispatch under the lock.
// phaseMetric() эмитится из той же worker-нити — подключать через
// Qt::QueuedConnection.
// ---------------------------------------------------------------------------
class IqCombiner : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    explicit IqCombiner(int channelCount, Pipeline* output, QObject* parent = nullptr);

    // Set per-channel RX gain in dB for normalisation.
    // scale = 1 / 10^(gainDb / 20).  Thread-safe.
    void setChannelGain(int channelIndex, double gainDb);

    // Фазовая калибровка (константный offset, вычитаемый из сырой фазы).
    // Thread-safe.
    void   setPhaseCalibrationDeg(double deg);
    double phaseCalibrationDeg() const;

    // Снимает текущую сырую фазу как нулевой reference (calibration = raw).
    // Вызывать, когда физически каналы принимают один и тот же сигнал
    // (например, общая антенна/splitter). Возвращает применённый offset в град.
    double calibrateNow();

    // IPipelineHandler — uses meta.channel.channelIndex to route blocks.
    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void processBlock(const float* iq, int count, double sampleRateHz,
                      const BlockMeta& meta) override;

    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;
    void onRetune(double newFreqHz) override;

signals:
    // rawDeg        — мгновенная фаза ch0·conj(ch1), [-180, 180]
    // calibratedDeg — rawDeg - phaseCalibrationDeg_, приведено к [-180, 180]
    // coherence     — |Σ cross| / √(Σ|c0|²·Σ|c1|²), [0, 1]; >0.9 = хорошая когерентность
    void phaseMetric(double rawDeg, double calibratedDeg, double coherence);

    // Per-channel I/Q imbalance.
    // amplitudeDb   — 10·log10(ΣI² / ΣQ²); 0 dB = идеальный баланс, ±знак = какая компонента сильнее
    // crossCorr     — ΣI·Q / √(ΣI²·ΣQ²), [-1, 1]; |x|<0.01 = ортогонально; большой модуль = I/Q не ортогональны
    void iqImbalance(int channelIndex, double amplitudeDb, double crossCorr);

private:
    struct Slot {
        std::vector<float> data;
        uint64_t           timestamp{0};
        bool               filled{false};
    };

    void resetSlots();
    void combineAndDispatch(int count, double sampleRateHz);
    void accumulatePhase(int count);   // called under mutex_ in combineAndDispatch
    void accumulateChannelIq(int idx, const float* data, int count);  // under mutex_
    void maybeEmitPhase();              // called under mutex_
    void maybeEmitIqImbalance();        // called under mutex_

    Pipeline*         output_;
    int               channelCount_;
    std::vector<Slot> slots_;
    std::vector<float> gainScale_;   // linear: 1/10^(gain/20)
    std::vector<float> combined_;    // output buffer
    std::mutex         mutex_;

    // ── Межканальная метрика ────────────────────────────────────────────────
    std::atomic<double> phaseCalibrationDeg_{0.0};
    double  crossReAcc_{0.0};
    double  crossImAcc_{0.0};
    double  pow0Acc_{0.0};
    double  pow1Acc_{0.0};
    int     accBlocks_{0};
    using Clock = std::chrono::steady_clock;
    Clock::time_point lastEmit_{};
    static constexpr int kEmitIntervalMs = 200;

    // ── Per-channel I/Q imbalance accumulators ───────────────────────────────
    std::vector<double> sumI2_;   // ΣI²  per channel
    std::vector<double> sumQ2_;   // ΣQ²  per channel
    std::vector<double> sumIQ_;   // ΣI·Q per channel
    int     iqAccBlocks_{0};
    Clock::time_point lastIqEmit_{};
};
