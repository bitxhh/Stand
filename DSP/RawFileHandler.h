#pragma once

#include "../Core/IPipelineHandler.h"

#include <QString>
#include <fstream>

// Пишет сырые I/Q float32 сэмплы в бинарный файл (.cf32 формат).
class RawFileHandler : public IPipelineHandler {
public:
    explicit RawFileHandler(const QString& path);
    ~RawFileHandler() override;

    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

private:
    QString       path_;
    std::ofstream file_;
};
