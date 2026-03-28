#include "BandpassExporter.h"
#include "Logger.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr double kPi      = 3.14159265358979323846;
static constexpr int    kFirTaps = 127;   // odd → symmetric, linear phase

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
BandpassExporter::BandpassExporter(double inputSampleRateHz,
                                   double stationOffsetHz,
                                   double bandwidthHz,
                                   double outputSampleRateHz)
    : inputSR_(inputSampleRateHz)
    , stationOffset_(stationOffsetHz)
    , bandwidth_(bandwidthHz)
    , outputSR_(outputSampleRateHz)
{
    // ── Validate decimation ──────────────────────────────────────────────────
    decimation_ = static_cast<int>(std::round(inputSR_ / outputSR_));
    if (decimation_ < 1)
        throw std::invalid_argument("outputSampleRateHz must be ≤ inputSampleRateHz");

    const double actualOutputSR = inputSR_ / decimation_;
    if (std::abs(actualOutputSR - outputSR_) > 1.0) {
        LOG_WARN("BandpassExporter: inputSR / outputSR is not integer — "
                 "actual output SR will be " + std::to_string(actualOutputSR) + " Hz");
        outputSR_ = actualOutputSR;
    }

    // ── Design FIR lowpass ───────────────────────────────────────────────────
    // Cutoff = min(bandwidth, outputSR/2 * 0.9) normalised to inputSR/2.
    // The 0.9 guard prevents spectral leakage right at the Nyquist edge
    // after decimation.
    const double cutoff = std::min(bandwidth_,
                                   outputSR_ / 2.0 * 0.9);
    const double cutoffNorm = cutoff / (inputSR_ / 2.0);
    firCoeffs_    = designLowpassFir(kFirTaps, cutoffNorm);
    firDelayLine_.assign(kFirTaps, {0.0, 0.0});

    // ── NCO ──────────────────────────────────────────────────────────────────
    // Negative sign: to shift a signal at +offset to DC, multiply by e^{-j2π·offset·n/SR}.
    ncoPhaseIncrement_ = -2.0 * kPi * stationOffset_ / inputSR_;

    LOG_INFO("BandpassExporter: SR=" + std::to_string(inputSR_)
             + " offset=" + std::to_string(stationOffset_)
             + " BW=" + std::to_string(bandwidth_)
             + " outSR=" + std::to_string(outputSR_)
             + " decimation=" + std::to_string(decimation_));
}

// ---------------------------------------------------------------------------
// FIR design — windowed sinc (Blackman window for high stopband attenuation)
// ---------------------------------------------------------------------------
std::vector<double> BandpassExporter::designLowpassFir(int numTaps, double cutoffNorm) {
    std::vector<double> h(numTaps);
    const int M = numTaps - 1;   // filter order
    const int mid = M / 2;

    for (int n = 0; n < numTaps; ++n) {
        // Ideal sinc lowpass
        double sinc;
        if (n == mid) {
            sinc = 2.0 * cutoffNorm;   // limit of sinc at 0
        } else {
            const double x = 2.0 * kPi * cutoffNorm * (n - mid);
            sinc = std::sin(x) / (kPi * (n - mid));
        }

        // Blackman window: -74 dB stopband, good for SDR use
        const double window = 0.42
                            - 0.50 * std::cos(2.0 * kPi * n / M)
                            + 0.08 * std::cos(4.0 * kPi * n / M);

        h[n] = sinc * window;
    }

    // Normalise to unity passband gain
    double sum = 0.0;
    for (double v : h) sum += v;
    if (sum > 0.0)
        for (double& v : h) v /= sum;

    return h;
}

// ---------------------------------------------------------------------------
// FIR filter — one sample in, one sample out (complex)
// ---------------------------------------------------------------------------
std::complex<double> BandpassExporter::filterSample(std::complex<double> x) {
    // Write new sample into circular delay line
    firDelayLine_[firHead_] = x;
    firHead_ = (firHead_ + 1) % kFirTaps;

    // Convolve
    std::complex<double> acc{0.0, 0.0};
    int idx = firHead_;
    for (int i = 0; i < kFirTaps; ++i) {
        acc += firCoeffs_[i] * firDelayLine_[idx];
        idx  = (idx + 1) % kFirTaps;
    }
    return acc;
}

// ---------------------------------------------------------------------------
// WAV I/O
// ---------------------------------------------------------------------------
bool BandpassExporter::open(const QString& path) {
    fileHandle_ = std::fopen(path.toStdString().c_str(), "wb");
    if (!fileHandle_) {
        LOG_ERROR("BandpassExporter: cannot open " + path.toStdString());
        return false;
    }
    samplesWritten_   = 0;
    decimationCounter_ = 0;
    ncoPhase_          = 0.0;
    firDelayLine_.assign(kFirTaps, {0.0, 0.0});
    firHead_ = 0;

    writeWavHeader(0);   // placeholder — patched in close()
    LOG_INFO("BandpassExporter: opened " + path.toStdString());
    return true;
}

