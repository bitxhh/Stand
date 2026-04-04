#include "Application.h"

#include <algorithm>
#include <cmath>
#include <QCoreApplication>
#include <QSize>
#include <QFileDialog>
#include <QStandardPaths>
#include "qcustomplot.h"
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
    deviceFFTpage     = createDeviceFFTpage();
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

    // ── App controllers (non-UI: pipeline, handlers, audio, worker thread) ───
    ctrl_ = new AppController(this->device.get(), ChannelDescriptor{}, this);

    connect(ctrl_, &AppController::fftReady,
            this,  &DeviceDetailWindow::onFftReady, Qt::QueuedConnection);
    connect(ctrl_, &AppController::streamError,
            this,  &DeviceDetailWindow::onStreamError, Qt::QueuedConnection);
    connect(ctrl_, &AppController::streamFinished,
            this,  &DeviceDetailWindow::onStreamFinished, Qt::QueuedConnection);

    // RX1 controller — idle until dual RX is enabled
    ctrl1_ = new AppController(this->device.get(),
                               ChannelDescriptor{ChannelDescriptor::RX, 1}, this);
    connect(ctrl1_, &AppController::fftReady,
            this,   &DeviceDetailWindow::onFftReady1, Qt::QueuedConnection);
    connect(ctrl1_, &AppController::streamError,
            this,   &DeviceDetailWindow::onStreamError, Qt::QueuedConnection);

    // TX controller
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

    // Classifier controller
    classifierCtrl_ = new ClassifierController(ctrl_, this);
    connect(classifierCtrl_, &ClassifierController::classifierStarted, this,
            [this]() {
                if (classifierLabel_) {
                    classifierLabel_->setStyleSheet("color: #00cc44; font-size: 11px;");
                    classifierLabel_->setText("Ready");
                }
            });
    connect(classifierCtrl_, &ClassifierController::classifierStopped, this,
            [this]() {
                if (classifierLabel_) {
                    classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");
                    classifierLabel_->setText("Unavailable");
                }
                if (classifierCheck_) {
                    QSignalBlocker b(classifierCheck_);
                    classifierCheck_->setChecked(false);
                }
            });
    connect(classifierCtrl_, &ClassifierController::classifierError, this,
            [this](const QString& msg) {
                if (classifierLabel_) {
                    classifierLabel_->setStyleSheet("color: #ffaa00; font-size: 11px;");
                    classifierLabel_->setText(msg);
                }
            });
    connect(classifierCtrl_, &ClassifierController::classificationReady, this,
            [this](const QString& type, double confidence) {
                if (classifierLabel_) {
                    classifierLabel_->setStyleSheet("color: #00cc44; font-size: 11px;");
                    classifierLabel_->setText(
                        QString("%1  %2%").arg(type)
                                         .arg(static_cast<int>(confidence * 100)));
                }
            });

    connect(ctrl_, &AppController::demodStatus, this,
            [this](const QString& msg, bool isError) {
                demodStatusLabel_->setStyleSheet(
                    isError ? "color: #ff4444; font-size: 11px;"
                            : "color: #00cc44; font-size: 11px;");
                demodStatusLabel_->setText(msg);
                if (isError)
                    QMessageBox::critical(this, "Audio", msg);
            });

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
    // Stop the watchdog before teardown — the QFutureWatcher captures `this`
    // in its lambda; if the future is still in-flight when we start destroying
    // members, it would access freed memory via the this pointer.
    connectionTimer->stop();
    connectionWatcher.waitForFinished();   // blocks until any pending scan completes
    // AppController destructor handles stream teardown (it's a child of this)
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

    initStatusLabel    = new QLabel("Device not initialized", page);
    controlStatusLabel = new QLabel(page);
    controlStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

    sampleRateSelector = new QComboBox(page);
    for (double rate : device->supportedSampleRates())
        sampleRateSelector->addItem(QString("%1 Hz").arg(rate, 0, 'f', 0), rate);

    auto* initButton = new QPushButton("Initialize device", page);
    calibrateButton  = new QPushButton("Calibrate",         page);

    const bool ready = controller_->isInitialized();
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

    // ── Spectrum plot ─────────────────────────────────────────────────────────
    fftPlot = new QCustomPlot(page);
    fftPlot->setMinimumHeight(200);
    setupFftPlot();

    // ── Dual RX toggle + RX1 frequency ───────────────────────────────────────
    auto* dualRxRow  = new QWidget(page);
    auto* dualRxHlay = new QHBoxLayout(dualRxRow);
    dualRxHlay->setContentsMargins(0, 0, 0, 0);

    dualRxCheck_ = new QCheckBox("Dual RX", dualRxRow);
    dualRxCheck_->setToolTip("Enable second RX channel (RX1) with independent tuning");

    auto* freq1Label = new QLabel("RX1 (MHz):", dualRxRow);
    freqSpinBox1_ = new QDoubleSpinBox(dualRxRow);
    freqSpinBox1_->setRange(kFreqMinMHz, kFreqMaxMHz);
    freqSpinBox1_->setDecimals(3);
    freqSpinBox1_->setSingleStep(0.1);
    freqSpinBox1_->setValue(kFreqDefaultMHz);
    freqSpinBox1_->setFixedWidth(110);
    auto* applyFreq1Btn = new QPushButton("Apply", dualRxRow);
    applyFreq1Btn->setFixedWidth(60);

    freq1Label->hide(); freqSpinBox1_->hide(); applyFreq1Btn->hide();
    dualRxHlay->addWidget(dualRxCheck_);
    dualRxHlay->addSpacing(8);
    dualRxHlay->addWidget(freq1Label);
    dualRxHlay->addWidget(freqSpinBox1_);
    dualRxHlay->addWidget(applyFreq1Btn);
    dualRxHlay->addStretch();

    connect(dualRxCheck_, &QCheckBox::toggled, this,
            [this, freq1Label, applyFreq1Btn](bool on) {
        freq1Label->setVisible(on);
        freqSpinBox1_->setVisible(on);
        applyFreq1Btn->setVisible(on);
        onDualRxToggled(on);
    });

    auto applyFreq1 = [this]() {
        if (!controller_->isInitialized()) return;
        const double mhz = freqSpinBox1_->value();
        controller_->setFrequencyChannel({ChannelDescriptor::RX, 1}, mhz);
        ctrl1_->setFftCenterFreq(mhz);
        if (centerLine1_) {
            centerLine1_->start->setCoords(mhz, -130.0);
            centerLine1_->end->setCoords  (mhz,   10.0);
            fftPlot1_->replot(QCustomPlot::rpQueuedReplot);
        }
    };
    connect(applyFreq1Btn, &QPushButton::clicked,            this, applyFreq1);
    connect(freqSpinBox1_, &QDoubleSpinBox::editingFinished, this, applyFreq1);

    // ── RX1 spectrum plot (hidden until dual RX enabled) ─────────────────────
    fftPlot1_ = new QCustomPlot(page);
    fftPlot1_->setMinimumHeight(180);
    setupFftPlot1();
    fftPlot1_->hide();

    // ── Frequency control ─────────────────────────────────────────────────────
    auto* freqRow  = new QWidget(page);
    auto* freqHlay = new QHBoxLayout(freqRow);
    freqHlay->setContentsMargins(0, 0, 0, 0);

    auto* freqLabel = new QLabel("Center freq (MHz):", freqRow);

    freqSpinBox = new QDoubleSpinBox(freqRow);
    freqSpinBox->setRange(kFreqMinMHz, kFreqMaxMHz);
    freqSpinBox->setDecimals(3);
    freqSpinBox->setSingleStep(0.1);
    freqSpinBox->setValue(kFreqDefaultMHz);
    freqSpinBox->setFixedWidth(110);

    freqSlider = new QSlider(Qt::Horizontal, freqRow);
    freqSlider->setRange(static_cast<int>(kFreqMinMHz), static_cast<int>(kFreqMaxMHz));
    freqSlider->setValue(static_cast<int>(kFreqDefaultMHz));

    auto* applyFreqBtn = new QPushButton("Apply", freqRow);
    applyFreqBtn->setFixedWidth(60);

    freqHlay->addWidget(freqLabel);
    freqHlay->addWidget(freqSpinBox);
    freqHlay->addWidget(freqSlider, 1);
    freqHlay->addWidget(applyFreqBtn);

    connect(freqSlider,  &QSlider::valueChanged,        this, &DeviceDetailWindow::onFreqSliderChanged);
    connect(freqSpinBox, &QDoubleSpinBox::valueChanged, this, &DeviceDetailWindow::onFreqSpinChanged);

    auto applyFreq = [this]() {
        if (!controller_->isInitialized()) return;
        const double mhz = freqSpinBox->value();
        controller_->setFrequency(mhz);
        ctrl_->setFftCenterFreq(mhz);
        if (centerLine_) {
            centerLine_->start->setCoords(mhz, -130.0);
            centerLine_->end->setCoords  (mhz,   10.0);
            fftPlot->replot(QCustomPlot::rpQueuedReplot);
        }
    };
    connect(freqSlider,   &QSlider::sliderReleased,        this, applyFreq);
    connect(freqSpinBox,  &QDoubleSpinBox::editingFinished, this, applyFreq);
    connect(applyFreqBtn, &QPushButton::clicked,            this, applyFreq);

    // ── Recording controls (compact) ─────────────────────────────────────────
    // Default paths
    recordPath_ = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                  + "/capture_iq_i16.raw";
    wavPath_    = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
                  + "/station_iq.wav";

    auto* recordRow  = new QWidget(page);
    auto* recordHlay = new QHBoxLayout(recordRow);
    recordHlay->setContentsMargins(0, 0, 0, 0);

    recordCheckBox = new QCheckBox("Record raw", recordRow);
    recordCheckBox->setToolTip("Record raw I/Q int16 samples to .raw file");

    wavCheckBox = new QCheckBox("Export WAV", recordRow);
    wavCheckBox->setToolTip("Export filtered I/Q to .wav file");

    auto* recSettingsBtn = new QPushButton("Settings\u2026", recordRow);
    recSettingsBtn->setFixedWidth(80);
    connect(recSettingsBtn, &QPushButton::clicked, this, &DeviceDetailWindow::openRecordSettings);

    recordHlay->addWidget(recordCheckBox);
    recordHlay->addSpacing(12);
    recordHlay->addWidget(wavCheckBox);
    recordHlay->addSpacing(12);
    recordHlay->addWidget(recSettingsBtn);
    recordHlay->addStretch();

    // ── Demodulator mode selector ───────────────────────────────────────────
    auto* demodRow  = new QWidget(page);
    auto* demodHlay = new QHBoxLayout(demodRow);
    demodHlay->setContentsMargins(0, 0, 0, 0);

    auto* modeLabel = new QLabel("Mode:", demodRow);
    modeCombo_ = new QComboBox(demodRow);
    modeCombo_->addItem("Off",  0);
    modeCombo_->addItem("FM",   1);
    modeCombo_->addItem("AM",   2);
    modeCombo_->setFixedWidth(60);

    // FM-specific controls
    fmBwLabel_ = new QLabel("BW (kHz):", demodRow);
    fmBwSpin_ = new QDoubleSpinBox(demodRow);
    fmBwSpin_->setRange(50.0, 250.0);
    fmBwSpin_->setDecimals(0);
    fmBwSpin_->setSingleStep(10.0);
    fmBwSpin_->setValue(100.0);
    fmBwSpin_->setFixedWidth(70);
    fmBwSpin_->setToolTip("One-sided filter bandwidth (kHz).\n"
                          "WBFM broadcast: 100-150 kHz");

    fmDeemphLabel_ = new QLabel("De-emph:", demodRow);
    fmDeemphCombo = new QComboBox(demodRow);
    fmDeemphCombo->addItem("EU  50 µs", 50e-6);
    fmDeemphCombo->addItem("US  75 µs", 75e-6);
    fmDeemphCombo->setToolTip("Europe / Russia: 50 µs\nUSA, Canada, Japan: 75 µs");

    // AM-specific controls
    amBwLabel_ = new QLabel("BW (kHz):", demodRow);
    amBwSpin_ = new QDoubleSpinBox(demodRow);
    amBwSpin_->setRange(1.0, 20.0);
    amBwSpin_->setDecimals(1);
    amBwSpin_->setSingleStep(1.0);
    amBwSpin_->setValue(5.0);
    amBwSpin_->setFixedWidth(70);
    amBwSpin_->setToolTip("Audio bandwidth (kHz).\n"
                          "AM broadcast: 4-5 kHz\n"
                          "SSB / amateur: 2-3 kHz");

    // Shared volume control
    auto* volLabel = new QLabel("Vol:", demodRow);
    demodVolSlider_ = new QSlider(Qt::Horizontal, demodRow);
    demodVolSlider_->setRange(0, 100);
    demodVolSlider_->setValue(80);
    demodVolSlider_->setFixedWidth(80);
    demodVolLabel_ = new QLabel("80%", demodRow);
    demodVolLabel_->setFixedWidth(34);

    demodHlay->addWidget(modeLabel);
    demodHlay->addWidget(modeCombo_);
    demodHlay->addSpacing(8);
    demodHlay->addWidget(fmBwLabel_);
    demodHlay->addWidget(fmBwSpin_);
    demodHlay->addWidget(fmDeemphLabel_);
    demodHlay->addWidget(fmDeemphCombo);
    demodHlay->addWidget(amBwLabel_);
    demodHlay->addWidget(amBwSpin_);
    demodHlay->addSpacing(8);
    demodHlay->addWidget(volLabel);
    demodHlay->addWidget(demodVolSlider_);
    demodHlay->addWidget(demodVolLabel_);
    demodHlay->addStretch();

    // Initially hide all mode-specific + shared controls (mode = Off)
    fmBwLabel_->hide();    fmBwSpin_->hide();
    fmDeemphLabel_->hide(); fmDeemphCombo->hide();
    amBwLabel_->hide();    amBwSpin_->hide();
    volLabel->hide();
    demodVolSlider_->hide(); demodVolLabel_->hide();

    // Mode change → swap demodulator handler
    connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DeviceDetailWindow::onModeChanged);

    // Volume → audio sink
    connect(demodVolSlider_, &QSlider::valueChanged, this, [this](int v) {
        demodVolLabel_->setText(QString("%1%").arg(v));
        ctrl_->setVolume(static_cast<float>(v) / 100.0f);
    });

    // FM BW change
    connect(fmBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
        updateFilterBand(modeCombo_->currentIndex() != 0);
        ctrl_->setDemodParam("Bandwidth", bwKHz * 1000.0);
    });

    // AM BW change
    connect(amBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
        updateFilterBand(modeCombo_->currentIndex() != 0);
        ctrl_->setDemodParam("Bandwidth", bwKHz * 1000.0);
    });

    // When LO changes → update VFO range and filter band
    connect(freqSpinBox, &QDoubleSpinBox::valueChanged, this, [this](double loMHz) {
        if (demodVfoSpin_) {
            const double sr = device->sampleRate();
            const double halfBand = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
            demodVfoSpin_->setRange(loMHz - halfBand, loMHz + halfBand);
        }
        updateFilterBand(modeCombo_->currentIndex() != 0);
    });

    // ── VFO tuner ─────────────────────────────────────────────────────────────
    auto* vfoRow  = new QWidget(page);
    auto* vfoHlay = new QHBoxLayout(vfoRow);
    vfoHlay->setContentsMargins(0, 0, 0, 0);

    auto* vfoLabel = new QLabel("VFO:", vfoRow);
    vfoLabel->setToolTip("Tune demodulator to a station within the capture band.\n"
                         "Click anywhere on the spectrum to jump here.");

    demodVfoSpin_ = new QDoubleSpinBox(vfoRow);
    demodVfoSpin_->setRange(kFreqMinMHz, kFreqMaxMHz);
    demodVfoSpin_->setDecimals(3);
    demodVfoSpin_->setSingleStep(0.1);
    demodVfoSpin_->setValue(kFreqDefaultMHz);
    demodVfoSpin_->setFixedWidth(110);
    demodVfoSpin_->setEnabled(false);
    demodVfoSpin_->setToolTip("Station frequency (MHz). Edit or click the spectrum.");

    auto* vfoHintLabel = new QLabel("\u2190 click spectrum to tune", vfoRow);
    vfoHintLabel->setStyleSheet("color: gray; font-size: 10px;");

    vfoHlay->addWidget(vfoLabel);
    vfoHlay->addWidget(demodVfoSpin_);
    vfoHlay->addWidget(vfoHintLabel);
    vfoHlay->addStretch();

    // VFO changed → update filter band + apply offset to active handler
    connect(demodVfoSpin_, &QDoubleSpinBox::valueChanged, this, [this](double vfoMHz) {
        updateFilterBand(modeCombo_->currentIndex() != 0);
        const double offsetHz = (vfoMHz - freqSpinBox->value()) * 1e6;
        ctrl_->setDemodOffset(offsetHz);
    });

    // ── Signal level indicator ────────────────────────────────────────────────
    auto* levelRow  = new QWidget(page);
    auto* levelHlay = new QHBoxLayout(levelRow);
    levelHlay->setContentsMargins(0, 0, 0, 0);

    demodLevelLabel_ = new QLabel("\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF", levelRow);
    demodLevelLabel_->setStyleSheet("color: gray; font-size: 10px;");
    demodLevelLabel_->setToolTip("Signal level after demodulation.\n"
                                 "Gray = weak / no station\n"
                                 "Green = good signal");

    levelHlay->addWidget(demodLevelLabel_);
    levelHlay->addStretch();

    // ── Stream controls ───────────────────────────────────────────────────────
    auto* btnRow  = new QWidget(page);
    auto* btnHlay = new QHBoxLayout(btnRow);
    btnHlay->setContentsMargins(0, 0, 0, 0);

    streamStartButton = new QPushButton("▶  Start", btnRow);
    streamStopButton  = new QPushButton("■  Stop",  btnRow);
    streamStopButton->setEnabled(false);
    streamStartButton->setEnabled(controller_->isInitialized());

    connect(streamStartButton, &QPushButton::clicked, this, &DeviceDetailWindow::startStream);
    connect(streamStopButton,  &QPushButton::clicked, this, &DeviceDetailWindow::stopStream);

    btnHlay->addWidget(streamStartButton);
    btnHlay->addWidget(streamStopButton);
    btnHlay->addStretch();

    streamStatusLabel = new QLabel("Idle", page);
    streamStatusLabel->setStyleSheet("color: gray;");

    // ── Demod status label ───────────────────────────────────────────────────
    demodStatusLabel_ = new QLabel(page);
    demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");

    layout->addWidget(title);
    layout->addSpacing(4);
    layout->addWidget(fftPlot, 1);
    layout->addWidget(dualRxRow);
    layout->addWidget(fftPlot1_, 1);
    layout->addWidget(freqRow);
    layout->addWidget(recordRow);
    layout->addWidget(demodRow);
    layout->addWidget(vfoRow);
    layout->addWidget(levelRow);
    layout->addWidget(demodStatusLabel_);

    // ── Classifier row ────────────────────────────────────────────────────────
    auto* clsRow  = new QWidget(page);
    auto* clsHlay = new QHBoxLayout(clsRow);
    clsHlay->setContentsMargins(0, 0, 0, 0);

    classifierCheck_ = new QCheckBox("Classifier", clsRow);
    classifierCheck_->setToolTip(
        "Enable Python NN signal classifier.\n"
        "Requires Python + classifier_service.py.\n"
        "Path configured via STAND_PYTHON_EXE / STAND_CLASSIFIER_SCRIPT env vars\n"
        "or defaults: python3 / Python/classifier_service.py");

    classifierLabel_ = new QLabel("Unavailable", clsRow);
    classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");

    clsHlay->addWidget(classifierCheck_);
    clsHlay->addSpacing(8);
    clsHlay->addWidget(classifierLabel_);
    clsHlay->addStretch();

    connect(classifierCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            const QString pyExe    = qEnvironmentVariable("STAND_PYTHON_EXE",    "python3");
            const QString script   = qEnvironmentVariable("STAND_CLASSIFIER_SCRIPT",
                                         QCoreApplication::applicationDirPath()
                                         + "/../Python/classifier_service.py");
            classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");
            classifierLabel_->setText("Starting…");
            classifierCtrl_->start(pyExe, script);
        } else {
            classifierCtrl_->stop();
        }
    });

    layout->addWidget(clsRow);
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

    txStartButton_ = new QPushButton("▶  Start TX", btnRow);
    txStopButton_  = new QPushButton("■  Stop TX",  btnRow);
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
// FFT plot setup
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::setupFftPlot() {
    fftPlot->addGraph();
    fftPlot->graph(0)->setPen(QPen(QColor(0, 180, 255), 1.2));

    fftPlot->xAxis->setLabel("Frequency (MHz)");
    fftPlot->yAxis->setLabel("Power (dB)");
    fftPlot->yAxis->setRange(-120, 0);

    fftPlot->setBackground(QBrush(QColor(30, 30, 30)));
    fftPlot->xAxis->setBasePen(QPen(Qt::white));
    fftPlot->yAxis->setBasePen(QPen(Qt::white));
    fftPlot->xAxis->setTickPen(QPen(Qt::white));
    fftPlot->yAxis->setTickPen(QPen(Qt::white));
    fftPlot->xAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot->yAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot->xAxis->setTickLabelColor(Qt::white);
    fftPlot->yAxis->setTickLabelColor(Qt::white);
    fftPlot->xAxis->setLabelColor(Qt::white);
    fftPlot->yAxis->setLabelColor(Qt::white);
    // Zoom with scroll wheel (X axis only); no drag — drag causes "snap-back"
    // because onFftReady calls rescale() every frame.
    fftPlot->setInteractions(QCP::iRangeZoom);
    fftPlot->axisRect()->setRangeZoom(Qt::Horizontal);

    centerLine_ = new QCPItemLine(fftPlot);
    centerLine_->setPen(QPen(QColor(255, 60, 60), 1.2, Qt::DashLine));
    centerLine_->setAntialiased(false);
    centerLine_->start->setCoords(kFreqDefaultMHz, -130.0);
    centerLine_->end->setCoords  (kFreqDefaultMHz,   10.0);
    centerLine_->start->setType(QCPItemPosition::ptPlotCoords);
    centerLine_->end->setType  (QCPItemPosition::ptPlotCoords);

    // Filter band — semi-transparent green rect ±BW/2 around LO (red line).
    // Shown only when FM Radio is enabled.
    vfoBand_ = new QCPItemRect(fftPlot);
    vfoBand_->setBrush(QBrush(QColor(0, 200, 80, 40)));
    vfoBand_->setPen(QPen(QColor(0, 200, 80, 120), 1.0));
    vfoBand_->topLeft->setType(QCPItemPosition::ptPlotCoords);
    vfoBand_->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    vfoBand_->topLeft->setCoords(kFreqDefaultMHz - 0.1, 10.0);
    vfoBand_->bottomRight->setCoords(kFreqDefaultMHz + 0.1, -130.0);
    vfoBand_->setVisible(false);

    // X-axis range change (from scroll-wheel zoom):
    // — clamp so the user can't zoom out beyond the capture band
    // — track whether the user is zoomed in (skip auto-rescale in onFftReady)
    connect(fftPlot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        if (fftPlot->graph(0)->dataCount() < 2) return;
        const double lo   = fftPlot->graph(0)->data()->begin()->key;
        const double hi   = (fftPlot->graph(0)->data()->end() - 1)->key;
        const double span = hi - lo;
        if (newRange.size() > span * 1.01) {
            // Zoomed out beyond data — snap back to full band and clear zoom flag
            QSignalBlocker b(fftPlot->xAxis);
            fftPlot->xAxis->setRange(lo, hi);
            plotUserZoomed_ = false;
        } else {
            plotUserZoomed_ = true;
        }
    });

    // Y-axis: clamp to valid dBFS range (zoom in is allowed, zoom out is not)
    connect(fftPlot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        constexpr double yMin = -130.0, yMax = 10.0;
        if (newRange.lower < yMin || newRange.upper > yMax) {
            QSignalBlocker b(fftPlot->yAxis);
            fftPlot->yAxis->setRange(
                std::max(newRange.lower, yMin),
                std::min(newRange.upper, yMax));
        }
    });

    // Double-click: reset zoom to full capture band
    connect(fftPlot, &QCustomPlot::mouseDoubleClick, this, [this](QMouseEvent*) {
        plotUserZoomed_ = false;
        if (fftPlot->graph(0)->dataCount() >= 2) {
            QSignalBlocker b(fftPlot->xAxis);
            fftPlot->xAxis->setRange(
                fftPlot->graph(0)->data()->begin()->key,
                (fftPlot->graph(0)->data()->end() - 1)->key);
        }
        fftPlot->yAxis->setRange(-120.0, 0.0);
        fftPlot->replot(QCustomPlot::rpQueuedReplot);
    });

    // Click on spectrum → tune VFO to that frequency (demod must be active)
    connect(fftPlot, &QCustomPlot::mousePress, this,
            [this](QMouseEvent* event) {
        if (!modeCombo_ || modeCombo_->currentIndex() == 0) return;
        if (!demodVfoSpin_) return;
        const double clickedMHz = fftPlot->xAxis->pixelToCoord(event->pos().x());
        const double lo   = freqSpinBox->value();
        const double sr   = device->sampleRate();
        const double half = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
        const double clamped = std::clamp(clickedMHz, lo - half, lo + half);
        demodVfoSpin_->setValue(clamped);
    });
}

