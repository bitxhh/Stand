#include "FmDemodHandler.h"
#include "FmDemodulator.h"

FmDemodHandler::FmDemodHandler(double stationOffsetHz,
                               double deemphTauSec,
                               double bandwidthHz,
                               QObject* parent)
    : BaseDemodHandler(stationOffsetHz, parent)
{
    setParam(QStringLiteral("Bandwidth"), bandwidthHz);
    setParam(QStringLiteral("De-emphasis"), deemphTauSec);
}

std::vector<demod::ParamDesc> FmDemodHandler::paramDescriptors() const {
    return {
        demod::SpinParam{
            QStringLiteral("Bandwidth"),
            50, 250, 150,
            QStringLiteral(" kHz"), 10, 1000.0
        },
        demod::ComboParam{
            QStringLiteral("De-emphasis"),
            {{QStringLiteral("50 µs (EU)"), 50e-6},
             {QStringLiteral("75 µs (US)"), 75e-6}},
            1  // default: 75 µs
        }
    };
}

std::unique_ptr<BaseDemodulator>
FmDemodHandler::createDemodulator(double sampleRateHz, double offsetHz,
                                  const std::map<QString, double>& params) {
    auto get = [&](const QString& k, double def) {
        auto it = params.find(k);
        return it != params.end() ? it->second : def;
    };
    const double bw  = get(QStringLiteral("Bandwidth"),   150'000.0);
    const double tau = get(QStringLiteral("De-emphasis"),  75e-6);
    return std::make_unique<FmDemodulator>(sampleRateHz, offsetHz, tau, bw);
}

void FmDemodHandler::applyParam(BaseDemodulator& dem,
                                const QString& name, double value) {
    if (name == QLatin1String("Bandwidth"))
        static_cast<FmDemodulator&>(dem).setBandwidth(value);
    // De-emphasis: takes effect on next stream start (IIR recalc needs rebuild)
}
