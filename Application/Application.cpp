#include "Application.h"

#include <algorithm>
#include <cmath>
#include <QSize>
#include <QFileDialog>
#include <QStandardPaths>
#include "qcustomplot.h"

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceDetailWindow — constructor
// ═══════════════════════════════════════════════════════════════════════════════
DeviceDetailWindow::DeviceDetailWindow(std::shared_ptr<Device> device, LimeManager& manager, QWidget* parent)
    : QMainWindow(parent)
    , device(std::move(device))
    , manager(manager)
    , controller_(new DeviceController(this->device, this))   // owns the command layer
{
    setWindowTitle(QString::fromStdString(this->device->GetSerial()));

    // ── Wire controller signals once, here ───────────────────────────────────
    // All hardware-state changes flow back through these four connections;
    // individual page builders never need to know about LimeException.
    connect(controller_, &DeviceController::deviceInitialized,
            this,        &DeviceDetailWindow::onDeviceInitialized);
    connect(controller_, &DeviceController::sampleRateChanged,
            this,        &DeviceDetailWindow::onSampleRateChanged);
    connect(controller_, &DeviceController::statusChanged,
            this,        &DeviceDetailWindow::onControllerStatus);
    connect(controller_, &DeviceController::errorOccurred,
            this,        &DeviceDetailWindow::onControllerError);

    // ── Layout ────────────────────────────────────────────────────────────────
    auto* central    = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);
    auto* splitter   = new QSplitter(Qt::Horizontal, central);
    splitter->setChildrenCollapsible(false);

    // ── Sidebar ───────────────────────────────────────────────────────────────
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

    // ── Pages ─────────────────────────────────────────────────────────────────
    contentStack = new QStackedWidget(central);
    contentStack->addWidget(new QWidget(contentStack));   // index 0 — empty placeholder

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

    // ── Connection watchdog ───────────────────────────────────────────────────
    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    connect(connectionTimer, &QTimer::timeout, this, &DeviceDetailWindow::checkDeviceConnection);
    connect(&connectionWatcher,
            &QFutureWatcher<std::vector<std::shared_ptr<Device>>>::finished,
            this, &DeviceDetailWindow::handleConnectionCheckFinished);
    connectionTimer->start();
    checkDeviceConnection();
}