void BandpassExporter::close() {
    if (!fileHandle_) return;
    patchWavHeader();
    std::fclose(fileHandle_);
    fileHandle_ = nullptr;
    LOG_INFO("BandpassExporter: closed, wrote "
             + std::to_string(samplesWritten_) + " IQ pairs at "
             + std::to_string(static_cast<int>(outputSR_)) + " Hz");
}

// ---------------------------------------------------------------------------
// Main processing loop
// ---------------------------------------------------------------------------
void BandpassExporter::pushBlock(const QVector<int16_t>& iqBlock) {
    if (!fileHandle_) return;
    if (iqBlock.size() < 2 || (iqBlock.size() % 2) != 0) return;

    const int numSamples = iqBlock.size() / 2;

    for (int i = 0; i < numSamples; ++i) {
        // ── 1. Convert int16 to normalised complex float ─────────────────────
        const double iVal = static_cast<double>(iqBlock[2 * i])     / 32768.0;
        const double qVal = static_cast<double>(iqBlock[2 * i + 1]) / 32768.0;
        std::complex<double> sample{iVal, qVal};

        // ── 2. Frequency shift: multiply by e^{j·phase} ─────────────────────
        //    Wraps phase to [-π, π] every block to prevent floating-point drift.
        sample *= std::complex<double>(std::cos(ncoPhase_), std::sin(ncoPhase_));
        ncoPhase_ += ncoPhaseIncrement_;
        if (ncoPhase_ >  kPi) ncoPhase_ -= 2.0 * kPi;
        if (ncoPhase_ < -kPi) ncoPhase_ += 2.0 * kPi;

        // ── 3. FIR lowpass ───────────────────────────────────────────────────
        const auto filtered = filterSample(sample);

        // ── 4. Decimate ──────────────────────────────────────────────────────
        ++decimationCounter_;
        if (decimationCounter_ < decimation_) continue;
        decimationCounter_ = 0;

        // ── 5. Write I and Q as float32 ──────────────────────────────────────
        writeFloat32(static_cast<float>(filtered.real()));
        writeFloat32(static_cast<float>(filtered.imag()));
        ++samplesWritten_;
    }
}

// ---------------------------------------------------------------------------
// WAV helpers
// ---------------------------------------------------------------------------

// Writes a 4-byte little-endian uint32 to file
static void writeU32(FILE* f, uint32_t v) {
    uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFF),
        static_cast<uint8_t>((v >> 8)  & 0xFF),
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >> 24) & 0xFF)
    };
    std::fwrite(b, 1, 4, f);
}

static void writeU16(FILE* f, uint16_t v) {
    uint8_t b[2] = { static_cast<uint8_t>(v & 0xFF),
                     static_cast<uint8_t>((v >> 8) & 0xFF) };
    std::fwrite(b, 1, 2, f);
}

void BandpassExporter::writeFloat32(float value) {
    std::fwrite(&value, sizeof(float), 1, fileHandle_);
}

void BandpassExporter::writeWavHeader(int64_t numSamples) {
    // RIFF WAV, IEEE float32, 2 channels (I=left, Q=right)
    const uint32_t sampleRate   = static_cast<uint32_t>(outputSR_);
    const uint16_t numChannels  = 2;
    const uint16_t bitsPerSmp   = 32;
    const uint32_t byteRate     = sampleRate * numChannels * (bitsPerSmp / 8);
    const uint16_t blockAlign   = numChannels * (bitsPerSmp / 8);
    const uint32_t dataBytes    = static_cast<uint32_t>(numSamples * numChannels * sizeof(float));
    const uint32_t chunkSize    = 36 + dataBytes;   // RIFF chunk size

    std::fwrite("RIFF", 1, 4, fileHandle_);
    writeU32(fileHandle_, chunkSize);
    std::fwrite("WAVE", 1, 4, fileHandle_);

    // fmt sub-chunk
    std::fwrite("fmt ", 1, 4, fileHandle_);
    writeU32(fileHandle_, 18);          // sub-chunk size (18 = PCM + extra size field)
    writeU16(fileHandle_, 3);           // audio format: 3 = IEEE float
    writeU16(fileHandle_, numChannels);
    writeU32(fileHandle_, sampleRate);
    writeU32(fileHandle_, byteRate);
    writeU16(fileHandle_, blockAlign);
    writeU16(fileHandle_, bitsPerSmp);
    writeU16(fileHandle_, 0);           // extra params size = 0

    // data sub-chunk
    std::fwrite("data", 1, 4, fileHandle_);
    writeU32(fileHandle_, dataBytes);
}

void BandpassExporter::patchWavHeader() {
    if (!fileHandle_) return;
    std::rewind(fileHandle_);
    writeWavHeader(samplesWritten_);
    std::fflush(fileHandle_);
}
