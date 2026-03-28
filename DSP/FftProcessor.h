#pragma once

#include <QMetaType>
#include <QVector>
#include <cstdint>
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
    // Process one block of interleaved I/Q int16 samples.
    // Returns a frame ready to hand to QCustomPlot::setData().
    //
    // centerFreqMHz — value from the spin-box (e.g. 102.0)
    // sampleRateHz  — value from Device::get_sample_rate() (e.g. 2 000 000)
    static FftFrame process(const QVector<int16_t>& iqSamples,
                            double centerFreqMHz,
                            double sampleRateHz);
};
