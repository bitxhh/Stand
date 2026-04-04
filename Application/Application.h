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

#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

#include <memory>

#include "../Core/IDeviceManager.h"
#include "../Hardware/DeviceController.h"
#include "AppController.h"

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
    QSlider*     gainSlider_{nullptr};
    QLabel*      gainValueLabel_{nullptr};

    // ── FFT page ──────────────────────────────────────────────────────────────
    QCustomPlot*    fftPlot{nullptr};
    QCPItemLine*    centerLine_{nullptr};
    QPushButton*    streamStartButton{nullptr};
    QPushButton*    streamStopButton{nullptr};
    QLabel*         streamStatusLabel{nullptr};
    QDoubleSpinBox* freqSpinBox{nullptr};
    QSlider*        freqSlider{nullptr};

    // Recording
    QCheckBox*  recordCheckBox{nullptr};
    QCheckBox*  wavCheckBox{nullptr};

    // Recording settings (stored as values, edited via dialog)
    QString recordPath_;
    QString wavPath_;
    double  wavOffset_{0.0};
    double  wavBw_{100'000.0};

    // ── Demodulator (FM / AM) ────────────────────────────────────────────────
    QComboBox*      modeCombo_{nullptr};       // "Off" / "FM" / "AM"
    QDoubleSpinBox* demodVfoSpin_{nullptr};    // VFO absolute freq (MHz)
    QSlider*        demodVolSlider_{nullptr};  // volume 0-100
    QLabel*         demodVolLabel_{nullptr};   // volume %
    QLabel*         demodStatusLabel_{nullptr};
    QLabel*         demodLevelLabel_{nullptr}; // SNR bar

    // FM-specific (shown only in FM mode)
    QLabel*         fmBwLabel_{nullptr};
    QDoubleSpinBox* fmBwSpin_{nullptr};
    QLabel*         fmDeemphLabel_{nullptr};
    QComboBox*      fmDeemphCombo{nullptr};

    // AM-specific (shown only in AM mode)
    QLabel*         amBwLabel_{nullptr};
    QDoubleSpinBox* amBwSpin_{nullptr};

    // ── Spectrum filter band ──────────────────────────────────────────────────
    QCPItemRect*    vfoBand_{nullptr};

    // ── App controller (owns Pipeline, handlers, audio, worker thread) ───────
    AppController*  ctrl_{nullptr};

    // ── Plot zoom state ───────────────────────────────────────────────────────
    // True when the user has scrolled in — onFftReady skips rescale to keep zoom.
    // Reset by double-click or by zooming back out to the full capture band.
    bool plotUserZoomed_{false};

    // ── Metrics ───────────────────────────────────────────────────────────────
    QTimer* metricsTimer_{nullptr};
    void    updateDemodMetrics();
    void    onModeChanged(int index);

    // ── Helpers ───────────────────────────────────────────────────────────────
    QWidget* createDeviceInfoPage();
    QWidget* createDeviceControlPage();
    QWidget* createDeviceFFTpage();
    void     refreshCurrentSampleRate() const;
    void     setupFftPlot();
    void     updateFilterBand(bool visible);
    void     openRecordSettings();

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
