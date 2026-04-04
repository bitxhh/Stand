#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AmDemodulator.h"
#include "DspUtils.h"

#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

static constexpr double kPi = 3.14159265358979323846;

// Generate a pure AM-modulated carrier at carrierHz (int16, interleaved I/Q).
// envelope(t) = 1 + m * sin(2*pi*modFreqHz*t), carrier at carrierHz.
// carrierHz is a baseband frequency (relative to LO), NOT at DC —
// the DC blocker would kill a carrier at 0 Hz.
static QVector<int16_t> makeAmSignal(double sr, int numSamples,
                                     double modFreqHz,
                                     double carrierHz  = 50'000.0,
                                     double depth      = 0.8,
                                     double amplitude  = 0.5)
{
    QVector<int16_t> iq(numSamples * 2);
    for (int n = 0; n < numSamples; ++n) {
        const double env = 1.0 + depth * std::sin(2.0 * kPi * modFreqHz * n / sr);
        const double phase = 2.0 * kPi * carrierHz * n / sr;
        iq[2 * n]     = static_cast<int16_t>(env * amplitude * std::cos(phase) * 32767.0);
        iq[2 * n + 1] = static_cast<int16_t>(env * amplitude * std::sin(phase) * 32767.0);
    }
    return iq;
}

// Compute DFT amplitude at a single frequency (Goertzel-like).
static double dftAmplitude(const QVector<float>& signal, double fs, double freq)
{
    double re = 0.0, im = 0.0;
    const int N = signal.size();
    for (int n = 0; n < N; ++n) {
        re += signal[n] * std::cos(2.0 * kPi * freq * n / fs);
        im -= signal[n] * std::sin(2.0 * kPi * freq * n / fs);
    }
    return 2.0 * std::sqrt(re * re + im * im) / N;
}

// Run demodulator over multiple blocks.
static QVector<float> runDemod(AmDemodulator& dem,
                               const QVector<int16_t>& iq,
                               int blockSize = 16384)
{
    QVector<float> out;
    const int total = iq.size() / 2;
    for (int offset = 0; offset < total; offset += blockSize) {
        const int count = std::min(blockSize, total - offset);
        const QVector<int16_t> block(iq.constData() + offset * 2,
                                     iq.constData() + (offset + count) * 2);
        const auto chunk = dem.pushBlock(block);
        out.append(chunk);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// AM-T1 — AM demodulator recovers 1 kHz tone from 80% AM modulation
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AM demodulator recovers 1 kHz tone from 80% modulation", "[am][demod]") {
    constexpr double kSR        = 4'000'000.0;
    constexpr double kAmFreq    = 1'000.0;
    constexpr double kCarrier   = 50'000.0;   // carrier at +50 kHz from LO
    constexpr int    kBlocks    = 10;

    AmDemodulator dem(kSR, kCarrier, 5'000.0);

    const auto iq = makeAmSignal(kSR, kBlocks * 16384, kAmFreq, kCarrier, 0.8);
    const auto audio = runDemod(dem, iq);

    REQUIRE(!audio.isEmpty());

    // Discard first quarter — FIR warmup
    const int skip = audio.size() / 4;
    const QVector<float> steady(audio.constData() + skip, audio.constData() + audio.size());

    const double audioSR = dem.audioSampleRate();
    const double amp1kHz = dftAmplitude(steady, audioSR, kAmFreq);
    const double amp3kHz = dftAmplitude(steady, audioSR, 3'000.0);

    INFO("Audio SR: " << audioSR);
    INFO("Amplitude @ 1 kHz: " << amp1kHz);
    INFO("Amplitude @ 3 kHz (spurious): " << amp3kHz);

    // 1 kHz component must be dominant
    CHECK(amp1kHz > 0.01);
    // Spurious at 3 kHz must be well below signal
    CHECK(amp1kHz > amp3kHz * 3.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// AM-T2 — Audio sample rate is ~50 kHz for all supported input rates
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AM: audio SR is ~50 kHz for standard input rates", "[am][chain]") {
    for (double sr : {2'500'000.0, 4'000'000.0, 8'000'000.0, 10'000'000.0}) {
        AmDemodulator dem(sr, 0.0, 5'000.0);
        INFO("Input SR: " << sr << "  Audio SR: " << dem.audioSampleRate());
        CHECK_THAT(dem.audioSampleRate(), Catch::Matchers::WithinAbs(50'000.0, 5'000.0));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AM-T3 — Audio sample count matches decimation ratio
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AM: audio sample count matches D1*D2 decimation", "[am][chain]") {
    constexpr double kSR     = 4'000'000.0;
    constexpr int    kBlocks = 4;
    constexpr int    kN      = kBlocks * 16384;

    AmDemodulator dem(kSR, 50'000.0, 5'000.0);
    const auto iq    = makeAmSignal(kSR, kN, 1'000.0, 50'000.0);
    const auto audio = runDemod(dem, iq);

    const int D1       = dem.decimation1();
    const int D2       = 10;
    const int expected = kN / (D1 * D2);
    const int tolerance = expected / 10;

    INFO("D1=" << D1 << "  expected: " << expected << "  got: " << audio.size());
    CHECK(std::abs(audio.size() - expected) <= tolerance);
}

// ─────────────────────────────────────────────────────────────────────────────
// AM-T4 — FIR design sanity (reuses same design function)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AM FIR design: DC gain = 1.0", "[am][fir]") {
    const auto h = dsp::designLowpassFir(31, 0.05);
    double dcGain = 0.0;
    for (double c : h) dcGain += c;
    CHECK_THAT(dcGain, Catch::Matchers::WithinAbs(1.0, 1e-6));
}

// ─────────────────────────────────────────────────────────────────────────────
// AM-T5 — Carrier DC is removed (only AC modulation passes)
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("AM: carrier DC removed, output has zero mean", "[am][dc]") {
    constexpr double kSR     = 4'000'000.0;
    constexpr int    kBlocks = 10;

    AmDemodulator dem(kSR, 50'000.0, 5'000.0);
    const auto iq    = makeAmSignal(kSR, kBlocks * 16384, 1'000.0, 50'000.0, 0.8);
    const auto audio = runDemod(dem, iq);

    // Skip warmup
    const int skip = audio.size() / 2;
    double sum = 0.0;
    for (int i = skip; i < audio.size(); ++i)
        sum += audio[i];
    const double mean = sum / (audio.size() - skip);

    INFO("Audio mean (should be ~0): " << mean);
    // Mean should be close to zero — DC removed (allow small residual from settling)
    CHECK(std::abs(mean) < 0.05);
}
