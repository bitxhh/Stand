#pragma once

#include <QString>

// ---------------------------------------------------------------------------
// RecordingSettings — captures the user's preferences from the recording
// settings dialog. Consumed by RadioMonitorPage/CombinedRxController to
// wire up file-writing handlers at stream start, and by DemodulatorPanel
// for per-demodulator filtered/audio capture.
// ---------------------------------------------------------------------------
struct RecordingSettings {
    enum class RawFormat {
        Float32 = 0,   // .cf32 (32-bit IEEE float interleaved I/Q)
        Float64 = 1,   // .cf64 (64-bit IEEE float interleaved I/Q)
    };

    QString   outputDir;

    bool      recordRawPerChannel{false};
    bool      recordCombined     {false};
    bool      recordFiltered     {false};   // per-demod BandpassHandler
    bool      recordAudio        {false};   // per-demod audio WAV

    RawFormat rawFormat    {RawFormat::Float32};

    [[nodiscard]] QString rawExtension() const {
        return rawFormat == RawFormat::Float64 ? QStringLiteral(".cf64")
                                               : QStringLiteral(".cf32");
    }
};
