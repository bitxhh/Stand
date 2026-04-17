#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "IqCombiner.h"
#include "Pipeline.h"
#include "IPipelineHandler.h"

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Test sink — captures blocks dispatched by IqCombiner via the output Pipeline.
// ---------------------------------------------------------------------------
class TestSink : public IPipelineHandler {
public:
    void processBlock(const float* iq, int count, double sampleRateHz) override {
        lastSr = sampleRateHz;
        lastData.assign(iq, iq + count * 2);
        callCount++;
    }

    std::vector<float> lastData;
    double lastSr{0.0};
    int callCount{0};
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::vector<float> makeConstIq(int count, float iVal, float qVal) {
    std::vector<float> buf(count * 2);
    for (int n = 0; n < count; ++n) {
        buf[2 * n]     = iVal;
        buf[2 * n + 1] = qVal;
    }
    return buf;
}

static BlockMeta meta(int chIdx, uint64_t ts = 0) {
    return {{ChannelDescriptor::RX, chIdx}, ts};
}

// ═══════════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("IqCombiner: single channel passthrough", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(1, &pipe);

    auto data = makeConstIq(64, 0.5f, -0.3f);
    combiner.processBlock(data.data(), 64, 2e6, meta(0));

    REQUIRE(sink.callCount == 1);
    REQUIRE(sink.lastData.size() == 128);
    // With gain 0 dB, scale = 1.0, output equals input.
    REQUIRE_THAT(sink.lastData[0], WithinAbs(0.5, 1e-6));
    REQUIRE_THAT(sink.lastData[1], WithinAbs(-0.3, 1e-6));
}

TEST_CASE("IqCombiner: two channels equal gain → average", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(2, &pipe);

    constexpr int N = 32;
    auto ch0 = makeConstIq(N, 0.8f, 0.2f);
    auto ch1 = makeConstIq(N, 0.4f, 0.6f);

    // Send ch0 first — should not dispatch yet.
    combiner.processBlock(ch0.data(), N, 2e6, meta(0, 100));
    REQUIRE(sink.callCount == 0);

    // Send ch1 — now both slots filled, dispatch.
    combiner.processBlock(ch1.data(), N, 2e6, meta(1, 100));
    REQUIRE(sink.callCount == 1);

    // Average of (0.8+0.4)/2=0.6, (0.2+0.6)/2=0.4
    REQUIRE_THAT(sink.lastData[0], WithinAbs(0.6, 1e-5));
    REQUIRE_THAT(sink.lastData[1], WithinAbs(0.4, 1e-5));
}

TEST_CASE("IqCombiner: gain normalization", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(2, &pipe);

    // CH0 at 20 dB gain → scale = 1/10 = 0.1
    // CH1 at  0 dB gain → scale = 1.0
    combiner.setChannelGain(0, 20.0);
    combiner.setChannelGain(1, 0.0);

    constexpr int N = 16;
    // CH0 raw I=1.0 → after scale: 1.0*0.1 = 0.1
    // CH1 raw I=0.1 → after scale: 0.1*1.0 = 0.1
    // Average = (0.1 + 0.1) / 2 = 0.1
    auto ch0 = makeConstIq(N, 1.0f, 0.0f);
    auto ch1 = makeConstIq(N, 0.1f, 0.0f);

    combiner.processBlock(ch0.data(), N, 2e6, meta(0));
    combiner.processBlock(ch1.data(), N, 2e6, meta(1));

    REQUIRE(sink.callCount == 1);
    REQUIRE_THAT(sink.lastData[0], WithinAbs(0.1, 1e-5));
}

TEST_CASE("IqCombiner: multiple blocks in sequence", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(2, &pipe);

    constexpr int N = 8;
    auto a0 = makeConstIq(N, 1.0f, 0.0f);
    auto a1 = makeConstIq(N, 0.0f, 1.0f);
    auto b0 = makeConstIq(N, 0.5f, 0.5f);
    auto b1 = makeConstIq(N, 0.5f, 0.5f);

    // Block A
    combiner.processBlock(a0.data(), N, 2e6, meta(0, 100));
    combiner.processBlock(a1.data(), N, 2e6, meta(1, 100));
    REQUIRE(sink.callCount == 1);
    REQUIRE_THAT(sink.lastData[0], WithinAbs(0.5, 1e-5));
    REQUIRE_THAT(sink.lastData[1], WithinAbs(0.5, 1e-5));

    // Block B
    combiner.processBlock(b0.data(), N, 2e6, meta(0, 200));
    combiner.processBlock(b1.data(), N, 2e6, meta(1, 200));
    REQUIRE(sink.callCount == 2);
    REQUIRE_THAT(sink.lastData[0], WithinAbs(0.5, 1e-5));
    REQUIRE_THAT(sink.lastData[1], WithinAbs(0.5, 1e-5));
}

TEST_CASE("IqCombiner: onStreamStarted resets slots", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(2, &pipe);

    constexpr int N = 8;
    auto data = makeConstIq(N, 0.5f, 0.5f);

    // Fill only ch0.
    combiner.processBlock(data.data(), N, 2e6, meta(0));
    REQUIRE(sink.callCount == 0);

    // Reset — the partial ch0 data should be discarded.
    combiner.onStreamStarted(2e6);

    // Now send both channels from scratch — should combine.
    combiner.processBlock(data.data(), N, 2e6, meta(0));
    combiner.processBlock(data.data(), N, 2e6, meta(1));
    REQUIRE(sink.callCount == 1);
}

TEST_CASE("IqCombiner: out-of-range channel index ignored", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(2, &pipe);

    constexpr int N = 8;
    auto data = makeConstIq(N, 1.0f, 1.0f);

    // Channel index 5 — out of range, should be silently ignored.
    combiner.processBlock(data.data(), N, 2e6, meta(5));
    REQUIRE(sink.callCount == 0);
}

TEST_CASE("IqCombiner: single channel with gain", "[iqcombiner]") {
    Pipeline pipe;
    TestSink sink;
    pipe.addHandler(&sink);

    IqCombiner combiner(1, &pipe);
    combiner.setChannelGain(0, 6.0);   // ~0.501 scale

    constexpr int N = 16;
    auto data = makeConstIq(N, 1.0f, 0.0f);
    combiner.processBlock(data.data(), N, 2e6, meta(0));

    REQUIRE(sink.callCount == 1);
    const float expected = 1.0f / std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(sink.lastData[0], WithinAbs(expected, 1e-5));
}
