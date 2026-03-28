#include "FftHandler.h"

#include <QVector>

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
    // Сразу показываем первый кадр
    lastPlot_ = Clock::now() - std::chrono::milliseconds(plotIntervalMs_.load());
}

void FftHandler::onStreamStopped() {}

void FftHandler::processBlock(const int16_t* iq, int count, double sampleRateHz) {
    const auto now     = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPlot_);
    if (elapsed.count() < plotIntervalMs_.load()) return;
    lastPlot_ = now;

    // Копируем в QVector — происходит только plotFps раз/с
    const QVector<int16_t> block(iq, iq + count * 2);

    try {
        FftFrame frame = FftProcessor::process(block, centerFreqMhz_.load(), sampleRateHz);
        emit fftReady(std::move(frame));
    } catch (...) {
        // Не прерываем стрим из-за одного плохого FFT-кадра
    }
}
