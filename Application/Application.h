#pragma once

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

#include <memory>

#include "../Core/IDeviceManager.h"
#include "../Core/Pipeline.h"
#include "../Hardware/StreamWorker.h"
#include "../Hardware/DeviceController.h"
#include "../DSP/FftHandler.h"
#include "../DSP/FmDemodHandler.h"
#include "../DSP/RawFileHandler.h"
#include "../DSP/BandpassHandler.h"
#include "../Audio/FmAudioOutput.h"

#include <vector>

class QCustomPlot;
class QCPItemLine;
class QCPItemRect;

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::shared_ptr<IDevice> device, IDeviceManager& manager, QWidget* parent = nullptr);
    ~DeviceDetailWindow() override;

signals:
    void deviceDisconnected();

private slots:
    // ── Connection watchdog ───────────────────────────────────────────────────
    void checkDeviceConnection();
    void handleConnectionCheckFinished();

    // ── Reactions to DeviceController signals ─────────────────────────────────
    void onDeviceInitialized();
    void onSampleRateChanged(double hz);
    void onControllerStatus(const QString& message);
    void onControllerError(const QString& message);

    // ── FFT / stream ──────────────────────────────────────────────────────────
    void startStream();
    void stopStream();
    void onFftReady(FftFrame frame);
    void onStreamError(const QString& error);
    void onStreamFinished();
    void onFreqSliderChanged(int value);
    void onFreqSpinChanged(double value);

private:
    std::shared_ptr<IDevice> device;
    IDeviceManager&          manager;
    DeviceController*        controller_{nullptr};

    // ── Navigation ────────────────────────────────────────────────────────────
    QListWidget*    functionList{nullptr};
    QStackedWidget* contentStack{nullptr};
    QWidget*        deviceInfoPage{nullptr};
    QWidget*        deviceControlPage{nullptr};
    QWidget*        deviceFFTpage{nullptr};

    // ── Connection watchdog ───────────────────────────────────────────────────
    QTimer*                                           connectionTimer{nullptr};
    QFutureWatcher<QList<std::shared_ptr<IDevice>>>   connectionWatcher;

    // ── Device Info page ──────────────────────────────────────────────────────
    QLabel* currentSampleRateLabel{nullptr};

    // ── Device Control page ───────────────────────────────────────────────────
    QLabel*      initStatusLabel{nullptr};
    QLabel*      controlStatusLabel{nullptr};
    QComboBox*   sampleRateSelector{nullptr};
    QPushButton* calibrateButton{nullptr};
    QSlider*     lnaSlider{nullptr};
    QComboBox*   tiaCombo_{nullptr};
    QSlider*     pgaSlider{nullptr};
    QLabel*      lnaValueLabel{nullptr};
    QLabel*      pgaValueLabel{nullptr};

    // ── FFT page ──────────────────────────────────────────────────────────────
    QCustomPlot*    fftPlot{nullptr};
    QCPItemLine*    centerLine_{nullptr};
    QPushButton*    streamStartButton{nullptr};
    QPushButton*    streamStopButton{nullptr};
    QLabel*         streamStatusLabel{nullptr};
    QDoubleSpinBox* freqSpinBox{nullptr};
    QSlider*        freqSlider{nullptr};

    // Raw .raw recording
    QCheckBox*  recordCheckBox{nullptr};
    QLineEdit*  recordPathEdit{nullptr};

    // Bandpass WAV export
    QCheckBox*      wavCheckBox{nullptr};
    QDoubleSpinBox* wavOffsetSpin{nullptr};
    QDoubleSpinBox* wavBwSpin{nullptr};
    QLineEdit*      wavPathEdit{nullptr};

    // ── FM Radio ──────────────────────────────────────────────────────────────
    QCheckBox*      fmCheckBox{nullptr};
    QDoubleSpinBox* fmBwSpin_{nullptr};
    QComboBox*      fmDeemphCombo{nullptr};
    QSlider*        fmVolumeSlider{nullptr};
    QLabel*         fmVolumeLabel{nullptr};
    QLabel*         fmStatusLabel{nullptr};
    QLabel*         fmLevelLabel_{nullptr};
    QPushButton*    applyFmButton_{nullptr};

    FmAudioOutput*  fmAudio_{nullptr};

    // ── Spectrum filter band ──────────────────────────────────────────────────
    QCPItemRect*    vfoBand_{nullptr};

    // ── Pipeline ──────────────────────────────────────────────────────────────
    QThread*        streamThread{nullptr};
    StreamWorker*   streamWorker{nullptr};
    Pipeline*       pipeline_{nullptr};
    FftHandler*     fftHandler_{nullptr};
    FmDemodHandler* fmDemodHandler_{nullptr};

    // Non-QObject handlers — owned by DeviceDetailWindow, deleted in teardownStream
    std::vector<RawFileHandler*>     rawHandlers_;
    std::vector<BandpassHandler*>    wavHandlers_;

    // ── Helpers ───────────────────────────────────────────────────────────────
    QWidget* createDeviceInfoPage();
    QWidget* createDeviceControlPage();
    QWidget* createDeviceFFTpage();
    void     refreshCurrentSampleRate() const;
    void     setupFftPlot();
    void     teardownStream();
    void     updateFilterBand(bool visible);

    static constexpr double kFreqMinMHz     =   30.0;
    static constexpr double kFreqMaxMHz     = 3800.0;
    static constexpr double kFreqDefaultMHz =  102.0;
};

// ─────────────────────────────────────────────────────────────────────────────
class DeviceSelectionWindow : public QWidget {
    Q_OBJECT
public:
    explicit DeviceSelectionWindow(IDeviceManager& manager, QWidget* parent = nullptr);

private slots:
    void refreshDevices();
    void openDevice(const std::shared_ptr<IDevice>& device);

private:
    QLabel*      statusLabel{nullptr};
    QListWidget* deviceList{nullptr};
    QTimer*      refreshTimer{nullptr};
    QFutureWatcher<QList<std::shared_ptr<IDevice>>> refreshWatcher;
    IDeviceManager& manager;
};

// ─────────────────────────────────────────────────────────────────────────────
class Application {
public:
    Application(int& argc, char** argv, IDeviceManager& manager);
    int run();

private:
    QApplication          qtApp;
    DeviceSelectionWindow selectionWindow;
};
