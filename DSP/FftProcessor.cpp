#include "FftProcessor.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

constexpr double kEps = 1e-12;
constexpr double kPi  = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// PlanCache — one FFTW plan per (fftSize, thread).
// thread_local means no mutex needed; each worker thread owns its cache.
// ---------------------------------------------------------------------------
struct CachedPlan {
    int           size = 0;
    fftw_complex* in   = nullptr;
    fftw_complex* out  = nullptr;
    fftw_plan     plan = nullptr;

    ~CachedPlan() {
        if (plan) fftw_destroy_plan(plan);
        if (in)   fftw_free(in);
        if (out)  fftw_free(out);
    }
    // Non-copyable
    CachedPlan()                             = default;
    CachedPlan(const CachedPlan&)            = delete;
    CachedPlan& operator=(const CachedPlan&) = delete;
};

// Returns a ready-to-execute plan for the requested size.
// Re-creates only when the size changes (which is "never" in normal use).
CachedPlan& getPlan(int fftSize) {
    // One cache entry per thread — map keyed by size covers future
    // multi-resolution scenarios without complication.
    thread_local std::unordered_map<int, CachedPlan> cache;
    auto& entry = cache[fftSize];

    if (entry.size != fftSize) {
        // Destroy old resources if any
        if (entry.plan) { fftw_destroy_plan(entry.plan); entry.plan = nullptr; }
        if (entry.in)   { fftw_free(entry.in);           entry.in   = nullptr; }
        if (entry.out)  { fftw_free(entry.out);          entry.out  = nullptr; }

        entry.in  = reinterpret_cast<fftw_complex*>(
                        fftw_malloc(sizeof(fftw_complex) * fftSize));
        entry.out = reinterpret_cast<fftw_complex*>(
                        fftw_malloc(sizeof(fftw_complex) * fftSize));

        if (!entry.in || !entry.out)
            throw std::runtime_error("FFTW malloc failed for size " + std::to_string(fftSize));

        // FFTW_MEASURE would be faster at runtime but takes seconds on first call.
        // FFTW_ESTIMATE is "good enough" for real-time display.
        entry.plan = fftw_plan_dft_1d(fftSize, entry.in, entry.out,
                                       FFTW_FORWARD, FFTW_ESTIMATE);
        if (!entry.plan)
            throw std::runtime_error("FFTW plan creation failed");

        entry.size = fftSize;
    }
    return entry;
}

// Hann window — cached per size alongside the plan.
const std::vector<double>& getHannWindow(int n) {
    thread_local std::unordered_map<int, std::vector<double>> wCache;
    auto& w = wCache[n];
    if (static_cast<int>(w.size()) != n) {
        w.resize(n);
        for (int i = 0; i < n; ++i)
            w[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * i / (n - 1)));
    }
    return w;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
FftFrame FftProcessor::process(const QVector<int16_t>& iqSamples,
                               double centerFreqMHz,
                               double sampleRateHz)
{
    if (iqSamples.size() < 2 || (iqSamples.size() % 2) != 0)
        throw std::runtime_error("IQ buffer must contain an even number of int16 values");

    const int fftSize = iqSamples.size() / 2;

    auto& cp     = getPlan(fftSize);
    auto& window = getHannWindow(fftSize);

    // ── Fill input buffer ────────────────────────────────────────────────────
    for (int i = 0; i < fftSize; ++i) {
        const double w = window[i];
        cp.in[i][0] = static_cast<double>(iqSamples[2 * i])     / 32768.0 * w;
        cp.in[i][1] = static_cast<double>(iqSamples[2 * i + 1]) / 32768.0 * w;
    }

    // ── Execute (reuses preallocated buffers and plan) ────────────────────────
    fftw_execute(cp.plan);

    // ── Build output frame with FFT-shift (DC in centre) ────────────────────
    FftFrame frame;
    frame.freqMHz.resize(fftSize);
    frame.powerDb.resize(fftSize);

    const double binWidthHz   = sampleRateHz / static_cast<double>(fftSize);
    const double startFreqMHz = centerFreqMHz - (sampleRateHz / 2.0) / 1e6;
    const double normSq       = static_cast<double>(fftSize);

    for (int k = 0; k < fftSize; ++k) {
        // FFT-shift: map output bin index to "DC-centred" display index
        const int shifted = (k + fftSize / 2) % fftSize;
        const double re   = cp.out[shifted][0];
        const double im   = cp.out[shifted][1];

        frame.powerDb[k] = 10.0 * std::log10((re * re + im * im) / normSq + kEps);
        frame.freqMHz[k] = startFreqMHz + (static_cast<double>(k) * binWidthHz) / 1e6;
    }

    return frame;
}
