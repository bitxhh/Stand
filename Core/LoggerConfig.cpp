#include "LoggerConfig.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

LoggerConfig& LoggerConfig::instance() {
    static LoggerConfig inst;
    return inst;
}

LoggerConfig::LoggerConfig() {
    params_ = {
        { QLatin1String(LogCat::kGainRx0),        "Gain RX0"                 },
        { QLatin1String(LogCat::kGainRx1),        "Gain RX1"                 },
        { QLatin1String(LogCat::kFreqRx0),        "Frequency RX0"            },
        { QLatin1String(LogCat::kFreqRx1),        "Frequency RX1"            },
        { QLatin1String(LogCat::kSampleRate),     "Sample rate"              },
        { QLatin1String(LogCat::kCalibration),    "Calibration events"       },
        { QLatin1String(LogCat::kPipelineTiming),  "Pipeline handler timings" },
        { QLatin1String(LogCat::kAudioUnderrun),   "Audio underruns"          },
        { QLatin1String(LogCat::kPipelineDrop),    "Pipeline drops"           },
        { QLatin1String(LogCat::kDeviceLifecycle), "Device lifecycle"         },
        { QLatin1String(LogCat::kStreamIo),        "Stream I/O"               },
        { QLatin1String(LogCat::kDemodInit),       "Demodulator init"         },
        { QLatin1String(LogCat::kCombinedRx),      "Combined RX controller"   },
    };
    for (const auto& p : params_)
        enabled_[p.key] = false;
    load();
}

bool LoggerConfig::isEnabled(const QString& key) const {
    return enabled_.value(key, false);
}

void LoggerConfig::setEnabled(const QString& key, bool on) {
    enabled_[key] = on;
}

QString LoggerConfig::jsonPath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    return QDir(base).filePath(QStringLiteral("logger_settings.json"));
}

void LoggerConfig::save() const {
    QJsonObject o;
    for (auto it = enabled_.constBegin(); it != enabled_.constEnd(); ++it)
        o[it.key()] = it.value();
    QFile f(jsonPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

void LoggerConfig::load() {
    QFile f(jsonPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (enabled_.contains(it.key()))
            enabled_[it.key()] = it.value().toBool();
    }
}
