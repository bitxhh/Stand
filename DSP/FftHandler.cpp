#include "FftHandler.h"

FftHandler::FftHandler(QObject* parent)
    : QObject(parent)
    , lastPlot_(Clock::now())
{}

void FftHandler::setCenterFrequency(double mhz) {
    centerFreqMhz_.store(mhz);
}

void FftHandler::setPlotFps(int fps) {
    if (fps > 0)
        plotIntervalMs_.store(1000 / fps);
}

void FftHandler::onStreamStarted(double sampleRateHz) {
    sampleRate_ = sampleRateHz;
    avgPowerDb_.clear();    // reset EMA on each new stream
    // Сразу показываем первый кадр
    lastPlot_ = Clock::now() - std::chrono::milliseconds(plotIntervalMs_.load());
}

void FftHandler::onStreamStopped() {}

void FftHandler::processBlock(const float* iq, int count, double sampleRateHz) {
    const auto now     = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPlot_);
    if (elapsed.count() < plotIntervalMs_.load()) return;
    lastPlot_ = now;

    try {
        const double currentCenter = centerFreqMhz_.load();
        FftFrame frame = FftProcessor::process(iq, count, currentCenter, sampleRateHz);

        // Temporal EMA: blend new frame into running average.
        // Reset if the center frequency changed (frequency axis shifted).
        if (avgPowerDb_.size() != frame.powerDb.size() || avgCenterMhz_ != currentCenter) {
            avgPowerDb_   = frame.powerDb;
            avgCenterMhz_ = currentCenter;
        } else {
            for (int i = 0; i < avgPowerDb_.size(); ++i)
                avgPowerDb_[i] = kAlpha * frame.powerDb[i] + (1.0 - kAlpha) * avgPowerDb_[i];
        }
        frame.powerDb = avgPowerDb_;

        emit fftReady(std::move(frame));
    } catch (...) {
        // Не прерываем стрим из-за одного плохого FFT-кадра
    }
}
