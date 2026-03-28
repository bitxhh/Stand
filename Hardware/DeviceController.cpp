#include "DeviceController.h"
#include "Logger.h"

#include <algorithm>

DeviceController::DeviceController(std::shared_ptr<IDevice> device, QObject* parent)
    : QObject(parent)
    , device_(std::move(device))
{}

void DeviceController::initDevice() {
    try {
        device_->init();
        emit deviceInitialized();
        emit sampleRateChanged(device_->sampleRate());
        emit statusChanged(
            QString("Initialized — %1 Hz").arg(device_->sampleRate(), 0, 'f', 0));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::calibrate(double sampleRateHz) {
    try {
        device_->setSampleRate(sampleRateHz);
        device_->calibrate();
        emit sampleRateChanged(device_->sampleRate());
        emit statusChanged(
            QString("Calibrated at %1 Hz").arg(device_->sampleRate(), 0, 'f', 0));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::setSampleRate(double sampleRateHz) {
    try {
        device_->setSampleRate(sampleRateHz);
        emit sampleRateChanged(device_->sampleRate());
        emit statusChanged(
            QString("Sample rate: %1 Hz").arg(device_->sampleRate(), 0, 'f', 0));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::setGain(int lna, int tia, int pga) {
    static constexpr double kLnaDb[] = {0.0, 5.0, 10.0, 15.0, 20.0, 25.5};
    static constexpr double kTiaDb[] = {0.0, 9.0, 12.0};

    const double lnaDb  = kLnaDb[std::clamp(lna, 0, 5)];
    const double tiaDb  = kTiaDb[std::clamp(tia, 0, 2)];
    const double pgaDb  = static_cast<double>(std::clamp(pga, 0, 31));
    const double totalDb = lnaDb + tiaDb + pgaDb;

    LOG_DEBUG("setGain: LNA=" + std::to_string(lna) + " TIA=" + std::to_string(tia)
              + " PGA=" + std::to_string(pga) + " total=" + std::to_string(totalDb) + " dB");

    try {
        device_->setGain(totalDb);
        emit statusChanged(
            QString("Gain: %1 dB  (LNA %2 + TIA %3 + PGA %4)")
                .arg(totalDb, 0, 'f', 1)
                .arg(lnaDb,   0, 'f', 1)
                .arg(tiaDb,   0, 'f', 1)
                .arg(pgaDb,   0, 'f', 0));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::setFrequency(double freqMHz) {
    try {
        device_->setFrequency(freqMHz * 1e6);
        emit statusChanged(QString("Centre: %1 MHz").arg(freqMHz, 0, 'f', 3));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}
