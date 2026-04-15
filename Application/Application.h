#pragma once

#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
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
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <QtConcurrent/QtConcurrent>

#include <memory>

#include "../Core/IDeviceManager.h"
#include "../Hardware/DeviceController.h"
#include "RxController.h"
#include "ChannelPanel.h"
#include "SessionManager.h"

class TxController;

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::shared_ptr<IDevice> device, IDeviceManager& manager, QWidget* parent = nullptr);
    ~DeviceDetailWindow() override;

signals:
    void deviceDisconnected();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    // ── Connection watchdog ───────────────────────────────────────────────────
    void checkDeviceConnection();
    void handleConnectionCheckFinished();

    // ── Reactions to DeviceController signals ─────────────────────────────────
    void onDeviceInitialized();
    void onSampleRateChanged(double hz);
    void onControllerStatus(const QString& message);
    void onControllerError(const QString& message);

    // ── Stream ────────────────────────────────────────────────────────────────
    void startStream();
    void stopStream();
    void onStreamError(const QString& error);
    void onStreamFinished();

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

    // ── FFT page — stream controls ────────────────────────────────────────────
    QPushButton* streamStartButton{nullptr};
    QPushButton* streamStopButton{nullptr};
    QLabel*      streamStatusLabel{nullptr};

    // ── Per-channel panels — one per RX channel from device->availableChannels() ──
    QVector<ChannelPanel*> channelPanels_;

    // ── DSP thread pool — shared across all RX-channel RxControllers ────────
    QThreadPool* dspPool_{nullptr};

    // ── Plot render timer (delegates to each ChannelPanel::replotIfDirty()) ───
    QTimer* plotTimer_{nullptr};

    // ── Metrics timer (delegates to each ChannelPanel::updateMetrics()) ───────
    QTimer* metricsTimer_{nullptr};

    // ── TX page ───────────────────────────────────────────────────────────────
    QWidget*        txPage_{nullptr};
    QDoubleSpinBox* txFreqSpin_{nullptr};
    QSlider*        txGainSlider_{nullptr};
    QLabel*         txGainLabel_{nullptr};
    QDoubleSpinBox* txToneOffsetSpin_{nullptr};
    QDoubleSpinBox* txAmplitudeSpin_{nullptr};
    QPushButton*    txStartButton_{nullptr};
    QPushButton*    txStopButton_{nullptr};
    QLabel*         txStatusLabel_{nullptr};
    TxController*   txCtrl_{nullptr};

    // ── Helpers ───────────────────────────────────────────────────────────────
    QWidget* createDeviceInfoPage();
    QWidget* createDeviceControlPage();
    QWidget* createDeviceFFTpage();
    QWidget* createTxPage();
    void     stopAllStreams();
    void     refreshCurrentSampleRate() const;

    static constexpr double kFreqMinMHz     =   30.0;
    static constexpr double kFreqMaxMHz     = 3800.0;
    static constexpr double kFreqDefaultMHz =  102.0;
};

// ─────────────────────────────────────────────────────────────────────────────
class DeviceSelectionWindow : public QWidget {
    Q_OBJECT
public:
    explicit DeviceSelectionWindow(IDeviceManager& manager,
                                   SessionManager& sessions,
                                   QWidget* parent = nullptr);

private slots:
    void refreshDevices();
    void updateDeviceButtons();
    void openDevice(const std::shared_ptr<IDevice>& device);

private:
    QLabel*      statusLabel{nullptr};
    QListWidget* deviceList{nullptr};
    QTimer*      refreshTimer{nullptr};
    QFutureWatcher<QList<std::shared_ptr<IDevice>>> refreshWatcher;
    IDeviceManager& manager;
    SessionManager& sessions_;
    QList<std::shared_ptr<IDevice>> lastDevices_;
};

// ─────────────────────────────────────────────────────────────────────────────
class Application {
public:
    Application(int& argc, char** argv, IDeviceManager& manager);
    int run();

private:
    QApplication          qtApp;
    SessionManager        sessionManager_;
    DeviceSelectionWindow selectionWindow;
};
