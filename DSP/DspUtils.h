#pragma once

#include <complex>
#include <vector>
#include <cmath>

namespace dsp {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Blackman-windowed sinc FIR lowpass design.
// cutoffNorm = fc / fs, range [0, 0.5].  Returns unity-gain normalised taps.
// ---------------------------------------------------------------------------
std::vector<double> designLowpassFir(int numTaps, double cutoffNorm);

// ---------------------------------------------------------------------------
// DC blocker — first-order IIR highpass for complex I/Q.
// Removes LO leakage (DC spike at 0 Hz in baseband).
// H(z) = (1 - z^-1) / (1 - alpha * z^-1)
// alpha = 0.9999 → cutoff ~32 Hz at 2 MHz SR.
// ---------------------------------------------------------------------------
struct DcBlocker {
    std::complex<double> prevIn{0.0, 0.0};
    std::complex<double> prevOut{0.0, 0.0};
    double alpha = 0.9999;

    std::complex<double> process(std::complex<double> s) {
        auto out = s - prevIn + alpha * prevOut;
        prevIn  = s;
        prevOut = out;
        return out;
    }

    void reset() {
        prevIn  = {0.0, 0.0};
        prevOut = {0.0, 0.0};
    }
};

// ---------------------------------------------------------------------------
// NCO — numerically controlled oscillator for frequency shifting.
// mix() multiplies input by e^{j*phase} and advances phase.
// ---------------------------------------------------------------------------
struct Nco {
    double phase    = 0.0;
    double phaseInc = 0.0;

    void setFrequency(double offsetHz, double sampleRate) {
        phaseInc = -2.0 * kPi * offsetHz / sampleRate;
    }

    std::complex<double> mix(std::complex<double> s) {
        auto result = s * std::complex<double>(std::cos(phase), std::sin(phase));
        phase += phaseInc;
        if (phase >  kPi) phase -= 2.0 * kPi;
        if (phase < -kPi) phase += 2.0 * kPi;
        return result;
    }

    void reset() { phase = 0.0; }
};

// ---------------------------------------------------------------------------
// First-order IIR highpass.
// H(z) = alpha * (y[n-1] + x[n] - x[n-1])
// alpha = exp(-2*pi*fc/fs)
// ---------------------------------------------------------------------------
struct IirHighpass1 {
    double alpha  = 0.0;
    double state  = 0.0;
    double prevIn = 0.0;

    void setCutoff(double fc, double fs) {
        alpha = std::exp(-2.0 * kPi * fc / fs);
    }

    double process(double x) {
        double out = alpha * (state + x - prevIn);
        state  = out;
        prevIn = x;
        return out;
    }

    void reset() {
        state  = 0.0;
        prevIn = 0.0;
    }
};

} // namespace dsp
