#include "AmDemodHandler.h"
#include "AmDemodulator.h"

AmDemodHandler::AmDemodHandler(double stationOffsetHz,
                               double bandwidthHz,
                               QObject* parent)
    : BaseDemodHandler(stationOffsetHz, parent)
{
    setParam(QStringLiteral("Bandwidth"), bandwidthHz);
}

std::vector<demod::ParamDesc> AmDemodHandler::paramDescriptors() const {
    return {
        demod::SpinParam{
            QStringLiteral("Bandwidth"),
            1, 20, 5,
            QStringLiteral(" kHz"), 1, 1000.0
        }
    };
}

std::unique_ptr<BaseDemodulator>
AmDemodHandler::createDemodulator(double sampleRateHz, double offsetHz,
                                  const std::map<QString, double>& params) {
    auto it = params.find(QStringLiteral("Bandwidth"));
    const double bw = (it != params.end()) ? it->second : 5'000.0;
    return std::make_unique<AmDemodulator>(sampleRateHz, offsetHz, bw);
}

void AmDemodHandler::applyParam(BaseDemodulator& dem,
                                const QString& name, double value) {
    if (name == QLatin1String("Bandwidth"))
        static_cast<AmDemodulator&>(dem).setBandwidth(value);
}
