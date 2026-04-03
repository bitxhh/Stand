#include "AmDemodHandler.h"
#include "Logger.h"

AmDemodHandler::AmDemodHandler(double stationOffsetHz,
                               double bandwidthHz,
                               QObject* parent)
    : QObject(parent)
    , stationOffsetHz_(stationOffsetHz)
    , bandwidthHz_(bandwidthHz)
{}

void AmDemodHandler::setBandwidth(double hz) {
    pendingBw_.store(hz);
}

void AmDemodHandler::setOffset(double hz) {
    pendingOffset_.store(hz);
}

void AmDemodHandler::onStreamStarted(double sampleRateHz) {
    try {
        dem_ = std::make_unique<AmDemodulator>(
            sampleRateHz, stationOffsetHz_, bandwidthHz_);
        LOG_INFO("AmDemodHandler: ready — audio SR="
                 + std::to_string(static_cast<int>(dem_->audioSampleRate())) + " Hz");
    } catch (const std::exception& ex) {
        LOG_ERROR(std::string("AmDemodHandler init failed: ") + ex.what());
        dem_.reset();
    }
}

void AmDemodHandler::onStreamStopped() {
    dem_.reset();
}

void AmDemodHandler::processBlock(const int16_t* iq, int count, double sampleRateHz) {
    if (!dem_) {
        onStreamStarted(sampleRateHz);
        if (!dem_) return;
    }

    const double pending = pendingBw_.exchange(0.0);
    if (pending > 0.0)
        dem_->setBandwidth(pending);

    const double pendingOff = pendingOffset_.exchange(1e38);
    if (pendingOff < 1e37)
        dem_->setOffset(pendingOff);

    const QVector<int16_t> block(iq, iq + count * 2);
    const QVector<float> audio = dem_->pushBlock(block);
    if (!audio.isEmpty())
        emit audioReady(audio, dem_->audioSampleRate());
}
