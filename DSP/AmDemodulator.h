#pragma once

#include "BaseDemodulator.h"

// ---------------------------------------------------------------------------
// AmDemodulator — AM envelope demodulator built on BaseDemodulator.
//
// AM-specific stages (demodulateIF):
//   Envelope detection: sqrt(I² + Q²)  →  DC removal (IIR HP ~20 Hz)
//
// setBandwidth() redesigns FIR2 (audio bandwidth filter).
// ---------------------------------------------------------------------------
class AmDemodulator : public BaseDemodulator {
public:
    explicit AmDemodulator(double inputSampleRateHz,
                           double stationOffsetHz,
                           double bandwidthHz = 5'000.0);

    void setBandwidth(double bandwidthHz);

protected:
    DemodResult demodulateIF(std::complex<double> ifSample, double ifPower) override;
    void resetDemodState() override;
    const char* demodName() const override { return "AmDemodulator"; }

private:
    dsp::IirHighpass1 envDc_;   // envelope DC removal (~20 Hz)
};
