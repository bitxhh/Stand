#include "Application.h"

#include <algorithm>
#include <cmath>
#include <QScrollArea>
#include <QSize>
#include "../Core/ChannelDescriptor.h"
#include "TxController.h"

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceDetailWindow — constructor
// ═══════════════════════════════════════════════════════════════════════════════
DeviceDetailWindow::DeviceDetailWindow(std::shared_ptr<IDevice> device, IDeviceManager& manager, QWidget* parent)
    : QMainWindow(parent)
    , device(std::move(device))
    , manager(manager)
    , controller_(new DeviceController(this->device, this))
{
    setWindowTitle(this->device->id());

    connect(controller_, &DeviceController::deviceInitialized,
            this,        &DeviceDetailWindow::onDeviceInitialized);
    connect(controller_, &DeviceController::sampleRateChanged,
            this,        &DeviceDetailWindow::onSampleRateChanged);
    connect(controller_, &DeviceController::statusChanged,
            this,        &DeviceDetailWindow::onControllerStatus);
    connect(controller_, &DeviceController::errorOccurred,
            this,        &DeviceDetailWindow::onControllerError);

    // ── DSP thread pool — one per DeviceDetailWindow, shared across all RX pipelines.
    // maxThreadCount left at default (QThread::idealThreadCount()) so the pool
    // scales automatically to the machine's core count.
    dspPool_ = new QThreadPool(this);

    // ── Create one ChannelPanel per RX channel reported by the device ────────
    for (const auto& chInfo : this->device->availableChannels()) {
        if (chInfo.descriptor.direction != ChannelDescriptor::RX)
            continue;
        ChannelPanel::Config cfg;
        cfg.channel        = chInfo.descriptor;
        cfg.displayName    = chInfo.displayName;
        cfg.freqMinMHz     = kFreqMinMHz;
        cfg.freqMaxMHz     = kFreqMaxMHz;
        cfg.freqDefaultMHz = kFreqDefaultMHz;
        auto* panel = new ChannelPanel(cfg, this->device.get(), controller_, this);
        auto* ctrl  = new RxController(this->device.get(), chInfo.descriptor, dspPool_, this);
        panel->setRxController(ctrl);
        connect(ctrl, &RxController::streamError,
                this, &DeviceDetailWindow::onStreamError,    Qt::QueuedConnection);
        connect(ctrl, &RxController::streamFinished,
                this, &DeviceDetailWindow::onStreamFinished, Qt::QueuedConnection);
        channelPanels_.append(panel);
    }

    // ── Build UI ──────────────────────────────────────────────────────────────
    auto* central    = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);
    auto* splitter   = new QSplitter(Qt::Horizontal, central);
    splitter->setChildrenCollapsible(false);

    functionList = new QListWidget(central);
    functionList->setMinimumWidth(160);
    functionList->setSelectionMode(QAbstractItemView::SingleSelection);
    functionList->setSpacing(4);

    auto* infoItem    = new QListWidgetItem("Device info",    functionList);
    auto* controlItem = new QListWidgetItem("Device control", functionList);
    auto* fftItem     = new QListWidgetItem("FFT",            functionList);
    auto* txNavItem   = new QListWidgetItem("Transmit",       functionList);
    infoItem->setSizeHint(QSize(0, 48));
    controlItem->setSizeHint(QSize(0, 48));
    fftItem->setSizeHint(QSize(0, 48));
    txNavItem->setSizeHint(QSize(0, 48));

    contentStack = new QStackedWidget(central);
    contentStack->addWidget(new QWidget(contentStack));

    deviceInfoPage    = createDeviceInfoPage();
    deviceControlPage = createDeviceControlPage();
    deviceFFTpage     = createDeviceFFTpage();   // uses channelPanels_
    txPage_           = createTxPage();
    contentStack->addWidget(deviceInfoPage);
    contentStack->addWidget(deviceControlPage);
    contentStack->addWidget(deviceFFTpage);
    contentStack->addWidget(txPage_);

    connect(functionList, &QListWidget::itemClicked, this,
        [this, infoItem, controlItem, fftItem, txNavItem](QListWidgetItem* item) {
            if      (item == infoItem)    contentStack->setCurrentWidget(deviceInfoPage);
            else if (item == controlItem) contentStack->setCurrentWidget(deviceControlPage);
            else if (item == fftItem)     contentStack->setCurrentWidget(deviceFFTpage);
            else if (item == txNavItem)   contentStack->setCurrentWidget(txPage_);
        });

    functionList->clearSelection();
    contentStack->setCurrentIndex(0);

    splitter->addWidget(functionList);
    splitter->addWidget(contentStack);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter);
    setCentralWidget(central);

    // ── TX controller ─────────────────────────────────────────────────────────
    txCtrl_ = new TxController(this->device.get(),
                               ChannelDescriptor{ChannelDescriptor::TX, 0}, this);
    connect(txCtrl_, &TxController::txStatus, this,
            [this](const QString& msg) {
                if (txStatusLabel_) txStatusLabel_->setText(msg);
            }, Qt::QueuedConnection);
    connect(txCtrl_, &TxController::txError, this,
            [this](const QString& err) {
                if (txStatusLabel_) {
                    txStatusLabel_->setStyleSheet("color: #ff4444;");
                    txStatusLabel_->setText("Error: " + err);
                }
                if (txStartButton_) txStartButton_->setEnabled(true);
                if (txStopButton_)  txStopButton_->setEnabled(false);
                QMessageBox::critical(this, "TX error", err);
            }, Qt::QueuedConnection);
    connect(txCtrl_, &TxController::txFinished, this,
            [this]() {
                if (txStatusLabel_) {
                    txStatusLabel_->setStyleSheet("color: gray;");
                    txStatusLabel_->setText("Idle");
                }
                if (txStartButton_) txStartButton_->setEnabled(true);
                if (txStopButton_)  txStopButton_->setEnabled(false);
            }, Qt::QueuedConnection);

    // ── Plot render timer: max 20 fps, delegates to each ChannelPanel ─────────
    plotTimer_ = new QTimer(this);
    plotTimer_->setInterval(50);
    plotTimer_->setTimerType(Qt::CoarseTimer);
    connect(plotTimer_, &QTimer::timeout, this, [this]() {
        for (auto* p : channelPanels_) p->replotIfDirty();
    });
    plotTimer_->start();

    // ── Connection watchdog ───────────────────────────────────────────────────
    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    connect(connectionTimer, &QTimer::timeout, this, &DeviceDetailWindow::checkDeviceConnection);
    connect(&connectionWatcher,
            &QFutureWatcher<QList<std::shared_ptr<IDevice>>>::finished,
            this, &DeviceDetailWindow::handleConnectionCheckFinished);
    connectionTimer->start();
    checkDeviceConnection();
}

