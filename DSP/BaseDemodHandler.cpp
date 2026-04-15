#include "BaseDemodHandler.h"
#include "Logger.h"

BaseDemodHandler::BaseDemodHandler(double stationOffsetHz, QObject* parent)
    : QObject(parent)
    , stationOffsetHz_(stationOffsetHz)
{}

void BaseDemodHandler::setParam(const QString& name, double value) {
    std::lock_guard lock(paramMutex_);
    params_[name] = value;
    pendingParams_.emplace_back(name, value);
}

double BaseDemodHandler::param(const QString& name) const {
    std::lock_guard lock(paramMutex_);
    auto it = params_.find(name);
    return it != params_.end() ? it->second : 0.0;
}

void BaseDemodHandler::setOffset(double hz) {
    pendingOffset_.store(hz);
}

void BaseDemodHandler::onStreamStarted(double sampleRateHz) {
    std::map<QString, double> paramsCopy;
    {
        std::lock_guard lock(paramMutex_);
        paramsCopy = params_;
        pendingParams_.clear();
    }
    try {
        dem_ = createDemodulator(sampleRateHz, stationOffsetHz_, paramsCopy);
        LOG_INFO(std::string(handlerName()) + ": ready — audio SR="
                 + std::to_string(static_cast<int>(dem_->audioSampleRate())) + " Hz");
    } catch (const std::exception& ex) {
        LOG_ERROR(std::string(handlerName()) + " init failed: " + ex.what());
        dem_.reset();
    }
}

void BaseDemodHandler::onStreamStopped() {
    dem_.reset();
}

void BaseDemodHandler::processBlock(const float* iq, int count, double sampleRateHz) {
    // Lazy-init: handler may have been added mid-stream via pipeline_->addHandler(),
    // in which case onStreamStarted() was never called for it.
    if (!dem_) {
        onStreamStarted(sampleRateHz);
        if (!dem_) return;
    }

    // Apply pending param changes
    {
        std::lock_guard lock(paramMutex_);
        for (const auto& [name, value] : pendingParams_)
            applyParam(*dem_, name, value);
        pendingParams_.clear();
    }

    // Apply pending VFO offset (sentinel 1e38 = no change)
    const double pendingOff = pendingOffset_.exchange(1e38);
    if (pendingOff < 1e37)
        dem_->setOffset(pendingOff);

    const QVector<float> audio = dem_->pushBlock(iq, count);
    if (!audio.isEmpty())
        emit audioReady(audio, dem_->audioSampleRate());
}
