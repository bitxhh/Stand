#include "FmDemodulator.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr double kPi       = 3.14159265358979323846;
// FIR1: complex anti-alias lowpass before D1 decimation.
// At 500 kHz IF the Nyquist edge is 250 kHz.  With 150 kHz passband the
// transition band is 100 kHz = 0.025 normalised.  Blackman needs ~12/Δf
// taps for full -74 dB stopband → 480, but 255 gives ~-55 dB at Nyquist
// which is sufficient (adjacent FM stations are 200-300 kHz apart).
// Debug keeps 31 taps to avoid USB FIFO overflow at high sample rates.
#ifndef NDEBUG
static constexpr int kFir1Taps = 31;    // debug: fast enough, adequate stopband
#else
static constexpr int kFir1Taps = 255;   // release: good adjacent-channel rejection
#endif
// 255 taps: stopband starts at ~21 kHz, fully rejects FM stereo subcarrier
// (23–53 kHz) before D2=10 decimation to 50 kHz.  At 63 taps the stopband
// only started at ~41 kHz, letting stereo fold into 0–15 kHz audio.
static constexpr int    kFir2Taps = 255;   // real lowpass, ~15 kHz cutoff
static constexpr double kFmMaxDev = 75'000.0;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
FmDemodulator::FmDemodulator(double inputSampleRateHz,
                             double stationOffsetHz,
                             double deemphTauSec,
                             double bandwidthHz)
    : inputSR_(inputSampleRateHz)
    , stationOffset_(stationOffsetHz)
    , deemphTau_(deemphTauSec)
    , bandwidth_(bandwidthHz)
{
    // ── Stage-1 decimation: target IF ≈ 500 kHz ─────────────────────────────
    // 500 kHz (not 250 kHz) gives a 100 kHz transition band (Nyquist 250 kHz
    // minus 150 kHz passband).  255 taps with Blackman window achieve ~-55 dB
    // at 250 kHz — sufficient for adjacent FM channel rejection.
    D1_   = std::max(1, static_cast<int>(std::round(inputSR_ / 500'000.0)));
    ifSR_ = inputSR_ / D1_;

    if (ifSR_ < 400'000.0)
        throw std::invalid_argument(
            "FmDemodulator: IF rate " + std::to_string(static_cast<int>(ifSR_))
            + " Hz is too low for WBFM (need ≥ 400 kHz). "
            "Raise the device sample rate.");

    // ── Stage-2 decimation: fixed D2 = 5 ────────────────────────────────────
    audioSR_ = ifSR_ / static_cast<double>(D2_);

    // ── Stage-1 FIR: complex lowpass ─────────────────────────────────────────
    fir1Delay_.assign(kFir1Taps, {0.0, 0.0});
    setBandwidth(bandwidth_);   // designs FIR1 coefficients

    // ── Stage-2 FIR: real lowpass ─────────────────────────────────────────────
    const double cutoff2 = std::min(15'000.0, audioSR_ / 2.0 * 0.9);
    fir2Coeffs_ = designLowpassFir(kFir2Taps, cutoff2 / ifSR_);  // fc/fs ∈ [0, 0.5]
    fir2Delay_.assign(kFir2Taps, 0.0);

    // ── NCO ──────────────────────────────────────────────────────────────────
    ncoPhaseInc_ = -2.0 * kPi * stationOffset_ / inputSR_;

    // ── FM discriminator gain ─────────────────────────────────────────────────
    demodGain_ = ifSR_ / (2.0 * kPi * kFmMaxDev);

    // ── De-emphasis pole ─────────────────────────────────────────────────────
    deemphP_ = std::exp(-1.0 / (deemphTau_ * ifSR_));

    LOG_INFO("FmDemodulator: inputSR=" + std::to_string(static_cast<int>(inputSR_))
             + " offset=" + std::to_string(static_cast<int>(stationOffset_)) + " Hz"
             + " D1=" + std::to_string(D1_)
             + " IF=" + std::to_string(static_cast<int>(ifSR_)) + " Hz"
             + " D2=" + std::to_string(D2_)
             + " audio=" + std::to_string(static_cast<int>(audioSR_)) + " Hz"
             + " BW=" + std::to_string(static_cast<int>(bandwidth_)) + " Hz"
             + " demodGain=" + std::to_string(demodGain_)
             + " deemph τ=" + std::to_string(static_cast<int>(deemphTau_ * 1e6)) + " µs");
}

// ---------------------------------------------------------------------------
// setBandwidth — redesign FIR1 on the fly, clear delay line
// ---------------------------------------------------------------------------
void FmDemodulator::setBandwidth(double bandwidthHz) {
    // Clamp: ≥ 50 kHz (minimum for WBFM mono), ≤ ifSR/2 * 0.9 (anti-alias guard)
    bandwidth_ = std::clamp(bandwidthHz, 50'000.0, ifSR_ / 2.0 * 0.9);

    const double cutoffNorm = bandwidth_ / inputSR_;   // fc/fs ∈ [0, 0.5]
    fir1Coeffs_ = designLowpassFir(kFir1Taps, cutoffNorm);
    // Clear delay line so the filter starts fresh without history artefacts
    std::fill(fir1Delay_.begin(), fir1Delay_.end(), std::complex<double>{0.0, 0.0});
    fir1Head_ = 0;

    // Reset noise floor — it will re-calibrate over the next kNoiseFloorWarmup samples
    noiseFloor_  = -1.0;
    noiseWarmup_ = 0;

    LOG_INFO("FmDemodulator: bandwidth set to "
             + std::to_string(static_cast<int>(bandwidth_)) + " Hz"
             + "  cutoffNorm=" + std::to_string(cutoffNorm)
             + "  FIR1 taps=" + std::to_string(kFir1Taps));
}

// ---------------------------------------------------------------------------
// setOffset — retune NCO, reset all state that depends on signal position
// ---------------------------------------------------------------------------
void FmDemodulator::setOffset(double offsetHz) {
    stationOffset_ = offsetHz;
    ncoPhaseInc_   = -2.0 * kPi * stationOffset_ / inputSR_;

    // Reset DC blocker — previous input position is now irrelevant
    dcPrevIn_  = {0.0, 0.0};
    dcPrevOut_ = {0.0, 0.0};

    // Reset FIR1 delay line and decimation counter
    std::fill(fir1Delay_.begin(), fir1Delay_.end(), std::complex<double>{0.0, 0.0});
    fir1Head_    = 0;
    dec1Counter_ = 0;

    // Reset discriminator state
    prevIF_ = {1.0, 0.0};

    // Reset FIR2 delay line and decimation counter
    std::fill(fir2Delay_.begin(), fir2Delay_.end(), 0.0);
    fir2Head_    = 0;
    dec2Counter_ = 0;

    // Re-calibrate noise floor at new frequency
    noiseFloor_  = -1.0;
    noiseWarmup_ = 0;

    LOG_INFO("FmDemodulator: offset set to "
             + std::to_string(static_cast<int>(offsetHz)) + " Hz"
             + "  ncoPhaseInc=" + std::to_string(ncoPhaseInc_));
}

// ---------------------------------------------------------------------------
// Windowed-sinc FIR design — Blackman window (-74 dB stopband)
// Identical to BandpassExporter::designLowpassFir — factored here to keep
// FmDemodulator self-contained.
// ---------------------------------------------------------------------------
std::vector<double> FmDemodulator::designLowpassFir(int numTaps, double cutoffNorm) {
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

    // Unity passband gain normalisation
    double sum = 0.0;
    for (double v : h) sum += v;
    if (sum > 0.0)
        for (double& v : h) v /= sum;

    return h;
}

// ---------------------------------------------------------------------------
// Stage-1 FIR — split into push (O(1)) and compute (O(N)) so that the
// expensive dot product is only evaluated at decimation points (every D1
// input samples).  This gives a D1× speedup (8× at the typical SR set).
// ---------------------------------------------------------------------------
void FmDemodulator::fir1Push(std::complex<double> x) {
    fir1Delay_[fir1Head_] = x;
    fir1Head_ = (fir1Head_ + 1) % kFir1Taps;
}

std::complex<double> FmDemodulator::fir1Compute() const {
    std::complex<double> acc{0.0, 0.0};
    int idx = fir1Head_;
    for (int i = 0; i < kFir1Taps; ++i) {
        acc += fir1Coeffs_[i] * fir1Delay_[idx];
        idx  = (idx + 1) % kFir1Taps;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Stage-2 FIR step — one real sample in, one real sample out
// ---------------------------------------------------------------------------
double FmDemodulator::fir2Step(double x) {
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
// Main processing entry point
// ---------------------------------------------------------------------------
QVector<float> FmDemodulator::pushBlock(const QVector<int16_t>& iqBlock) {
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

        // ── 2. DC blocker — removes LO leakage (DC spike at 0 Hz) ────────────
        // H(z) = (1 - z⁻¹) / (1 - α·z⁻¹)
        // Applied BEFORE NCO shift to suppress the hardware DC offset in the
        // raw baseband. Without this, LimeSDR's LO leakage saturates the
        // discriminator when stationOffset == 0 (LO tuned to the station).
        const std::complex<double> dcBlocked =
            s - dcPrevIn_ + kDcAlpha * dcPrevOut_;
        dcPrevIn_  = s;
        dcPrevOut_ = dcBlocked;
        s = dcBlocked;

        // ── 3. NCO frequency shift ────────────────────────────────────────────
        s *= std::complex<double>(std::cos(ncoPhase_), std::sin(ncoPhase_));
        ncoPhase_ += ncoPhaseInc_;
        if (ncoPhase_ >  kPi) ncoPhase_ -= 2.0 * kPi;
        if (ncoPhase_ < -kPi) ncoPhase_ += 2.0 * kPi;

        // ── 4. Stage-1 FIR: push sample into delay line (O(1)) ──────────────
        fir1Push(s);

        // ── 5. Stage-1 decimation — compute FIR only at output points ────────
        if (++dec1Counter_ < D1_) continue;
        dec1Counter_ = 0;

        const auto filtered1 = fir1Compute();   // O(N), but only every D1 samples

        // ── 6. IF signal power — noise floor calibration + SNR ───────────────
        const double ifPower = filtered1.real() * filtered1.real()
                             + filtered1.imag() * filtered1.imag();
        ifPowerAvg_ = (1.0 - kPowerAlpha) * ifPowerAvg_ + kPowerAlpha * ifPower;

        // Track noise floor as a slow-decaying minimum of IF power.
        // FM signals have nearly constant envelope, so the "quiet periods"
        // approach doesn't work.  Instead: initialise to current power, then
        // decay slowly toward the minimum seen so far.  When the signal drops
        // (e.g. briefly off-tune), the floor drifts down; in steady state it
        // sits just below the signal level, giving a realistic SNR estimate.
        // NOTE: the old warmup-based calibration assumed the first 8 ms were
        // signal-free, but LMS_StartStream delivers samples almost immediately,
        // so noiseFloor_ ended up ≈ signal power → SNR always ≈ 0 → NOISE.
        if (noiseFloor_ < 0.0) {
            noiseFloor_ = ifPowerAvg_;
        } else if (ifPowerAvg_ < noiseFloor_) {
            // Current power dipped below floor — update down quickly
            noiseFloor_ = noiseFloor_ * 0.99 + ifPowerAvg_ * 0.01;
        } else {
            // Drift floor up very slowly so it tracks long-term noise changes
            noiseFloor_ = noiseFloor_ * 0.9999 + ifPowerAvg_ * 0.0001;
        }

        // ── 7. FM discriminator ───────────────────────────────────────────────
        const std::complex<double> prod = filtered1 * std::conj(prevIF_);
        prevIF_ = filtered1;
        const double demod = std::atan2(prod.imag(), prod.real()) * demodGain_;

        // Track demodulator output power — used to confirm the discriminator
        // stage is producing valid output (not just noise from atan2).
        demodPowerAvg_ = (1.0 - kDemodAlpha) * demodPowerAvg_
                       + kDemodAlpha * (demod * demod);

        // ── 8. De-emphasis ────────────────────────────────────────────────────
        deemphState_ = (1.0 - deemphP_) * demod + deemphP_ * deemphState_;

        // ── 9. Stage-2 FIR audio lowpass ──────────────────────────────────────
        const double filtered2 = fir2Step(deemphState_);

        // ── 10. Stage-2 decimation ────────────────────────────────────────────
        if (++dec2Counter_ < D2_) continue;
        dec2Counter_ = 0;

        // ── 11. Output ────────────────────────────────────────────────────────
        audio.push_back(static_cast<float>(filtered2));
    }

    // ── Diagnostics — once per kDiagInterval IF-samples ──────────────────────
    // Runs outside the inner loop to avoid branching overhead per sample.
    diagBlockCount_ += numSamples / D1_;   // approximate IF-sample count
    if (diagBlockCount_ >= kDiagInterval) {
        diagBlockCount_ = 0;

        const double ifRms    = std::sqrt(ifPowerAvg_);
        const double demodRms = std::sqrt(demodPowerAvg_);

        // SNR relative to calibrated noise floor.
        // noiseFloor_ < 0 means warmup not finished yet — skip SNR output.
        double snrDb = 0.0;
        const char* quality;
        if (noiseFloor_ > 0.0) {
            snrDb = 10.0 * std::log10(ifPowerAvg_ / noiseFloor_);
            quality = snrDb > 6.0 ? "SIGNAL" : snrDb > 2.0 ? "MARGINAL" : "NOISE";
        } else {
            quality = "WARMUP";
        }

        // Publish for callers (UI meter, etc.)
        ifRmsOut_ = ifRms;
        snrDbOut_ = snrDb;

        LOG_DEBUG("FmDemodulator"
                  "  if_rms="    + std::to_string(ifRms)
                + "  snr="       + std::to_string(snrDb) + " dB"
                + "  demod_rms=" + std::to_string(demodRms)
                + "  [" + quality + "]");
    }

    return audio;
}
