#include "FileNaming.h"

#include <QDateTime>
#include <QDir>

namespace FileNaming {

QString perChannelSource(const ChannelDescriptor& ch) {
    return QStringLiteral("rx%1").arg(ch.channelIndex);
}

QString combinedSource(const QList<ChannelDescriptor>& channels) {
    switch (channels.size()) {
        case 0:  return QStringLiteral("rx0");
        case 1:  return perChannelSource(channels.first());
        case 2:  return QStringLiteral("dualrx");
        case 3:  return QStringLiteral("triplerx");
        case 4:  return QStringLiteral("quadrorx");
        default: return QStringLiteral("multirx%1").arg(channels.size());
    }
}

QString formatFrequency(double hz) {
    return QStringLiteral("%1MHz").arg(hz / 1e6, 0, 'f', 3);
}

QString formatSampleRate(double hz) {
    if (hz >= 1e6)
        return QStringLiteral("%1MSps").arg(hz / 1e6, 0, 'f', 3);
    return QStringLiteral("%1kSps").arg(hz / 1e3, 0, 'f', 3);
}

QString currentTimestamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
}

static QString joinDir(const QString& dir, const QString& name) {
    if (dir.isEmpty()) return name;
    return QDir(dir).filePath(name);
}

QString compose(const QString& dir,
                const QString& timestamp,
                const QString& source,
                double         centerFreqHz,
                double         sampleRateHz,
                const QString& extension) {
    const QString ext = extension.startsWith('.') ? extension : ('.' + extension);
    const QString name = QStringLiteral("%1_%2_%3_%4%5")
        .arg(timestamp, source,
             formatFrequency(centerFreqHz),
             formatSampleRate(sampleRateHz),
             ext);
    return joinDir(dir, name);
}

QString composeWithSuffix(const QString& dir,
                          const QString& timestamp,
                          const QString& source,
                          const QString& suffix,
                          double         centerFreqHz,
                          double         sampleRateHz,
                          const QString& extension) {
    const QString ext = extension.startsWith('.') ? extension : ('.' + extension);
    const QString name = QStringLiteral("%1_%2_%3_%4_%5%6")
        .arg(timestamp, source, suffix,
             formatFrequency(centerFreqHz),
             formatSampleRate(sampleRateHz),
             ext);
    return joinDir(dir, name);
}

}  // namespace FileNaming
