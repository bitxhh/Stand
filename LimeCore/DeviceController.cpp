#include "DeviceController.h"
#include "LimeException.h"
#include "Logger.h"

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
        emit statusChanged(QString("Initialized — %1 Hz")
                               .arg(sr, 0, 'f', 0));
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
    // Normalise each stage to [0, 1], then blend.
    const double lnaNorm = static_cast<double>(lna) / kLnaMax;
    const double tiaNorm = static_cast<double>(tia) / kTiaMax;
    const double pgaNorm = static_cast<double>(pga) / kPgaMax;

    // LNA is the primary gain element; TIA and PGA are trimmed by weights.
    const double combined = lnaNorm
                          + tiaNorm * kTiaWeight
                          + pgaNorm * kPgaWeight;

    // combined can slightly exceed 1.0 when all stages are maxed — clamp
    // happens inside Device::set_normalized_gain, but log intent here.
    LOG_DEBUG("setGain: LNA=" + std::to_string(lna)
              + " TIA=" + std::to_string(tia)
              + " PGA=" + std::to_string(pga)
              + " → normalized=" + std::to_string(combined));

    try {
        device_->set_normalized_gain(combined);
        emit statusChanged(QString("Gain set (LNA %1 / TIA %2 / PGA %3)")
                               .arg(lna).arg(tia).arg(pga));
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
