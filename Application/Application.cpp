#include "Application.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <QSize>
#include <QFileDialog>
#include <QStandardPaths>
#include "qcustomplot.h"
#include "Logger.h"

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
    infoItem->setSizeHint(QSize(0, 48));
    controlItem->setSizeHint(QSize(0, 48));
    fftItem->setSizeHint(QSize(0, 48));

    contentStack = new QStackedWidget(central);
    contentStack->addWidget(new QWidget(contentStack));

    deviceInfoPage    = createDeviceInfoPage();
    deviceControlPage = createDeviceControlPage();
    deviceFFTpage     = createDeviceFFTpage();
    contentStack->addWidget(deviceInfoPage);
    contentStack->addWidget(deviceControlPage);
    contentStack->addWidget(deviceFFTpage);

    connect(functionList, &QListWidget::itemClicked, this,
        [this, infoItem, controlItem, fftItem](QListWidgetItem* item) {
            if      (item == infoItem)    contentStack->setCurrentWidget(deviceInfoPage);
            else if (item == controlItem) contentStack->setCurrentWidget(deviceControlPage);
            else if (item == fftItem)     contentStack->setCurrentWidget(deviceFFTpage);
        });

    functionList->clearSelection();
    contentStack->setCurrentIndex(0);

    splitter->addWidget(functionList);
    splitter->addWidget(contentStack);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    mainLayout->addWidget(splitter);
    setCentralWidget(central);

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
    teardownStream();
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
        controller_->calibrate(sampleRateSelector->currentData().toDouble());
    });

    connect(sampleRateSelector, &QComboBox::currentIndexChanged, this, [this]() {
        if (controller_->isInitialized())
            controller_->setSampleRate(sampleRateSelector->currentData().toDouble());
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
    fftPlot->setMinimumHeight(300);
    setupFftPlot();

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
        if (fftHandler_) fftHandler_->setCenterFrequency(mhz);
        if (centerLine_) {
            centerLine_->start->setCoords(mhz, -130.0);
            centerLine_->end->setCoords  (mhz,   10.0);
            fftPlot->replot(QCustomPlot::rpQueuedReplot);
        }
    };
    connect(freqSlider,   &QSlider::sliderReleased,        this, applyFreq);
    connect(freqSpinBox,  &QDoubleSpinBox::editingFinished, this, applyFreq);
    connect(applyFreqBtn, &QPushButton::clicked,            this, applyFreq);

    // ── Record to file ────────────────────────────────────────────────────────
    auto* recordRow  = new QWidget(page);
    auto* recordHlay = new QHBoxLayout(recordRow);
    recordHlay->setContentsMargins(0, 0, 0, 0);

    recordCheckBox = new QCheckBox("Record raw .raw", recordRow);
    recordPathEdit = new QLineEdit(
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
            + "/capture_iq_i16.raw", recordRow);
    recordPathEdit->setEnabled(false);
    auto* browseBtn = new QPushButton("...", recordRow);
    browseBtn->setFixedWidth(30);

    connect(recordCheckBox, &QCheckBox::toggled, recordPathEdit, &QLineEdit::setEnabled);
    connect(browseBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, "Save IQ capture", recordPathEdit->text(),
            "Raw IQ (*.raw);;All files (*)");
        if (!path.isEmpty()) recordPathEdit->setText(path);
    });

    recordHlay->addWidget(recordCheckBox);
    recordHlay->addWidget(recordPathEdit, 1);
    recordHlay->addWidget(browseBtn);

    // ── Bandpass WAV export ───────────────────────────────────────────────────
    auto* wavRow  = new QWidget(page);
    auto* wavHlay = new QHBoxLayout(wavRow);
    wavHlay->setContentsMargins(0, 0, 0, 0);

    wavCheckBox = new QCheckBox("Export filtered .wav", wavRow);

    auto* wOffsetLabel = new QLabel("Offset (Hz):", wavRow);
    wavOffsetSpin = new QDoubleSpinBox(wavRow);
    wavOffsetSpin->setRange(-15e6, 15e6);
    wavOffsetSpin->setDecimals(0);
    wavOffsetSpin->setSingleStep(100'000.0);
    wavOffsetSpin->setValue(0.0);
    wavOffsetSpin->setFixedWidth(100);
    wavOffsetSpin->setEnabled(false);
    wavOffsetSpin->setToolTip(
        "Station offset from LO (Hz). E.g. LO=102 MHz, station=104 MHz → +2 000 000");

    auto* wBwLabel = new QLabel("BW (Hz):", wavRow);
    wavBwSpin = new QDoubleSpinBox(wavRow);
    wavBwSpin->setRange(10'000.0, 500'000.0);
    wavBwSpin->setDecimals(0);
    wavBwSpin->setSingleStep(10'000.0);
    wavBwSpin->setValue(100'000.0);
    wavBwSpin->setFixedWidth(90);
    wavBwSpin->setEnabled(false);

    wavPathEdit = new QLineEdit(
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
            + "/station_iq.wav", wavRow);
    wavPathEdit->setEnabled(false);
    auto* wavBrowseBtn = new QPushButton("...", wavRow);
    wavBrowseBtn->setFixedWidth(30);
    wavBrowseBtn->setEnabled(false);

    connect(wavCheckBox, &QCheckBox::toggled, this, [this, wavBrowseBtn](bool on) {
        wavOffsetSpin->setEnabled(on);
        wavBwSpin->setEnabled(on);
        wavPathEdit->setEnabled(on);
        wavBrowseBtn->setEnabled(on);
    });
    connect(wavBrowseBtn, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(
            this, "Save filtered WAV", wavPathEdit->text(),
            "WAV (*.wav);;All files (*)");
        if (!path.isEmpty()) wavPathEdit->setText(path);
    });

    wavHlay->addWidget(wavCheckBox);
    wavHlay->addWidget(wOffsetLabel);
    wavHlay->addWidget(wavOffsetSpin);
    wavHlay->addWidget(wBwLabel);
    wavHlay->addWidget(wavBwSpin);
    wavHlay->addWidget(wavPathEdit, 1);
    wavHlay->addWidget(wavBrowseBtn);

    // ── FM Radio ──────────────────────────────────────────────────────────────
    // Simplified model: offset is always 0 — tune LO directly to the station.
    // Only user parameter: filter bandwidth (BW).
    // The filter band is shown on the spectrum centered on the LO (red line).
    auto* fmRow  = new QWidget(page);
    auto* fmHlay = new QHBoxLayout(fmRow);
    fmHlay->setContentsMargins(0, 0, 0, 0);

    fmCheckBox = new QCheckBox("FM Radio", fmRow);
    fmCheckBox->setToolTip(
        "Demodulate and play the station at the current center frequency.\n"
        "Tune the LO (Center freq) directly to the station frequency.\n"
        "Adjust BW to match the station's broadcast bandwidth.");

    // Filter bandwidth — the only FM-specific parameter
    auto* fmBwLabel = new QLabel("BW (kHz):", fmRow);
    fmBwSpin_ = new QDoubleSpinBox(fmRow);
    fmBwSpin_->setRange(50.0, 250.0);
    fmBwSpin_->setDecimals(0);
    fmBwSpin_->setSingleStep(10.0);
    fmBwSpin_->setValue(100.0);
    fmBwSpin_->setFixedWidth(70);
    fmBwSpin_->setEnabled(false);
    fmBwSpin_->setToolTip(
        "One-sided filter bandwidth (kHz).\n"
        "WBFM broadcast: 100–150 kHz\n"
        "Narrower = less noise but may cut audio highs.");

    // De-emphasis
    auto* fmDeemphLabel = new QLabel("De-emph:", fmRow);
    fmDeemphCombo = new QComboBox(fmRow);
    fmDeemphCombo->addItem("EU  50 µs", 50e-6);
    fmDeemphCombo->addItem("US  75 µs", 75e-6);
    fmDeemphCombo->setEnabled(false);
    fmDeemphCombo->setToolTip("Europe / Russia / most of the world: 50 µs\nUSA, Canada, Japan: 75 µs");

    // Volume
    auto* fmVolLabel = new QLabel("Vol:", fmRow);
    fmVolumeSlider = new QSlider(Qt::Horizontal, fmRow);
    fmVolumeSlider->setRange(0, 100);
    fmVolumeSlider->setValue(80);
    fmVolumeSlider->setFixedWidth(80);
    fmVolumeSlider->setEnabled(false);
    fmVolumeLabel = new QLabel("80%", fmRow);
    fmVolumeLabel->setFixedWidth(34);

    // Enable/disable controls and start/stop FM demodulator immediately.
    connect(fmCheckBox, &QCheckBox::toggled, this, [this](bool on) {
        fmBwSpin_->setEnabled(on);
        fmDeemphCombo->setEnabled(on);
        fmVolumeSlider->setEnabled(on);
        if (fmVfoSpin_) fmVfoSpin_->setEnabled(on);
        updateFilterBand(on);

        if (!on) {
            // FM disabled: remove handler from the running pipeline (if any)
            if (pipeline_ && fmDemodHandler_) {
                pipeline_->removeHandler(fmDemodHandler_);
                delete fmDemodHandler_;
                fmDemodHandler_ = nullptr;
            }
            if (fmAudio_) fmAudio_->teardown();
            if (fmStatusLabel) fmStatusLabel->setText("");
            return;
        }

        // FM enabled: if stream is not running, FM will be set up in startStream()
        if (!pipeline_) return;

        const double tau      = fmDeemphCombo->currentData().toDouble();
        const double bwHz     = fmBwSpin_->value() * 1000.0;
        const double offsetHz = fmVfoSpin_
                                ? (fmVfoSpin_->value() - freqSpinBox->value()) * 1e6
                                : 0.0;

        delete fmAudio_;
        fmAudio_ = new FmAudioOutput(this);
        fmAudio_->setVolume(static_cast<float>(fmVolumeSlider->value()) / 100.0f);

        connect(fmAudio_, &FmAudioOutput::statusChanged, this,
                [this](const QString& msg, bool isError) {
                    fmStatusLabel->setStyleSheet(
                        isError ? "color: #ff4444; font-size: 11px;"
                                : "color: #00cc44; font-size: 11px;");
                    fmStatusLabel->setText(msg);
                    if (isError)
                        QMessageBox::critical(this, "FM audio", msg);
                });

        fmStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

        if (fmDemodHandler_) {
            pipeline_->removeHandler(fmDemodHandler_);
            delete fmDemodHandler_;
            fmDemodHandler_ = nullptr;
        }
        fmDemodHandler_ = new FmDemodHandler(offsetHz, tau, bwHz, this);
        connect(fmDemodHandler_, &FmDemodHandler::audioReady,
                fmAudio_,        &FmAudioOutput::push,
                Qt::QueuedConnection);
        pipeline_->addHandler(fmDemodHandler_);

        fmStatusLabel->setText("FM: waiting for first audio block…");
    });

    // Volume → audio sink
    connect(fmVolumeSlider, &QSlider::valueChanged, this, [this](int v) {
        fmVolumeLabel->setText(QString("%1%").arg(v));
        if (fmAudio_) fmAudio_->setVolume(static_cast<float>(v) / 100.0f);
    });

    // BW change → update filter band on spectrum + FmDemodHandler on the fly
    connect(fmBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
        updateFilterBand(fmCheckBox->isChecked());
        if (fmDemodHandler_) fmDemodHandler_->setBandwidth(bwKHz * 1000.0);
    });

    // When LO changes → update VFO range and keep filter band centred correctly
    connect(freqSpinBox, &QDoubleSpinBox::valueChanged, this, [this](double loMHz) {
        if (fmVfoSpin_) {
            const double sr = device->sampleRate();
            const double halfBand = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
            fmVfoSpin_->setRange(loMHz - halfBand, loMHz + halfBand);
        }
        updateFilterBand(fmCheckBox->isChecked());
    });

    fmHlay->addWidget(fmCheckBox);
    fmHlay->addSpacing(8);
    fmHlay->addWidget(fmBwLabel);
    fmHlay->addWidget(fmBwSpin_);
    fmHlay->addSpacing(8);
    fmHlay->addWidget(fmDeemphLabel);
    fmHlay->addWidget(fmDeemphCombo);
    fmHlay->addSpacing(8);
    fmHlay->addWidget(fmVolLabel);
    fmHlay->addWidget(fmVolumeSlider);
    fmHlay->addWidget(fmVolumeLabel);
    fmHlay->addStretch();

    // ── VFO tuner ─────────────────────────────────────────────────────────────
    // Allows tuning to any station within the capture band without retuning LO.
    // Click on the spectrum to jump the VFO there, or edit the spinbox directly.
    auto* vfoRow  = new QWidget(page);
    auto* vfoHlay = new QHBoxLayout(vfoRow);
    vfoHlay->setContentsMargins(0, 0, 0, 0);

    auto* vfoLabel = new QLabel("VFO:", vfoRow);
    vfoLabel->setToolTip("Tune FM demodulator to a specific station.\n"
                         "Click anywhere on the spectrum to jump here.");

    fmVfoSpin_ = new QDoubleSpinBox(vfoRow);
    fmVfoSpin_->setRange(kFreqMinMHz, kFreqMaxMHz);
    fmVfoSpin_->setDecimals(3);
    fmVfoSpin_->setSingleStep(0.1);
    fmVfoSpin_->setValue(kFreqDefaultMHz);
    fmVfoSpin_->setFixedWidth(110);
    fmVfoSpin_->setEnabled(false);
    fmVfoSpin_->setToolTip("Station frequency (MHz). Edit or click the spectrum.");

    auto* vfoHintLabel = new QLabel("← click spectrum to tune", vfoRow);
    vfoHintLabel->setStyleSheet("color: gray; font-size: 10px;");

    vfoHlay->addWidget(vfoLabel);
    vfoHlay->addWidget(fmVfoSpin_);
    vfoHlay->addWidget(vfoHintLabel);
    vfoHlay->addStretch();

    // VFO changed → update filter band + apply NCO offset to running demodulator
    connect(fmVfoSpin_, &QDoubleSpinBox::valueChanged, this, [this](double vfoMHz) {
        updateFilterBand(fmCheckBox->isChecked());
        if (fmDemodHandler_) {
            const double offsetHz = (vfoMHz - freqSpinBox->value()) * 1e6;
            fmDemodHandler_->setOffset(offsetHz);
        }
    });

    // ── FM signal level indicator ─────────────────────────────────────────────
    auto* fmRow2  = new QWidget(page);
    auto* fmHlay2 = new QHBoxLayout(fmRow2);
    fmHlay2->setContentsMargins(0, 0, 0, 0);

    fmLevelLabel_ = new QLabel("▯▯▯▯▯▯▯▯▯▯", fmRow2);
    fmLevelLabel_->setStyleSheet("color: gray; font-size: 10px;");
    fmLevelLabel_->setToolTip("Signal level after FM demodulation.\n"
                              "Gray = weak / no station\n"
                              "Green = good signal\n"
                              "Red = clipping (reduce gain)");

    fmHlay2->addWidget(fmLevelLabel_);
    fmHlay2->addStretch();

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

    // ── FM status label ───────────────────────────────────────────────────────
    fmStatusLabel = new QLabel(page);
    fmStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

    layout->addWidget(title);
    layout->addSpacing(4);
    layout->addWidget(fftPlot, 1);
    layout->addWidget(freqRow);
    layout->addWidget(recordRow);
    layout->addWidget(wavRow);
    layout->addWidget(fmRow);
    layout->addWidget(vfoRow);
    layout->addWidget(fmRow2);
    layout->addWidget(fmStatusLabel);
    layout->addWidget(btnRow);
    layout->addWidget(streamStatusLabel);
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

    // Click on spectrum → tune VFO to that frequency (FM must be enabled)
    connect(fftPlot, &QCustomPlot::mousePress, this,
            [this](QMouseEvent* event) {
        if (!fmCheckBox || !fmCheckBox->isChecked()) return;
        if (!fmVfoSpin_) return;
        const double clickedMHz = fftPlot->xAxis->pixelToCoord(event->pos().x());
        // Clamp to the current capture band shown on the plot
        const double lo   = freqSpinBox->value();
        const double sr   = device->sampleRate();
        const double half = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
        const double clamped = std::clamp(clickedMHz, lo - half, lo + half);
        fmVfoSpin_->setValue(clamped);
    });
}

