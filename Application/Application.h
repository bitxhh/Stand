#pragma once

#include <QAbstractItemView>
#include <QToolBar>
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
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
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
class RadioMonitorPage;

class DeviceDetailWindow : public QMainWindow {
    Q_OBJECT
public:
    DeviceDetailWindow(std::shared_ptr<IDevice> device, IDeviceManager& manager, QWidget* parent = nullptr);
    ~DeviceDetailWindow() override;

signals:
    void deviceDisconnected();
    void openDeviceSelectionRequested();

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
    // Stream lifecycle is owned by RadioMonitorPage; DeviceDetailWindow reacts
    // via radioMonitorPage_ signals (streamStarted / streamStopped / error).

private:
    std::shared_ptr<IDevice> device;
    IDeviceManager&          manager;
    DeviceController*        controller_{nullptr};

    // ── Navigation ────────────────────────────────────────────────────────────
    QListWidget*      functionList{nullptr};
    QStackedWidget*   contentStack{nullptr};
    QWidget*          deviceInfoPage{nullptr};
    QWidget*          deviceControlPage{nullptr};
    RadioMonitorPage* radioMonitorPage_{nullptr};

    // ── Connection watchdog ───────────────────────────────────────────────────
    QTimer*                                           connectionTimer{nullptr};
    QFutureWatcher<QList<std::shared_ptr<IDevice>>>   connectionWatcher;

    // ── Device Info page ──────────────────────────────────────────────────────
    QLabel* currentSampleRateLabel{nullptr};

    // ── Device Control page ───────────────────────────────────────────────────
    QLabel*      initStatusLabel{nullptr};
    QLabel*      controlStatusLabel{nullptr};
    QComboBox*   sampleRateSelector{nullptr};
    QPushButton* resetButton_{nullptr};
    QPushButton* calibrateButton{nullptr};
    // Channel selection (RX count + optional single-channel assignment)
    QSpinBox*   channelCountSpin_{nullptr};
    QComboBox*  channelAssignCombo_{nullptr};   // visible only when count==1 && numRx>1
    QWidget*    channelAssignRow_{nullptr};

    // Per-RX-channel gain sliders, indexed by RX channel index.
    QVector<QSlider*> gainSliders_;
    QVector<QLabel*>  gainValueLabels_;
    QVector<QWidget*> gainRows_;                // whole row widget for show/hide

    // Helper — builds the list of ChannelDescriptors currently selected in UI.
    [[nodiscard]] QList<ChannelDescriptor> selectedChannels() const;
    void updateChannelRowVisibility();
    void autoOpenDevice();
    void applyChannelSelectionChange();

    // ── DSP thread pool — shared across RadioMonitorPage's pipeline ──────────
    QThreadPool* dspPool_{nullptr};

    // ── Plot render timer (delegates to RadioMonitorPage::replotIfDirty()) ───
    QTimer* plotTimer_{nullptr};

    // ── Metrics timer (delegates to RadioMonitorPage::updateMetrics()) ────────
    QTimer* metricsTimer_{nullptr};

    // ── Chip temperature indicator in status bar ─────────────────────────────
    QLabel* temperatureLabel_{nullptr};
    QTimer* temperatureTimer_{nullptr};
    void    updateTemperature();

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

    // ── Init progress bar ─────────────────────────────────────────────────────
    QProgressBar* initProgressBar_{nullptr};
    QLabel*       initProgressLabel_{nullptr};
    void          setNavEnabled(bool enabled);

    // ── Helpers ───────────────────────────────────────────────────────────────
    QWidget* createDeviceInfoPage();
    QWidget* createDeviceControlPage();
    QWidget* createRadioMonitorPage();
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