// ── RX1 spectrum plot setup ───────────────────────────────────────────────────
void DeviceDetailWindow::setupFftPlot1() {
    fftPlot1_->addGraph();
    fftPlot1_->graph(0)->setPen(QPen(QColor(255, 160, 0), 1.2));  // amber — distinct from RX0 blue

    fftPlot1_->xAxis->setLabel("Frequency (MHz) — RX1");
    fftPlot1_->yAxis->setLabel("Power (dB)");
    fftPlot1_->yAxis->setRange(-120, 0);

    fftPlot1_->setBackground(QBrush(QColor(30, 30, 30)));
    fftPlot1_->xAxis->setBasePen(QPen(Qt::white));
    fftPlot1_->yAxis->setBasePen(QPen(Qt::white));
    fftPlot1_->xAxis->setTickPen(QPen(Qt::white));
    fftPlot1_->yAxis->setTickPen(QPen(Qt::white));
    fftPlot1_->xAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot1_->yAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot1_->xAxis->setTickLabelColor(Qt::white);
    fftPlot1_->yAxis->setTickLabelColor(Qt::white);
    fftPlot1_->xAxis->setLabelColor(Qt::white);
    fftPlot1_->yAxis->setLabelColor(Qt::white);
    fftPlot1_->setInteractions(QCP::iRangeZoom);
    fftPlot1_->axisRect()->setRangeZoom(Qt::Horizontal);

    centerLine1_ = new QCPItemLine(fftPlot1_);
    centerLine1_->setPen(QPen(QColor(255, 60, 60), 1.2, Qt::DashLine));
    centerLine1_->setAntialiased(false);
    centerLine1_->start->setCoords(kFreqDefaultMHz, -130.0);
    centerLine1_->end->setCoords  (kFreqDefaultMHz,   10.0);
    centerLine1_->start->setType(QCPItemPosition::ptPlotCoords);
    centerLine1_->end->setType  (QCPItemPosition::ptPlotCoords);
}

