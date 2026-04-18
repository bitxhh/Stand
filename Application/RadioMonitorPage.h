#pragma once

#include "../Core/ChannelDescriptor.h"
#include "../Core/DeviceSettings.h"
#include "../Core/RecordingSettings.h"
#include "../DSP/FftProcessor.h"

#include <QWidget>
#include <QList>
#include <QVector>

class QCustomPlot;
class QCPItemLine;
class QCPItemRect;
class QDoubleSpinBox;
class QSlider;
class QPushButton;
class QLabel;
class QCheckBox;
class QVBoxLayout;
class QThreadPool;

class IDevice;
class DeviceController;
class CombinedRxController;
class DemodulatorPanel;

// ---------------------------------------------------------------------------
// RadioMonitorPage — единая вкладка радиомониторинга.
//
// Layout:
//   [ Freq spinbox+slider / Apply ]
//   [ FFT plot (single spectrum, combined I/Q) ]
//   [ + Add demodulator ] [ Record ] [ Settings ]
//   [ DemodulatorPanel 1 … DemodulatorPanel N ]  (макс 4)
//   [ Start / Stop ] [ Status ]
//
// Owns CombinedRxController. The page is responsible for wiring
// CombinedRxController → QCustomPlot (single FFT), and for creating /
// destroying DemodulatorPanel instances that each hold their own demod
// handler and audio output attached via ctrl->addExtraHandler.
// ---------------------------------------------------------------------------
class RadioMonitorPage : public QWidget {
    Q_OBJECT

public:
    RadioMonitorPage(IDevice*          device,
                     DeviceController* controller,
                     QThreadPool*      dspPool,
                     QWidget*          parent = nullptr);
    ~RadioMonitorPage() override;

    void setActiveChannels(const QList<ChannelDescriptor>& channels);
    void setChannelGains(const QVector<double>& gainsDb);

    // DeviceDetailWindow drives these from its timers.
    void replotIfDirty();
    void updateMetrics();

    // Called by DeviceDetailWindow when the device finishes initialization,
    // so buttons can be enabled and the VFO ranges can be set from SR.
    void onDeviceReady();

    // Synchronous stream teardown — blocks until workers exit.
    void shutdown();

    [[nodiscard]] bool isStreaming() const;
    [[nodiscard]] double centerFreqMHz() const;

    // Persistence — DemodPanel slot states.
    [[nodiscard]] QList<DemodPanelSettings> demodPanelStates() const;
    void restoreDemodPanels(const QList<DemodPanelSettings>& panels);

signals:
    void streamStarted();
    void streamStopped();
    void errorOccurred(const QString& message);

private slots:
    void onFftReady(FftFrame frame);
    void applyFrequency();
    void startStream();
    void stopStream();
    void onStreamFinishedInternal();
    void onStreamErrorInternal(const QString& err);

    void addDemodulator();
    void removeDemodulator(int slotIndex);
    void openRecordingSettings();

private:
    void buildUi();
    void setupFftPlot();
    void updateFilterBands();
    void pushRecordingContextToPanels(const QString& timestamp,
                                      const QString& combinedSource,
                                      double         centerFreqHz);
    void loadRecordingSettings();
    void saveRecordingSettings() const;

    IDevice*          device_;
    DeviceController* controller_;
    QThreadPool*      dspPool_;

    CombinedRxController* ctrl_{nullptr};

    // ── Active RX channels + gains (set by DeviceDetailWindow) ───────────────
    QList<ChannelDescriptor> activeChannels_;
    QVector<double>          gainsDb_;

    // ── Frequency controls ───────────────────────────────────────────────────
    QDoubleSpinBox* freqSpin_{nullptr};
    QSlider*        freqSlider_{nullptr};
    QPushButton*    applyBtn_{nullptr};

    // ── FFT plot ─────────────────────────────────────────────────────────────
    QCustomPlot*    fftPlot_{nullptr};
    QCPItemLine*    centerLine_{nullptr};
    QVector<QCPItemRect*> vfoBands_;    // one per DemodulatorPanel, index = slot
    bool            plotUserZoomed_{false};
    bool            fftDirty_{false};

    // ── Demodulator panels ───────────────────────────────────────────────────
    QVBoxLayout*    panelsLayout_{nullptr};
    QVector<DemodulatorPanel*> panels_;
    QPushButton*    addDemodBtn_{nullptr};

    // ── Stream controls ──────────────────────────────────────────────────────
    QPushButton*    startBtn_{nullptr};
    QPushButton*    stopBtn_{nullptr};
    QLabel*         statusLabel_{nullptr};

    // ── Recording ────────────────────────────────────────────────────────────
    QCheckBox*        recordCheck_{nullptr};
    QPushButton*      settingsBtn_{nullptr};
    RecordingSettings recordingSettings_{};
    QString           sessionTimestamp_;     // set at startStream, reused for mid-session panels

    static constexpr int    kMaxDemods       = 4;
    static constexpr double kFreqMinMHz      =   30.0;
    static constexpr double kFreqMaxMHz      = 3800.0;
    static constexpr double kFreqDefaultMHz  =  102.0;
};