// Filter band: ±BW/2 around VFO frequency, visible only when FM is on.
void DeviceDetailWindow::updateFilterBand(bool visible) {
    if (!vfoBand_) return;
    vfoBand_->setVisible(visible);
    if (visible) {
        const double vfoMHz = fmVfoSpin_ ? fmVfoSpin_->value() : freqSpinBox->value();
        const double bwMHz  = (fmBwSpin_ ? fmBwSpin_->value() : 100.0) / 1000.0;
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
    if (streamWorker) return;

    // Always sync LO to the spin-box value before streaming — the user may have
    // typed a frequency without pressing Apply, and we don't want to stream on
    // the wrong frequency silently.
    if (controller_->isInitialized())
        controller_->setFrequency(freqSpinBox->value());

    LOG_INFO("startStream: record=" + std::to_string(recordCheckBox->isChecked())
             + " wav=" + std::to_string(wavCheckBox->isChecked())
             + " fm=" + std::to_string(fmCheckBox->isChecked())
             + " lo=" + std::to_string(freqSpinBox->value()) + " MHz");

    // ── Build pipeline ────────────────────────────────────────────────────────
    pipeline_ = new Pipeline(this);

    // FFT handler — always active
    fftHandler_ = new FftHandler(this);
    fftHandler_->setCenterFrequency(freqSpinBox->value());
    pipeline_->addHandler(fftHandler_);

    connect(fftHandler_, &FftHandler::fftReady,
            this,        &DeviceDetailWindow::onFftReady,
            Qt::QueuedConnection);

    // Raw .raw recording
    if (recordCheckBox->isChecked()) {
        auto* rawHandler = new RawFileHandler(recordPathEdit->text());
        pipeline_->addHandler(rawHandler);
        // Ownership: pipeline dispatches but doesn't own; store ptr in a list for cleanup
        // For simplicity, wrap in QObject-managed via deleteLater on stream stop
        // (handler will be cleaned in teardownStream via pipeline_->clearHandlers + delete)
        rawHandlers_.push_back(rawHandler);
    }

    // Bandpass WAV export
    if (wavCheckBox->isChecked()) {
        auto* wavHandler = new BandpassHandler(
            wavPathEdit->text(),
            wavOffsetSpin->value(),
            wavBwSpin->value());
        pipeline_->addHandler(wavHandler);
        wavHandlers_.push_back(wavHandler);
    }

    // FM demodulation
    if (fmCheckBox->isChecked()) {
        const double tau      = fmDeemphCombo->currentData().toDouble();
        const double bwHz     = fmBwSpin_->value() * 1000.0;
        const double offsetHz = fmVfoSpin_
                                ? (fmVfoSpin_->value() - freqSpinBox->value()) * 1e6
                                : 0.0;

        LOG_INFO("FM demod: LO=" + std::to_string(freqSpinBox->value())
                 + " MHz  VFO=" + std::to_string(fmVfoSpin_ ? fmVfoSpin_->value() : freqSpinBox->value())
                 + " MHz  offset=" + std::to_string(static_cast<int>(offsetHz))
                 + " Hz  BW=" + std::to_string(static_cast<int>(bwHz)) + " Hz");

        fmDemodHandler_ = new FmDemodHandler(offsetHz, tau, bwHz, this);
        pipeline_->addHandler(fmDemodHandler_);

        delete fmAudio_;
        fmAudio_ = new FmAudioOutput(this);
        fmAudio_->setVolume(static_cast<float>(fmVolumeSlider->value()) / 100.0f);

        connect(fmDemodHandler_, &FmDemodHandler::audioReady,
                fmAudio_,        &FmAudioOutput::push,
                Qt::QueuedConnection);

        connect(fmAudio_, &FmAudioOutput::statusChanged, this,
                [this](const QString& msg, bool isError) {
                    fmStatusLabel->setStyleSheet(
                        isError ? "color: #ff4444; font-size: 11px;"
                                : "color: #00cc44; font-size: 11px;");
                    fmStatusLabel->setText(msg);
                    if (isError)
                        QMessageBox::critical(this, "FM audio", msg);
                });

        fmStatusLabel->setStyleSheet("color: gray; font-size: 11px;");
        fmStatusLabel->setText("FM: waiting for first audio block…");
    }

    // ── Start worker thread ───────────────────────────────────────────────────
    streamThread = new QThread(this);
    streamWorker = new StreamWorker(device.get(), pipeline_);
    streamWorker->moveToThread(streamThread);

    connect(streamThread, &QThread::started,            streamWorker, &StreamWorker::run);
    connect(streamWorker, &StreamWorker::statusMessage, streamStatusLabel, &QLabel::setText,                 Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::errorOccurred, this,         &DeviceDetailWindow::onStreamError,    Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::finished,      this,         &DeviceDetailWindow::onStreamFinished, Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::finished,      streamThread, &QThread::quit,                        Qt::QueuedConnection);
    connect(streamThread, &QThread::finished,           streamWorker, &QObject::deleteLater);
    connect(streamThread, &QThread::finished,           streamThread, &QObject::deleteLater);

    streamThread->start();

    // LMS_GetDeviceList (called by watchdog) causes USB interference during streaming.
    connectionTimer->stop();

    // Start metrics refresh (500 ms interval)
    if (!metricsTimer_) {
        metricsTimer_ = new QTimer(this);
        connect(metricsTimer_, &QTimer::timeout, this, &DeviceDetailWindow::updateFmMetrics);
    }
    metricsTimer_->start(500);

    plotUserZoomed_ = false;   // new stream may have different LO/SR — reset zoom
    calibrateButton->setEnabled(false);
    streamStartButton->setEnabled(false);
    streamStopButton->setEnabled(true);
    streamStatusLabel->setStyleSheet("color: #00cc44;");
    streamStatusLabel->setText("Streaming…");
}

void DeviceDetailWindow::stopStream() {
    if (streamWorker) streamWorker->stop();
    streamStopButton->setEnabled(false);
}

void DeviceDetailWindow::teardownStream() {
    if (streamWorker) streamWorker->stop();
    if (streamThread) { streamThread->quit(); streamThread->wait(3000); }
    streamWorker = nullptr;
    streamThread = nullptr;

    if (pipeline_) {
        pipeline_->clearHandlers();
        delete pipeline_;
        pipeline_ = nullptr;
    }

    if (metricsTimer_) metricsTimer_->stop();

    delete fftHandler_;      fftHandler_     = nullptr;
    delete fmDemodHandler_;  fmDemodHandler_ = nullptr;

    for (auto* h : rawHandlers_) delete h;
    rawHandlers_.clear();
    for (auto* h : wavHandlers_) delete h;
    wavHandlers_.clear();

    if (fmAudio_) {
        // Disconnect statusChanged before teardown so the emitted signal
        // doesn't fire into partially-destroyed window state (e.g. during
        // QObject child cleanup in the destructor).
        disconnect(fmAudio_, nullptr, this, nullptr);
        fmAudio_->teardown();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// FM metrics
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::updateFmMetrics() {
    if (!fmDemodHandler_ || !fmLevelLabel_) return;

    const double snr   = fmDemodHandler_->snrDb();
    const double ifRms = fmDemodHandler_->ifRms();

    // Level bar: 10 blocks, each block ≈ 1.5 dB SNR (0..15 dB range)
    constexpr int   kBlocks   = 10;
    constexpr double kSnrMax  = 15.0;
    const int filled = static_cast<int>(
        std::clamp(snr / kSnrMax * kBlocks, 0.0, static_cast<double>(kBlocks)));

    QString bar;
    bar.reserve(kBlocks);
    for (int i = 0; i < kBlocks; ++i)
        bar += (i < filled) ? QChar(0x25AE) : QChar(0x25AF);  // ▮ / ▯

    QString color;
    if (snr > 6.0)       color = "#00cc44";   // green — good signal
    else if (snr > 2.0)  color = "#ffaa00";   // orange — marginal
    else                 color = "#888888";    // gray — noise

    fmLevelLabel_->setStyleSheet(
        QString("color: %1; font-size: 10px;").arg(color));
    fmLevelLabel_->setText(
        QString("%1  SNR %2 dB  IF %3")
            .arg(bar)
            .arg(snr,   0, 'f', 1)
            .arg(ifRms, 0, 'f', 3));
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
    if (metricsTimer_) metricsTimer_->stop();

    // streamWorker/streamThread are deleted via deleteLater connected to QThread::finished
    streamWorker = nullptr;
    streamThread = nullptr;

    if (pipeline_) {
        pipeline_->clearHandlers();
        delete pipeline_;
        pipeline_ = nullptr;
    }

    delete fftHandler_;      fftHandler_     = nullptr;
    delete fmDemodHandler_;  fmDemodHandler_ = nullptr;

    for (auto* h : rawHandlers_) delete h;
    rawHandlers_.clear();
    for (auto* h : wavHandlers_) delete h;
    wavHandlers_.clear();

    calibrateButton->setEnabled(controller_->isInitialized());
    streamStartButton->setEnabled(controller_->isInitialized());
    streamStopButton->setEnabled(false);
    streamStatusLabel->setStyleSheet("color: gray;");
    streamStatusLabel->setText("Idle");
    if (fmAudio_) {
        disconnect(fmAudio_, nullptr, this, nullptr);
        fmAudio_->teardown();
    }
    if (fmStatusLabel) fmStatusLabel->setText("");
    if (fmLevelLabel_) fmLevelLabel_->setText("▯▯▯▯▯▯▯▯▯▯");

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
        teardownStream();
        // Defer the signal so we fully unwind the QFutureWatcher callback
        // before the deviceDisconnected handler opens a QMessageBox (which
        // would re-enter the event loop while still on the watcher's stack).
        QTimer::singleShot(0, this, [this]() { emit deviceDisconnected(); });
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceSelectionWindow
// ═══════════════════════════════════════════════════════════════════════════════
DeviceSelectionWindow::DeviceSelectionWindow(IDeviceManager& manager, QWidget* parent)
    : QWidget(parent)
    , manager(manager)
{
    setWindowTitle("Stand — SDR");
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
        deviceList->clear();
        const auto devices = refreshWatcher.result();
        if (devices.empty()) {
            statusLabel->setText("No devices found. Waiting for connection…");
            return;
        }
        statusLabel->setText("Select a device to open its window.");
        for (const auto& dev : devices) {
            auto* item = new QListWidgetItem(deviceList);
            item->setSizeHint(QSize(0, 40));
            auto* btn = new QPushButton(dev->name(), deviceList);
            connect(btn, &QPushButton::clicked, this, [this, dev]() { openDevice(dev); });
            deviceList->setItemWidget(item, btn);
        }
    });

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

void DeviceSelectionWindow::openDevice(const std::shared_ptr<IDevice>& dev) {
    refreshTimer->stop();
    auto* window = new DeviceDetailWindow(dev, manager);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &DeviceDetailWindow::deviceDisconnected, this, [this, window]() {
        QMessageBox::warning(window, "Device disconnected",
            "Connection to the device was lost. Returning to device search.");
        window->close();
        show();
        refreshTimer->start();
    });
    window->show();
    hide();
}

// ═══════════════════════════════════════════════════════════════════════════════
// Application
// ═══════════════════════════════════════════════════════════════════════════════
Application::Application(int& argc, char** argv, IDeviceManager& manager)
    : qtApp(argc, argv)
    , selectionWindow(manager)
{
    QApplication::setWindowIcon(QIcon(":/assets/icon.jpg"));
}

int Application::run() {
    selectionWindow.resize(400, 300);
    selectionWindow.show();
    return QApplication::exec();
}
