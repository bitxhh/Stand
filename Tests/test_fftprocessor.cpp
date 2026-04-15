#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "FftProcessor.h"

#include <cmath>
#include <algorithm>

static constexpr double kPi = 3.14159265358979323846;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Build an interleaved I/Q buffer for a complex tone at freqHz.
// Signal = exp(j*2π*freqHz*n/sr)  →  I=cos(...), Q=sin(...)
static QVector<float> makeComplexTone(int n, double sr, double freqHz,
                                      double amplitude = 0.9)
{
    QVector<float> iq(n * 2);
    for (int i = 0; i < n; ++i) {
        const double phase = 2.0 * kPi * freqHz * i / sr;
        iq[2 * i]     = static_cast<float>(std::cos(phase) * amplitude);
        iq[2 * i + 1] = static_cast<float>(std::sin(phase) * amplitude);
    }
    return iq;
}

// Returns the index of the bin with maximum power in a FftFrame.
static int peakBin(const FftFrame& frame) {
    return static_cast<int>(
        std::max_element(frame.powerDb.begin(), frame.powerDb.end())
        - frame.powerDb.begin());
}

// ─────────────────────────────────────────────────────────────────────────────
// T8a — DC input: peak must land at the centre bin (0 Hz offset)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FftProcessor: DC tone peaks at centre bin", "[fft]") {
    // DC: I = constant, Q = 0  →  frequency = 0 Hz = centerFreqMHz
    constexpr int    kN   = 4096;
    constexpr double kSR  = 4'000'000.0;
    constexpr double kCtr = 102.0;

    QVector<float> iq(kN * 2, 0.0f);
    for (int i = 0; i < kN; ++i) {
        iq[2 * i]     = 0.885f;   // I ≈ 0.885 full scale
        iq[2 * i + 1] = 0.0f;    // Q = 0
    }

    const FftFrame frame = FftProcessor::process(iq.constData(), iq.size() / 2, kCtr, kSR);

    REQUIRE(frame.powerDb.size() == kN);
    REQUIRE(frame.freqMHz.size() == kN);

    const int peak = peakBin(frame);
    const int centre = kN / 2;

    INFO("Peak bin: " << peak << "  centre bin: " << centre);
    INFO("Peak freq: " << frame.freqMHz[peak] << " MHz");

    // Peak must be within ±1 bin of centre (DC = centerFreq after FFT-shift)
    CHECK(std::abs(peak - centre) <= 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// T8b — Known-offset tone: peak lands at the expected frequency bin
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FftProcessor: tone at +500 kHz offset peaks at correct bin", "[fft]") {
    constexpr int    kN      = 4096;
    constexpr double kSR     = 4'000'000.0;
    constexpr double kCtr    = 102.0;
    constexpr double kOffset = 500'000.0;   // +500 kHz from centre

    const auto iq = makeComplexTone(kN, kSR, kOffset);
    const FftFrame frame = FftProcessor::process(iq.constData(), iq.size() / 2, kCtr, kSR);

    const int peak = peakBin(frame);

    // Expected bin: centre + offset / binWidth
    const double binWidthHz = kSR / static_cast<double>(kN);
    const int expected = kN / 2 + static_cast<int>(std::round(kOffset / binWidthHz));

    INFO("Bin width: " << binWidthHz << " Hz");
    INFO("Expected bin: " << expected << "  Peak bin: " << peak);
    INFO("Peak freq: " << frame.freqMHz[peak] << " MHz"
         << "  Expected: " << (kCtr + kOffset / 1e6) << " MHz");

    // Hann window broadens the peak by ~2 bins — allow ±2
    CHECK(std::abs(peak - expected) <= 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// T8c — Frequency axis: first/last bins match SR/2 around centre
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FftProcessor: frequency axis spans SR/2 around centre", "[fft]") {
    constexpr int    kN   = 2048;
    constexpr double kSR  = 4'000'000.0;
    constexpr double kCtr = 100.0;

    QVector<float> iq(kN * 2, 0.0f);
    const FftFrame frame = FftProcessor::process(iq.constData(), iq.size() / 2, kCtr, kSR);

    const double expectedFirst = kCtr - (kSR / 2.0) / 1e6;   // 98.0 MHz
    const double expectedLast  = kCtr + (kSR / 2.0) / 1e6    // ~102.0 MHz
                                 - (kSR / kN) / 1e6;          // minus one bin

    CHECK_THAT(frame.freqMHz.front(),
               Catch::Matchers::WithinAbs(expectedFirst, 0.001));
    CHECK_THAT(frame.freqMHz.back(),
               Catch::Matchers::WithinAbs(expectedLast, 0.001));
}

// ─────────────────────────────────────────────────────────────────────────────
// T8d — Signal-to-noise: tone clearly above noise floor
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FftProcessor: signal bin is at least 20 dB above noise floor", "[fft]") {
    constexpr int    kN      = 4096;
    constexpr double kSR     = 4'000'000.0;
    constexpr double kCtr    = 102.0;
    constexpr double kOffset = 300'000.0;

    const auto iq = makeComplexTone(kN, kSR, kOffset, 0.9);
    const FftFrame frame = FftProcessor::process(iq.constData(), iq.size() / 2, kCtr, kSR);

    const int peak = peakBin(frame);
    const double peakDb = frame.powerDb[peak];

    // Average power of all bins except the peak and its neighbours
    double noiseSum = 0.0;
    int    noiseCount = 0;
    for (int i = 0; i < static_cast<int>(frame.powerDb.size()); ++i) {
        if (std::abs(i - peak) > 10) {
            noiseSum += frame.powerDb[i];
            ++noiseCount;
        }
    }
    const double noiseFloorDb = noiseSum / noiseCount;
    const double snrDb = peakDb - noiseFloorDb;

    INFO("Peak: " << peakDb << " dB  Noise floor: " << noiseFloorDb << " dB  SNR: " << snrDb << " dB");
    CHECK(snrDb >= 20.0);
}
