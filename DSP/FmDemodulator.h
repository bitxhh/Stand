#pragma once

#include "BaseDemodulator.h"

// ---------------------------------------------------------------------------
// FmDemodulator — WBFM demodulator built on BaseDemodulator.
//
// FM-specific stages (demodulateIF):
//   FM discriminator (atan2 of conjugate product)  →  de-emphasis IIR
//
// setBandwidth() redesigns FIR1 (pre-decimation channel filter).
// ---------------------------------------------------------------------------
class FmDemodulator : public BaseDemodulator {
public:
    explicit FmDemodulator(double inputSampleRateHz,
                           double stationOffsetHz,
                           double deemphTauSec    = 50e-6,
                           double bandwidthHz     = 150'000.0);

    void setBandwidth(double bandwidthHz);

protected:
    double demodulateIF(std::complex<double> ifSample, double ifPower) override;
    void resetDemodState() override;
    const char* demodName() const override { return "FmDemodulator"; }

private:
    double deemphTau_;

    // FM discriminator state
    std::complex<double> prevIF_{1.0, 0.0};
    double               demodGain_;

    // De-emphasis IIR
    double deemphP_{0.0};
    double deemphState_{0.0};
};
