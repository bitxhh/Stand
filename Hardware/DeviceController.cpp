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

void DeviceController::setGain(double dB) {
    try {
        device_->setGain(dB);
        emit statusChanged(QString("Gain: %1 dB").arg(dB, 0, 'f', 1));
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

void DeviceController::setFrequencyChannel(ChannelDescriptor ch, double freqMHz) {
    try {
        device_->setFrequency(ch, freqMHz * 1e6);
        emit statusChanged(QString("RX%1: %2 MHz")
                           .arg(ch.channelIndex).arg(freqMHz, 0, 'f', 3));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}
