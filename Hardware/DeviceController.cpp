#include "DeviceController.h"
#include "LimeException.h"
#include "Logger.h"

#include <algorithm>

DeviceController::DeviceController(std::shared_ptr<Device> device, QObject* parent)
    : QObject(parent)
    , device_(std::move(device))
{}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void DeviceController::initDevice() {
    try {
        device_->init_device();

        const double sr = device_->get_sample_rate();
        emit deviceInitialized();
        emit sampleRateChanged(sr);
        emit statusChanged(QString("Initialized — %1 Hz").arg(sr, 0, 'f', 0));
    } catch (const LimeException& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::calibrate(double sampleRateHz) {
    try {
        device_->calibrate(sampleRateHz);

        const double sr = device_->get_sample_rate();
        emit sampleRateChanged(sr);
        emit statusChanged(QString("Calibrated at %1 Hz").arg(sr, 0, 'f', 0));
    } catch (const LimeException& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
void DeviceController::setSampleRate(double sampleRateHz) {
    try {
        device_->set_sample_rate(sampleRateHz);

        const double actual = device_->get_sample_rate();
        emit sampleRateChanged(actual);
        emit statusChanged(QString("Sample rate: %1 Hz").arg(actual, 0, 'f', 0));
    } catch (const LimeException& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::setGain(int lna, int tia, int pga) {
    // ── Physical dB values for LMS7002M RX chain ─────────────────────────────
    //
    // LNA (Low Noise Amplifier) — 6 gain indices (0..5)
    //   These correspond to hardware gain steps in the RF front-end.
    //   Approximate values: 0=0dB, 1=5dB, 2=10dB, 3=15dB, 4=20dB, 5=25.5dB
    //
    // TIA (Trans-Impedance Amplifier) — 3 indices (0..2)
    //   0 = 0 dB (bypass), 1 = 9 dB, 2 = 12 dB
    //   TIA is the first gain stage after the LNA; it sets noise figure floor.
    //
    // PGA (Programmable Gain Amplifier) — 32 steps (0..31), 1 dB/step
    //   Located in the baseband section after ADC filtering.
    //   Fine-tune the overall gain without affecting noise figure.
    //
    // Strategy: set each stage via their summed dB total using LMS_SetGaindB,
    // which distributes the requested dB across all stages proportionally.
    // This is more accurate than LMS_SetNormalizedGain (which uses a [0,1]
    // mapping that doesn't correspond to physical dB steps).
    //
    // Recommended starting points for FM reception:
    //   Weak signal:   LNA=4, TIA=2, PGA=20 → ~57 dB
    //   Normal signal: LNA=3, TIA=1, PGA=10 → ~34 dB
    //   Strong/local:  LNA=1, TIA=0, PGA=5  → ~10 dB

    static constexpr double kLnaDb[6] = {0.0, 5.0, 10.0, 15.0, 20.0, 25.5};
    static constexpr double kTiaDb[3] = {0.0, 9.0, 12.0};

    const double lnaDb = kLnaDb[std::clamp(lna, 0, 5)];
    const double tiaDb = kTiaDb[std::clamp(tia, 0, 2)];
    const double pgaDb = static_cast<double>(std::clamp(pga, 0, 31));
    const double totalDb = lnaDb + tiaDb + pgaDb;

    LOG_DEBUG("setGain: LNA=" + std::to_string(lna) + " (" + std::to_string(lnaDb) + " dB)"
              + "  TIA=" + std::to_string(tia) + " (" + std::to_string(tiaDb) + " dB)"
              + "  PGA=" + std::to_string(pga) + " (" + std::to_string(pgaDb) + " dB)"
              + "  total=" + std::to_string(totalDb) + " dB");

    try {
        device_->set_gain_db(totalDb);
        emit statusChanged(
            QString("Gain: %1 dB  (LNA %2 + TIA %3 + PGA %4)")
                .arg(totalDb, 0, 'f', 1)
                .arg(lnaDb,   0, 'f', 1)
                .arg(tiaDb,   0, 'f', 1)
                .arg(pgaDb,   0, 'f', 0));
    } catch (const LimeException& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::setFrequency(double freqMHz) {
    const double freqHz = freqMHz * 1e6;
    try {
        device_->set_center_frequency(freqHz);
        emit statusChanged(QString("Centre: %1 MHz").arg(freqMHz, 0, 'f', 3));
    } catch (const LimeException& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}
