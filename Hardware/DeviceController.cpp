#include "DeviceController.h"
#include "Logger.h"

#include <algorithm>
#include <QtConcurrent/QtConcurrent>

DeviceController::DeviceController(std::shared_ptr<IDevice> device, QObject* parent)
    : QObject(parent)
    , device_(std::move(device))
{}

void DeviceController::initDevice(const QList<ChannelDescriptor>& channels) {
    try {
        device_->init(channels);
        emit deviceInitialized();
        emit sampleRateChanged(device_->sampleRate());
        emit statusChanged(
            QString("Initialized — %1 Hz").arg(device_->sampleRate(), 0, 'f', 0));
    } catch (const std::exception& ex) {
        emit errorOccurred(QString::fromStdString(ex.what()));
    }
}

void DeviceController::calibrate(const QList<ChannelDescriptor>& channels, double calBwHz) {
    try {
        device_->calibrate(channels, calBwHz);
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

void DeviceController::setGainChannel(ChannelDescriptor ch, double dB) {
    try {
        device_->setGain(ch, dB);
        const QString dir = (ch.direction == ChannelDescriptor::TX) ? "TX" : "RX";
        emit statusChanged(QString("%1%2 Gain: %3 dB")
                           .arg(dir).arg(ch.channelIndex).arg(dB, 0, 'f', 1));
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

void DeviceController::autoOpen(const QList<ChannelDescriptor>& channels, double sampleRateHz) {
    (void)QtConcurrent::run([this, channels, sampleRateHz]() {
        try {
            emit statusChanged("Initializing…");
            device_->init(channels);
            emit deviceInitialized();
            emit sampleRateChanged(device_->sampleRate());

            device_->setSampleRate(sampleRateHz);
            emit sampleRateChanged(device_->sampleRate());
            emit statusChanged("Calibrating…");

            device_->calibrate(channels, -1.0);
            emit sampleRateChanged(device_->sampleRate());
            emit statusChanged(
                QString("Ready — %1 Hz").arg(device_->sampleRate(), 0, 'f', 0));
        } catch (const std::exception& ex) {
            emit errorOccurred(QString::fromStdString(ex.what()));
        }
    });
}