DeviceDetailWindow::~DeviceDetailWindow() {
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
        QString("Serial: %1").arg(QString::fromStdString(this->device->GetSerial())), page);
    auto* detailLabel = new QLabel(
        QString("Info: %1").arg(QString::fromStdString(std::string(this->device->GetInfo()))), page);
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

    initStatusLabel   = new QLabel("Device not initialized", page);
    controlStatusLabel = new QLabel(page);
    controlStatusLabel->setStyleSheet("color: gray; font-size: 11px;");

    // ── Sample rate ───────────────────────────────────────────────────────────
    sampleRateSelector = new QComboBox(page);
    for (double rate : manager.sampleRates)
        sampleRateSelector->addItem(QString("%1 Hz").arg(rate, 0, 'f', 0), rate);

    auto* initButton = new QPushButton("Initialize device", page);
    calibrateButton  = new QPushButton("Calibrate",         page);

    const bool ready = device->is_initialized();
    sampleRateSelector->setEnabled(ready);
    calibrateButton->setEnabled(ready);

    // Buttons delegate directly to the controller — no logic in these lambdas.
    connect(initButton, &QPushButton::clicked,
            controller_, &DeviceController::initDevice);

    connect(calibrateButton, &QPushButton::clicked, this, [this]() {
        controller_->calibrate(sampleRateSelector->currentData().toDouble());
    });

    connect(sampleRateSelector, &QComboBox::currentIndexChanged, this, [this]() {
        if (controller_->isInitialized())
            controller_->setSampleRate(sampleRateSelector->currentData().toDouble());
    });

    // ── Gain sliders ──────────────────────────────────────────────────────────
    auto makeGainRow = [&](const QString& name, int min, int max,
                           QSlider*& slider, QLabel*& valLabel) -> QWidget* {
        auto* row  = new QWidget(page);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);
        auto* lbl  = new QLabel(name, row);
        lbl->setFixedWidth(36);
        slider     = new QSlider(Qt::Horizontal, row);
        slider->setRange(min, max);
        slider->setValue(min);
        slider->setEnabled(ready);
        valLabel   = new QLabel(QString::number(min), row);
        valLabel->setFixedWidth(28);
        hlay->addWidget(lbl);
        hlay->addWidget(slider);
        hlay->addWidget(valLabel);
        return row;
    };

    auto* lnaRow = makeGainRow("LNA", 0,  5, lnaSlider, lnaValueLabel);
    auto* tiaRow = makeGainRow("TIA", 0,  2, tiaSlider, tiaValueLabel);
    auto* pgaRow = makeGainRow("PGA", 0, 31, pgaSlider, pgaValueLabel);

    // Live label update on drag; send to controller only on release.
    auto wireSlider = [this](QSlider* s, QLabel* lbl) {
        connect(s, &QSlider::valueChanged, lbl, [lbl](int v) {
            lbl->setText(QString::number(v));
        });
        connect(s, &QSlider::sliderReleased, this, [this]() {
            controller_->setGain(lnaSlider->value(),
                                 tiaSlider->value(),
                                 pgaSlider->value());
        });
    };
    wireSlider(lnaSlider, lnaValueLabel);
    wireSlider(tiaSlider, tiaValueLabel);
    wireSlider(pgaSlider, pgaValueLabel);

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(initStatusLabel);
    layout->addWidget(controlStatusLabel);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Sample rate", page));
    layout->addWidget(sampleRateSelector);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Gain", page));
    layout->addWidget(lnaRow);
    layout->addWidget(tiaRow);
    layout->addWidget(pgaRow);
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

    // Sync spin ↔ slider (UI only, no hardware).
    connect(freqSlider,  &QSlider::valueChanged,        this, &DeviceDetailWindow::onFreqSliderChanged);
    connect(freqSpinBox, &QDoubleSpinBox::valueChanged, this, &DeviceDetailWindow::onFreqSpinChanged);

    // Apply frequency to hardware.
    // Three triggers so the user never has to think about it:
    //   • releasing the slider after a drag
    //   • pressing Enter / leaving the spinbox
    //   • clicking the Apply button (manual fallback)
    auto applyFreq = [this]() {
        if (!controller_->isInitialized()) return;
        const double mhz = freqSpinBox->value();
        controller_->setFrequency(mhz);
        // Move the centre-frequency marker immediately — no need to wait for
        // the next samplesReady frame.
        if (centerLine_) {
            centerLine_->start->setCoords(mhz, -130.0);
            centerLine_->end->setCoords  (mhz,   10.0);
            fftPlot->replot(QCustomPlot::rpQueuedReplot);
        }
    };
    connect(freqSlider,   &QSlider::sliderReleased,          this, applyFreq);
    connect(freqSpinBox,  &QDoubleSpinBox::editingFinished,   this, applyFreq);
    connect(applyFreqBtn, &QPushButton::clicked,              this, applyFreq);

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
        if (!path.isEmpty())
            recordPathEdit->setText(path);
    });

    recordHlay->addWidget(recordCheckBox);
    recordHlay->addWidget(recordPathEdit, 1);
    recordHlay->addWidget(browseBtn);

    // ── Bandpass WAV export ───────────────────────────────────────────────────
    auto* wavRow  = new QWidget(page);
    auto* wavHlay = new QHBoxLayout(wavRow);
    wavHlay->setContentsMargins(0, 0, 0, 0);

    wavCheckBox = new QCheckBox("Export filtered .wav", wavRow);

    auto* offsetLabel = new QLabel("Offset (Hz):", wavRow);
    wavOffsetSpin = new QDoubleSpinBox(wavRow);
    wavOffsetSpin->setRange(-15e6, 15e6);
    wavOffsetSpin->setDecimals(0);
    wavOffsetSpin->setSingleStep(100'000.0);
    wavOffsetSpin->setValue(0.0);
    wavOffsetSpin->setFixedWidth(100);
    wavOffsetSpin->setEnabled(false);
    wavOffsetSpin->setToolTip(
        "Frequency offset of target station relative to the LO centre (Hz).\n"
        "E.g. LO=102 MHz, station=104 MHz → offset=+2 000 000");

    auto* bwLabel = new QLabel("BW (Hz):", wavRow);
    wavBwSpin = new QDoubleSpinBox(wavRow);
    wavBwSpin->setRange(10'000.0, 500'000.0);
    wavBwSpin->setDecimals(0);
    wavBwSpin->setSingleStep(10'000.0);
    wavBwSpin->setValue(100'000.0);
    wavBwSpin->setFixedWidth(90);
    wavBwSpin->setEnabled(false);
    wavBwSpin->setToolTip("One-sided bandpass width (Hz). 100 000 Hz = ±100 kHz, good for WBFM.");

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
        if (!path.isEmpty())
            wavPathEdit->setText(path);
    });

    wavHlay->addWidget(wavCheckBox);
    wavHlay->addWidget(offsetLabel);
    wavHlay->addWidget(wavOffsetSpin);
    wavHlay->addWidget(bwLabel);
    wavHlay->addWidget(wavBwSpin);
    wavHlay->addWidget(wavPathEdit, 1);
    wavHlay->addWidget(wavBrowseBtn);

    // ── Stream controls ───────────────────────────────────────────────────────
    auto* btnRow  = new QWidget(page);
    auto* btnHlay = new QHBoxLayout(btnRow);
    btnHlay->setContentsMargins(0, 0, 0, 0);

    streamStartButton = new QPushButton("▶  Start", btnRow);
    streamStopButton  = new QPushButton("■  Stop",  btnRow);
    streamStopButton->setEnabled(false);
    streamStartButton->setEnabled(device->is_initialized());

    connect(streamStartButton, &QPushButton::clicked, this, &DeviceDetailWindow::startStream);
    connect(streamStopButton,  &QPushButton::clicked, this, &DeviceDetailWindow::stopStream);

    btnHlay->addWidget(streamStartButton);
    btnHlay->addWidget(streamStopButton);
    btnHlay->addStretch();

    streamStatusLabel = new QLabel("Idle", page);
    streamStatusLabel->setStyleSheet("color: gray;");

    layout->addWidget(title);
    layout->addSpacing(4);
    layout->addWidget(fftPlot, 1);
    layout->addWidget(freqRow);
    layout->addWidget(recordRow);
    layout->addWidget(wavRow);
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

    fftPlot->xAxis->setLabel("Frequency offset (MHz)");
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
    fftPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    // ── Centre-frequency marker ───────────────────────────────────────────────
    // QCPItemLine with both points at the same X spans the full Y range.
    // Coordinates are in axis units (MHz / dB).
    centerLine_ = new QCPItemLine(fftPlot);
    centerLine_->setPen(QPen(QColor(255, 60, 60), 1.2, Qt::DashLine));
    centerLine_->setAntialiased(false);
    // Initial position at default LO; updated every time frequency is applied.
    centerLine_->start->setCoords(kFreqDefaultMHz, -130.0);
    centerLine_->end->setCoords  (kFreqDefaultMHz,   10.0);
    // Use axis rect coordinates so the line always spans full plot height
    // even when the Y axis is zoomed.
    centerLine_->start->setType(QCPItemPosition::ptPlotCoords);
    centerLine_->end->setType  (QCPItemPosition::ptPlotCoords);

    // ── Zoom guard ────────────────────────────────────────────────────────────
    // Allow zooming in freely, but cap zoom-out at ±SR/2 * 1.05 on X
    // and prevent zooming out past [-130, 10] dB on Y.
    // The actual X limits are updated in onSamplesReady when SR is known.
    connect(fftPlot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        // Clamp: don't let the user zoom out beyond the full spectrum width.
        // maxHalfSpan is set lazily from the last known data range.
        if (fftPlot->graph(0)->dataCount() < 2) return;
        const double lo = fftPlot->graph(0)->data()->begin()->key;
        const double hi = (fftPlot->graph(0)->data()->end() - 1)->key;
        const double span = hi - lo;
        if (newRange.size() > span * 1.05) {
            QSignalBlocker b(fftPlot->xAxis);
            fftPlot->xAxis->setRange(lo - span * 0.025, hi + span * 0.025);
        }
    });
    connect(fftPlot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        // Y hard limits: [−130, +10] dBFS
        constexpr double yMin = -130.0, yMax = 10.0;
        if (newRange.lower < yMin || newRange.upper > yMax) {
            QSignalBlocker b(fftPlot->yAxis);
            fftPlot->yAxis->setRange(
                std::max(newRange.lower, yMin),
                std::min(newRange.upper, yMax));
        }
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceController reaction slots
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::onDeviceInitialized() {
    initStatusLabel->setText("Device initialized ✓");
    initStatusLabel->setStyleSheet("color: #00cc44;");

    const bool ready = true;
    sampleRateSelector->setEnabled(ready);
    calibrateButton->setEnabled(ready);
    lnaSlider->setEnabled(ready);
    tiaSlider->setEnabled(ready);
    pgaSlider->setEnabled(ready);
    streamStartButton->setEnabled(ready);

    refreshCurrentSampleRate();

    // A single informational dialog — no logic here, just presentation.
    QMessageBox::information(this, "Initialization", "Device initialized successfully.");
}

void DeviceDetailWindow::onSampleRateChanged(double hz) {
    if (currentSampleRateLabel)
        currentSampleRateLabel->setText(
            QString("Current sample rate: %1 Hz").arg(hz, 0, 'f', 0));

    // Keep the combo-box in sync without triggering another setSampleRate call.
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

    streamThread = new QThread(this);
    streamWorker = new StreamWorker(device);
    streamWorker->moveToThread(streamThread);

    if (recordCheckBox->isChecked())
        streamWorker->enableFileOutput(recordPathEdit->text());

    if (wavCheckBox->isChecked()) {
        streamWorker->enableBandpassWav(
            wavPathEdit->text(),
            wavOffsetSpin->value(),
            wavBwSpin->value()
        );
    }

    connect(streamThread, &QThread::started,            streamWorker, &StreamWorker::run);
    connect(streamWorker, &StreamWorker::samplesReady,  this,         &DeviceDetailWindow::onSamplesReady,   Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::statusMessage, streamStatusLabel, &QLabel::setText,                 Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::errorOccurred, this,         &DeviceDetailWindow::onStreamError,    Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::finished,      this,         &DeviceDetailWindow::onStreamFinished, Qt::QueuedConnection);
    connect(streamWorker, &StreamWorker::finished,      streamThread, &QThread::quit,                        Qt::QueuedConnection);
    connect(streamThread, &QThread::finished,           streamWorker, &QObject::deleteLater);
    connect(streamThread, &QThread::finished,           streamThread, &QObject::deleteLater);

    streamThread->start();

    streamStartButton->setEnabled(false);
    streamStopButton->setEnabled(true);
    streamStatusLabel->setStyleSheet("color: #00cc44;");
    streamStatusLabel->setText("Streaming…");
}

void DeviceDetailWindow::stopStream() {
    if (streamWorker)
        streamWorker->stop();
    streamStopButton->setEnabled(false);
}

void DeviceDetailWindow::teardownStream() {
    if (streamWorker) streamWorker->stop();
    if (streamThread) { streamThread->quit(); streamThread->wait(3000); }
    streamWorker = nullptr;
    streamThread = nullptr;
}

void DeviceDetailWindow::onSamplesReady(QVector<int16_t> samples) {
    try {
        const auto frame = FftProcessor::process(
            samples,
            freqSpinBox->value(),
            device->get_sample_rate()
        );
        fftPlot->graph(0)->setData(frame.freqMHz, frame.powerDb);

        // Keep the centre-frequency marker in sync with the spinbox value.
        // This handles the case where frequency was applied before the stream
        // started (line position is already correct) as well as mid-stream changes.
        if (centerLine_) {
            const double mhz = freqSpinBox->value();
            centerLine_->start->setCoords(mhz, -130.0);
            centerLine_->end->setCoords  (mhz,   10.0);
        }

        fftPlot->xAxis->rescale();

        // rpQueuedReplot: merges redundant repaint requests into one,
        // preventing stutter if the main thread is briefly busy.
        fftPlot->replot(QCustomPlot::rpQueuedReplot);
    } catch (const std::exception& ex) {
        onStreamError(QString("FFT error: %1").arg(ex.what()));
    }
}

void DeviceDetailWindow::onStreamError(const QString& error) {
    streamStatusLabel->setStyleSheet("color: #ff4444;");
    streamStatusLabel->setText("Error: " + error);
    QMessageBox::warning(this, "Stream error", error);
}

void DeviceDetailWindow::onStreamFinished() {
    streamWorker = nullptr;
    streamThread = nullptr;
    streamStartButton->setEnabled(device->is_initialized());
    streamStopButton->setEnabled(false);
    streamStatusLabel->setStyleSheet("color: gray;");
    streamStatusLabel->setText("Idle");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Frequency spin / slider sync (UI-only, no hardware call)
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
    if (!device->is_initialized()) {
        currentSampleRateLabel->setText("Current sample rate: device not initialized");
        return;
    }
    try {
        const double sr = device->get_sample_rate();
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
        manager.refresh_devices();
        return manager.get_devices();
    }));
}

