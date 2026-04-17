#pragma once

#include "../Core/IPipelineHandler.h"

#include <cstdint>
#include <mutex>
#include <vector>

class Pipeline;

// ---------------------------------------------------------------------------
// IqCombiner — merges I/Q blocks from N coherent RX channels into one output.
//
// Registered as an IPipelineHandler in each per-channel PrePipeline.
// When all N channels have delivered a block with the same timestamp,
// the combiner gain-normalises each channel (÷ linear gain), averages
// the I/Q samples, and dispatches the result to the output Pipeline.
//
// Threading: processBlock() is called from different RxWorker threads
// (one per channel). Internal mutex serialises access; the thread that
// fills the last slot performs the combine+dispatch under the lock.
// ---------------------------------------------------------------------------
class IqCombiner : public IPipelineHandler {
public:
    explicit IqCombiner(int channelCount, Pipeline* output);

    // Set per-channel RX gain in dB for normalisation.
    // scale = 1 / 10^(gainDb / 20).  Thread-safe.
    void setChannelGain(int channelIndex, double gainDb);

    // IPipelineHandler — uses meta.channel.channelIndex to route blocks.
    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void processBlock(const float* iq, int count, double sampleRateHz,
                      const BlockMeta& meta) override;

    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;
    void onRetune(double newFreqHz) override;

private:
    struct Slot {
        std::vector<float> data;
        uint64_t           timestamp{0};
        bool               filled{false};
    };

    void resetSlots();
    void combineAndDispatch(int count, double sampleRateHz);

    Pipeline*         output_;
    int               channelCount_;
    std::vector<Slot> slots_;
    std::vector<float> gainScale_;   // linear: 1/10^(gain/20)
    std::vector<float> combined_;    // output buffer
    std::mutex         mutex_;
};
