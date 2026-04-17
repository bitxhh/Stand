#pragma once

#include <QString>

// ---------------------------------------------------------------------------
// DeviceSettings — per-device UI state persisted as JSON, keyed by serial.
//
// Complements LMS_SaveConfig/LMS_LoadConfig (chip register dump, .ini):
//   • JSON stores UI-level choices (SR, channel count, gains, freqs, TX)
//     that survive across sessions and restore the UI before init.
//   • INI stores LimeSuite chip configuration and is loaded after init()
//     to restore the full register state.
//
// Storage: <AppDataLocation>/Stand/devices/<sanitized-serial>.{json,ini}
// ---------------------------------------------------------------------------
struct DeviceSettings {
    double sampleRate      = 2'000'000.0;
    int    channelCount    = 1;
    int    channelAssign   = 0;                 // which RX if channelCount == 1
    double gainRx[2]       = { 0.0, 0.0 };      // dB per RX channel
    double freqRxMHz[2]    = { 102.0, 102.0 };  // center freq per RX

    // TX page
    double txFreqMHz       = 102.0;
    double txGainDb        = 0.0;
    double txToneOffsetHz  = 0.0;
    double txAmplitude     = 0.3;

    [[nodiscard]] static QString storageDir();
    [[nodiscard]] static QString jsonPathFor(const QString& serial);
    [[nodiscard]] static QString iniPathFor (const QString& serial);

    [[nodiscard]] static DeviceSettings load(const QString& serial);
    bool save(const QString& serial) const;
};
