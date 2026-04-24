#include "IqCombiner.h"
#include "../Core/Pipeline.h"

#include <cmath>
#include <cstring>

IqCombiner::IqCombiner(int channelCount, Pipeline* output, QObject* parent)
    : QObject(parent)
    , output_(output)
    , channelCount_(channelCount)
    , slots_(channelCount)
    , gainScale_(channelCount, 1.0f)
    , sumI2_(channelCount, 0.0)
    , sumQ2_(channelCount, 0.0)
    , sumIQ_(channelCount, 0.0)
{}

void IqCombiner::setChannelGain(int channelIndex, double gainDb) {
    if (channelIndex < 0 || channelIndex >= channelCount_) return;
    const float scale = 1.0f / std::pow(10.0f, static_cast<float>(gainDb) / 20.0f);
    std::lock_guard lock(mutex_);
    gainScale_[channelIndex] = scale;
}

void IqCombiner::setPhaseCalibrationDeg(double deg) {
    phaseCalibrationDeg_.store(deg);
}

double IqCombiner::phaseCalibrationDeg() const {
    return phaseCalibrationDeg_.load();
}

double IqCombiner::calibrateNow() {
    double raw = 0.0;
    {
        std::lock_guard lock(mutex_);
        // Используем аккумулированный cross-продукт, если есть данные; иначе 0.
        if (accBlocks_ > 0 &&
            (crossReAcc_ != 0.0 || crossImAcc_ != 0.0)) {
            raw = std::atan2(crossImAcc_, crossReAcc_) * 180.0 / M_PI;
        }
    }
    phaseCalibrationDeg_.store(raw);
    return raw;
}

// Fallback — no metadata, treat as channel 0.
void IqCombiner::processBlock(const float* iq, int count, double sampleRateHz) {
    processBlock(iq, count, sampleRateHz, BlockMeta{{ChannelDescriptor::RX, 0}, 0});
}

void IqCombiner::processBlock(const float* iq, int count, double sampleRateHz,
                               const BlockMeta& meta) {
    const int floatCount = count * 2;

    std::lock_guard lock(mutex_);

    // If this is a single-channel combiner, skip buffering — just scale and dispatch.
    // Must come before the channelIndex bounds check: channelIndex may be 1 (RX1)
    // while channelCount_==1, which would otherwise be rejected as out-of-range.
    if (channelCount_ == 1) {
        accumulateChannelIq(0, iq, count);
        ++iqAccBlocks_;
        combined_.resize(floatCount);
        const float s = gainScale_[0];
        for (int i = 0; i < floatCount; ++i)
            combined_[i] = iq[i] * s;
        output_->dispatchBlock(combined_.data(), count, sampleRateHz);
        maybeEmitIqImbalance();
        return;
    }

    const int idx = meta.channel.channelIndex;
    if (idx < 0 || idx >= channelCount_) return;

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

    // All channels present — compute cross-channel metric then combine.
    accumulatePhase(count);
    for (int ch = 0; ch < channelCount_; ++ch)
        accumulateChannelIq(ch, slots_[ch].data.data(), count);
    ++iqAccBlocks_;
    combineAndDispatch(count, sampleRateHz);
    maybeEmitPhase();
    maybeEmitIqImbalance();
}

void IqCombiner::accumulatePhase(int count) {
    // ch0 and ch1 cross-product: Σ c0·conj(c1) where c = I + jQ.
    // (I0+jQ0)(I1-jQ1) = (I0·I1 + Q0·Q1) + j(Q0·I1 - I0·Q1)
    if (channelCount_ < 2) return;
    const float* a = slots_[0].data.data();
    const float* b = slots_[1].data.data();

    double cre = 0.0, cim = 0.0, p0 = 0.0, p1 = 0.0;
    for (int n = 0; n < count; ++n) {
        const float i0 = a[2*n],   q0 = a[2*n + 1];
        const float i1 = b[2*n],   q1 = b[2*n + 1];
        cre += double(i0)*i1 + double(q0)*q1;
        cim += double(q0)*i1 - double(i0)*q1;
        p0  += double(i0)*i0 + double(q0)*q0;
        p1  += double(i1)*i1 + double(q1)*q1;
    }
    crossReAcc_ += cre;
    crossImAcc_ += cim;
    pow0Acc_    += p0;
    pow1Acc_    += p1;
    ++accBlocks_;
}

