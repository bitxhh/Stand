#include "Pipeline.h"

#include <algorithm>

Pipeline::Pipeline(QObject* parent) : QObject(parent) {}

void Pipeline::addHandler(IPipelineHandler* handler) {
    std::lock_guard lock(mutex_);
    handlers_.push_back(handler);
}

void Pipeline::removeHandler(IPipelineHandler* handler) {
    std::lock_guard lock(mutex_);
    handlers_.erase(std::remove(handlers_.begin(), handlers_.end(), handler),
                    handlers_.end());
}

void Pipeline::clearHandlers() {
    std::lock_guard lock(mutex_);
    handlers_.clear();
}

void Pipeline::dispatchBlock(const int16_t* iq, int count, double sampleRateHz) {
    // Снимок списка под мьютексом — callbacks вызываем без блокировки
    std::vector<IPipelineHandler*> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = handlers_;
    }
    for (auto* h : snapshot)
        h->processBlock(iq, count, sampleRateHz);
}

void Pipeline::notifyStarted(double sampleRateHz) {
    std::lock_guard lock(mutex_);
    for (auto* h : handlers_)
        h->onStreamStarted(sampleRateHz);
}

void Pipeline::notifyStopped() {
    std::lock_guard lock(mutex_);
    for (auto* h : handlers_)
        h->onStreamStopped();
}
