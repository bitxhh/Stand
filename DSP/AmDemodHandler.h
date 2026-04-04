#pragma once

#include "BaseDemodHandler.h"

// ---------------------------------------------------------------------------
// AmDemodHandler — AM envelope handler.
// Params: Bandwidth (1–20 kHz).
// ---------------------------------------------------------------------------
class AmDemodHandler : public BaseDemodHandler {
    Q_OBJECT

public:
    explicit AmDemodHandler(double stationOffsetHz = 0.0,
                            double bandwidthHz     = 5'000.0,
                            QObject* parent        = nullptr);

    std::vector<demod::ParamDesc> paramDescriptors() const override;

protected:
    std::unique_ptr<BaseDemodulator>
    createDemodulator(double sampleRateHz, double offsetHz,
                      const std::map<QString, double>& params) override;

    void applyParam(BaseDemodulator& dem,
                    const QString& name, double value) override;

    const char* handlerName() const override { return "AmDemodHandler"; }
};
