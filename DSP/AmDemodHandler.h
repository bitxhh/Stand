#pragma once

#include "../Core/IPipelineHandler.h"
#include "AmDemodulator.h"

#include <QObject>
#include <QVector>
#include <atomic>
#include <memory>

class AmDemodHandler : public QObject, public IPipelineHandler {
    Q_OBJECT

public:
    explicit AmDemodHandler(double stationOffsetHz = 0.0,
                            double bandwidthHz     = 5'000.0,
                            QObject* parent        = nullptr);

    void setBandwidth(double hz);
    void setOffset(double hz);

    [[nodiscard]] double snrDb() const { return dem_ ? dem_->snrDb() : 0.0; }
    [[nodiscard]] double ifRms() const { return dem_ ? dem_->ifRms() : 0.0; }

    void processBlock(const int16_t* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

signals:
    void audioReady(QVector<float> samples, double sampleRateHz);

private:
    double stationOffsetHz_;
    double bandwidthHz_;

    std::unique_ptr<AmDemodulator> dem_;
    std::atomic<double> pendingBw_{0.0};
    std::atomic<double> pendingOffset_{1e38};
};
