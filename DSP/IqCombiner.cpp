#include "IqCombiner.h"
#include "../Core/Pipeline.h"

#include <cmath>
#include <cstring>

IqCombiner::IqCombiner(int channelCount, Pipeline* output)
    : output_(output)
    , channelCount_(channelCount)
    , slots_(channelCount)
    , gainScale_(channelCount, 1.0f)
{}

void IqCombiner::setChannelGain(int channelIndex, double gainDb) {
    if (channelIndex < 0 || channelIndex >= channelCount_) return;
    const float scale = 1.0f / std::pow(10.0f, static_cast<float>(gainDb) / 20.0f);
    std::lock_guard lock(mutex_);
    gainScale_[channelIndex] = scale;
}

// Fallback — no metadata, treat as channel 0.
void IqCombiner::processBlock(const float* iq, int count, double sampleRateHz) {
    processBlock(iq, count, sampleRateHz, BlockMeta{{ChannelDescriptor::RX, 0}, 0});
}

void IqCombiner::processBlock(const float* iq, int count, double sampleRateHz,
                               const BlockMeta& meta) {
    const int idx = meta.channel.channelIndex;
    if (idx < 0 || idx >= channelCount_) return;

    const int floatCount = count * 2;

    std::lock_guard lock(mutex_);

    // If this is a single-channel combiner, skip buffering — just scale and dispatch.
    if (channelCount_ == 1) {
        combined_.resize(floatCount);
        const float s = gainScale_[0];
        for (int i = 0; i < floatCount; ++i)
            combined_[i] = iq[i] * s;
        output_->dispatchBlock(combined_.data(), count, sampleRateHz);
        return;
    }

    // Store a copy of the incoming block in the slot.
    auto& slot = slots_[idx];
    slot.data.resize(floatCount);
    std::memcpy(slot.data.data(), iq, floatCount * sizeof(float));
    slot.timestamp = meta.timestamp;
    slot.filled = true;

    // Check if all slots are filled.
    for (int i = 0; i < channelCount_; ++i) {
        if (!slots_[i].filled) return;
    }

    // All channels present — combine.
    combineAndDispatch(count, sampleRateHz);
}

void IqCombiner::combineAndDispatch(int count, double sampleRateHz) {
    const int floatCount = count * 2;
    combined_.resize(floatCount);

    const float invN = 1.0f / static_cast<float>(channelCount_);

    // First channel: scale into combined buffer.
    {
        const float s = gainScale_[0] * invN;
        const float* src = slots_[0].data.data();
        for (int i = 0; i < floatCount; ++i)
            combined_[i] = src[i] * s;
    }

    // Remaining channels: accumulate.
    for (int ch = 1; ch < channelCount_; ++ch) {
        const float s = gainScale_[ch] * invN;
        const float* src = slots_[ch].data.data();
        for (int i = 0; i < floatCount; ++i)
            combined_[i] += src[i] * s;
    }

    resetSlots();
    output_->dispatchBlock(combined_.data(), count, sampleRateHz);
}

void IqCombiner::resetSlots() {
    for (auto& slot : slots_)
        slot.filled = false;
}

void IqCombiner::onStreamStarted(double /*sampleRateHz*/) {
    std::lock_guard lock(mutex_);
    resetSlots();
}

void IqCombiner::onStreamStopped() {
    std::lock_guard lock(mutex_);
    resetSlots();
}

void IqCombiner::onRetune(double /*newFreqHz*/) {
    std::lock_guard lock(mutex_);
    resetSlots();
}
