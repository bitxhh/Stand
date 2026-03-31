#pragma once

#include <QVector>
#include <complex>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// FmDemodulator — real-time WBFM demodulator
//
// DSP chain (all in double precision, float32 output):
//
//   int16 I/Q  →  DC blocker  →  NCO freq-shift  →  FIR1 LPF (31/127 taps, complex)
//              →  decimate D1  →  IF @ ~250 kHz
//              →  FM discriminator (atan2 of conjugate product)
//              →  de-emphasis IIR (first-order, τ = 75 µs EU / 50 µs US)
//              →  FIR2 LPF (63 taps, real, cutoff 15 kHz)
//              →  decimate D2 = 5  →  audio @ ~50 kHz
//              →  QVector<float> (normalised, volume applied externally)
//
// Audio sample rate = inputSR / (D1 * 5).  D1 = round(inputSR / 250 000).
// For the standard LimeSDR SR set {2, 4, 8, 10, 15, 20, 25, 30} MHz,
// the IF is always exactly 250 kHz and audio is always exactly 50 kHz.
//
// Usage (inside StreamWorker::run):
//   FmDemodulator dem(inputSR, stationOffsetHz);
//   while (running) {
//       QVector<int16_t> raw = ...;
//       QVector<float> audio = dem.pushBlock(raw);
//       if (!audio.isEmpty()) emit audioReady(audio, dem.audioSampleRate());
//   }
//
// Thread safety: call all methods from the SAME thread (StreamWorker thread).
// ---------------------------------------------------------------------------
class FmDemodulator {
public:
    // inputSampleRateHz — LimeSDR host SR (e.g. 2e6)
    // stationOffsetHz   — target station offset from LO, same sign convention
    //                     as BandpassExporter:
    //                     centre = 102 MHz, station = 104 MHz → offset = +2e6
    // deemphTauSec      — 75e-6 (Europe, default) or 50e-6 (USA / Japan)
    explicit FmDemodulator(double inputSampleRateHz,
                           double stationOffsetHz,
                           double deemphTauSec    = 75e-6,
                           double bandwidthHz     = 100'000.0);

    // Process one raw I/Q block.
    [[nodiscard]] QVector<float> pushBlock(const QVector<int16_t>& iqBlock);

    // Change the pre-decimation bandpass width on the fly.
    // Redesigns FIR1 and clears the delay line — takes effect on the next block.
    // bandwidthHz: one-sided cutoff, clamped to [50 kHz, ifSR/2 * 0.9].
    void setBandwidth(double bandwidthHz);

    // Retune NCO to a new station offset within the current capture band.
    // offsetHz = target station frequency − LO frequency (signed).
    // Resets filter delay lines, DC blocker, and noise floor — brief audio glitch
    // expected (same as setBandwidth).
    void setOffset(double offsetHz);

    [[nodiscard]] double audioSampleRate() const { return audioSR_; }
    [[nodiscard]] double ifSampleRate()    const { return ifSR_;    }
    [[nodiscard]] int    decimation1()     const { return D1_;      }
    [[nodiscard]] double bandwidth()       const { return bandwidth_; }

    // Current signal quality metrics — safe to read from any thread
    // (written atomically as doubles by the worker thread).
    // snrDb()   > 6  → signal present, demodulation should work
    // snrDb()   > 2  → marginal — may be noisy audio
    // snrDb()   ≤ 2  → noise only — wrong frequency or too little gain
    [[nodiscard]] double ifRms()   const { return ifRmsOut_; }
    [[nodiscard]] double snrDb()   const { return snrDbOut_; }

    // FIR design helper — public for unit tests.
    static std::vector<double> designLowpassFir(int numTaps, double cutoffNorm);

private:
    // ── Parameters ───────────────────────────────────────────────────────────
    double inputSR_;
    double stationOffset_;
    double deemphTau_;
    double bandwidth_;     // one-sided FIR1 cutoff (Hz), user-adjustable

    int    D1_;       // stage-1: inputSR  → ~250 kHz IF
    double ifSR_;
    int    D2_{5};    // stage-2: ~250 kHz → ~50 kHz audio (fixed)
    double audioSR_;

    // ── DC blocker — high-pass IIR before NCO ────────────────────────────────
    // Removes LO leakage (DC spike at 0 Hz in baseband).
    // H(z) = (1 - z⁻¹) / (1 - α·z⁻¹),  α ≈ 1 - 2π·fc/Fs
    // α = 0.9999 → cutoff ≈ 32 Hz at 2 MHz SR — transparent for FM audio.
    std::complex<double> dcPrevIn_{0.0, 0.0};
    std::complex<double> dcPrevOut_{0.0, 0.0};
    static constexpr double kDcAlpha = 0.9999;

    // ── Signal power / SNR estimator ─────────────────────────────────────────
    // Running average of |filtered1|² at IF rate — measures received power.
    // Noise floor is calibrated during the first kNoiseFloorWarmup IF-samples
    // after construction (or after setBandwidth), then held fixed.
    double ifPowerAvg_{0.0};
    double noiseFloor_{-1.0};   // < 0 → not yet calibrated
    int    noiseWarmup_{0};
    static constexpr int    kNoiseFloorWarmup = 2000;   // ~8 ms at 250 kHz IF
    static constexpr double kPowerAlpha       = 0.01;   // ~100 IF samples τ

    // Demodulator output power — for diagnosing discriminator stage separately
    double demodPowerAvg_{0.0};
    static constexpr double kDemodAlpha = 0.01;

    // Published to callers via ifRms() / snrDb()
    double ifRmsOut_{0.0};
    double snrDbOut_{0.0};

    int diagBlockCount_{0};
    // Log every 4096 IF-samples ≈ 16 ms at 250 kHz — readable but not spammy
    static constexpr int kDiagInterval = 4096;

    // ── NCO ──────────────────────────────────────────────────────────────────
    double ncoPhase_{0.0};
    double ncoPhaseInc_;

    // ── Stage-1 FIR — complex ────────────────────────────────────────────────
    std::vector<double>               fir1Coeffs_;
    std::vector<std::complex<double>> fir1Delay_;
    int                               fir1Head_{0};
    int                               dec1Counter_{0};

    // ── FM discriminator ─────────────────────────────────────────────────────
    std::complex<double> prevIF_{1.0, 0.0};
    double               demodGain_;

    // ── De-emphasis IIR ───────────────────────────────────────────────────────
    double deemphP_{0.0};
    double deemphState_{0.0};

    // ── Stage-2 FIR — real ───────────────────────────────────────────────────
    std::vector<double> fir2Coeffs_;
    std::vector<double> fir2Delay_;
    int                 fir2Head_{0};
    int                 dec2Counter_{0};

    std::complex<double> fir1Step(std::complex<double> x);
    double               fir2Step(double x);
};
