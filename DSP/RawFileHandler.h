#pragma once

#include "../Core/IPipelineHandler.h"

#include <QString>
#include <fstream>

// Пишет сырые I/Q int16 сэмплы в бинарный файл.
class RawFileHandler : public IPipelineHandler {
public:
    explicit RawFileHandler(const QString& path);
    ~RawFileHandler() override;

    void processBlock(const int16_t* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

private:
    QString       path_;
    std::ofstream file_;
};
