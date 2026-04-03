#pragma once

#include <QVector>
#include <complex>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// AmDemodulator — real-time AM envelope demodulator
//
// DSP chain (all in double precision, float32 output):
//
//   int16 I/Q  ->  DC blocker  ->  NCO freq-shift  ->  FIR1 LPF (complex)
//              ->  decimate D1  ->  IF @ ~500 kHz
//              ->  envelope detection: sqrt(I^2 + Q^2)
//              ->  DC removal (IIR highpass, ~20 Hz)
//              ->  FIR2 LPF (real, cutoff ~5 kHz)
//              ->  decimate D2 = 10  ->  audio @ ~50 kHz
//              ->  QVector<float> (normalised, volume applied externally)
//
// Audio sample rate = inputSR / (D1 * 10).  D1 = round(inputSR / 500 000).
// Same IF and audio SR as FmDemodulator — FmAudioOutput resampler works as-is.
//
// Thread safety: call all methods from the SAME thread (StreamWorker thread).
// ---------------------------------------------------------------------------
class AmDemodulator {
public:
    explicit AmDemodulator(double inputSampleRateHz,
                           double stationOffsetHz,
                           double bandwidthHz = 5'000.0);

    [[nodiscard]] QVector<float> pushBlock(const QVector<int16_t>& iqBlock);

    void setBandwidth(double bandwidthHz);
    void setOffset(double offsetHz);

    [[nodiscard]] double audioSampleRate() const { return audioSR_; }
    [[nodiscard]] double ifSampleRate()    const { return ifSR_;    }
    [[nodiscard]] int    decimation1()     const { return D1_;      }
    [[nodiscard]] double bandwidth()       const { return bandwidth_; }

    [[nodiscard]] double ifRms()  const { return ifRmsOut_; }
    [[nodiscard]] double snrDb()  const { return snrDbOut_; }

    static std::vector<double> designLowpassFir(int numTaps, double cutoffNorm);

private:
    // -- Parameters -----------------------------------------------------------
    double inputSR_;
    double stationOffset_;
    double bandwidth_;

    int    D1_;
    double ifSR_;
    int    D2_{10};
    double audioSR_;

    // -- DC blocker (before NCO) — removes LO leakage -------------------------
    std::complex<double> dcPrevIn_{0.0, 0.0};
    std::complex<double> dcPrevOut_{0.0, 0.0};
    static constexpr double kDcAlpha = 0.9999;

    // -- IF power tracking (diagnostic) ----------------------------------------
    double ifPowerAvg_{0.0};
    static constexpr double kPowerAlpha = 0.01;

    // -- SNR estimator ---------------------------------------------------------
    // Audio power vs HF noise power (above audio cutoff, below IF Nyquist).
    double hpAlpha_{0.0};
    double hpState_{0.0};
    double hpPrevIn_{0.0};
    double audioPowerAvg_{0.0};
    double noisePowerAvg_{0.0};
    static constexpr double kSnrAlpha = 0.005;

    // Published to callers
    double ifRmsOut_{0.0};
    double snrDbOut_{0.0};

    int diagBlockCount_{0};
    static constexpr int kDiagInterval = 4096;

    // -- NCO -------------------------------------------------------------------
    double ncoPhase_{0.0};
    double ncoPhaseInc_;

    // -- Stage-1 FIR (complex) -------------------------------------------------
    std::vector<double>               fir1Coeffs_;
    std::vector<std::complex<double>> fir1Delay_;
    int                               fir1Head_{0};
    int                               dec1Counter_{0};

    // -- Envelope DC removal (after sqrt) --------------------------------------
    // IIR highpass at ~20 Hz removes carrier DC; audio AC passes through.
    double envDcAlpha_{0.0};
    double envDcPrevIn_{0.0};
    double envDcState_{0.0};

    // -- Stage-2 FIR (real) ----------------------------------------------------
    std::vector<double> fir2Coeffs_;
    std::vector<double> fir2Delay_;
    int                 fir2Head_{0};
    int                 dec2Counter_{0};

    void                 fir1Push(std::complex<double> x);
    std::complex<double> fir1Compute() const;
    double               fir2Step(double x);
};
