#pragma once

#include "ChannelDescriptor.h"

#include <QList>
#include <QString>

// ---------------------------------------------------------------------------
// FileNaming — builds recording filenames using the convention
//
//   {YYYYMMDD}_{HHMMSS}_{source}_{centerFreq}_{sampleRate}.{ext}
//
// Source tags:
//   single-channel   : "rx0" / "rx1" (channel index from ChannelDescriptor)
//   combined 2-chan  : "dualrx"
//   combined 3-chan  : "triplerx"
//   combined 4-chan  : "quadrorx"
//
// Examples:
//   20260412_153045_rx0_102.000MHz_4.000MSps.cf32
//   20260412_153045_dualrx_bp150kHz_102.000MHz_500.000kSps.cf32
//   20260412_153045_dualrx_fm0_102.000MHz_48.000kSps.wav
//
// Pure Qt (no widget dependencies) — safe to call from any layer.
// ---------------------------------------------------------------------------
namespace FileNaming {

QString perChannelSource(const ChannelDescriptor& ch);
QString combinedSource(const QList<ChannelDescriptor>& channels);

QString formatFrequency(double hz);
QString formatSampleRate(double hz);
QString currentTimestamp();

QString compose(const QString& dir,
                const QString& timestamp,
                const QString& source,
                double         centerFreqHz,
                double         sampleRateHz,
                const QString& extension);

QString composeWithSuffix(const QString& dir,
                          const QString& timestamp,
                          const QString& source,
                          const QString& suffix,
                          double         centerFreqHz,
                          double         sampleRateHz,
                          const QString& extension);

}  // namespace FileNaming
