#include "AmDemodulator.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <string>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
AmDemodulator::AmDemodulator(double inputSampleRateHz,
                             double stationOffsetHz,
                             double bandwidthHz)
    : BaseDemodulator(inputSampleRateHz, stationOffsetHz,
                      100'000.0,         // FIR1 cutoff = fixed 100 kHz (wide anti-alias)
                      bandwidthHz,       // FIR2 cutoff = user bandwidth
                      bandwidthHz,       // SNR HP cutoff = at audio bandwidth edge
                      20'000.0)          // min IF for AM
{
    bandwidth_ = std::clamp(bandwidthHz, 1'000.0, audioSR_ / 2.0 * 0.9);

    // Envelope DC removal: IIR highpass at ~20 Hz
    envDc_.setCutoff(20.0, ifSR_);

    LOG_INFO("AmDemodulator: inputSR=" + std::to_string(static_cast<int>(inputSR_))
             + " D1=" + std::to_string(D1_)
             + " IF=" + std::to_string(static_cast<int>(ifSR_)) + " Hz"
             + " audio=" + std::to_string(static_cast<int>(audioSR_)) + " Hz"
             + " BW=" + std::to_string(static_cast<int>(bandwidth_)) + " Hz");
}

// ---------------------------------------------------------------------------
// setBandwidth — redesigns FIR2 (audio bandwidth)
// ---------------------------------------------------------------------------
void AmDemodulator::setBandwidth(double bandwidthHz) {
    bandwidth_ = std::clamp(bandwidthHz, 1'000.0, audioSR_ / 2.0 * 0.9);
    redesignFir2(bandwidth_);
    setSnrHpCutoff(bandwidth_);
    resetSnr();

    LOG_INFO("AmDemodulator: bandwidth set to "
             + std::to_string(static_cast<int>(bandwidth_)) + " Hz");
}

// ---------------------------------------------------------------------------
// demodulateIF — envelope detection + DC removal
// ---------------------------------------------------------------------------
BaseDemodulator::DemodResult
AmDemodulator::demodulateIF(std::complex<double> /*ifSample*/, double ifPower) {
    const double envelope = std::sqrt(ifPower);
    const double envHp = envDc_.process(envelope);
    return {envHp, envHp};
}

// ---------------------------------------------------------------------------
// resetDemodState — called by base setOffset()
// ---------------------------------------------------------------------------
void AmDemodulator::resetDemodState() {
    envDc_.reset();
}