DeviceDetailWindow::~DeviceDetailWindow() {
    // closeEvent() handles the full teardown sequence (streams + device->close()).
    // This guard prevents a crash if the window is deleted without being closed first.
    connectionTimer->stop();
    connectionWatcher.waitForFinished();
}

void DeviceDetailWindow::closeEvent(QCloseEvent* event) {
    // ── 1. Stop timers — prevent new watchdog/plot/metrics calls during teardown ──
    connectionTimer->stop();
    connectionWatcher.waitForFinished();
    if (plotTimer_)    plotTimer_->stop();
    if (metricsTimer_) metricsTimer_->stop();

    // ── 2. Synchronously stop all RX streams ─────────────────────────────────────
    // shutdown() calls teardownStream() which blocks until the worker thread exits
    // (up to 3 s). Must happen before device->close() / LMS_Close().
    for (auto* p : channelPanels_)
        if (auto* ctrl = p->appController())
            ctrl->shutdown();

    // ── 3. Synchronously stop TX ─────────────────────────────────────────────────
    if (txCtrl_) txCtrl_->shutdown();

    // ── 4. Close hardware — tears down LMS handle, resets state to Connected ─────
    // After this call the device is no longer initialised; reopening the same device
    // from DeviceSelectionWindow will show a fresh "not initialized" UI.
    device->close();

    QMainWindow::closeEvent(event);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pages
// ═══════════════════════════════════════════════════════════════════════════════
QWidget* DeviceDetailWindow::createDeviceInfoPage() {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("Device info", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    auto* serialLabel = new QLabel(
        QString("Serial: %1").arg(device->id()), page);
    auto* detailLabel = new QLabel(
        QString("Info: %1").arg(device->name()), page);
    detailLabel->setWordWrap(true);

    currentSampleRateLabel = new QLabel(page);
    refreshCurrentSampleRate();

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(serialLabel);
    layout->addWidget(detailLabel);
    layout->addWidget(currentSampleRateLabel);
    layout->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
QWidget* DeviceDetailWindow::createDeviceControlPage() {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("Device control", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    const bool ready   = controller_->isInitialized();
    initStatusLabel    = new QLabel(ready ? "Device initialized \u2713" : "Device not initialized", page);
    if (ready) initStatusLabel->setStyleSheet("color: #00cc44;");
    controlStatusLabel = new QLabel(page);
    controlStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

    sampleRateSelector = new QComboBox(page);
    for (double rate : device->supportedSampleRates())
        sampleRateSelector->addItem(QString("%1 Hz").arg(rate, 0, 'f', 0), rate);

    auto* initButton = new QPushButton("Initialize device", page);
    calibrateButton  = new QPushButton("Calibrate",         page);
    sampleRateSelector->setEnabled(ready);
    calibrateButton->setEnabled(ready);

    connect(initButton, &QPushButton::clicked,
            controller_, &DeviceController::initDevice);

    connect(calibrateButton, &QPushButton::clicked, this, [this]() {
        stopAllStreams();
        controller_->calibrate(sampleRateSelector->currentData().toDouble());
    });

    connect(sampleRateSelector, &QComboBox::currentIndexChanged, this, [this]() {
        if (controller_->isInitialized()) {
            stopAllStreams();
            controller_->setSampleRate(sampleRateSelector->currentData().toDouble());
        }
    });

    // Single gain slider — LimeSuite distributes across LNA+PGA automatically.
    // TIA is fixed at max (12 dB) in LimeDevice::init() via LMS_WriteParam.
    auto* gainRow  = new QWidget(page);
    auto* gainHlay = new QHBoxLayout(gainRow);
    gainHlay->setContentsMargins(0, 0, 0, 0);
    auto* gainLbl  = new QLabel("Gain", gainRow);
    gainLbl->setFixedWidth(36);
    gainLbl->setToolTip("Total RX gain 0–68 dB.\n"
                        "TIA is fixed at 12 dB (max); LimeSuite distributes\n"
                        "the remaining gain across LNA and PGA automatically.");
    gainSlider_ = new QSlider(Qt::Horizontal, gainRow);
    gainSlider_->setRange(0, 68);
    gainSlider_->setValue(0);
    gainSlider_->setEnabled(ready);
    gainSlider_->setToolTip(gainLbl->toolTip());
    gainValueLabel_ = new QLabel("0 dB", gainRow);
    gainValueLabel_->setFixedWidth(52);
    gainHlay->addWidget(gainLbl);
    gainHlay->addWidget(gainSlider_);
    gainHlay->addWidget(gainValueLabel_);

    connect(gainSlider_, &QSlider::valueChanged, gainValueLabel_, [this](int v) {
        gainValueLabel_->setText(QString("%1 dB").arg(v));
    });
    connect(gainSlider_, &QSlider::sliderReleased, this, [this]() {
        controller_->setGain(gainSlider_->value());
    });

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(initStatusLabel);
    layout->addWidget(controlStatusLabel);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Sample rate", page));
    layout->addWidget(sampleRateSelector);
    layout->addSpacing(12);
    auto* gainLabel = new QLabel("Gain", page);
    auto* gainHint  = new QLabel("Recommended for FM: 30–50 dB", page);
    gainHint->setStyleSheet("color: gray; font-size: 10px;");
    layout->addWidget(gainLabel);
    layout->addWidget(gainHint);
    layout->addWidget(gainRow);
    layout->addSpacing(12);
    layout->addWidget(initButton);
    layout->addWidget(calibrateButton);
    layout->addStretch();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
QWidget* DeviceDetailWindow::createDeviceFFTpage() {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("FFT", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    // ── Per-channel panels in a scroll area ───────────────────────────────────
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* panelsWidget = new QWidget(scroll);
    auto* panelsLayout = new QVBoxLayout(panelsWidget);
    panelsLayout->setContentsMargins(0, 0, 0, 0);
    panelsLayout->setSpacing(8);
    for (auto* panel : channelPanels_)
        panelsLayout->addWidget(panel);
    panelsLayout->addStretch();
    scroll->setWidget(panelsWidget);

    // ── Stream start / stop ───────────────────────────────────────────────────
    auto* btnRow  = new QWidget(page);
    auto* btnHlay = new QHBoxLayout(btnRow);
    btnHlay->setContentsMargins(0, 0, 0, 0);

    streamStartButton = new QPushButton("\u25B6  Start", btnRow);
    streamStopButton  = new QPushButton("\u25A0  Stop",  btnRow);
    streamStopButton->setEnabled(false);
    streamStartButton->setEnabled(controller_->isInitialized());

    connect(streamStartButton, &QPushButton::clicked, this, &DeviceDetailWindow::startStream);
    connect(streamStopButton,  &QPushButton::clicked, this, &DeviceDetailWindow::stopStream);

    btnHlay->addWidget(streamStartButton);
    btnHlay->addWidget(streamStopButton);
    btnHlay->addStretch();

    streamStatusLabel = new QLabel("Idle", page);
    streamStatusLabel->setStyleSheet("color: gray;");

    layout->addWidget(title);
    layout->addSpacing(4);
    layout->addWidget(scroll, 1);
    layout->addWidget(btnRow);
    layout->addWidget(streamStatusLabel);
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
QWidget* DeviceDetailWindow::createTxPage() {
    auto* page   = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel("Transmit", page);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");

    auto* warnLabel = new QLabel(
        "\u26A0  Connect an attenuator before transmitting. Direct antenna output "
        "may violate local regulations.", page);
    warnLabel->setWordWrap(true);
    warnLabel->setStyleSheet("color: #ffaa00; font-size: 11px;");

    // ── TX frequency ─────────────────────────────────────────────────────────
    auto* freqRow  = new QWidget(page);
    auto* freqHlay = new QHBoxLayout(freqRow);
    freqHlay->setContentsMargins(0, 0, 0, 0);

    auto* freqLabel = new QLabel("TX freq (MHz):", freqRow);
    freqLabel->setFixedWidth(100);
    txFreqSpin_ = new QDoubleSpinBox(freqRow);
    txFreqSpin_->setRange(kFreqMinMHz, kFreqMaxMHz);
    txFreqSpin_->setDecimals(3);
    txFreqSpin_->setSingleStep(0.1);
    txFreqSpin_->setValue(kFreqDefaultMHz);
    txFreqSpin_->setFixedWidth(110);

    freqHlay->addWidget(freqLabel);
    freqHlay->addWidget(txFreqSpin_);
    freqHlay->addStretch();

    // ── TX gain ───────────────────────────────────────────────────────────────
    auto* gainRow  = new QWidget(page);
    auto* gainHlay = new QHBoxLayout(gainRow);
    gainHlay->setContentsMargins(0, 0, 0, 0);

    auto* gainLabel = new QLabel("TX gain (dB):", gainRow);
    gainLabel->setFixedWidth(100);
    txGainSlider_ = new QSlider(Qt::Horizontal, gainRow);
    txGainSlider_->setRange(0, 52);
    txGainSlider_->setValue(0);
    txGainSlider_->setToolTip("TX output power: 0–52 dB (PAD + PGA).\n"
                              "Start at 0 dB and increase gradually.");
    txGainLabel_ = new QLabel("0 dB", gainRow);
    txGainLabel_->setFixedWidth(52);

    connect(txGainSlider_, &QSlider::valueChanged, txGainLabel_,
            [this](int v) { txGainLabel_->setText(QString("%1 dB").arg(v)); });

    gainHlay->addWidget(gainLabel);
    gainHlay->addWidget(txGainSlider_, 1);
    gainHlay->addWidget(txGainLabel_);

    // ── Tone source ───────────────────────────────────────────────────────────
    auto* toneRow  = new QWidget(page);
    auto* toneHlay = new QHBoxLayout(toneRow);
    toneHlay->setContentsMargins(0, 0, 0, 0);

    auto* toneOffsetLabel = new QLabel("Tone offset (kHz):", toneRow);
    toneOffsetLabel->setFixedWidth(130);
    toneOffsetLabel->setToolTip("Offset of the CW tone from the TX LO frequency.\n"
                                "0 = transmit exactly at LO.");
    txToneOffsetSpin_ = new QDoubleSpinBox(toneRow);
    txToneOffsetSpin_->setRange(-10000.0, 10000.0);
    txToneOffsetSpin_->setDecimals(1);
    txToneOffsetSpin_->setSingleStep(10.0);
    txToneOffsetSpin_->setValue(0.0);
    txToneOffsetSpin_->setFixedWidth(100);
    txToneOffsetSpin_->setToolTip(toneOffsetLabel->toolTip());

    auto* amplLabel = new QLabel("Amplitude:", toneRow);
    amplLabel->setToolTip("I/Q sample scale factor 0.0–1.0.\n"
                          "0.3 is a safe default to avoid clipping.");
    txAmplitudeSpin_ = new QDoubleSpinBox(toneRow);
    txAmplitudeSpin_->setRange(0.0, 1.0);
    txAmplitudeSpin_->setDecimals(2);
    txAmplitudeSpin_->setSingleStep(0.05);
    txAmplitudeSpin_->setValue(0.3);
    txAmplitudeSpin_->setFixedWidth(70);
    txAmplitudeSpin_->setToolTip(amplLabel->toolTip());

    toneHlay->addWidget(toneOffsetLabel);
    toneHlay->addWidget(txToneOffsetSpin_);
    toneHlay->addSpacing(12);
    toneHlay->addWidget(amplLabel);
    toneHlay->addWidget(txAmplitudeSpin_);
    toneHlay->addStretch();

    // ── TX buttons ────────────────────────────────────────────────────────────
    auto* btnRow  = new QWidget(page);
    auto* btnHlay = new QHBoxLayout(btnRow);
    btnHlay->setContentsMargins(0, 0, 0, 0);

    txStartButton_ = new QPushButton("\u25B6  Start TX", btnRow);
    txStopButton_  = new QPushButton("\u25A0  Stop TX",  btnRow);
    txStopButton_->setEnabled(false);
    txStartButton_->setEnabled(controller_->isInitialized());

    connect(txStartButton_, &QPushButton::clicked, this, [this]() {
        TxController::TxConfig cfg;
        cfg.freqMHz      = txFreqSpin_->value();
        cfg.gainDb       = txGainSlider_->value();
        cfg.toneOffsetHz = txToneOffsetSpin_->value() * 1000.0;
        cfg.amplitude    = static_cast<float>(txAmplitudeSpin_->value());

        txCtrl_->startTx(cfg);

        txStartButton_->setEnabled(false);
        txStopButton_->setEnabled(true);
        txStatusLabel_->setStyleSheet("color: #00cc44;");
        txStatusLabel_->setText("Transmitting\u2026");
    });

    connect(txStopButton_, &QPushButton::clicked, this, [this]() {
        txCtrl_->stopTx();
        txStopButton_->setEnabled(false);
    });

    btnHlay->addWidget(txStartButton_);
    btnHlay->addWidget(txStopButton_);
    btnHlay->addStretch();

    txStatusLabel_ = new QLabel("Idle", page);
    txStatusLabel_->setStyleSheet("color: gray;");

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(warnLabel);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Frequency", page));
    layout->addWidget(freqRow);
    layout->addSpacing(8);
    layout->addWidget(new QLabel("Gain", page));
    layout->addWidget(gainRow);
    layout->addSpacing(8);
    layout->addWidget(new QLabel("Source: CW tone", page));
    layout->addWidget(toneRow);
    layout->addSpacing(12);
    layout->addWidget(btnRow);
    layout->addWidget(txStatusLabel_);
    layout->addStretch();
    return page;
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceController reaction slots
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onDeviceInitialized() {
    initStatusLabel->setText("Device initialized \u2713");
    initStatusLabel->setStyleSheet("color: #00cc44;");

    sampleRateSelector->setEnabled(true);
    calibrateButton->setEnabled(true);
    gainSlider_->setEnabled(true);
    streamStartButton->setEnabled(true);
    if (txStartButton_) txStartButton_->setEnabled(true);

    refreshCurrentSampleRate();
    QMessageBox::information(this, "Initialization", "Device initialized successfully.");
}

void DeviceDetailWindow::onSampleRateChanged(double hz) {
    if (currentSampleRateLabel)
        currentSampleRateLabel->setText(
            QString("Current sample rate: %1 Hz").arg(hz, 0, 'f', 0));
    if (sampleRateSelector) {
        QSignalBlocker blocker(sampleRateSelector);
        for (int i = 0; i < sampleRateSelector->count(); ++i) {
            if (std::abs(sampleRateSelector->itemData(i).toDouble() - hz) < 1.0) {
                sampleRateSelector->setCurrentIndex(i);
                break;
            }
        }
    }
}

void DeviceDetailWindow::onControllerStatus(const QString& message) {
    if (controlStatusLabel)
        controlStatusLabel->setText(message);
}

void DeviceDetailWindow::onControllerError(const QString& message) {
    if (controlStatusLabel) {
        controlStatusLabel->setStyleSheet("color: #ff4444; font-size: 11px;");
        controlStatusLabel->setText(message);
    }
    QMessageBox::critical(this, "Device error", message);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stream control
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::startStream() {
    // Don't restart if any channel is already streaming
    if (std::ranges::any_of(channelPanels_,
            [](auto* p) { return p->appController()->isStreaming(); })) return;

    // Pre-prepare streams from UI thread (LimeSuite quirk):
    // LMS_SetupStream stops ALL device streams — must be called from UI thread
    // before any workers start, so workers skip setup and just call LMS_StartStream.
    for (auto* p : channelPanels_)
        device->prepareStream(p->channel());

    // Start each channel
    for (auto* p : channelPanels_) {
        p->appController()->startStream(p->buildStreamConfig());
        p->onStreamStarted();
    }

    // Forward first channel's stream status to the status label
    if (!channelPanels_.isEmpty())
        connect(channelPanels_.first()->appController(), &RxController::streamStatus,
                streamStatusLabel, &QLabel::setText, Qt::QueuedConnection);

    // LMS_GetDeviceList (called by watchdog) causes USB interference during streaming.
    connectionTimer->stop();

    // Lazily create metrics timer; connect to each panel's updateMetrics()
    if (!metricsTimer_) {
        metricsTimer_ = new QTimer(this);
        connect(metricsTimer_, &QTimer::timeout, this, [this]() {
            for (auto* p : channelPanels_) p->updateMetrics();
        });
    }
    metricsTimer_->start(500);

    calibrateButton->setEnabled(false);
    streamStartButton->setEnabled(false);
    streamStopButton->setEnabled(true);
    streamStatusLabel->setStyleSheet("color: #00cc44;");
    streamStatusLabel->setText("Streaming\u2026");
}

void DeviceDetailWindow::stopStream() {
    for (auto* p : channelPanels_)
        if (p->appController()->isStreaming())
            p->appController()->stopStream();
    streamStopButton->setEnabled(false);
}

void DeviceDetailWindow::onStreamError(const QString& error) {
    streamStatusLabel->setStyleSheet("color: #ff4444;");
    streamStatusLabel->setText("Error: " + error);
    QMessageBox::warning(this, "Stream error", error);
}

void DeviceDetailWindow::onStreamFinished() {
    // Called from each channel's RxController via QueuedConnection.
    // Allow restart only when ALL channels have finished.
    if (std::ranges::any_of(channelPanels_,
            [](auto* p) { return p->appController()->isStreaming(); })) return;

    if (metricsTimer_) metricsTimer_->stop();

    // Disconnect streamStatus so stale connection doesn't persist across streams
    if (!channelPanels_.isEmpty())
        disconnect(channelPanels_.first()->appController(),
                   &RxController::streamStatus, streamStatusLabel, nullptr);

    for (auto* p : channelPanels_) p->onStreamStopped();

    calibrateButton->setEnabled(controller_->isInitialized());
    streamStartButton->setEnabled(controller_->isInitialized());
    streamStopButton->setEnabled(false);
    streamStatusLabel->setStyleSheet("color: gray;");
    streamStatusLabel->setText("Idle");

    // Resume connection watchdog now that streaming has stopped.
    connectionTimer->start();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::refreshCurrentSampleRate() const {
    if (!currentSampleRateLabel) return;
    if (device->state() < DeviceState::Ready) {
        currentSampleRateLabel->setText("Current sample rate: device not initialized");
        return;
    }
    try {
        const double sr = device->sampleRate();
        currentSampleRateLabel->setText(
            QString("Current sample rate: %1 Hz").arg(sr, 0, 'f', 0));
        if (sampleRateSelector) {
            QSignalBlocker blocker(sampleRateSelector);
            for (int i = 0; i < sampleRateSelector->count(); ++i) {
                if (std::abs(sampleRateSelector->itemData(i).toDouble() - sr) < 1.0) {
                    sampleRateSelector->setCurrentIndex(i);
                    break;
                }
            }
        }
    } catch (const std::exception& ex) {
        currentSampleRateLabel->setText(QString("Sample rate: %1").arg(ex.what()));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Connection watchdog
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::checkDeviceConnection() {
    if (connectionWatcher.isRunning()) return;
    connectionWatcher.setFuture(QtConcurrent::run([this]() {
        manager.refresh();
        return manager.devices();
    }));
}

void DeviceDetailWindow::handleConnectionCheckFinished() {
    const auto devices   = connectionWatcher.result();
    const bool connected = std::ranges::any_of(devices,
        [&](const std::shared_ptr<IDevice>& d) {
            return d->id() == device->id();
        });
    if (!connected) {
        connectionTimer->stop();
        stopAllStreams();
        // Defer the signal so we fully unwind the QFutureWatcher callback
        // before the deviceDisconnected handler opens a QMessageBox (which
        // would re-enter the event loop while still on the watcher's stack).
        QTimer::singleShot(0, this, [this]() { emit deviceDisconnected(); });
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// RXTX coordination helper
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::stopAllStreams() {
    for (auto* p : channelPanels_)
        if (p->appController()->isStreaming())
            p->appController()->stopStream();
    if (txCtrl_->isTransmitting()) txCtrl_->stopTx();
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceSelectionWindow
// ═══════════════════════════════════════════════════════════════════════════════
DeviceSelectionWindow::DeviceSelectionWindow(IDeviceManager& manager,
                                             SessionManager& sessions,
                                             QWidget* parent)
    : QWidget(parent)
    , manager(manager)
    , sessions_(sessions)
{
    setWindowTitle("Stand — SDR");
    setMinimumWidth(320);
    auto* layout = new QVBoxLayout(this);
    statusLabel  = new QLabel("Searching for SDR devices...", this);
    deviceList   = new QListWidget(this);
    layout->addWidget(statusLabel);
    layout->addWidget(deviceList);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(1000);
    connect(refreshTimer, &QTimer::timeout, this, &DeviceSelectionWindow::refreshDevices);

    connect(&refreshWatcher,
            &QFutureWatcher<QList<std::shared_ptr<IDevice>>>::finished,
            this, [this]() {
        lastDevices_ = refreshWatcher.result();
        updateDeviceButtons();
    });

    // When a window is opened/closed, refresh the button states immediately.
    connect(&sessions_, &SessionManager::sessionChanged,
            this, &DeviceSelectionWindow::updateDeviceButtons);

    refreshDevices();
    refreshTimer->start();
}

void DeviceSelectionWindow::refreshDevices() {
    if (refreshWatcher.isRunning()) return;
    refreshWatcher.setFuture(QtConcurrent::run([this]() {
        manager.refresh();
        return manager.devices();
    }));
}

void DeviceSelectionWindow::updateDeviceButtons() {
    deviceList->clear();
    if (lastDevices_.empty()) {
        statusLabel->setText("No devices found. Waiting for connection\u2026");
        return;
    }
    statusLabel->setText("Select a device to open its window.");
    for (const auto& dev : lastDevices_) {
        const bool inUse = sessions_.isInUse(dev->id());
        auto* item = new QListWidgetItem(deviceList);
        item->setSizeHint(QSize(0, 40));
        const QString label = inUse ? dev->name() + "  [In use]" : dev->name();
        auto* btn = new QPushButton(label, deviceList);
        btn->setEnabled(!inUse);
        connect(btn, &QPushButton::clicked, this, [this, dev]() { openDevice(dev); });
        deviceList->setItemWidget(item, btn);
    }
}

void DeviceSelectionWindow::openDevice(const std::shared_ptr<IDevice>& dev) {
    if (sessions_.isInUse(dev->id())) return;
    sessions_.markInUse(dev->id());

    auto* window = new DeviceDetailWindow(dev, manager);
    window->setAttribute(Qt::WA_DeleteOnClose);

    connect(window, &DeviceDetailWindow::deviceDisconnected, window, [window]() {
        QMessageBox::warning(window, "Device disconnected",
            "Connection to the device was lost. The window will close.");
        window->close();
    });
    // Release the session slot whenever the window is destroyed
    // (covers both normal X-close and disconnect-triggered close).
    connect(window, &QObject::destroyed, this, [this, devId = dev->id()]() {
        sessions_.release(devId);
    });

    window->resize(1600, 750);
    window->show();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Application
// ═══════════════════════════════════════════════════════════════════════════════
Application::Application(int& argc, char** argv, IDeviceManager& manager)
    : qtApp(argc, argv)
    , selectionWindow(manager, sessionManager_)
{
    QApplication::setWindowIcon(QIcon(":/assets/icon.jpg"));
}

int Application::run() {
    selectionWindow.resize(400, 300);
    selectionWindow.show();
    return QApplication::exec();
}
