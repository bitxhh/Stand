#include "BaseDemodulator.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
BaseDemodulator::BaseDemodulator(double inputSR, double stationOffsetHz,
                                 double fir1CutoffHz, double fir2CutoffHz,
                                 double snrHpCutoffHz, double minIfHz,
                                 int fir1Taps, int fir2Taps)
    : inputSR_(inputSR)
    , stationOffset_(stationOffsetHz)
    , bandwidth_(0.0)
    , fir1Taps_(fir1Taps)
    , fir2Taps_(fir2Taps)
{
    D1_   = std::max(1, static_cast<int>(std::round(inputSR_ / 500'000.0)));
    ifSR_ = inputSR_ / D1_;

    if (ifSR_ < minIfHz)
        throw std::invalid_argument(
            std::string("Demodulator: IF rate ") + std::to_string(static_cast<int>(ifSR_))
            + " Hz is too low (need >= " + std::to_string(static_cast<int>(minIfHz))
            + " Hz). Raise the device sample rate.");

    audioSR_ = ifSR_ / static_cast<double>(D2_);

    // ── FIR1: complex anti-alias lowpass ─────────────────────────────────────
    fir1Delay_.assign(fir1Taps_, {0.0, 0.0});
    fir1Coeffs_ = dsp::designLowpassFir(fir1Taps_, fir1CutoffHz / inputSR_);

    // ── FIR2: real audio lowpass ─────────────────────────────────────────────
    fir2Delay_.assign(fir2Taps_, 0.0);
    const double cutoff2 = std::min(fir2CutoffHz, audioSR_ / 2.0 * 0.9);
    fir2Coeffs_ = dsp::designLowpassFir(fir2Taps_, cutoff2 / ifSR_);

    // ── NCO ──────────────────────────────────────────────────────────────────
    nco_.setFrequency(stationOffset_, inputSR_);

    // ── SNR highpass ─────────────────────────────────────────────────────────
    snrHp_.setCutoff(snrHpCutoffHz, ifSR_);
}

// ---------------------------------------------------------------------------
// redesignFir1 / redesignFir2
// ---------------------------------------------------------------------------
void BaseDemodulator::redesignFir1(double cutoffHz) {
    fir1Coeffs_ = dsp::designLowpassFir(fir1Taps_, cutoffHz / inputSR_);
    std::fill(fir1Delay_.begin(), fir1Delay_.end(), std::complex<double>{0.0, 0.0});
    fir1Head_ = 0;
}

void BaseDemodulator::redesignFir2(double cutoffHz) {
    const double cutoff = std::min(cutoffHz, audioSR_ / 2.0 * 0.9);
    fir2Coeffs_ = dsp::designLowpassFir(fir2Taps_, cutoff / ifSR_);
    std::fill(fir2Delay_.begin(), fir2Delay_.end(), 0.0);
    fir2Head_ = 0;
}

void BaseDemodulator::setSnrHpCutoff(double cutoffHz) {
    snrHp_.setCutoff(cutoffHz, ifSR_);
}

void BaseDemodulator::resetSnr() {
    snrHp_.reset();
    audioPowerAvg_ = 0.0;
    noisePowerAvg_ = 0.0;
}

// ---------------------------------------------------------------------------
// setOffset
// ---------------------------------------------------------------------------
void BaseDemodulator::setOffset(double offsetHz) {
    stationOffset_ = offsetHz;
    nco_.setFrequency(stationOffset_, inputSR_);

    dc_.reset();

    std::fill(fir1Delay_.begin(), fir1Delay_.end(), std::complex<double>{0.0, 0.0});
    fir1Head_    = 0;
    dec1Counter_ = 0;

    std::fill(fir2Delay_.begin(), fir2Delay_.end(), 0.0);
    fir2Head_    = 0;
    dec2Counter_ = 0;

    resetSnr();
    resetDemodState();

    LOG_INFO(std::string(demodName()) + ": offset set to "
             + std::to_string(static_cast<int>(offsetHz)) + " Hz");
}

// ---------------------------------------------------------------------------
// FIR1
// ---------------------------------------------------------------------------
void BaseDemodulator::fir1Push(std::complex<double> x) {
    fir1Delay_[fir1Head_] = x;
    fir1Head_ = (fir1Head_ + 1) % fir1Taps_;
}

std::complex<double> BaseDemodulator::fir1Compute() const {
    std::complex<double> acc{0.0, 0.0};
    int idx = fir1Head_;
    for (int i = 0; i < fir1Taps_; ++i) {
        acc += fir1Coeffs_[i] * fir1Delay_[idx];
        idx  = (idx + 1) % fir1Taps_;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// FIR2
// ---------------------------------------------------------------------------
double BaseDemodulator::fir2Step(double x) {
    fir2Delay_[fir2Head_] = x;
    fir2Head_ = (fir2Head_ + 1) % fir2Taps_;

    double acc = 0.0;
    int idx = fir2Head_;
    for (int i = 0; i < fir2Taps_; ++i) {
        acc += fir2Coeffs_[i] * fir2Delay_[idx];
        idx  = (idx + 1) % fir2Taps_;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Main processing
// ---------------------------------------------------------------------------
QVector<float> BaseDemodulator::pushBlock(const QVector<int16_t>& iqBlock) {
    if (iqBlock.size() < 2 || (iqBlock.size() % 2) != 0)
        return {};

    const int numSamples = iqBlock.size() / 2;

    QVector<float> audio;
    audio.reserve(numSamples / (D1_ * D2_) + 4);

    for (int i = 0; i < numSamples; ++i) {

        // ── 1. int16 → normalised complex ────────────────────────────────────
        const double iVal = static_cast<double>(iqBlock[2 * i])     / 32768.0;
        const double qVal = static_cast<double>(iqBlock[2 * i + 1]) / 32768.0;
        std::complex<double> s{iVal, qVal};

        // ── 2. DC blocker ────────────────────────────────────────────────────
        s = dc_.process(s);

        // ── 3. NCO frequency shift ───────────────────────────────────────────
        s = nco_.mix(s);

        // ── 4. FIR1 push (O(1)) ─────────────────────────────────────────────
        fir1Push(s);

        // ── 5. Stage-1 decimation ────────────────────────────────────────────
        if (++dec1Counter_ < D1_) continue;
        dec1Counter_ = 0;

        const auto filtered1 = fir1Compute();

        // ── 6. IF power (diagnostic) ─────────────────────────────────────────
        const double ifPower = filtered1.real() * filtered1.real()
                             + filtered1.imag() * filtered1.imag();
        ifPowerAvg_ = (1.0 - kPowerAlpha) * ifPowerAvg_ + kPowerAlpha * ifPower;

        // ── 7. Subclass demodulation ─────────────────────────────────────────
        const auto [audioSample, snrSample] = demodulateIF(filtered1, ifPower);

        // ── 8. SNR noise estimator ───────────────────────────────────────────
        const double hpOut = snrHp_.process(snrSample);
        noisePowerAvg_ = (1.0 - kSnrAlpha) * noisePowerAvg_
                       + kSnrAlpha * (hpOut * hpOut);

        // ── 9. FIR2 audio lowpass ────────────────────────────────────────────
        const double filtered2 = fir2Step(audioSample);

        // ── 10. Stage-2 decimation ───────────────────────────────────────────
        if (++dec2Counter_ < D2_) continue;
        dec2Counter_ = 0;

        // ── 10a. Audio power for SNR ─────────────────────────────────────────
        audioPowerAvg_ = (1.0 - kSnrAlpha) * audioPowerAvg_
                       + kSnrAlpha * (filtered2 * filtered2);

        // ── 11. Output ───────────────────────────────────────────────────────
        audio.push_back(static_cast<float>(filtered2));
    }

    // ── Diagnostics ──────────────────────────────────────────────────────────
    diagBlockCount_ += numSamples / D1_;
    if (diagBlockCount_ >= kDiagInterval) {
        diagBlockCount_ = 0;

        const double ifRms = std::sqrt(ifPowerAvg_);

        double snr = 0.0;
        const char* quality;
        if (noisePowerAvg_ > 1e-20) {
            snr = 10.0 * std::log10(audioPowerAvg_ / noisePowerAvg_);
            if (snr < 0.0) snr = 0.0;
            quality = snr > 6.0 ? "SIGNAL" : snr > 2.0 ? "MARGINAL" : "NOISE";
        } else {
            quality = "WARMUP";
        }

        ifRmsOut_ = ifRms;
        snrDbOut_ = snr;

        LOG_DEBUG(std::string(demodName())
                  + "  if_rms="    + std::to_string(ifRms)
                  + "  snr="       + std::to_string(snr) + " dB"
                  + "  audio_pwr=" + std::to_string(audioPowerAvg_)
                  + "  noise_pwr=" + std::to_string(noisePowerAvg_)
                  + "  [" + quality + "]");
    }

    return audio;
}