void IqCombiner::accumulateChannelIq(int idx, const float* data, int count) {
    double sI2 = 0.0, sQ2 = 0.0, sIQ = 0.0;
    for (int n = 0; n < count; ++n) {
        const double I = data[2*n];
        const double Q = data[2*n + 1];
        sI2 += I * I;
        sQ2 += Q * Q;
        sIQ += I * Q;
    }
    sumI2_[idx] += sI2;
    sumQ2_[idx] += sQ2;
    sumIQ_[idx] += sIQ;
}

void IqCombiner::maybeEmitIqImbalance() {
    const auto now = Clock::now();
    if (lastIqEmit_.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastIqEmit_).count()
        < kEmitIntervalMs) {
        return;
    }
    if (iqAccBlocks_ == 0) return;

    for (int ch = 0; ch < channelCount_; ++ch) {
        const double i2 = sumI2_[ch];
        const double q2 = sumQ2_[ch];
        const double iq = sumIQ_[ch];
        const double ampDb = (i2 > 0.0 && q2 > 0.0)
            ? 10.0 * std::log10(i2 / q2)
            : 0.0;
        const double denom = std::sqrt(i2 * q2);
        const double cc    = denom > 0.0 ? iq / denom : 0.0;
        emit iqImbalance(ch, ampDb, cc);
    }

    for (int ch = 0; ch < channelCount_; ++ch) {
        sumI2_[ch] = sumQ2_[ch] = sumIQ_[ch] = 0.0;
    }
    iqAccBlocks_ = 0;
    lastIqEmit_  = now;
}

void IqCombiner::maybeEmitPhase() {
    const auto now = Clock::now();
    if (lastEmit_.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit_).count()
        < kEmitIntervalMs) {
        return;
    }
    if (accBlocks_ == 0) return;

    const double rawDeg = std::atan2(crossImAcc_, crossReAcc_) * 180.0 / M_PI;
    const double denom  = std::sqrt(pow0Acc_ * pow1Acc_);
    const double mag    = std::sqrt(crossReAcc_*crossReAcc_ + crossImAcc_*crossImAcc_);
    const double coh    = denom > 0.0 ? std::min(1.0, mag / denom) : 0.0;

    double cal = rawDeg - phaseCalibrationDeg_.load();
    // wrap to [-180, 180]
    while (cal >  180.0) cal -= 360.0;
    while (cal < -180.0) cal += 360.0;

    // Reset accumulators for the next window.
    crossReAcc_ = crossImAcc_ = pow0Acc_ = pow1Acc_ = 0.0;
    accBlocks_  = 0;
    lastEmit_   = now;

    emit phaseMetric(rawDeg, cal, coh);
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
    crossReAcc_ = crossImAcc_ = pow0Acc_ = pow1Acc_ = 0.0;
    accBlocks_  = 0;
    lastEmit_   = {};
    for (int ch = 0; ch < channelCount_; ++ch)
        sumI2_[ch] = sumQ2_[ch] = sumIQ_[ch] = 0.0;
    iqAccBlocks_ = 0;
    lastIqEmit_  = {};
}

void IqCombiner::onStreamStopped() {
    std::lock_guard lock(mutex_);
    resetSlots();
}

void IqCombiner::onRetune(double /*newFreqHz*/) {
    std::lock_guard lock(mutex_);
    resetSlots();
    crossReAcc_ = crossImAcc_ = pow0Acc_ = pow1Acc_ = 0.0;
    accBlocks_  = 0;
    for (int ch = 0; ch < channelCount_; ++ch)
        sumI2_[ch] = sumQ2_[ch] = sumIQ_[ch] = 0.0;
    iqAccBlocks_ = 0;
    // Keep lastEmit_/lastIqEmit_ as is — retune doesn't need to reset emit cadence.
}
