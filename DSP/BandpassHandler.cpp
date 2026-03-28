#include "BandpassHandler.h"
#include "Logger.h"

BandpassHandler::BandpassHandler(const QString& path,
                                 double stationOffsetHz,
                                 double bandwidthHz,
                                 double outputSrHz)
    : path_(path)
    , stationOffsetHz_(stationOffsetHz)
    , bandwidthHz_(bandwidthHz)
    , outputSrHz_(outputSrHz)
{}

void BandpassHandler::onStreamStarted(double sampleRateHz) {
    try {
        exp_ = std::make_unique<BandpassExporter>(
            sampleRateHz, stationOffsetHz_, bandwidthHz_, outputSrHz_);
        if (!exp_->open(path_)) {
            LOG_ERROR("BandpassHandler: cannot open WAV: " + path_.toStdString());
            exp_.reset();
        } else {
            LOG_INFO("BandpassHandler: writing to " + path_.toStdString());
        }
    } catch (const std::exception& ex) {
        LOG_ERROR(std::string("BandpassHandler init failed: ") + ex.what());
        exp_.reset();
    }
}

void BandpassHandler::onStreamStopped() {
    if (exp_) {
        exp_->close();
        exp_.reset();
        LOG_INFO("BandpassHandler: closed " + path_.toStdString());
    }
}

void BandpassHandler::processBlock(const int16_t* iq, int count, double /*sampleRateHz*/) {
    if (!exp_) return;
    const QVector<int16_t> block(iq, iq + count * 2);
    exp_->pushBlock(block);
}
