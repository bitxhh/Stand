#pragma once

#include "BaseDemodHandler.h"

// ---------------------------------------------------------------------------
// FmDemodHandler — WBFM handler.
// Params: Bandwidth (50–250 kHz), De-emphasis (50/75 µs).
// ---------------------------------------------------------------------------
class FmDemodHandler : public BaseDemodHandler {
    Q_OBJECT

public:
    explicit FmDemodHandler(double stationOffsetHz = 0.0,
                            double deemphTauSec    = 75e-6,
                            double bandwidthHz     = 150'000.0,
                            QObject* parent        = nullptr);

    std::vector<demod::ParamDesc> paramDescriptors() const override;

protected:
    std::unique_ptr<BaseDemodulator>
    createDemodulator(double sampleRateHz, double offsetHz,
                      const std::map<QString, double>& params) override;

    void applyParam(BaseDemodulator& dem,
                    const QString& name, double value) override;

    const char* handlerName() const override { return "FmDemodHandler"; }
};
