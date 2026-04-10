#pragma once

#include <QWidget>
#include <QString>
#include "../Core/ChannelDescriptor.h"
#include "../DSP/FftProcessor.h"
#include "RxController.h"

class QCustomPlot;
class QCPItemLine;
class QCPItemRect;
class QDoubleSpinBox;
class QSlider;
class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class IDevice;
class DeviceController;
class ClassifierController;

// ---------------------------------------------------------------------------
// ChannelPanel — self-contained per-channel QWidget.
//
// Owns: FFT plot, frequency spin/slider, gain slider, demod controls (FM/AM),
// VFO spin, level indicator, recording checkboxes, classifier toggle.
//
// Call setRxController() to wire it to an RxController (and recreate
// the embedded ClassifierController). Must be called before startStream.
//
// replotIfDirty() — called from DDW's plotTimer, 20fps max.
// buildStreamConfig() — assembles StreamConfig for RxController::startStream().
// updateMetrics()    — called from DDW's metricsTimer (every 500ms).
// ---------------------------------------------------------------------------
class ChannelPanel : public QWidget {
    Q_OBJECT

public:
    struct Config {
        ChannelDescriptor channel;
        QString           displayName;
        double            freqMinMHz     = 30.0;
        double            freqMaxMHz     = 3800.0;
        double            freqDefaultMHz = 102.0;
    };

    ChannelPanel(const Config& cfg,
                 IDevice*          device,
                 DeviceController* controller,
                 QWidget*          parent = nullptr);
    ~ChannelPanel() override;

    // Wire to an RxController (creates ClassifierController internally).
    // Safe to call multiple times; disconnects previous controller first.
    void setRxController(RxController* ctrl);

    [[nodiscard]] RxController*    appController() const { return ctrl_; }
    [[nodiscard]] ChannelDescriptor channel()       const { return cfg_.channel; }

    // Called by DDW plotTimer (50ms) — replots only if new FFT data arrived.
    void replotIfDirty();

    // Called by DDW metricsTimer (500ms) — updates SNR bar.
    void updateMetrics();

    // Called by DDW startStream / stopStream.
    void onStreamStarted();
    void onStreamStopped();

    // Assemble StreamConfig from current widget state.
    [[nodiscard]] RxController::StreamConfig buildStreamConfig() const;

    // Recording path accessors — for openRecordSettings dialog.
    [[nodiscard]] QString rawPath()   const { return rawPath_; }
    [[nodiscard]] QString wavPath()   const { return wavPath_; }
    [[nodiscard]] double  wavOffset() const { return wavOffset_; }
    [[nodiscard]] double  wavBw()     const { return wavBw_; }

    void setRawPath(const QString& p)  { rawPath_   = p; }
    void setWavPath(const QString& p)  { wavPath_   = p; }
    void setWavOffset(double off)      { wavOffset_ = off; }
    void setWavBw(double bw)           { wavBw_     = bw; }

signals:
    void frequencyApplied(ChannelDescriptor ch, double mhz);
    void gainChanged(ChannelDescriptor ch, double dB);

private slots:
    void onFftReady(FftFrame frame);
    void applyFrequency();
    void onModeChanged(int index);
    void openRecordSettings();

private:
    void buildUi();
    void setupFftPlot();
    void updateFilterBand(bool visible);
    void applyDemodParams();

    Config            cfg_;
    IDevice*          device_;
    DeviceController* controller_;
    RxController*    ctrl_{nullptr};
    ClassifierController* classifierCtrl_{nullptr};

    // ── FFT plot ──────────────────────────────────────────────────────────────
    QCustomPlot* fftPlot_{nullptr};
    QCPItemLine* centerLine_{nullptr};
    QCPItemRect* vfoBand_{nullptr};
    bool         plotUserZoomed_{false};
    bool         fftDirty_{false};

    // ── Frequency ─────────────────────────────────────────────────────────────
    QDoubleSpinBox* freqSpinBox_{nullptr};
    QSlider*        freqSlider_{nullptr};

    // ── Gain ──────────────────────────────────────────────────────────────────
    QSlider* gainSlider_{nullptr};
    QLabel*  gainValueLabel_{nullptr};

    // ── Demodulator ───────────────────────────────────────────────────────────
    QComboBox*      modeCombo_{nullptr};
    QLabel*         volLabel_{nullptr};
    QDoubleSpinBox* demodVfoSpin_{nullptr};
    QSlider*        demodVolSlider_{nullptr};
    QLabel*         demodVolLabel_{nullptr};
    QLabel*         demodStatusLabel_{nullptr};
    QLabel*         demodLevelLabel_{nullptr};

    QLabel*         fmBwLabel_{nullptr};
    QDoubleSpinBox* fmBwSpin_{nullptr};
    QLabel*         fmDeemphLabel_{nullptr};
    QComboBox*      fmDeemphCombo_{nullptr};

    QLabel*         amBwLabel_{nullptr};
    QDoubleSpinBox* amBwSpin_{nullptr};

    // ── Recording ─────────────────────────────────────────────────────────────
    QCheckBox* recordCheckBox_{nullptr};
    QCheckBox* wavCheckBox_{nullptr};
    QString    rawPath_;
    QString    wavPath_;
    double     wavOffset_{0.0};
    double     wavBw_{100'000.0};

    // ── Classifier ────────────────────────────────────────────────────────────
    QCheckBox* classifierCheck_{nullptr};
    QLabel*    classifierLabel_{nullptr};
};
