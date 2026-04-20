#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "IDevice.h"

// ---------------------------------------------------------------------------
// DeviceController — тонкая командная прослойка между UI и IDevice.
//
// Правила:
//   * Никаких Qt-виджетов.
//   * Ловит исключения → emit errorOccurred.
//   * Все public slots безопасны для вызова из main thread.
// ---------------------------------------------------------------------------
class DeviceController : public QObject {
    Q_OBJECT

public:
    explicit DeviceController(std::shared_ptr<IDevice> device, QObject* parent = nullptr);

    [[nodiscard]] const IDevice& device()        const { return *device_; }
    [[nodiscard]] bool           isInitialized() const {
        return device_->state() >= DeviceState::Ready;
    }

public slots:
    // channels empty = все доступные RX (обратная совместимость).
    void initDevice(const QList<ChannelDescriptor>& channels = {});
    // calBwHz — полоса для LMS_Calibrate. -1.0 = вычислить из текущего Fs.
    void calibrate(const QList<ChannelDescriptor>& channels = {}, double calBwHz = -1.0);

    // Auto-open sequence (background thread): init → setSampleRate → calibrate.
    // Emits deviceInitialized, sampleRateChanged, statusChanged/errorOccurred.
    void autoOpen(const QList<ChannelDescriptor>& channels, double sampleRateHz);

    void setSampleRate(double sampleRateHz);

    void setGain(double dB);   // 0–68.5 dB → IDevice::setGain() (channel 0, backward compat)

    // Channel-aware gain — для RX1, TX и будущих каналов
    void setGainChannel(ChannelDescriptor ch, double dB);

    // freqMHz — значение из спинбокса (30..3800 МГц), применяется к RX channel 0
    void setFrequency(double freqMHz);

    // Channel-aware — для RX1 и будущих каналов
    void setFrequencyChannel(ChannelDescriptor ch, double freqMHz);

signals:
    void deviceInitialized();
    void sampleRateChanged(double hz);
    void statusChanged(const QString& message);
    void errorOccurred(const QString& message);
    void progressChanged(int percent, const QString& stage);

private:
    std::shared_ptr<IDevice> device_;
};
