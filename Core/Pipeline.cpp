#include "Pipeline.h"

#include <algorithm>

Pipeline::Pipeline(QObject* parent) : QObject(parent) {}

void Pipeline::addHandler(IPipelineHandler* handler) {
    std::unique_lock lock(mutex_);
    handlers_.push_back(handler);
}

void Pipeline::removeHandler(IPipelineHandler* handler) {
    std::unique_lock lock(mutex_);
    handlers_.erase(std::remove(handlers_.begin(), handlers_.end(), handler),
                    handlers_.end());
}

void Pipeline::clearHandlers() {
    std::unique_lock lock(mutex_);
    handlers_.clear();
}

void Pipeline::dispatchBlock(const int16_t* iq, int count, double sampleRateHz) {
    // shared_lock позволяет параллельные dispatch, но блокирует
    // add/remove/clear до завершения — так delete handler'а в teardown
    // не произойдёт, пока processBlock ещё работает.
    std::shared_lock lock(mutex_);
    for (auto* h : handlers_)
        h->processBlock(iq, count, sampleRateHz);
}

void Pipeline::notifyStarted(double sampleRateHz) {
    std::shared_lock lock(mutex_);
    for (auto* h : handlers_)
        h->onStreamStarted(sampleRateHz);
}

void Pipeline::notifyStopped() {
    std::shared_lock lock(mutex_);
    for (auto* h : handlers_)
        h->onStreamStopped();
}
