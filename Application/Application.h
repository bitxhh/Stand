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

#include "../LimeCore/LimeManager.h"
#include "../LimeCore/StreamWorker.h"
#include "../LimeCore/DeviceController.h"
#include "../LimeCore/BandpassExporter.h"
#include "../FFTProcessor/FftProcessor.h"

class QCustomPlot;
class QCPItemLine;

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::shared_ptr<Device> device, LimeManager& manager, QWidget* parent = nullptr);
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
    void onSamplesReady(QVector<int16_t> samples);
    void onStreamError(const QString& error);
    void onStreamFinished();
    void onFreqSliderChanged(int value);
    void onFreqSpinChanged(double value);

private:
    std::shared_ptr<Device> device;
    LimeManager&            manager;
    DeviceController*       controller_{nullptr};   // ← new

    // ── Navigation ────────────────────────────────────────────────────────────
    QListWidget*    functionList{nullptr};
    QStackedWidget* contentStack{nullptr};
    QWidget*        deviceInfoPage{nullptr};
    QWidget*        deviceControlPage{nullptr};
    QWidget*        deviceFFTpage{nullptr};

    // ── Connection watchdog ───────────────────────────────────────────────────
    QTimer*                                              connectionTimer{nullptr};
    QFutureWatcher<std::vector<std::shared_ptr<Device>>> connectionWatcher;

    // ── Device Info page ──────────────────────────────────────────────────────
    QLabel* currentSampleRateLabel{nullptr};

    // ── Device Control page ───────────────────────────────────────────────────
    QLabel*      initStatusLabel{nullptr};
    QLabel*      controlStatusLabel{nullptr};   // ← replaces per-call QMessageBox feedback
    QComboBox*   sampleRateSelector{nullptr};
    QPushButton* calibrateButton{nullptr};
    QSlider*     lnaSlider{nullptr};
    QSlider*     tiaSlider{nullptr};
    QSlider*     pgaSlider{nullptr};
    QLabel*      lnaValueLabel{nullptr};
    QLabel*      tiaValueLabel{nullptr};
    QLabel*      pgaValueLabel{nullptr};

    // ── FFT page ──────────────────────────────────────────────────────────────
    QCustomPlot*    fftPlot{nullptr};
    QCPItemLine*    centerLine_{nullptr};   // red vertical line at LO centre
    QPushButton*    streamStartButton{nullptr};
    QPushButton*    streamStopButton{nullptr};
    QLabel*         streamStatusLabel{nullptr};
    QDoubleSpinBox* freqSpinBox{nullptr};
    QSlider*        freqSlider{nullptr};
    QCheckBox*      recordCheckBox{nullptr};
    QLineEdit*      recordPathEdit{nullptr};

    // Bandpass WAV export
    QCheckBox*      wavCheckBox{nullptr};
    QDoubleSpinBox* wavOffsetSpin{nullptr};
    QDoubleSpinBox* wavBwSpin{nullptr};
    QLineEdit*      wavPathEdit{nullptr};

    // ── Streaming ─────────────────────────────────────────────────────────────
    QThread*      streamThread{nullptr};
    StreamWorker* streamWorker{nullptr};

    // ── Helpers ───────────────────────────────────────────────────────────────
    QWidget* createDeviceInfoPage();
    QWidget* createDeviceControlPage();
    QWidget* createDeviceFFTpage();
    void     refreshCurrentSampleRate() const;
    void     setupFftPlot();
    void     teardownStream();

    static constexpr double kFreqMinMHz     =   30.0;
    static constexpr double kFreqMaxMHz     = 3800.0;
    static constexpr double kFreqDefaultMHz =  102.0;
};

// ─────────────────────────────────────────────────────────────────────────────
class DeviceSelectionWindow : public QWidget {
    Q_OBJECT
public:
    explicit DeviceSelectionWindow(LimeManager& manager, QWidget* parent = nullptr);

private slots:
    void refreshDevices();
    void openDevice(const std::shared_ptr<Device>& device);

private:
    QLabel*      statusLabel{nullptr};
    QListWidget* deviceList{nullptr};
    QTimer*      refreshTimer{nullptr};
    QFutureWatcher<std::vector<std::shared_ptr<Device>>> refreshWatcher;
    LimeManager& manager;
};

// ─────────────────────────────────────────────────────────────────────────────
class Application {
public:
    Application(int& argc, char** argv, LimeManager& manager);
    int run();

private:
    QApplication          qtApp;
    DeviceSelectionWindow selectionWindow;
};
