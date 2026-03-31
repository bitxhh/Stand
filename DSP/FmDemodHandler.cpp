#include "FmDemodHandler.h"
#include "Logger.h"

FmDemodHandler::FmDemodHandler(double stationOffsetHz,
                               double deemphTauSec,
                               double bandwidthHz,
                               QObject* parent)
    : QObject(parent)
    , stationOffsetHz_(stationOffsetHz)
    , deemphTauSec_(deemphTauSec)
    , bandwidthHz_(bandwidthHz)
{}

void FmDemodHandler::setBandwidth(double hz) {
    pendingBw_.store(hz);
}

void FmDemodHandler::setOffset(double hz) {
    pendingOffset_.store(hz);
}

void FmDemodHandler::onStreamStarted(double sampleRateHz) {
    try {
        dem_ = std::make_unique<FmDemodulator>(
            sampleRateHz, stationOffsetHz_, deemphTauSec_, bandwidthHz_);
        LOG_INFO("FmDemodHandler: ready — audio SR="
                 + std::to_string(static_cast<int>(dem_->audioSampleRate())) + " Hz");
    } catch (const std::exception& ex) {
        LOG_ERROR(std::string("FmDemodHandler init failed: ") + ex.what());
        dem_.reset();
    }
}

void FmDemodHandler::onStreamStopped() {
    dem_.reset();
}

void FmDemodHandler::processBlock(const int16_t* iq, int count, double /*sampleRateHz*/) {
    if (!dem_) return;

    // Применяем ожидающее изменение полосы
    const double pending = pendingBw_.exchange(0.0);
    if (pending > 0.0)
        dem_->setBandwidth(pending);

    // Применяем ожидающую перестройку VFO (sentinel 1e38 = нет изменений)
    const double pendingOff = pendingOffset_.exchange(1e38);
    if (pendingOff < 1e37)
        dem_->setOffset(pendingOff);

    const QVector<int16_t> block(iq, iq + count * 2);
    const QVector<float> audio = dem_->pushBlock(block);
    if (!audio.isEmpty())
        emit audioReady(audio, dem_->audioSampleRate());
}