void DeviceDetailWindow::onFftReady1(FftFrame frame) {
    if (!fftPlot1_ || !fftPlot1_->isVisible()) return;
    fftPlot1_->graph(0)->setData(frame.freqMHz, frame.powerDb);

    if (centerLine1_) {
        const double mhz = freqSpinBox1_ ? freqSpinBox1_->value() : kFreqDefaultMHz;
        centerLine1_->start->setCoords(mhz, -130.0);
        centerLine1_->end->setCoords  (mhz,   10.0);
    }

    if (!frame.freqMHz.isEmpty()) {
        QSignalBlocker b(fftPlot1_->xAxis);
        fftPlot1_->xAxis->setRange(frame.freqMHz.first(), frame.freqMHz.last());
    }
    fftPlot1_->replot(QCustomPlot::rpQueuedReplot);
}

void DeviceDetailWindow::onDualRxToggled(bool enabled) {
    fftPlot1_->setVisible(enabled);

    if (enabled) {
        // Set RX1 LO to spinbox value
        if (controller_->isInitialized())
            controller_->setFrequencyChannel({ChannelDescriptor::RX, 1},
                                             freqSpinBox1_->value());
        // If already streaming, start RX1 stream too
        if (ctrl_->isStreaming() && !ctrl1_->isStreaming()) {
            AppController::StreamConfig cfg;
            cfg.loFreqMHz = freqSpinBox1_->value();
            ctrl1_->startStream(cfg);
        }
    } else {
        if (ctrl1_->isStreaming())
            ctrl1_->stopStream();
    }
}

