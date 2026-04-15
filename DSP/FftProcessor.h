#pragma once

#include <QMetaType>
#include <QVector>
#include <memory>

struct FftFrame {
    QVector<double> freqMHz;
    QVector<double> powerDb;
};
Q_DECLARE_METATYPE(FftFrame)

// ---------------------------------------------------------------------------
// FftProcessor — stateless public API, stateful plan cache underneath.
//
// FFTW plan creation is expensive (~ms). Plans are cached by fftSize in a
// thread_local map so the same worker thread always reuses its plan.
// Calling process() from multiple threads is safe — each thread gets its own
// plan instance.
// ---------------------------------------------------------------------------
class FftProcessor {
public:
    // Process one block of interleaved float32 I/Q samples (normalized to [-1, 1]).
    // Returns a frame ready to hand to QCustomPlot::setData().
    //
    // iq           — interleaved float32 [I0,Q0,I1,Q1,...], count I/Q pairs
    // centerFreqMHz — value from the spin-box (e.g. 102.0)
    // sampleRateHz  — value from Device::get_sample_rate() (e.g. 2 000 000)
    static FftFrame process(const float* iq, int count,
                            double centerFreqMHz,
                            double sampleRateHz);
};
