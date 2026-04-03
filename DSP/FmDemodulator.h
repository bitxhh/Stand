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
//   int16 I/Q  →  DC blocker  →  NCO freq-shift  →  FIR1 LPF (31/255 taps, complex)
//              →  decimate D1  →  IF @ ~500 kHz
//              →  FM discriminator (atan2 of conjugate product)
//              →  de-emphasis IIR (first-order, τ = 50 µs EU / 75 µs US)
//              →  FIR2 LPF (255 taps, real, cutoff 15 kHz)
//              →  decimate D2 = 10  →  audio @ ~50 kHz
//              →  QVector<float> (normalised, volume applied externally)
//
// Audio sample rate = inputSR / (D1 * 10).  D1 = round(inputSR / 500 000).
// For the standard LimeSDR SR set {2.5, 4, 5, 8, 10, 15, 20} MHz,
// the IF is always exactly 500 kHz and audio is always exactly 50 kHz.
//
// Why 500 kHz IF (not 250 kHz):
//   FIR1 must anti-alias D1 decimation.  Transition band = (IF/2 - BW) / inputSR.
//   At 250 kHz IF: transition = 25 kHz / inputSR → needs ~1000 taps (impractical).
//   At 500 kHz IF: transition = 100 kHz / inputSR → ~255 taps give ~-55 dB at Nyquist.
//   Wider transition = much better adjacent-channel rejection.
//
// Why 255-tap FIR2 (not 63):
//   FM multiplex after discriminator contains stereo subcarrier at 23–53 kHz.
//   With D2=10 output at 50 kHz, content at 35–50 kHz folds into 0–15 kHz (audio).
//   63 taps: stopband starts at ~41 kHz — stereo bleeds into audio.
//   255 taps: stopband starts at ~21 kHz — stereo fully rejected before decimation.
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
    // deemphTauSec      — 50e-6 (Europe / Russia, default) or 75e-6 (USA / Japan)
    explicit FmDemodulator(double inputSampleRateHz,
                           double stationOffsetHz,
                           double deemphTauSec    = 50e-6,
                           double bandwidthHz     = 150'000.0);

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
    int    D2_{10};   // stage-2: ~500 kHz → ~50 kHz audio (fixed)
    double audioSR_;

    // ── DC blocker — high-pass IIR before NCO ────────────────────────────────
    // Removes LO leakage (DC spike at 0 Hz in baseband).
    // H(z) = (1 - z⁻¹) / (1 - α·z⁻¹),  α ≈ 1 - 2π·fc/Fs
    // α = 0.9999 → cutoff ≈ 32 Hz at 2 MHz SR — transparent for FM audio.
    std::complex<double> dcPrevIn_{0.0, 0.0};
    std::complex<double> dcPrevOut_{0.0, 0.0};
    static constexpr double kDcAlpha = 0.9999;

    // ── Signal power / SNR estimator (FM quieting) ─────────────────────────
    // FM has constant envelope — IF power alone cannot distinguish signal from
    // noise.  Instead, measure noise AFTER the discriminator:
    //   - Audio power: running avg of FIR2 output² (< 15 kHz, the useful audio)
    //   - Noise power: running avg of HP-filtered discriminator output² (> 15 kHz)
    // SNR = 10·log10(audioPower / noisePower).
    // A strong FM station "quiets" the HF noise; weak/absent signal → broadband.
    double ifPowerAvg_{0.0};
    static constexpr double kPowerAlpha = 0.01;   // ~100 IF samples τ

    // IIR highpass on discriminator output — extracts noise above ~15 kHz
    // H(z) = α·(y[n-1] + x[n] - x[n-1]),  α = exp(-2π·fc/fs)
    double hpAlpha_{0.0};        // computed in ctor from ifSR_
    double hpState_{0.0};        // filter state
    double hpPrevIn_{0.0};       // previous discriminator sample

    double audioPowerAvg_{0.0};  // running avg of FIR2 output² (audio band)
    double noisePowerAvg_{0.0};  // running avg of HP output² (HF noise)
    static constexpr double kSnrAlpha = 0.005;   // slower EMA for stable SNR

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

    void                 fir1Push(std::complex<double> x);
    std::complex<double> fir1Compute() const;
    double               fir2Step(double x);
};
