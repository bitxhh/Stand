#pragma once

#include "DspUtils.h"

#include <QVector>
#include <complex>
#include <vector>

// ---------------------------------------------------------------------------
// Default FIR tap counts — shared by FM and AM subclasses.
// Debug uses 31 taps to avoid USB FIFO overflow at high sample rates.
// ---------------------------------------------------------------------------
#ifndef NDEBUG
inline constexpr int kDefaultFir1Taps = 31;
#else
inline constexpr int kDefaultFir1Taps = 255;
#endif
inline constexpr int kDefaultFir2Taps = 255;

// ---------------------------------------------------------------------------
// BaseDemodulator — common DSP pipeline for all demodulators.
//
//   float32 I/Q  →  DC blocker  →  NCO shift  →  FIR1 LPF (complex)
//              →  decimate D1  →  IF @ ~500 kHz
//              →  [virtual demodulateIF]
//              →  FIR2 LPF (real)
//              →  decimate D2 = 10  →  audio @ ~50 kHz
//
// Subclasses implement demodulateIF() — the only stage that differs:
//   FM: discriminator + de-emphasis
//   AM: envelope + DC removal
//   SSB/NFM/CW: future
//
// Thread safety: call all methods from the SAME thread (RxWorker thread).
// ---------------------------------------------------------------------------
class BaseDemodulator {
public:
    virtual ~BaseDemodulator() = default;

    [[nodiscard]] QVector<float> pushBlock(const float* iq, int count);
    void setOffset(double offsetHz);

    [[nodiscard]] double audioSampleRate() const { return audioSR_; }
    [[nodiscard]] double ifSampleRate()    const { return ifSR_;    }
    [[nodiscard]] int    decimation1()     const { return D1_;      }
    [[nodiscard]] double bandwidth()       const { return bandwidth_; }
    [[nodiscard]] double ifRms()           const { return ifRmsOut_; }
    [[nodiscard]] double snrDb()           const { return snrDbOut_; }

protected:
    // Result of subclass demodulation: two real samples.
    struct DemodResult {
        double audioSample;   // → FIR2 input
        double snrSample;     // → noise HP estimator input
    };

    BaseDemodulator(double inputSR, double stationOffsetHz,
                    double fir1CutoffHz, double fir2CutoffHz,
                    double snrHpCutoffHz, double minIfHz,
                    int fir1Taps = kDefaultFir1Taps,
                    int fir2Taps = kDefaultFir2Taps);

    // Subclass implements: demodulate one IF-rate sample.
    // ifSample: complex signal after FIR1 + D1 decimation.
    // ifPower:  |ifSample|² (already computed for diagnostics).
    virtual DemodResult demodulateIF(std::complex<double> ifSample, double ifPower) = 0;

    // Called after base resets state in setOffset(). Override to reset
    // subclass-specific state (discriminator, de-emphasis, etc.).
    virtual void resetDemodState() {}

    // Subclass name for log messages.
    virtual const char* demodName() const = 0;

    // Subclass tools — redesign filters or SNR estimator on the fly.
    void redesignFir1(double cutoffHz);
    void redesignFir2(double cutoffHz);
    void setSnrHpCutoff(double cutoffHz);
    void resetSnr();

    // Accessible by subclass
    double inputSR_;
    double ifSR_;
    double audioSR_;
    int    D1_;
    double bandwidth_;   // user-facing bandwidth (meaning depends on subclass)

private:
    double stationOffset_;
    int    D2_{10};
    int    fir1Taps_;
    int    fir2Taps_;

    // ── DSP blocks ───────────────────────────────────────────────────────────
    dsp::DcBlocker    dc_;
    dsp::Nco          nco_;
    dsp::IirHighpass1 snrHp_;

    // ── IF power ─────────────────────────────────────────────────────────────
    double ifPowerAvg_{0.0};
    static constexpr double kPowerAlpha = 0.01;

    // ── SNR estimator ────────────────────────────────────────────────────────
    double audioPowerAvg_{0.0};
    double noisePowerAvg_{0.0};
    static constexpr double kSnrAlpha = 0.005;

    double ifRmsOut_{0.0};
    double snrDbOut_{0.0};

    int diagBlockCount_{0};
    static constexpr int kDiagInterval = 4096;

    // ── Stage-1 FIR (complex) ────────────────────────────────────────────────
    std::vector<double>               fir1Coeffs_;
    std::vector<std::complex<double>> fir1Delay_;
    int                               fir1Head_{0};
    int                               dec1Counter_{0};

    // ── Stage-2 FIR (real) ───────────────────────────────────────────────────
    std::vector<double> fir2Coeffs_;
    std::vector<double> fir2Delay_;
    int                 fir2Head_{0};
    int                 dec2Counter_{0};

    void                 fir1Push(std::complex<double> x);
    std::complex<double> fir1Compute() const;
    double               fir2Step(double x);
};
