#include "FftProcessor.h"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

constexpr double kEps = 1e-12;
constexpr double kPi  = 3.14159265358979323846;

// fftwf_plan_dft_1d / fftwf_destroy_plan modify global FFTW planner state
// and are NOT thread-safe. Two RxWorkers (ch0, ch1) race here on startup.
// All plan creation/destruction must go through this single mutex.
// fftwf_execute() and fftwf_malloc/free are thread-safe and need no lock.
std::mutex s_plannerMutex;

// ---------------------------------------------------------------------------
// PlanCache — one FFTW plan per (fftSize, thread).
// thread_local means each worker thread owns its cache entries; s_plannerMutex
// is only held during the brief first-time plan creation (~1 s) and teardown.
//
// Uses single-precision (fftwf_) — ~2× faster than double on AVX2,
// sufficient accuracy for spectrum display.
// FFTW_MEASURE: FFTW benchmarks several algorithms on first call (~1 s)
// and selects the fastest for this CPU. Subsequent calls are faster.
// ---------------------------------------------------------------------------
struct CachedPlan {
    int            size = 0;
    fftwf_complex* in   = nullptr;
    fftwf_complex* out  = nullptr;
    fftwf_plan     plan = nullptr;

    ~CachedPlan() {
        if (plan) {
            std::lock_guard<std::mutex> lock(s_plannerMutex);
            fftwf_destroy_plan(plan);
        }
        if (in)  fftwf_free(in);
        if (out) fftwf_free(out);
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
        if (entry.plan) {
            std::lock_guard<std::mutex> lock(s_plannerMutex);
            fftwf_destroy_plan(entry.plan);
            entry.plan = nullptr;
        }
        if (entry.in)  { fftwf_free(entry.in);  entry.in  = nullptr; }
        if (entry.out) { fftwf_free(entry.out); entry.out = nullptr; }

        // fftwf_malloc guarantees SIMD alignment (32-byte for AVX2)
        entry.in  = reinterpret_cast<fftwf_complex*>(
                        fftwf_malloc(sizeof(fftwf_complex) * fftSize));
        entry.out = reinterpret_cast<fftwf_complex*>(
                        fftwf_malloc(sizeof(fftwf_complex) * fftSize));

        if (!entry.in || !entry.out)
            throw std::runtime_error("FFTW malloc failed for size " + std::to_string(fftSize));

        // FFTW_MEASURE: benchmarks algorithms on first call (~1 s per size),
        // then reuses the winner for every subsequent execute — worthwhile
        // for a long-running real-time app.
        {
            std::lock_guard<std::mutex> lock(s_plannerMutex);
            entry.plan = fftwf_plan_dft_1d(fftSize, entry.in, entry.out,
                                            FFTW_FORWARD, FFTW_MEASURE);
        }
        if (!entry.plan)
            throw std::runtime_error("FFTW plan creation failed");

        entry.size = fftSize;
    }
    return entry;
}

// Hann window coefficients — float, cached per size alongside the plan.
const std::vector<float>& getHannWindow(int n) {
    thread_local std::unordered_map<int, std::vector<float>> wCache;
    auto& w = wCache[n];
    if (static_cast<int>(w.size()) != n) {
        w.resize(n);
        for (int i = 0; i < n; ++i)
            w[i] = 0.5f * (1.0f - std::cos(static_cast<float>(2.0 * kPi * i / (n - 1))));
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

    // ── Fill input buffer (float) ────────────────────────────────────────────
    for (int i = 0; i < fftSize; ++i) {
        const float w = window[i];
        cp.in[i][0] = static_cast<float>(iqSamples[2 * i])     / 32768.0f * w;
        cp.in[i][1] = static_cast<float>(iqSamples[2 * i + 1]) / 32768.0f * w;
    }

    // ── Execute (reuses preallocated buffers and plan) ────────────────────────
    fftwf_execute(cp.plan);

    // ── Build output frame with FFT-shift (DC in centre) ────────────────────
    FftFrame frame;
    frame.freqMHz.resize(fftSize);
    frame.powerDb.resize(fftSize);

    const double binWidthHz   = sampleRateHz / static_cast<double>(fftSize);
    const double startFreqMHz = centerFreqMHz - (sampleRateHz / 2.0) / 1e6;

    // Coherent normalization: divide by sum(window)^2 so that a full-scale
    // complex sine reads 0 dBFS regardless of FFT size or window shape.
    float winSum = 0.0f;
    for (float w : window) winSum += w;
    const double normSq = static_cast<double>(winSum) * static_cast<double>(winSum);

    for (int k = 0; k < fftSize; ++k) {
        // FFT-shift: map output bin index to "DC-centred" display index
        const int    shifted = (k + fftSize / 2) % fftSize;
        const double re      = static_cast<double>(cp.out[shifted][0]);
        const double im      = static_cast<double>(cp.out[shifted][1]);

        frame.powerDb[k] = 10.0 * std::log10((re * re + im * im) / normSq + kEps);
        frame.freqMHz[k] = startFreqMHz + (static_cast<double>(k) * binWidthHz) / 1e6;
    }

    return frame;
}
