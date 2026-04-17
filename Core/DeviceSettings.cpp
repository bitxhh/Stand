#include "DeviceSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

QString sanitizeSerial(const QString& serial) {
    QString out;
    out.reserve(serial.size());
    for (const QChar c : serial) {
        if (c.isLetterOrNumber() || c == '-' || c == '_')
            out.append(c);
        else
            out.append('_');
    }
    if (out.isEmpty()) out = QStringLiteral("unknown");
    return out;
}

} // namespace

QString DeviceSettings::storageDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("devices"));
}

QString DeviceSettings::jsonPathFor(const QString& serial) {
    return QDir(storageDir()).filePath(sanitizeSerial(serial) + QStringLiteral(".json"));
}

QString DeviceSettings::iniPathFor(const QString& serial) {
    return QDir(storageDir()).filePath(sanitizeSerial(serial) + QStringLiteral(".ini"));
}

DeviceSettings DeviceSettings::load(const QString& serial) {
    DeviceSettings s;
    QFile f(jsonPathFor(serial));
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return s;
    const QJsonObject o = doc.object();

    s.sampleRate    = o.value("sampleRate").toDouble(s.sampleRate);
    s.channelCount  = o.value("channelCount").toInt(s.channelCount);
    s.channelAssign = o.value("channelAssign").toInt(s.channelAssign);

    const QJsonArray gains = o.value("gainRx").toArray();
    for (int i = 0; i < 2 && i < gains.size(); ++i)
        s.gainRx[i] = gains[i].toDouble(s.gainRx[i]);

    const QJsonArray freqs = o.value("freqRxMHz").toArray();
    for (int i = 0; i < 2 && i < freqs.size(); ++i)
        s.freqRxMHz[i] = freqs[i].toDouble(s.freqRxMHz[i]);

    s.txFreqMHz      = o.value("txFreqMHz").toDouble(s.txFreqMHz);
    s.txGainDb       = o.value("txGainDb").toDouble(s.txGainDb);
    s.txToneOffsetHz = o.value("txToneOffsetHz").toDouble(s.txToneOffsetHz);
    s.txAmplitude    = o.value("txAmplitude").toDouble(s.txAmplitude);
    return s;
}

bool DeviceSettings::save(const QString& serial) const {
    QDir().mkpath(storageDir());

    QJsonObject o;
    o["sampleRate"]    = sampleRate;
    o["channelCount"]  = channelCount;
    o["channelAssign"] = channelAssign;
    o["gainRx"]        = QJsonArray{ gainRx[0], gainRx[1] };
    o["freqRxMHz"]     = QJsonArray{ freqRxMHz[0], freqRxMHz[1] };
    o["txFreqMHz"]     = txFreqMHz;
    o["txGainDb"]      = txGainDb;
    o["txToneOffsetHz"]= txToneOffsetHz;
    o["txAmplitude"]   = txAmplitude;

    QFile f(jsonPathFor(serial));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}
