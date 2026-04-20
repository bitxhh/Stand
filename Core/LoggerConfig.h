#pragma once

#include <QList>
#include <QMap>
#include <QString>

// Predefined parameter-log category keys — used as filter tokens in LoggerConfig.
namespace LogCat {
    inline constexpr const char* kSignalQuality  = "signal_quality";
    inline constexpr const char* kGainRx0        = "gain_rx0";
    inline constexpr const char* kGainRx1        = "gain_rx1";
    inline constexpr const char* kFreqRx0        = "freq_rx0";
    inline constexpr const char* kFreqRx1        = "freq_rx1";
    inline constexpr const char* kSampleRate     = "sample_rate";
    inline constexpr const char* kCalibration    = "calibration";
    inline constexpr const char* kPipelineTiming = "pipeline_timing";
    inline constexpr const char* kAudioUnderrun  = "audio_underrun";
    inline constexpr const char* kPipelineDrop   = "pipeline_drop";
}

// Runtime filter for LOG_PARAM calls.  Disabled keys are dropped before
// any string formatting, so the hot path is a single bool lookup.
// Persisted to <AppData>/Stand/logger_settings.json.
class LoggerConfig {
public:
    static LoggerConfig& instance();

    bool isEnabled(const QString& key) const;
    void setEnabled(const QString& key, bool on);

    struct ParamEntry { QString key; QString label; };
    const QList<ParamEntry>& allParams() const { return params_; }

    void save() const;
    void load();

private:
    LoggerConfig();

    static QString jsonPath();

    QMap<QString, bool> enabled_;
    QList<ParamEntry>   params_;
};
