#pragma once

#include "../Core/IPipelineHandler.h"
#include "../Core/RecordingSettings.h"

#include <QString>
#include <fstream>
#include <vector>

// ---------------------------------------------------------------------------
// RawFileHandler — writes interleaved I/Q samples to a binary file.
//
// Default format is 32-bit IEEE float (`.cf32`). Setting RawFormat::Float64
// promotes each sample to 64-bit float before writing (`.cf64`), trading ~2×
// disk bandwidth for native-precision MATLAB/Python ingestion.
// ---------------------------------------------------------------------------
class RawFileHandler : public IPipelineHandler {
public:
    using Format = RecordingSettings::RawFormat;

    explicit RawFileHandler(const QString& path, Format format = Format::Float32);
    ~RawFileHandler() override;

    void processBlock(const float* iq, int count, double sampleRateHz) override;
    void onStreamStarted(double sampleRateHz) override;
    void onStreamStopped() override;

private:
    QString            path_;
    Format             format_;
    std::ofstream      file_;
    std::vector<double> promoteBuf_;   // used only in Float64 mode
};