void DeviceDetailWindow::handleConnectionCheckFinished() {
    const auto devices   = connectionWatcher.result();
    const bool connected = std::ranges::any_of(devices, [&](const std::shared_ptr<Device>& d) {
        return d->GetSerial() == device->GetSerial();
    });
    if (!connected) {
        connectionTimer->stop();
        teardownStream();
        emit deviceDisconnected();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DeviceSelectionWindow
// ═══════════════════════════════════════════════════════════════════════════════
DeviceSelectionWindow::DeviceSelectionWindow(LimeManager& manager, QWidget* parent)
    : QWidget(parent)
    , manager(manager)
{
    setWindowTitle("LimeManager");
    auto* layout = new QVBoxLayout(this);
    statusLabel  = new QLabel("Searching for LimeSDR devices...", this);
    deviceList   = new QListWidget(this);
    layout->addWidget(statusLabel);
    layout->addWidget(deviceList);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(1000);
    connect(refreshTimer, &QTimer::timeout, this, &DeviceSelectionWindow::refreshDevices);

    connect(&refreshWatcher,
            &QFutureWatcher<std::vector<std::shared_ptr<Device>>>::finished,
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
            auto* btn = new QPushButton(
                QString::fromStdString(dev->GetSerial()), deviceList);
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
        manager.refresh_devices();
        return manager.get_devices();
    }));
}

void DeviceSelectionWindow::openDevice(const std::shared_ptr<Device>& dev) {
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
Application::Application(int& argc, char** argv, LimeManager& manager)
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
