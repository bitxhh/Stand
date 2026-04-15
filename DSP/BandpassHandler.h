#pragma once

#include "../Core/IPipelineHandler.h"
#include "BandpassExporter.h"

#include <QString>
#include <memory>

// Вырезает полосу вокруг stationOffsetHz и пишет в WAV.
class BandpassHandler : public IPipelineHandler {
public:
    BandpassHandler(const QString& path,
                    double stationOffsetHz,
                    double bandwidthHz     = 100'000.0,
                    double outputSrHz      = 250'000.0);

    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;
    void onRetune(double newFreqHz) override;

private:
    QString path_;
    double  stationOffsetHz_;
    double  bandwidthHz_;
    double  outputSrHz_;

    std::unique_ptr<BandpassExporter> exp_;
};
