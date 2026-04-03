#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "FmDemodulator.h"

#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static constexpr double kPi = 3.14159265358979323846;

// Generate a pure FM-modulated baseband I/Q signal (int16, interleaved).
// Carrier at 0 Hz, modulated by a sine at freqHz with deviation devHz.
static QVector<int16_t> makeFmSignal(double sr, int numSamples,
                                     double freqHz, double devHz,
                                     double amplitude = 0.9)
{
    QVector<int16_t> iq(numSamples * 2);
    double phase = 0.0;
    for (int n = 0; n < numSamples; ++n) {
        // Instantaneous phase increment = 2π * f_inst / sr
        // f_inst(t) = devHz * sin(2π * freqHz * t)
        phase += (2.0 * kPi * devHz * std::sin(2.0 * kPi * freqHz * n / sr)) / sr;
        while (phase >  kPi) phase -= 2.0 * kPi;
        while (phase < -kPi) phase += 2.0 * kPi;
        iq[2 * n]     = static_cast<int16_t>(std::cos(phase) * amplitude * 32767.0);
        iq[2 * n + 1] = static_cast<int16_t>(std::sin(phase) * amplitude * 32767.0);
    }
    return iq;
}

// Compute DFT amplitude at a single frequency (Goertzel-like, no FFT needed).
// Returns peak-normalised amplitude (1.0 = full-scale sine).
static double dftAmplitude(const QVector<float>& signal, double fs, double freq)
{
    double re = 0.0, im = 0.0;
    const int N = signal.size();
    for (int n = 0; n < N; ++n) {
        re += signal[n] * std::cos(2.0 * kPi * freq * n / fs);
        im -= signal[n] * std::sin(2.0 * kPi * freq * n / fs);
    }
    // Normalise: DFT of a unit sine gives N/2 at the signal frequency
    return 2.0 * std::sqrt(re * re + im * im) / N;
}

// Accumulate multiple blocks into one audio vector.
static QVector<float> runDemod(FmDemodulator& dem,
                               const QVector<int16_t>& iq,
                               int blockSize = 16384)
{
    QVector<float> out;
    const int total = iq.size() / 2;   // number of I/Q pairs
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
// T1 — FIR design: coefficients basic sanity
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FIR design: DC gain = 1.0", "[fir]") {
    // Passband sum of a properly normalised lowpass FIR = 1.0 (DC passes unchanged)
    for (int taps : {31, 63, 127}) {
        const auto h = FmDemodulator::designLowpassFir(taps, 0.1);
        REQUIRE(static_cast<int>(h.size()) == taps);

        double dcGain = 0.0;
        for (double c : h) dcGain += c;
        CHECK_THAT(dcGain, Catch::Matchers::WithinAbs(1.0, 1e-6));
    }
}

TEST_CASE("FIR design: coefficients are symmetric (linear phase)", "[fir]") {
    const auto h = FmDemodulator::designLowpassFir(31, 0.1);
    for (int i = 0; i < 15; ++i)
        CHECK_THAT(h[i], Catch::Matchers::WithinAbs(h[30 - i], 1e-12));
}

