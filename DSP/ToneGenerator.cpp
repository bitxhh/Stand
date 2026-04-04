#include "ToneGenerator.h"

#include <cmath>
#include <numbers>

ToneGenerator::ToneGenerator(double toneOffsetHz, float amplitude)
    : toneOffsetHz_(toneOffsetHz)
    , amplitude_(amplitude)
{}

void ToneGenerator::setToneOffset(double hz) {
    toneOffsetHz_ = hz;
}

void ToneGenerator::setAmplitude(float amp) {
    amplitude_ = amp;
}

void ToneGenerator::onTxStarted(double /*sampleRateHz*/) {
    phase_ = 0.0;
}

void ToneGenerator::onTxStopped() {
    phase_ = 0.0;
}

int ToneGenerator::generateBlock(int16_t* buffer, int count, double sampleRateHz) {
    const double phaseInc = 2.0 * std::numbers::pi * toneOffsetHz_ / sampleRateHz;
    const double scale    = static_cast<double>(amplitude_) * 32767.0;

    for (int i = 0; i < count; ++i) {
        buffer[2 * i]     = static_cast<int16_t>(scale * std::cos(phase_));
        buffer[2 * i + 1] = static_cast<int16_t>(scale * std::sin(phase_));
        phase_ += phaseInc;
    }
    // Wrap phase to avoid floating-point drift over long transmissions.
    // Use fmod rather than subtraction to handle large jumps cleanly.
    phase_ = std::fmod(phase_, 2.0 * std::numbers::pi);
    return count;
}
