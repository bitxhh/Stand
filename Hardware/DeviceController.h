#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "Device.h"

// ---------------------------------------------------------------------------
// DeviceController — thin command layer between UI and Device.
//
// Rules:
//   • Never touches any Qt widget.
//   • Translates raw UI values (slider ints) → normalized hardware params.
//   • Catches LimeException → emits errorOccurred so the UI can react.
//   • All public slots are safe to call from the main thread.
// ---------------------------------------------------------------------------
class DeviceController : public QObject {
    Q_OBJECT

public:
    explicit DeviceController(std::shared_ptr<Device> device, QObject* parent = nullptr);

    // Read-only access for UI widgets that need to query state directly.
    [[nodiscard]] const Device& device() const { return *device_; }
    [[nodiscard]] bool          isInitialized() const { return device_->is_initialized(); }

public slots:
    // ── Lifecycle ─────────────────────────────────────────────────────────────
    void initDevice();
    void calibrate(double sampleRateHz);

    // ── Parameters ────────────────────────────────────────────────────────────
    // sampleRateHz — one of the values from LimeManager::sampleRates
    void setSampleRate(double sampleRateHz);

    // Raw slider values: LNA [0..5], TIA [0..2], PGA [0..31]
    void setGain(int lna, int tia, int pga);

    // freqMHz — value from the spin-box (30..3800 MHz)
    void setFrequency(double freqMHz);

signals:
    // Emitted after successful initDevice() — lets UI re-enable controls.
    void deviceInitialized();

    // Emitted after every successful setSampleRate / calibrate.
    void sampleRateChanged(double hz);

    // Short human-readable status for a status-bar label.
    void statusChanged(const QString& message);

    // Hardware or parameter error — show in a QMessageBox or label.
    void errorOccurred(const QString& message);

private:
    std::shared_ptr<Device> device_;

    // Physical gain ranges — documented here for reference, used in .cpp
    // LNA: 0..5 indices → 0, 5, 10, 15, 20, 25.5 dB
    // TIA: 0..2 indices → 0, 9, 12 dB
    // PGA: 0..31        → 0..31 dB (1 dB/step)
    // Max total ≈ 68.5 dB
};