// Filter band: ±BW around VFO frequency, visible when demod is active.
void DeviceDetailWindow::updateFilterBand(bool visible) {
    if (!vfoBand_) return;
    vfoBand_->setVisible(visible);
    if (visible) {
        const double vfoMHz = demodVfoSpin_ ? demodVfoSpin_->value() : freqSpinBox->value();
        double bwMHz = 100.0 / 1000.0;   // default
        const int mode = modeCombo_ ? modeCombo_->currentIndex() : 0;
        if (mode == 1 && fmBwSpin_)       bwMHz = fmBwSpin_->value() / 1000.0;
        else if (mode == 2 && amBwSpin_)  bwMHz = amBwSpin_->value() / 1000.0;
        vfoBand_->topLeft->setCoords    (vfoMHz - bwMHz, 10.0);
        vfoBand_->bottomRight->setCoords(vfoMHz + bwMHz, -130.0);
    }
    fftPlot->replot(QCustomPlot::rpQueuedReplot);
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceController reaction slots
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onDeviceInitialized() {
    initStatusLabel->setText("Device initialized ✓");
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
// Recording settings dialog
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::openRecordSettings() {
    QDialog dlg(this);
    dlg.setWindowTitle("Recording Settings");
    auto* form = new QVBoxLayout(&dlg);

    // Raw file path
    auto* rawRow = new QHBoxLayout;
    rawRow->addWidget(new QLabel("Raw file:"));
    auto* rawEdit = new QLineEdit(recordPath_);
    rawRow->addWidget(rawEdit, 1);
    auto* rawBrowse = new QPushButton("...");
    rawBrowse->setFixedWidth(30);
    connect(rawBrowse, &QPushButton::clicked, &dlg, [&]() {
        const QString p = QFileDialog::getSaveFileName(
            &dlg, "Save IQ capture", rawEdit->text(), "Raw IQ (*.raw);;All files (*)");
        if (!p.isEmpty()) rawEdit->setText(p);
    });
    rawRow->addWidget(rawBrowse);
    form->addLayout(rawRow);

    // WAV file path
    auto* wavRow = new QHBoxLayout;
    wavRow->addWidget(new QLabel("WAV file:"));
    auto* wavEdit = new QLineEdit(wavPath_);
    wavRow->addWidget(wavEdit, 1);
    auto* wavBrowse = new QPushButton("...");
    wavBrowse->setFixedWidth(30);
    connect(wavBrowse, &QPushButton::clicked, &dlg, [&]() {
        const QString p = QFileDialog::getSaveFileName(
            &dlg, "Save filtered WAV", wavEdit->text(), "WAV (*.wav);;All files (*)");
        if (!p.isEmpty()) wavEdit->setText(p);
    });
    wavRow->addWidget(wavBrowse);
    form->addLayout(wavRow);

    // WAV offset
    auto* offsetRow = new QHBoxLayout;
    offsetRow->addWidget(new QLabel("WAV offset (Hz):"));
    auto* offsetSpin = new QDoubleSpinBox;
    offsetSpin->setRange(-15e6, 15e6);
    offsetSpin->setDecimals(0);
    offsetSpin->setSingleStep(100'000.0);
    offsetSpin->setValue(wavOffset_);
    offsetSpin->setToolTip("Station offset from LO (Hz)");
    offsetRow->addWidget(offsetSpin);
    offsetRow->addStretch();
    form->addLayout(offsetRow);

    // WAV bandwidth
    auto* bwRow = new QHBoxLayout;
    bwRow->addWidget(new QLabel("WAV bandwidth (Hz):"));
    auto* bwSpin = new QDoubleSpinBox;
    bwSpin->setRange(10'000.0, 500'000.0);
    bwSpin->setDecimals(0);
    bwSpin->setSingleStep(10'000.0);
    bwSpin->setValue(wavBw_);
    bwRow->addWidget(bwSpin);
    bwRow->addStretch();
    form->addLayout(bwRow);

    // OK / Cancel
    auto* buttons = new QHBoxLayout;
    auto* okBtn = new QPushButton("OK");
    auto* cancelBtn = new QPushButton("Cancel");
    buttons->addStretch();
    buttons->addWidget(okBtn);
    buttons->addWidget(cancelBtn);
    form->addLayout(buttons);

    connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        recordPath_ = rawEdit->text();
        wavPath_    = wavEdit->text();
        wavOffset_  = offsetSpin->value();
        wavBw_      = bwSpin->value();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stream control
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::startStream() {
    if (ctrl_->isStreaming()) return;

    // Always sync LO to the spin-box value before streaming
    if (controller_->isInitialized())
        controller_->setFrequency(freqSpinBox->value());

    // ── Build config ──────────────────────────────────────────────────────────
    AppController::StreamConfig cfg;
    cfg.loFreqMHz = freqSpinBox->value();
    cfg.recordRaw = recordCheckBox->isChecked();
    cfg.rawPath   = recordPath_;
    cfg.exportWav = wavCheckBox->isChecked();
    cfg.wavPath   = wavPath_;
    cfg.wavOffset = wavOffset_;
    cfg.wavBw     = wavBw_;

    const int mode = modeCombo_ ? modeCombo_->currentIndex() : 0;
    if (mode == 1)      cfg.demodMode = "FM";
    else if (mode == 2) cfg.demodMode = "AM";

    if (mode > 0 && demodVfoSpin_)
        cfg.demodOffsetHz = (demodVfoSpin_->value() - freqSpinBox->value()) * 1e6;

    ctrl_->startStream(cfg);

    // Start RX1 stream if dual RX is enabled
    if (dualRxCheck_ && dualRxCheck_->isChecked()) {
        AppController::StreamConfig cfg1;
        cfg1.loFreqMHz = freqSpinBox1_ ? freqSpinBox1_->value() : cfg.loFreqMHz;
        ctrl1_->startStream(cfg1);
    }

    // Connect stream status to label (forwarded through AppController)
    connect(ctrl_, &AppController::streamStatus,
            streamStatusLabel, &QLabel::setText, Qt::QueuedConnection);

    // Apply current demod params after handler is created
    if (mode == 1) {
        ctrl_->setDemodParam("Bandwidth", fmBwSpin_->value() * 1000.0);
        ctrl_->setDemodParam("De-emphasis", fmDeemphCombo->currentData().toDouble());
    } else if (mode == 2) {
        ctrl_->setDemodParam("Bandwidth", amBwSpin_->value() * 1000.0);
    }
    if (mode > 0) {
        ctrl_->setVolume(static_cast<float>(demodVolSlider_->value()) / 100.0f);
        demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");
        demodStatusLabel_->setText(cfg.demodMode + ": waiting for first audio block\u2026");
    }

    // LMS_GetDeviceList (called by watchdog) causes USB interference during streaming.
    connectionTimer->stop();

    // Start metrics refresh (500 ms interval)
    if (!metricsTimer_) {
        metricsTimer_ = new QTimer(this);
        connect(metricsTimer_, &QTimer::timeout, this, &DeviceDetailWindow::updateDemodMetrics);
    }
    metricsTimer_->start(500);

    plotUserZoomed_ = false;
    calibrateButton->setEnabled(false);
    streamStartButton->setEnabled(false);
    streamStopButton->setEnabled(true);
    streamStatusLabel->setStyleSheet("color: #00cc44;");
    streamStatusLabel->setText("Streaming\u2026");
}

void DeviceDetailWindow::stopStream() {
    ctrl_->stopStream();
    if (ctrl1_->isStreaming())
        ctrl1_->stopStream();
    streamStopButton->setEnabled(false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Demod metrics
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::updateDemodMetrics() {
    if (!demodLevelLabel_) return;
    if (!ctrl_->demodHandler()) return;

    const double snr   = ctrl_->snrDb();
    const double ifRms = ctrl_->ifRms();

    constexpr int    kBlocks  = 10;
    constexpr double kSnrMax  = 15.0;
    const int filled = static_cast<int>(
        std::clamp(snr / kSnrMax * kBlocks, 0.0, static_cast<double>(kBlocks)));

    QString bar;
    bar.reserve(kBlocks);
    for (int i = 0; i < kBlocks; ++i)
        bar += (i < filled) ? QChar(0x25AE) : QChar(0x25AF);

    QString color;
    if (snr > 6.0)       color = "#00cc44";
    else if (snr > 2.0)  color = "#ffaa00";
    else                 color = "#888888";

    demodLevelLabel_->setStyleSheet(
        QString("color: %1; font-size: 10px;").arg(color));
    demodLevelLabel_->setText(
        QString("%1  SNR %2 dB  IF %3")
            .arg(bar)
            .arg(snr,   0, 'f', 1)
            .arg(ifRms, 0, 'f', 3));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Demodulator mode switching
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onModeChanged(int index) {
    const bool active = (index != 0);
    const bool isFm   = (index == 1);
    const bool isAm   = (index == 2);

    // Show/hide mode-specific widgets
    fmBwLabel_->setVisible(isFm);     fmBwSpin_->setVisible(isFm);
    fmDeemphLabel_->setVisible(isFm); fmDeemphCombo->setVisible(isFm);
    amBwLabel_->setVisible(isAm);     amBwSpin_->setVisible(isAm);

    // Enable/disable shared controls
    demodVolSlider_->setVisible(active);
    demodVolLabel_->setVisible(active);
    if (demodVfoSpin_) demodVfoSpin_->setEnabled(active);

    updateFilterBand(active);

    // Teardown current handler
    ctrl_->teardownDemod();
    if (demodStatusLabel_) demodStatusLabel_->setText("");

    if (!active || !ctrl_->isStreaming()) return;

    // Create new demod handler via registry
    const double offsetHz = demodVfoSpin_
                            ? (demodVfoSpin_->value() - freqSpinBox->value()) * 1e6
                            : 0.0;

    QString mode;
    if (isFm)      mode = "FM";
    else if (isAm) mode = "AM";

    ctrl_->setDemodMode(mode, offsetHz);

    if (isFm) {
        ctrl_->setDemodParam("Bandwidth", fmBwSpin_->value() * 1000.0);
        ctrl_->setDemodParam("De-emphasis", fmDeemphCombo->currentData().toDouble());
    } else {
        ctrl_->setDemodParam("Bandwidth", amBwSpin_->value() * 1000.0);
    }
    ctrl_->setVolume(static_cast<float>(demodVolSlider_->value()) / 100.0f);

    demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");
    demodStatusLabel_->setText(mode + ": waiting for first audio block\u2026");
}

// ═══════════════════════════════════════════════════════════════════════════════
// FFT / stream slots
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onFftReady(FftFrame frame) {
    fftPlot->graph(0)->setData(frame.freqMHz, frame.powerDb);

    if (centerLine_) {
        const double mhz = freqSpinBox->value();
        centerLine_->start->setCoords(mhz, -130.0);
        centerLine_->end->setCoords  (mhz,   10.0);
    }

    // Auto-fit x-axis to capture band only when the user hasn't zoomed in.
    // QSignalBlocker prevents the rangeChanged handler from seeing this change
    // (which would otherwise flip plotUserZoomed_ back to true).
    if (!plotUserZoomed_ && !frame.freqMHz.isEmpty()) {
        QSignalBlocker b(fftPlot->xAxis);
        fftPlot->xAxis->setRange(frame.freqMHz.first(), frame.freqMHz.last());
    }

    fftPlot->replot(QCustomPlot::rpQueuedReplot);
}

void DeviceDetailWindow::onStreamError(const QString& error) {
    streamStatusLabel->setStyleSheet("color: #ff4444;");
    streamStatusLabel->setText("Error: " + error);
    QMessageBox::warning(this, "Stream error", error);
}

void DeviceDetailWindow::onStreamFinished() {
    // Cleanup is already done by AppController before emitting streamFinished.
    if (metricsTimer_) metricsTimer_->stop();

    // Disconnect streamStatus so stale connection doesn't persist across streams
    disconnect(ctrl_, &AppController::streamStatus, streamStatusLabel, nullptr);

    calibrateButton->setEnabled(controller_->isInitialized());
    streamStartButton->setEnabled(controller_->isInitialized());
    streamStopButton->setEnabled(false);
    streamStatusLabel->setStyleSheet("color: gray;");
    streamStatusLabel->setText("Idle");
    if (demodStatusLabel_) demodStatusLabel_->setText("");
    if (demodLevelLabel_)  demodLevelLabel_->setText("\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF");

    // Resume connection watchdog now that streaming has stopped.
    connectionTimer->start();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Frequency spin / slider sync
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onFreqSliderChanged(int value) {
    QSignalBlocker blocker(freqSpinBox);
    freqSpinBox->setValue(static_cast<double>(value));
}

void DeviceDetailWindow::onFreqSpinChanged(double value) {
    QSignalBlocker blocker(freqSlider);
    freqSlider->setValue(static_cast<int>(value));
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
        ctrl_->stopStream();
        if (ctrl1_->isStreaming())      ctrl1_->stopStream();
        if (txCtrl_->isTransmitting())  txCtrl_->stopTx();
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
    if (ctrl_->isStreaming())       ctrl_->stopStream();
    if (ctrl1_->isStreaming())      ctrl1_->stopStream();
    if (txCtrl_->isTransmitting())  txCtrl_->stopTx();
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
        statusLabel->setText("No devices found. Waiting for connection…");
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
