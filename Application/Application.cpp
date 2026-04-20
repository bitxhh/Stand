#include "Application.h"

#include <algorithm>
#include <cmath>
#include <QMenuBar>
#include <QScrollArea>
#include <QSize>
#include "../Core/ChannelDescriptor.h"
#include "../Core/DeviceSettings.h"
#include "LoggerOptionsDialog.h"
#include "RadioMonitorPage.h"
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

    // ── Load persisted UI state (keyed by serial) BEFORE building widgets ────
    // Chip-register state (.ini) is loaded later in onDeviceInitialized().
    const DeviceSettings settings = DeviceSettings::load(this->device->id());

    connect(controller_, &DeviceController::deviceInitialized,
            this,        &DeviceDetailWindow::onDeviceInitialized);
    connect(controller_, &DeviceController::sampleRateChanged,
            this,        &DeviceDetailWindow::onSampleRateChanged);
    connect(controller_, &DeviceController::statusChanged,
            this,        &DeviceDetailWindow::onControllerStatus);
    connect(controller_, &DeviceController::errorOccurred,
            this,        &DeviceDetailWindow::onControllerError);
    connect(controller_, &DeviceController::progressChanged,
            this, [this](int percent, const QString& stage) {
        if (initProgressBar_) {
            initProgressBar_->setValue(percent);
            initProgressBar_->setVisible(true);
        }
        if (initProgressLabel_) initProgressLabel_->setText(stage);
        if (percent == 100) {
            if (initProgressBar_) initProgressBar_->setVisible(false);
            if (initProgressLabel_) initProgressLabel_->clear();
            setNavEnabled(true);
        }
    }, Qt::QueuedConnection);

    // ── DSP thread pool — one per DeviceDetailWindow, shared with the
    // combined pipeline managed by RadioMonitorPage.
    // maxThreadCount left at default (QThread::idealThreadCount()) so the pool
    // scales automatically to the machine's core count.
    dspPool_ = new QThreadPool(this);

    // ── Build UI ──────────────────────────────────────────────────────────────
    auto* central    = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);
    auto* splitter   = new QSplitter(Qt::Horizontal, central);
    splitter->setChildrenCollapsible(false);

    functionList = new QListWidget(central);
    functionList->setMinimumWidth(160);
    functionList->setSelectionMode(QAbstractItemView::SingleSelection);
    functionList->setSpacing(4);

    auto* infoItem    = new QListWidgetItem("Device info",     functionList);
    auto* controlItem = new QListWidgetItem("Device control",  functionList);
    auto* monitorItem = new QListWidgetItem("Радиомониторинг", functionList);
    auto* txNavItem   = new QListWidgetItem("Transmit",        functionList);
    infoItem->setSizeHint(QSize(0, 48));
    controlItem->setSizeHint(QSize(0, 48));
    monitorItem->setSizeHint(QSize(0, 48));
    txNavItem->setSizeHint(QSize(0, 48));

    contentStack = new QStackedWidget(central);
    contentStack->addWidget(new QWidget(contentStack));

    deviceInfoPage    = createDeviceInfoPage();
    deviceControlPage = createDeviceControlPage();
    QWidget* radioPage = createRadioMonitorPage();   // builds radioMonitorPage_
    txPage_           = createTxPage();
    contentStack->addWidget(deviceInfoPage);
    contentStack->addWidget(deviceControlPage);
    contentStack->addWidget(radioPage);
    contentStack->addWidget(txPage_);

    connect(functionList, &QListWidget::itemClicked, this,
        [this, infoItem, controlItem, monitorItem, txNavItem, radioPage](QListWidgetItem* item) {
            if      (item == infoItem)    contentStack->setCurrentWidget(deviceInfoPage);
            else if (item == controlItem) contentStack->setCurrentWidget(deviceControlPage);
            else if (item == monitorItem) contentStack->setCurrentWidget(radioPage);
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

    // ── Menu bar ──────────────────────────────────────────────────────────────
    auto* toolsMenu = menuBar()->addMenu(tr("Tools"));
    connect(toolsMenu->addAction(tr("Logger settings…")), &QAction::triggered, this, [this]() {
        LoggerOptionsDialog dlg(this);
        dlg.exec();
    });

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

    // ── Apply saved UI state to widgets (after pages are built) ──────────────
    // Sample rate — select closest item in combo
    if (sampleRateSelector) {
        QSignalBlocker blocker(sampleRateSelector);
        for (int i = 0; i < sampleRateSelector->count(); ++i) {
            if (std::abs(sampleRateSelector->itemData(i).toDouble() - settings.sampleRate) < 1.0) {
                sampleRateSelector->setCurrentIndex(i);
                break;
            }
        }
    }
    if (channelCountSpin_) {
        QSignalBlocker blocker(channelCountSpin_);
        channelCountSpin_->setValue(std::clamp(settings.channelCount,
            channelCountSpin_->minimum(), channelCountSpin_->maximum()));
    }
    if (channelAssignCombo_) {
        QSignalBlocker blocker(channelAssignCombo_);
        const int idx = channelAssignCombo_->findData(settings.channelAssign);
        if (idx >= 0) channelAssignCombo_->setCurrentIndex(idx);
    }
    if (channelAssignRow_ && channelCountSpin_) {
        QList<ChannelDescriptor> rxChannels;
        for (const auto& info : this->device->availableChannels())
            if (info.descriptor.direction == ChannelDescriptor::RX)
                rxChannels.append(info.descriptor);
        channelAssignRow_->setVisible(rxChannels.size() > 1 && channelCountSpin_->value() == 1);
    }
    for (int i = 0; i < gainSliders_.size() && i < 2; ++i) {
        QSignalBlocker blocker(gainSliders_[i]);
        const int v = std::clamp(static_cast<int>(settings.gainRx[i]),
                                 gainSliders_[i]->minimum(),
                                 gainSliders_[i]->maximum());
        gainSliders_[i]->setValue(v);
        if (i < gainValueLabels_.size())
            gainValueLabels_[i]->setText(QString("%1 dB").arg(v));
    }
    if (txFreqSpin_)       txFreqSpin_->setValue(settings.txFreqMHz);
    if (txGainSlider_) {
        const int v = std::clamp(static_cast<int>(settings.txGainDb),
                                 txGainSlider_->minimum(),
                                 txGainSlider_->maximum());
        txGainSlider_->setValue(v);
        if (txGainLabel_) txGainLabel_->setText(QString("%1 dB").arg(v));
    }
    if (txToneOffsetSpin_) txToneOffsetSpin_->setValue(settings.txToneOffsetHz / 1000.0);
    if (txAmplitudeSpin_)  txAmplitudeSpin_->setValue(settings.txAmplitude);

    // Restore demodulator panels (mode / VFO / BW / volume / recording flags).
    if (radioMonitorPage_ && !settings.demodPanels.isEmpty())
        radioMonitorPage_->restoreDemodPanels(settings.demodPanels);

    // ── Plot render timer: max 20 fps, delegates to RadioMonitorPage ─────────
    plotTimer_ = new QTimer(this);
    plotTimer_->setInterval(50);
    plotTimer_->setTimerType(Qt::CoarseTimer);
    connect(plotTimer_, &QTimer::timeout, this, [this]() {
        if (radioMonitorPage_) radioMonitorPage_->replotIfDirty();
    });
    plotTimer_->start();

    // ── Chip temperature indicator (bottom-right on all pages via status bar) ─
    temperatureLabel_ = new QLabel("Temperature: — °C", this);
    statusBar()->addPermanentWidget(temperatureLabel_);
    temperatureTimer_ = new QTimer(this);
    temperatureTimer_->setInterval(5000);
    connect(temperatureTimer_, &QTimer::timeout, this, &DeviceDetailWindow::updateTemperature);
    temperatureTimer_->start();
    updateTemperature();

    // ── Connection watchdog ───────────────────────────────────────────────────
    connectionTimer = new QTimer(this);
    connectionTimer->setInterval(1000);
    connect(connectionTimer, &QTimer::timeout, this, &DeviceDetailWindow::checkDeviceConnection);
    connect(&connectionWatcher,
            &QFutureWatcher<QList<std::shared_ptr<IDevice>>>::finished,
            this, &DeviceDetailWindow::handleConnectionCheckFinished);
    connectionTimer->start();
    checkDeviceConnection();

    // ── Auto-open — init+setSampleRate+calibrate without user clicking Init ──
    autoOpenDevice();
}

void DeviceDetailWindow::updateTemperature() {
    if (!temperatureLabel_ || !device) return;
    const double t = device->temperature();
    if (std::isnan(t)) {
        temperatureLabel_->setText("Temperature: — °C");
    } else {
        temperatureLabel_->setText(QString("Temperature: %1 °C").arg(t, 0, 'f', 1));
    }
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
    if (plotTimer_)        plotTimer_->stop();
    if (metricsTimer_)     metricsTimer_->stop();
    if (temperatureTimer_) temperatureTimer_->stop();

    // ── 2. Synchronously stop RX stream ──────────────────────────────────────────
    // shutdown() blocks until worker threads exit (up to 3 s).
    // Must happen before device->close() / LMS_Close().
    if (radioMonitorPage_) radioMonitorPage_->shutdown();

    // ── 3. Synchronously stop TX ─────────────────────────────────────────────────
    if (txCtrl_) txCtrl_->shutdown();

    // ── 4. Persist settings (JSON for UI state, .ini for chip registers) ────────
    // Must run while the device handle is still open — LMS_SaveConfig needs it.
    if (controller_ && controller_->isInitialized()) {
        DeviceSettings s;
        if (sampleRateSelector && sampleRateSelector->currentIndex() >= 0)
            s.sampleRate = sampleRateSelector->currentData().toDouble();
        else
            s.sampleRate = device->sampleRate();
        if (channelCountSpin_)   s.channelCount  = channelCountSpin_->value();
        if (channelAssignCombo_) s.channelAssign = channelAssignCombo_->currentData().toInt();
        for (int i = 0; i < gainSliders_.size() && i < 2; ++i)
            s.gainRx[i] = gainSliders_[i]->value();
        // Unified center frequency — apply to all persisted RX slots.
        const double mhz = radioMonitorPage_
                           ? radioMonitorPage_->centerFreqMHz()
                           : device->frequency() / 1e6;
        for (int i = 0; i < 2; ++i) s.freqRxMHz[i] = mhz;
        if (txFreqSpin_)       s.txFreqMHz      = txFreqSpin_->value();
        if (txGainSlider_)     s.txGainDb       = txGainSlider_->value();
        if (txToneOffsetSpin_) s.txToneOffsetHz = txToneOffsetSpin_->value() * 1000.0;
        if (txAmplitudeSpin_)  s.txAmplitude    = txAmplitudeSpin_->value();

        if (radioMonitorPage_) s.demodPanels = radioMonitorPage_->demodPanelStates();

        s.save(device->id());
    }

    // ── 5. Close hardware — tears down LMS handle, resets state to Connected ─────
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

    initProgressBar_ = new QProgressBar(page);
    initProgressBar_->setRange(0, 100);
    initProgressBar_->setTextVisible(false);
    initProgressBar_->setFixedHeight(8);
    initProgressBar_->setVisible(false);
    initProgressLabel_ = new QLabel(page);
    initProgressLabel_->setStyleSheet("color: gray; font-size: 11px;");
    initProgressLabel_->setVisible(true);

    sampleRateSelector = new QComboBox(page);
    for (double rate : device->supportedSampleRates())
        sampleRateSelector->addItem(QString("%1 Hz").arg(rate, 0, 'f', 0), rate);

    resetButton_    = new QPushButton("Reset device", page);
    calibrateButton = new QPushButton("Calibrate",    page);
    sampleRateSelector->setEnabled(ready);
    calibrateButton->setEnabled(ready);

    // ── Channel selection ─────────────────────────────────────────────────────
    // Count RX channels available on the device.
    QList<ChannelDescriptor> rxChannels;
    for (const auto& info : device->availableChannels())
        if (info.descriptor.direction == ChannelDescriptor::RX)
            rxChannels.append(info.descriptor);
    const int numRx = rxChannels.size();

    auto* selectionRow  = new QWidget(page);
    auto* selectionHlay = new QHBoxLayout(selectionRow);
    selectionHlay->setContentsMargins(0, 0, 0, 0);
    auto* countLbl = new QLabel("RX channels", selectionRow);
    channelCountSpin_ = new QSpinBox(selectionRow);
    channelCountSpin_->setRange(1, std::max(1, numRx));
    channelCountSpin_->setValue(std::max(1, numRx));
    channelCountSpin_->setEnabled(!ready);  // locked after init
    selectionHlay->addWidget(countLbl);
    selectionHlay->addWidget(channelCountSpin_);
    selectionHlay->addStretch();

    // Combo visible only if device has >1 RX AND user picks count==1 (which RX to use).
    channelAssignRow_ = new QWidget(page);
    auto* assignHlay = new QHBoxLayout(channelAssignRow_);
    assignHlay->setContentsMargins(0, 0, 0, 0);
    auto* assignLbl = new QLabel("Use channel", channelAssignRow_);
    channelAssignCombo_ = new QComboBox(channelAssignRow_);
    for (const auto& ch : rxChannels)
        channelAssignCombo_->addItem(QString("RX%1").arg(ch.channelIndex), ch.channelIndex);
    channelAssignCombo_->setEnabled(!ready);
    assignHlay->addWidget(assignLbl);
    assignHlay->addWidget(channelAssignCombo_);
    assignHlay->addStretch();
    channelAssignRow_->setVisible(numRx > 1 && channelCountSpin_->value() == 1);

    connect(channelCountSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, numRx](int v) {
        if (channelAssignRow_)
            channelAssignRow_->setVisible(numRx > 1 && v == 1);
        updateChannelRowVisibility();
        applyChannelSelectionChange();
    });

    connect(channelAssignCombo_, &QComboBox::currentIndexChanged,
            this, [this](int) {
        applyChannelSelectionChange();
    });

    // ── Per-channel gain rows ─────────────────────────────────────────────────
    gainSliders_.clear();
    gainValueLabels_.clear();
    gainRows_.clear();
    auto* gainBox = new QWidget(page);
    auto* gainBoxLay = new QVBoxLayout(gainBox);
    gainBoxLay->setContentsMargins(0, 0, 0, 0);
    gainBoxLay->setSpacing(4);
    for (const auto& ch : rxChannels) {
        auto* row = new QWidget(gainBox);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);
        auto* lbl = new QLabel(QString("RX%1 gain").arg(ch.channelIndex), row);
        lbl->setFixedWidth(80);
        lbl->setToolTip("Total RX gain 0–68 dB.\n"
                        "TIA fixed at 12 dB; LimeSuite distributes\n"
                        "the remaining gain across LNA and PGA.");
        auto* slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(0, 68);
        slider->setValue(static_cast<int>(device->gain(ch)));
        slider->setEnabled(ready);
        slider->setToolTip(lbl->toolTip());
        auto* valueLbl = new QLabel(QString("%1 dB").arg(slider->value()), row);
        valueLbl->setFixedWidth(52);
        hlay->addWidget(lbl);
        hlay->addWidget(slider);
        hlay->addWidget(valueLbl);

        connect(slider, &QSlider::valueChanged, valueLbl, [valueLbl](int v) {
            valueLbl->setText(QString("%1 dB").arg(v));
        });
        connect(slider, &QSlider::sliderReleased, this, [this, slider, ch]() {
            controller_->setGainChannel(ch, slider->value());
            // Update the combiner's per-channel normalisation so the combined
            // I/Q stays level-balanced after a gain change mid-stream.
            if (radioMonitorPage_) {
                QVector<double> gains;
                for (auto* s : gainSliders_) gains.append(s->value());
                radioMonitorPage_->setChannelGains(gains);
            }
        });

        gainSliders_.append(slider);
        gainValueLabels_.append(valueLbl);
        gainRows_.append(row);
        gainBoxLay->addWidget(row);
    }

    connect(resetButton_, &QPushButton::clicked, this, [this]() {
        stopAllStreams();
        device->close();

        // Reset UI widgets to factory defaults.
        DeviceSettings def;
        if (sampleRateSelector) {
            QSignalBlocker b(sampleRateSelector);
            for (int i = 0; i < sampleRateSelector->count(); ++i) {
                if (std::abs(sampleRateSelector->itemData(i).toDouble() - def.sampleRate) < 1.0) {
                    sampleRateSelector->setCurrentIndex(i);
                    break;
                }
            }
        }
        if (channelCountSpin_) {
            QSignalBlocker b(channelCountSpin_);
            channelCountSpin_->setValue(
                std::clamp(def.channelCount,
                           channelCountSpin_->minimum(),
                           channelCountSpin_->maximum()));
            channelCountSpin_->setEnabled(true);
        }
        if (channelAssignCombo_) {
            QSignalBlocker b(channelAssignCombo_);
            const int idx = channelAssignCombo_->findData(def.channelAssign);
            if (idx >= 0) channelAssignCombo_->setCurrentIndex(idx);
            channelAssignCombo_->setEnabled(true);
        }
        updateChannelRowVisibility();
        for (int i = 0; i < gainSliders_.size() && i < 2; ++i) {
            QSignalBlocker b(gainSliders_[i]);
            gainSliders_[i]->setValue(static_cast<int>(def.gainRx[i]));
            gainSliders_[i]->setEnabled(false);
            if (i < gainValueLabels_.size())
                gainValueLabels_[i]->setText(QString("%1 dB").arg(static_cast<int>(def.gainRx[i])));
        }
        sampleRateSelector->setEnabled(false);
        calibrateButton->setEnabled(false);
        if (txStartButton_) txStartButton_->setEnabled(false);
        initStatusLabel->setText("Device not initialized");
        initStatusLabel->setStyleSheet("");
        controlStatusLabel->setStyleSheet("color: gray; font-size: 11px;");
        controlStatusLabel->setText("");

        // Persist defaults (keep current demod panel states).
        def.demodPanels = radioMonitorPage_ ? radioMonitorPage_->demodPanelStates()
                                             : QList<DemodPanelSettings>{};
        def.save(device->id());

        autoOpenDevice();
    });

    connect(calibrateButton, &QPushButton::clicked, this, [this]() {
        stopAllStreams();
        controller_->calibrate(selectedChannels());  // calBwHz = auto (из текущего Fs)
    });

    connect(sampleRateSelector, &QComboBox::currentIndexChanged, this, [this]() {
        if (controller_->isInitialized()) {
            stopAllStreams();
            controller_->setSampleRate(sampleRateSelector->currentData().toDouble());
        }
    });

    layout->addWidget(title);
    layout->addSpacing(8);
    layout->addWidget(initStatusLabel);
    layout->addWidget(controlStatusLabel);
    layout->addWidget(initProgressBar_);
    layout->addWidget(initProgressLabel_);
    layout->addSpacing(12);
    layout->addWidget(new QLabel("Sample rate", page));
    layout->addWidget(sampleRateSelector);
    layout->addSpacing(12);
    layout->addWidget(selectionRow);
    layout->addWidget(channelAssignRow_);
    layout->addSpacing(12);
    auto* gainLabel = new QLabel("Gain (per channel)", page);
    auto* gainHint  = new QLabel("Recommended for FM: 30–50 dB", page);
    gainHint->setStyleSheet("color: gray; font-size: 10px;");
    layout->addWidget(gainLabel);
    layout->addWidget(gainHint);
    layout->addWidget(gainBox);
    layout->addSpacing(12);
    layout->addWidget(resetButton_);
    layout->addWidget(calibrateButton);
    layout->addStretch();

    updateChannelRowVisibility();
    return page;
}

