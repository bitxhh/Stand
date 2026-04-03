#include "AmDemodulator.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

static constexpr double kPi = 3.14159265358979323846;

#ifndef NDEBUG
static constexpr int kFir1Taps = 31;
#else
static constexpr int kFir1Taps = 255;
#endif
static constexpr int kFir2Taps = 255;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AmDemodulator::AmDemodulator(double inputSampleRateHz,
                             double stationOffsetHz,
                             double bandwidthHz)
    : inputSR_(inputSampleRateHz)
    , stationOffset_(stationOffsetHz)
    , bandwidth_(bandwidthHz)
{
    // -- Stage-1 decimation: target IF ~ 500 kHz (same as FM) -----------------
    D1_   = std::max(1, static_cast<int>(std::round(inputSR_ / 500'000.0)));
    ifSR_ = inputSR_ / D1_;

    if (ifSR_ < 20'000.0)
        throw std::invalid_argument(
            "AmDemodulator: IF rate " + std::to_string(static_cast<int>(ifSR_))
            + " Hz is too low. Raise the device sample rate.");

    // -- Stage-2 decimation: D2 = 10 -> audio ~ 50 kHz -----------------------
    audioSR_ = ifSR_ / static_cast<double>(D2_);

    // -- Stage-1 FIR: complex lowpass (anti-alias for D1) ----------------------
    // Fixed wide cutoff — prevents aliasing during decimation.
    // AM bandwidth is narrow (3-20 kHz) but FIR1 operates at input rate where
    // such narrow cutoffs need thousands of taps.  Use ~100 kHz cutoff instead;
    // the actual AM bandwidth is enforced by FIR2 at IF rate.
    static constexpr double kFir1CutoffHz = 100'000.0;
    fir1Delay_.assign(kFir1Taps, {0.0, 0.0});
    fir1Coeffs_ = designLowpassFir(kFir1Taps, kFir1CutoffHz / inputSR_);

    // -- Stage-2 FIR: real lowpass (AM audio bandwidth) -----------------------
    // User-controlled: setBandwidth() redesigns this filter.
    fir2Delay_.assign(kFir2Taps, 0.0);
    setBandwidth(bandwidth_);   // designs FIR2 coefficients

    // -- NCO ------------------------------------------------------------------
    ncoPhaseInc_ = -2.0 * kPi * stationOffset_ / inputSR_;

    // -- Envelope DC removal: IIR highpass at ~20 Hz --------------------------
    envDcAlpha_ = std::exp(-2.0 * kPi * 20.0 / ifSR_);

    LOG_INFO("AmDemodulator: inputSR=" + std::to_string(static_cast<int>(inputSR_))
             + " offset=" + std::to_string(static_cast<int>(stationOffset_)) + " Hz"
             + " D1=" + std::to_string(D1_)
             + " IF=" + std::to_string(static_cast<int>(ifSR_)) + " Hz"
             + " D2=" + std::to_string(D2_)
             + " audio=" + std::to_string(static_cast<int>(audioSR_)) + " Hz"
             + " BW=" + std::to_string(static_cast<int>(bandwidth_)) + " Hz");
}

// ---------------------------------------------------------------------------
// setBandwidth
// ---------------------------------------------------------------------------
void AmDemodulator::setBandwidth(double bandwidthHz) {
    // AM broadcast: 1-20 kHz typical audio bandwidth.
    // FIR2 operates at IF rate (~500 kHz), so cutoffNorm is very feasible.
    bandwidth_ = std::clamp(bandwidthHz, 1'000.0, audioSR_ / 2.0 * 0.9);

    const double cutoffNorm = bandwidth_ / ifSR_;   // e.g. 5000/500000 = 0.01
    fir2Coeffs_ = designLowpassFir(kFir2Taps, cutoffNorm);
    std::fill(fir2Delay_.begin(), fir2Delay_.end(), 0.0);
    fir2Head_ = 0;

    // SNR highpass at the audio cutoff frequency
    hpAlpha_ = std::exp(-2.0 * kPi * bandwidth_ / ifSR_);

    // Reset SNR estimator
    hpState_  = 0.0;
    hpPrevIn_ = 0.0;
    audioPowerAvg_ = 0.0;
    noisePowerAvg_ = 0.0;

    LOG_INFO("AmDemodulator: bandwidth set to "
             + std::to_string(static_cast<int>(bandwidth_)) + " Hz"
             + "  cutoffNorm=" + std::to_string(cutoffNorm)
             + "  FIR2 taps=" + std::to_string(kFir2Taps));
}

// ---------------------------------------------------------------------------
// setOffset
// ---------------------------------------------------------------------------
void AmDemodulator::setOffset(double offsetHz) {
    stationOffset_ = offsetHz;
    ncoPhaseInc_   = -2.0 * kPi * stationOffset_ / inputSR_;

    dcPrevIn_  = {0.0, 0.0};
    dcPrevOut_ = {0.0, 0.0};

    std::fill(fir1Delay_.begin(), fir1Delay_.end(), std::complex<double>{0.0, 0.0});
    fir1Head_    = 0;
    dec1Counter_ = 0;

    envDcPrevIn_ = 0.0;
    envDcState_  = 0.0;

    std::fill(fir2Delay_.begin(), fir2Delay_.end(), 0.0);
    fir2Head_    = 0;
    dec2Counter_ = 0;

    hpState_  = 0.0;
    hpPrevIn_ = 0.0;
    audioPowerAvg_ = 0.0;
    noisePowerAvg_ = 0.0;

    LOG_INFO("AmDemodulator: offset set to "
             + std::to_string(static_cast<int>(offsetHz)) + " Hz");
}

// ---------------------------------------------------------------------------
// FIR design — same Blackman windowed-sinc as FmDemodulator
// ---------------------------------------------------------------------------
std::vector<double> AmDemodulator::designLowpassFir(int numTaps, double cutoffNorm) {
    std::vector<double> h(numTaps);
    const int M   = numTaps - 1;
    const int mid = M / 2;

    for (int n = 0; n < numTaps; ++n) {
        double sinc;
        if (n == mid) {
            sinc = 2.0 * cutoffNorm;
        } else {
            const double x = 2.0 * kPi * cutoffNorm * (n - mid);
            sinc = std::sin(x) / (kPi * (n - mid));
        }
        const double win = 0.42
                         - 0.50 * std::cos(2.0 * kPi * n / M)
                         + 0.08 * std::cos(4.0 * kPi * n / M);
        h[n] = sinc * win;
    }

    double sum = 0.0;
    for (double v : h) sum += v;
    if (sum > 0.0)
        for (double& v : h) v /= sum;

    return h;
}

// ---------------------------------------------------------------------------
// Stage-1 FIR
// ---------------------------------------------------------------------------
void AmDemodulator::fir1Push(std::complex<double> x) {
    fir1Delay_[fir1Head_] = x;
    fir1Head_ = (fir1Head_ + 1) % kFir1Taps;
}

std::complex<double> AmDemodulator::fir1Compute() const {
    std::complex<double> acc{0.0, 0.0};
    int idx = fir1Head_;
    for (int i = 0; i < kFir1Taps; ++i) {
        acc += fir1Coeffs_[i] * fir1Delay_[idx];
        idx  = (idx + 1) % kFir1Taps;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Stage-2 FIR
// ---------------------------------------------------------------------------
double AmDemodulator::fir2Step(double x) {
    fir2Delay_[fir2Head_] = x;
    fir2Head_ = (fir2Head_ + 1) % kFir2Taps;

    double acc = 0.0;
    int idx = fir2Head_;
    for (int i = 0; i < kFir2Taps; ++i) {
        acc += fir2Coeffs_[i] * fir2Delay_[idx];
        idx  = (idx + 1) % kFir2Taps;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Main processing
// ---------------------------------------------------------------------------
QVector<float> AmDemodulator::pushBlock(const QVector<int16_t>& iqBlock) {
    if (iqBlock.size() < 2 || (iqBlock.size() % 2) != 0)
        return {};

    const int numSamples = iqBlock.size() / 2;

    QVector<float> audio;
    audio.reserve(numSamples / (D1_ * D2_) + 4);

    for (int i = 0; i < numSamples; ++i) {

        // -- 1. int16 -> normalised complex -----------------------------------
        const double iVal = static_cast<double>(iqBlock[2 * i])     / 32768.0;
        const double qVal = static_cast<double>(iqBlock[2 * i + 1]) / 32768.0;
        std::complex<double> s{iVal, qVal};

        // -- 2. DC blocker ----------------------------------------------------
        const std::complex<double> dcBlocked =
            s - dcPrevIn_ + kDcAlpha * dcPrevOut_;
        dcPrevIn_  = s;
        dcPrevOut_ = dcBlocked;
        s = dcBlocked;

        // -- 3. NCO frequency shift -------------------------------------------
        s *= std::complex<double>(std::cos(ncoPhase_), std::sin(ncoPhase_));
        ncoPhase_ += ncoPhaseInc_;
        if (ncoPhase_ >  kPi) ncoPhase_ -= 2.0 * kPi;
        if (ncoPhase_ < -kPi) ncoPhase_ += 2.0 * kPi;

        // -- 4. FIR1 push (O(1)) ---------------------------------------------
        fir1Push(s);

        // -- 5. Stage-1 decimation --------------------------------------------
        if (++dec1Counter_ < D1_) continue;
        dec1Counter_ = 0;

        const auto filtered1 = fir1Compute();

        // -- 6. IF power (diagnostic) -----------------------------------------
        const double ifPower = filtered1.real() * filtered1.real()
                             + filtered1.imag() * filtered1.imag();
        ifPowerAvg_ = (1.0 - kPowerAlpha) * ifPowerAvg_ + kPowerAlpha * ifPower;

        // -- 7. AM envelope detection: magnitude ------------------------------
        const double envelope = std::sqrt(ifPower);

        // -- 8. Envelope DC removal (carrier offset) --------------------------
        // IIR highpass at ~20 Hz: passes audio, removes constant carrier level.
        const double envHp = envDcAlpha_ * (envDcState_ + envelope - envDcPrevIn_);
        envDcState_  = envHp;
        envDcPrevIn_ = envelope;

        // -- 8a. HF noise estimator (above audio cutoff) ----------------------
        const double hpOut = hpAlpha_ * (hpState_ + envHp - hpPrevIn_);
        hpState_  = hpOut;
        hpPrevIn_ = envHp;
        noisePowerAvg_ = (1.0 - kSnrAlpha) * noisePowerAvg_
                       + kSnrAlpha * (hpOut * hpOut);

        // -- 9. Stage-2 FIR audio lowpass -------------------------------------
        const double filtered2 = fir2Step(envHp);

        // -- 10. Stage-2 decimation -------------------------------------------
        if (++dec2Counter_ < D2_) continue;
        dec2Counter_ = 0;

        // -- 10a. Audio power for SNR -----------------------------------------
        audioPowerAvg_ = (1.0 - kSnrAlpha) * audioPowerAvg_
                       + kSnrAlpha * (filtered2 * filtered2);

        // -- 11. Output -------------------------------------------------------
        audio.push_back(static_cast<float>(filtered2));
    }

    // -- Diagnostics ----------------------------------------------------------
    diagBlockCount_ += numSamples / D1_;
    if (diagBlockCount_ >= kDiagInterval) {
        diagBlockCount_ = 0;

        const double ifRms = std::sqrt(ifPowerAvg_);

        double snrDb = 0.0;
        const char* quality;
        if (noisePowerAvg_ > 1e-20) {
            snrDb = 10.0 * std::log10(audioPowerAvg_ / noisePowerAvg_);
            if (snrDb < 0.0) snrDb = 0.0;
            quality = snrDb > 6.0 ? "SIGNAL" : snrDb > 2.0 ? "MARGINAL" : "NOISE";
        } else {
            quality = "WARMUP";
        }

        ifRmsOut_ = ifRms;
        snrDbOut_ = snrDb;

        LOG_DEBUG("AmDemodulator"
                  "  if_rms="      + std::to_string(ifRms)
                + "  snr="         + std::to_string(snrDb) + " dB"
                + "  audio_pwr="   + std::to_string(audioPowerAvg_)
                + "  noise_pwr="   + std::to_string(noisePowerAvg_)
                + "  [" + quality + "]");
    }

    return audio;
}
