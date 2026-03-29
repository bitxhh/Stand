#pragma once

#include "../Core/IPipelineHandler.h"
#include "FftProcessor.h"

#include <QObject>
#include <atomic>
#include <chrono>

// ---------------------------------------------------------------------------
// FftHandler — throttled FFT, вызывается из StreamWorker thread.
//
// setCenterFrequency() — потокобезопасно, можно звать из UI thread.
// fftReady() — эмитируется из StreamWorker thread; подключать через
//              Qt::QueuedConnection к UI-слотам.
// ---------------------------------------------------------------------------
class FftHandler : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    explicit FftHandler(QObject* parent = nullptr);

    void setCenterFrequency(double mhz);   // thread-safe
    void setPlotFps(int fps);              // thread-safe

    // IPipelineHandler
    void processBlock(const int16_t* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

signals:
    void fftReady(FftFrame frame);

private:
    std::atomic<double> centerFreqMhz_{102.0};
    std::atomic<int>    plotIntervalMs_{1000 / 30};
    double              sampleRate_{0.0};

    using Clock = std::chrono::steady_clock;
    Clock::time_point lastPlot_;

    // Temporal averaging — exponential moving average over FFT frames.
    // Smooths instantaneous noise spikes into the hill-shaped spectrum
    // that matches what HDSDR displays.
    static constexpr double kAlpha = 0.3;   // blend factor: 1.0 = no averaging
    QVector<double> avgPowerDb_;
    double          avgCenterMhz_{0.0};     // reset avg when center freq changes
};