TEST_CASE("FIR design: stopband attenuation >= 40 dB", "[fir]") {
    // cutoff = 0.1 (normalised); evaluate at 0.3 (well into stopband)
    const auto h = FmDemodulator::designLowpassFir(63, 0.1);
    const double omega = 2.0 * kPi * 0.3;   // stopband frequency
    double re = 0.0, im = 0.0;
    for (int n = 0; n < static_cast<int>(h.size()); ++n) {
        re += h[n] * std::cos(omega * n);
        im -= h[n] * std::sin(omega * n);
    }
    const double gainDb = 20.0 * std::log10(std::sqrt(re * re + im * im) + 1e-30);
    INFO("Stopband gain: " << gainDb << " dB");
    CHECK(gainDb < -40.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// T2 — DC blocker: constant I/Q input converges to near zero
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("DC blocker removes constant I/Q offset", "[dcblock]") {
    // Pure DC input: I=0.8, Q=0.0  →  discriminator should not see a signal
    // We measure IF power after 3 blocks: it should be much lower than if the
    // DC were not removed (which would saturate the discriminator).
    constexpr double kSR = 4'000'000.0;
    FmDemodulator dem(kSR, 0.0, 75e-6, 100'000.0);

    constexpr int kBlockSize = 16384;
    // DC at nearly full scale
    QVector<int16_t> dcBlock(kBlockSize * 2, 0);
    for (int i = 0; i < kBlockSize; ++i) {
        dcBlock[2 * i]     = 26000;   // I ≈ 0.79
        dcBlock[2 * i + 1] = 0;
    }

    // Run 3 blocks — DC blocker needs a few samples to settle
    for (int b = 0; b < 3; ++b) std::ignore = dem.pushBlock(dcBlock);

    // After settling, IF RMS should be very small (DC is blocked)
    const double ifRms = dem.ifRms();
    INFO("IF RMS after DC block: " << ifRms);
    CHECK(ifRms < 0.05);
}

// ─────────────────────────────────────────────────────────────────────────────
// T5 — FM discriminator: synthetic signal → correct audio frequency
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FM discriminator recovers 1 kHz tone from 75 kHz deviation", "[fm][discriminator]") {
    constexpr double kSR      = 4'000'000.0;
    constexpr double kFmFreq  =     1'000.0;   // modulating tone
    constexpr double kFmDev   =    75'000.0;   // FM deviation (standard WBFM)

    FmDemodulator dem(kSR, 0.0, 75e-6, 100'000.0);

    // 6 blocks → 6×16384 = 98304 input samples
    // Audio output: 98304 / (D1=16 × D2=5) = 1228 samples at ~50 kHz
    const int kBlocks = 6;
    const auto iq = makeFmSignal(kSR, kBlocks * 16384, kFmFreq, kFmDev);
    const auto audio = runDemod(dem, iq);

    REQUIRE(!audio.isEmpty());

    // Discard the first quarter — FIR delay lines and warmup transients
    const int skip = audio.size() / 4;
    const QVector<float> steady(audio.constData() + skip, audio.constData() + audio.size());

    const double audioSR    = dem.audioSampleRate();   // ~50 000 Hz
    const double amp1kHz    = dftAmplitude(steady, audioSR, kFmFreq);
    const double amp10kHz   = dftAmplitude(steady, audioSR, 10'000.0);  // spurious check

    INFO("Audio SR: " << audioSR);
    INFO("Audio samples: " << steady.size());
    INFO("Amplitude @ 1 kHz: " << amp1kHz);
    INFO("Amplitude @ 10 kHz (spurious): " << amp10kHz);

    // 1 kHz component must be dominant (> 0.25 accounting for de-emphasis rolloff)
    CHECK(amp1kHz > 0.25);
    // The spurious tone at 10 kHz must be at least 10 dB below the signal
    CHECK(amp1kHz > amp10kHz * 3.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// T7 — Full chain: output sample rate matches design
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Full chain: audio sample count matches D1*D2 decimation", "[fm][chain]") {
    constexpr double kSR     = 4'000'000.0;
    constexpr int    kBlocks = 4;
    constexpr int    kN      = kBlocks * 16384;

    FmDemodulator dem(kSR, 0.0, 75e-6, 100'000.0);
    const auto iq    = makeFmSignal(kSR, kN, 1'000.0, 75'000.0);
    const auto audio = runDemod(dem, iq);

    const int D1        = dem.decimation1();      // 8 for 4 MHz
    const int D2        = 10;
    const int expected  = kN / (D1 * D2);
    const int tolerance = expected / 10;          // ±10%

    INFO("D1=" << D1 << "  expected audio samples: " << expected
         << "  got: " << audio.size());

    CHECK(std::abs(audio.size() - expected) <= tolerance);
}

TEST_CASE("Full chain: audio SR is ~50 kHz for all supported input rates", "[fm][chain]") {
    for (double sr : {2'500'000.0, 4'000'000.0, 8'000'000.0, 10'000'000.0}) {
        FmDemodulator dem(sr, 0.0, 75e-6, 100'000.0);
        INFO("Input SR: " << sr << "  Audio SR: " << dem.audioSampleRate());
        // Audio rate = ifSR / D2 = (SR/D1) / 10
        // For all supported rates, IF ≈ 500 kHz → audio ≈ 50 kHz
        CHECK_THAT(dem.audioSampleRate(), Catch::Matchers::WithinAbs(50'000.0, 5'000.0));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// T4 — FIR1 + D1: out-of-band tone is attenuated after filtering
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("Out-of-band tone is attenuated by FIR1", "[fir][decimation]") {
    constexpr double kSR   = 4'000'000.0;
    constexpr int    kN    = 6 * 16384;

    // In-band: 50 kHz (well within BW=100 kHz)
    FmDemodulator dem_in(kSR, 0.0, 75e-6, 100'000.0);
    const auto iq_in    = makeFmSignal(kSR, kN, 1'000.0, 50'000.0);
    const auto audio_in = runDemod(dem_in, iq_in);

    // Out-of-band: we modulate at 1 kHz but with deviation 300 kHz
    // — most energy lands outside the 100 kHz BW and gets cut by FIR1.
    // We compare IF RMS: in-band should be much larger than out-of-band after filter.
    FmDemodulator dem_out(kSR, 0.0, 75e-6, 100'000.0);
    const auto iq_out    = makeFmSignal(kSR, kN, 1'000.0, 300'000.0);
    runDemod(dem_out, iq_out);

    INFO("In-band  IF RMS: " << dem_in.ifRms());
    INFO("Out-of-band IF RMS: " << dem_out.ifRms());

    // In-band signal should have higher IF power after FIR1 filtering.
    // Ratio is modest in Debug (31-tap FIR); Release (255-tap) gives > 1.5×.
    CHECK(dem_in.ifRms() > dem_out.ifRms() * 1.1);
}

// ─────────────────────────────────────────────────────────────────────────────
// T6 — FM quieting SNR: on-station signal gives SNR > 6 dB
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("FM quieting: on-station signal has high SNR", "[fm][snr]") {
    constexpr double kSR     = 4'000'000.0;
    constexpr double kFmFreq = 1'000.0;
    constexpr double kFmDev  = 75'000.0;
    constexpr int    kBlocks = 10;

    FmDemodulator dem(kSR, 0.0, 75e-6, 100'000.0);
    const auto iq = makeFmSignal(kSR, kBlocks * 16384, kFmFreq, kFmDev);
    runDemod(dem, iq);

    const double snr = dem.snrDb();
    INFO("SNR (on-station): " << snr << " dB");

    // A clean FM signal should have high SNR (audio power >> HF noise)
    CHECK(snr > 6.0);
}

TEST_CASE("FM quieting: noise-only input has low SNR", "[fm][snr]") {
    constexpr double kSR     = 4'000'000.0;
    constexpr int    kBlocks = 10;

    FmDemodulator dem(kSR, 0.0, 75e-6, 100'000.0);

    // Feed random noise — no FM signal
    QVector<int16_t> noise(kBlocks * 16384 * 2);
    std::srand(42);
    for (auto& s : noise)
        s = static_cast<int16_t>((std::rand() % 65536) - 32768);

    const QVector<int16_t> block(noise);
    for (int offset = 0; offset < noise.size() / 2; offset += 16384) {
        const int count = std::min(16384, static_cast<int>(noise.size() / 2 - offset));
        const QVector<int16_t> b(noise.constData() + offset * 2,
                                 noise.constData() + (offset + count) * 2);
        std::ignore = dem.pushBlock(b);
    }

    const double snr = dem.snrDb();
    INFO("SNR (noise-only): " << snr << " dB");

    // Noise should give low SNR (broadband — HF noise is not quieted)
    CHECK(snr < 4.0);
}