// ─────────────────────────────────────────────────────────────────────────────
QList<ChannelDescriptor> DeviceDetailWindow::selectedChannels() const {
    QList<ChannelDescriptor> result;
    if (!channelCountSpin_) return result;

    QList<ChannelDescriptor> rxChannels;
    for (const auto& info : device->availableChannels())
        if (info.descriptor.direction == ChannelDescriptor::RX)
            rxChannels.append(info.descriptor);

    const int count = channelCountSpin_->value();
    if (count == 1 && rxChannels.size() > 1 && channelAssignCombo_) {
        const int idx = channelAssignCombo_->currentData().toInt();
        for (const auto& ch : rxChannels)
            if (ch.channelIndex == idx) { result.append(ch); break; }
    } else {
        for (int i = 0; i < count && i < rxChannels.size(); ++i)
            result.append(rxChannels[i]);
    }
    return result;
}

void DeviceDetailWindow::updateChannelRowVisibility() {
    // Grey out gain rows that aren't in the current selection so the user
    // sees which channels the chosen count covers.
    const auto sel = selectedChannels();
    QList<ChannelDescriptor> rxChannels;
    for (const auto& info : device->availableChannels())
        if (info.descriptor.direction == ChannelDescriptor::RX)
            rxChannels.append(info.descriptor);
    for (int i = 0; i < gainRows_.size() && i < rxChannels.size(); ++i) {
        const bool active = sel.contains(rxChannels[i]);
        gainRows_[i]->setEnabled(active && controller_->isInitialized());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
QWidget* DeviceDetailWindow::createRadioMonitorPage() {
    radioMonitorPage_ = new RadioMonitorPage(device.get(), controller_, dspPool_, this);

    // Seed the page with the channels the user selected on the control page.
    // DeviceDetailWindow is the source of truth for active channels; the page
    // is updated again after init (onDeviceInitialized) to reflect the final
    // channel selection baked into the hardware.
    radioMonitorPage_->setActiveChannels(selectedChannels());

    // Propagate stream events to window-level housekeeping
    // (connection watchdog, metrics timer, calibrate button).
    connect(radioMonitorPage_, &RadioMonitorPage::streamStarted, this, [this]() {
        // LMS_GetDeviceList (watchdog) interferes with USB during streaming.
        connectionTimer->stop();
        if (calibrateButton) calibrateButton->setEnabled(false);
        // Channel selection is frozen while the stream is running.
        if (channelCountSpin_)   channelCountSpin_->setEnabled(false);
        if (channelAssignCombo_) channelAssignCombo_->setEnabled(false);
        if (!metricsTimer_) {
            metricsTimer_ = new QTimer(this);
            connect(metricsTimer_, &QTimer::timeout, this, [this]() {
                if (radioMonitorPage_) radioMonitorPage_->updateMetrics();
            });
        }
        metricsTimer_->start(500);
    });
    connect(radioMonitorPage_, &RadioMonitorPage::streamStopped, this, [this]() {
        if (metricsTimer_) metricsTimer_->stop();
        if (calibrateButton) calibrateButton->setEnabled(controller_->isInitialized());
        if (channelCountSpin_)   channelCountSpin_->setEnabled(true);
        if (channelAssignCombo_) channelAssignCombo_->setEnabled(true);
        connectionTimer->start();
    });
    connect(radioMonitorPage_, &RadioMonitorPage::errorOccurred, this,
            [this](const QString& err) {
                QMessageBox::warning(this, "Stream error", err);
            });

    return radioMonitorPage_;
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
    // Channel widgets remain editable while no stream is running; they are
    // gated on stream state by the streamStarted/streamStopped handlers.
    if (channelCountSpin_)   channelCountSpin_->setEnabled(true);
    if (channelAssignCombo_) channelAssignCombo_->setEnabled(true);
    for (auto* s : gainSliders_) s->setEnabled(true);
    updateChannelRowVisibility();
    if (txStartButton_) txStartButton_->setEnabled(true);

    // Hand the finalized channel selection and gains to the radio monitor page
    // so it can start the combined stream correctly.
    if (radioMonitorPage_) {
        radioMonitorPage_->setActiveChannels(selectedChannels());
        QVector<double> gains;
        for (auto* s : gainSliders_) gains.append(s->value());
        radioMonitorPage_->setChannelGains(gains);
        radioMonitorPage_->onDeviceReady();
    }

    if (resetButton_) resetButton_->setEnabled(true);
    refreshCurrentSampleRate();
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
    if (initProgressBar_)  initProgressBar_->setVisible(false);
    if (initProgressLabel_) initProgressLabel_->clear();
    setNavEnabled(true);
    if (resetButton_) resetButton_->setEnabled(true);
    QMessageBox::critical(this, "Device error", message);
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
// Auto-open
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::autoOpenDevice() {
    if (controller_->isInitialized()) return;
    const double sr = (sampleRateSelector && sampleRateSelector->currentIndex() >= 0)
                      ? sampleRateSelector->currentData().toDouble()
                      : device->sampleRate();
    if (resetButton_) resetButton_->setEnabled(false);
    setNavEnabled(false);
    if (initProgressBar_)  { initProgressBar_->setValue(0); initProgressBar_->setVisible(true); }
    if (initProgressLabel_)  initProgressLabel_->setText("Initializing…");
    controller_->autoOpen(selectedChannels(), sr);
}

void DeviceDetailWindow::setNavEnabled(bool enabled) {
    if (!functionList) return;
    const Qt::ItemFlags on  = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const Qt::ItemFlags off = Qt::ItemIsSelectable;
    // Index 1 is "Device control" — always accessible.
    for (int i = 0; i < functionList->count(); ++i) {
        if (i == 1) continue;
        if (auto* it = functionList->item(i))
            it->setFlags(enabled ? on : off);
    }
}

void DeviceDetailWindow::applyChannelSelectionChange() {
    // Only apply when device is initialized and no stream is running.
    // During initial auto-open or while streaming the handler is a no-op —
    // streamStarted disables the widgets, and pre-init changes are picked up
    // by the first autoOpenDevice() call naturally.
    if (!controller_->isInitialized()) return;
    if (radioMonitorPage_ && radioMonitorPage_->isStreaming()) return;

    stopAllStreams();
    device->close();

    // Reflect the transient "not initialized" state in the UI until autoOpen
    // finishes; the channel widgets themselves stay editable so the user can
    // tweak the selection again during reinit if needed.
    initStatusLabel->setText("Reinitializing...");
    initStatusLabel->setStyleSheet("");
    if (sampleRateSelector) sampleRateSelector->setEnabled(false);
    if (calibrateButton)    calibrateButton->setEnabled(false);
    if (resetButton_)       resetButton_->setEnabled(false);
    for (auto* s : gainSliders_) s->setEnabled(false);
    if (txStartButton_) txStartButton_->setEnabled(false);

    autoOpenDevice();
}

// ═══════════════════════════════════════════════════════════════════════════════
// RXTX coordination helper
// ═══════════════════════════════════════════════════════════════════════════════
void DeviceDetailWindow::stopAllStreams() {
    if (radioMonitorPage_ && radioMonitorPage_->isStreaming())
        radioMonitorPage_->shutdown();
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
