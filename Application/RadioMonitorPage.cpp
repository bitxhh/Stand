#include "RadioMonitorPage.h"

#include "CombinedRxController.h"
#include "DemodulatorPanel.h"
#include "RecordingSettingsDialog.h"
#include "../Core/FileNaming.h"
#include "../Core/IDevice.h"
#include "../Hardware/DeviceController.h"
#include "qcustomplot.h"

#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>

// ---------------------------------------------------------------------------
RadioMonitorPage::RadioMonitorPage(IDevice*          device,
                                   DeviceController* controller,
                                   QThreadPool*      dspPool,
                                   QWidget*          parent)
    : QWidget(parent)
    , device_(device)
    , controller_(controller)
    , dspPool_(dspPool)
{
    ctrl_ = new CombinedRxController(device_, dspPool_, this);
    connect(ctrl_, &CombinedRxController::fftReady,
            this,  &RadioMonitorPage::onFftReady, Qt::QueuedConnection);
    connect(ctrl_, &CombinedRxController::streamStatus,
            this,  [this](const QString& msg) {
                if (statusLabel_) statusLabel_->setText(msg);
            }, Qt::QueuedConnection);
    connect(ctrl_, &CombinedRxController::streamError,
            this,  &RadioMonitorPage::onStreamErrorInternal, Qt::QueuedConnection);
    connect(ctrl_, &CombinedRxController::streamFinished,
            this,  &RadioMonitorPage::onStreamFinishedInternal, Qt::QueuedConnection);

    loadRecordingSettings();
    buildUi();

    // Default channel selection: one RX0. DeviceDetailWindow overrides via
    // setActiveChannels() once the selection UI is known.
    activeChannels_.append({ChannelDescriptor::RX, 0});
    gainsDb_.append(0.0);
}

RadioMonitorPage::~RadioMonitorPage() {
    shutdown();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::buildUi() {
    auto* outer = new QVBoxLayout(this);

    auto* title = new QLabel("Радиомониторинг", this);
    title->setStyleSheet("font-weight: 600; font-size: 16px;");
    outer->addWidget(title);
    outer->addSpacing(4);

    // ── Frequency row ────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        auto* lbl = new QLabel("Center freq (MHz):", row);
        lbl->setFixedWidth(120);

        freqSpin_ = new QDoubleSpinBox(row);
        freqSpin_->setRange(kFreqMinMHz, kFreqMaxMHz);
        freqSpin_->setDecimals(3);
        freqSpin_->setSingleStep(0.1);
        freqSpin_->setValue(kFreqDefaultMHz);
        freqSpin_->setFixedWidth(110);

        freqSlider_ = new QSlider(Qt::Horizontal, row);
        freqSlider_->setRange(static_cast<int>(kFreqMinMHz),
                              static_cast<int>(kFreqMaxMHz));
        freqSlider_->setValue(static_cast<int>(kFreqDefaultMHz));

        applyBtn_ = new QPushButton("Apply", row);
        applyBtn_->setFixedWidth(60);

        hlay->addWidget(lbl);
        hlay->addWidget(freqSpin_);
        hlay->addWidget(freqSlider_, 1);
        hlay->addWidget(applyBtn_);
        outer->addWidget(row);

        connect(freqSlider_, &QSlider::valueChanged, this, [this](int v) {
            QSignalBlocker b(freqSpin_);
            freqSpin_->setValue(static_cast<double>(v));
        });
        connect(freqSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            QSignalBlocker b(freqSlider_);
            freqSlider_->setValue(static_cast<int>(v));
        });
        connect(applyBtn_,   &QPushButton::clicked,            this, &RadioMonitorPage::applyFrequency);
        connect(freqSlider_, &QSlider::sliderReleased,         this, &RadioMonitorPage::applyFrequency);
        connect(freqSpin_,   &QDoubleSpinBox::editingFinished, this, &RadioMonitorPage::applyFrequency);
    }

    // ── FFT plot ─────────────────────────────────────────────────────────────
    fftPlot_ = new QCustomPlot(this);
    fftPlot_->setMinimumHeight(240);
    setupFftPlot();
    outer->addWidget(fftPlot_, 1);

    // ── Controls row: + Add demod, Record, Settings ──────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        addDemodBtn_ = new QPushButton("+ Add demodulator", row);
        addDemodBtn_->setToolTip(QString("Up to %1 demodulators").arg(kMaxDemods));

        recordCheck_ = new QCheckBox("Record", row);
        recordCheck_->setToolTip(
            "Enable recording of raw/combined I/Q at the next stream start.\n"
            "Per-demodulator filtered/audio capture is controlled on each panel.");

        settingsBtn_ = new QPushButton("\u2699", row);
        settingsBtn_->setFixedWidth(32);
        settingsBtn_->setToolTip("Recording settings");

        hlay->addWidget(addDemodBtn_);
        hlay->addSpacing(12);
        hlay->addWidget(recordCheck_);
        hlay->addWidget(settingsBtn_);
        hlay->addStretch();
        outer->addWidget(row);

        connect(addDemodBtn_, &QPushButton::clicked, this, &RadioMonitorPage::addDemodulator);
        connect(settingsBtn_, &QPushButton::clicked, this, &RadioMonitorPage::openRecordingSettings);
    }

    // ── Demodulator panels area (scrollable) ─────────────────────────────────
    {
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMinimumHeight(140);

        auto* host = new QWidget(scroll);
        panelsLayout_ = new QVBoxLayout(host);
        panelsLayout_->setContentsMargins(0, 0, 0, 0);
        panelsLayout_->setSpacing(4);
        panelsLayout_->addStretch();
        scroll->setWidget(host);
        outer->addWidget(scroll);
    }

    // ── Stream start / stop ──────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        startBtn_ = new QPushButton("\u25B6  Start", row);
        stopBtn_  = new QPushButton("\u25A0  Stop",  row);
        startBtn_->setEnabled(controller_->isInitialized());
        stopBtn_->setEnabled(false);

        connect(startBtn_, &QPushButton::clicked, this, &RadioMonitorPage::startStream);
        connect(stopBtn_,  &QPushButton::clicked, this, &RadioMonitorPage::stopStream);

        statusLabel_ = new QLabel("Idle", row);
        statusLabel_->setStyleSheet("color: gray;");

        hlay->addWidget(startBtn_);
        hlay->addWidget(stopBtn_);
        hlay->addSpacing(12);
        hlay->addWidget(statusLabel_);
        hlay->addStretch();
        outer->addWidget(row);
    }
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::setupFftPlot() {
    fftPlot_->addGraph();
    fftPlot_->graph(0)->setPen(QPen(QColor(0, 200, 255), 1.2));

    fftPlot_->xAxis->setLabel("Frequency (MHz)");
    fftPlot_->yAxis->setLabel("Power (dB)");
    fftPlot_->yAxis->setRange(-120, 0);

    fftPlot_->setBackground(QBrush(QColor(30, 30, 30)));
    fftPlot_->xAxis->setBasePen(QPen(Qt::white));
    fftPlot_->yAxis->setBasePen(QPen(Qt::white));
    fftPlot_->xAxis->setTickPen(QPen(Qt::white));
    fftPlot_->yAxis->setTickPen(QPen(Qt::white));
    fftPlot_->xAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot_->yAxis->setSubTickPen(QPen(Qt::gray));
    fftPlot_->xAxis->setTickLabelColor(Qt::white);
    fftPlot_->yAxis->setTickLabelColor(Qt::white);
    fftPlot_->xAxis->setLabelColor(Qt::white);
    fftPlot_->yAxis->setLabelColor(Qt::white);
    fftPlot_->setInteractions(QCP::iRangeZoom);
    fftPlot_->axisRect()->setRangeZoom(Qt::Horizontal);

    // Center frequency marker (red dashed).
    centerLine_ = new QCPItemLine(fftPlot_);
    centerLine_->setPen(QPen(QColor(255, 60, 60), 1.2, Qt::DashLine));
    centerLine_->setAntialiased(false);
    centerLine_->start->setCoords(kFreqDefaultMHz, -130.0);
    centerLine_->end->setCoords  (kFreqDefaultMHz,   10.0);
    centerLine_->start->setType(QCPItemPosition::ptPlotCoords);
    centerLine_->end->setType  (QCPItemPosition::ptPlotCoords);

    // X-axis zoom clamp.
    connect(fftPlot_->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        if (fftPlot_->graph(0)->dataCount() < 2) return;
        const double lo   = fftPlot_->graph(0)->data()->begin()->key;
        const double hi   = (fftPlot_->graph(0)->data()->end() - 1)->key;
        const double span = hi - lo;
        if (newRange.size() > span * 1.01) {
            QSignalBlocker b(fftPlot_->xAxis);
            fftPlot_->xAxis->setRange(lo, hi);
            plotUserZoomed_ = false;
        } else {
            plotUserZoomed_ = true;
        }
    });

    // Y-axis clamp.
    connect(fftPlot_->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        constexpr double yMin = -130.0, yMax = 10.0;
        if (newRange.lower < yMin || newRange.upper > yMax) {
            QSignalBlocker b(fftPlot_->yAxis);
            fftPlot_->yAxis->setRange(std::max(newRange.lower, yMin),
                                      std::min(newRange.upper, yMax));
        }
    });

    // Double-click: reset zoom.
    connect(fftPlot_, &QCustomPlot::mouseDoubleClick, this, [this](QMouseEvent*) {
        plotUserZoomed_ = false;
        if (fftPlot_->graph(0)->dataCount() >= 2) {
            QSignalBlocker b(fftPlot_->xAxis);
            fftPlot_->xAxis->setRange(
                fftPlot_->graph(0)->data()->begin()->key,
                (fftPlot_->graph(0)->data()->end() - 1)->key);
        }
        fftPlot_->yAxis->setRange(-120.0, 0.0);
        fftPlot_->replot(QCustomPlot::rpQueuedReplot);
    });

    // Click on spectrum → tune first active demodulator's VFO.
    connect(fftPlot_, &QCustomPlot::mousePress, this, [this](QMouseEvent* event) {
        if (panels_.isEmpty()) return;
        const double mhz = fftPlot_->xAxis->pixelToCoord(event->pos().x());
        for (auto* p : panels_) {
            if (p->currentMode().isEmpty()) continue;
            p->tuneToMHz(mhz);
            break;
        }
    });
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::setActiveChannels(const QList<ChannelDescriptor>& channels) {
    activeChannels_ = channels;
    // Make gain vector match channel count if the caller hasn't supplied one yet.
    if (gainsDb_.size() != channels.size())
        gainsDb_.resize(channels.size());
}

void RadioMonitorPage::setChannelGains(const QVector<double>& gainsDb) {
    gainsDb_ = gainsDb;
    if (ctrl_ && ctrl_->isStreaming()) {
        for (int i = 0; i < gainsDb_.size(); ++i)
            ctrl_->setChannelGain(i, gainsDb_[i]);
    }
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::onDeviceReady() {
    if (startBtn_) startBtn_->setEnabled(true);
    const double sr = device_ ? device_->sampleRate() : 0.0;
    for (auto* p : panels_) p->setSampleRateHz(sr);
}

// ---------------------------------------------------------------------------
bool RadioMonitorPage::isStreaming() const {
    return ctrl_ && ctrl_->isStreaming();
}

double RadioMonitorPage::centerFreqMHz() const {
    return freqSpin_ ? freqSpin_->value() : kFreqDefaultMHz;
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::applyFrequency() {
    if (!controller_->isInitialized()) return;
    const double mhz = freqSpin_->value();

    // Push to all active RX channels (LimeSDR shares one PLL but we still
    // broadcast in case the device supports independent LO per channel).
    for (const auto& ch : activeChannels_)
        controller_->setFrequencyChannel(ch, mhz);

    if (ctrl_) ctrl_->setFftCenterFreq(mhz);
    for (auto* p : panels_) p->setCenterFreqMHz(mhz);

    if (centerLine_) {
        centerLine_->start->setCoords(mhz, -130.0);
        centerLine_->end->setCoords  (mhz,   10.0);
        fftPlot_->replot(QCustomPlot::rpQueuedReplot);
    }
    updateFilterBands();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::startStream() {
    if (isStreaming()) return;
    if (activeChannels_.isEmpty()) {
        QMessageBox::warning(this, "Stream", "No active RX channels selected.");
        return;
    }

    CombinedRxController::StreamConfig cfg;
    cfg.loFreqMHz = centerFreqMHz();
    cfg.channels  = activeChannels_;
    cfg.gainsDb.clear();
    for (int i = 0; i < activeChannels_.size(); ++i) {
        const double g = (i < gainsDb_.size()) ? gainsDb_[i] : 0.0;
        cfg.gainsDb.append(g);
    }

    const double sr        = device_ ? device_->sampleRate() : 0.0;
    const double centerHz  = cfg.loFreqMHz * 1e6;
    sessionTimestamp_          = FileNaming::currentTimestamp();
    const QString& timestamp   = sessionTimestamp_;
    const QString combinedSrc  = FileNaming::combinedSource(activeChannels_);

    // ── Raw/Combined recording paths ────────────────────────────────────────
    if (recordCheck_->isChecked() && !recordingSettings_.outputDir.isEmpty()) {
        QDir().mkpath(recordingSettings_.outputDir);
        cfg.rawFormat = recordingSettings_.rawFormat;
        const QString ext = recordingSettings_.rawExtension();

        if (recordingSettings_.recordRawPerChannel) {
            for (const auto& ch : activeChannels_) {
                cfg.rawPerChannelPaths.append(FileNaming::compose(
                    recordingSettings_.outputDir, timestamp,
                    FileNaming::perChannelSource(ch),
                    centerHz, sr, ext));
            }
        }
        if (recordingSettings_.recordCombined) {
            cfg.recordRaw = true;
            cfg.rawPath = FileNaming::compose(
                recordingSettings_.outputDir, timestamp,
                combinedSrc, centerHz, sr, ext);
        }
    }

    // Hand the session-wide context to every panel — each decides whether to
    // activate its own filtered/audio writers based on its local checkboxes.
    pushRecordingContextToPanels(timestamp, combinedSrc, centerHz);

    for (auto* p : panels_) {
        p->setCenterFreqMHz(cfg.loFreqMHz);
        p->setSampleRateHz(sr);
    }

    ctrl_->startStream(cfg);

    // Re-attach panel demodulators now that combinedPipeline_ is live.
    for (auto* p : panels_) p->onStreamStarted();

    startBtn_->setEnabled(false);
    stopBtn_->setEnabled(true);
    statusLabel_->setStyleSheet("color: #00cc44;");
    statusLabel_->setText("Streaming\u2026");

    emit streamStarted();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::stopStream() {
    if (!ctrl_) return;
    ctrl_->stopStream();
    stopBtn_->setEnabled(false);
}

void RadioMonitorPage::shutdown() {
    if (!ctrl_) return;
    for (auto* p : panels_) p->onStreamStopped();
    ctrl_->shutdown();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::onStreamErrorInternal(const QString& err) {
    if (statusLabel_) {
        statusLabel_->setStyleSheet("color: #ff4444;");
        statusLabel_->setText("Error: " + err);
    }
    emit errorOccurred(err);
}

void RadioMonitorPage::onStreamFinishedInternal() {
    for (auto* p : panels_) p->onStreamStopped();

    if (startBtn_) startBtn_->setEnabled(controller_->isInitialized());
    if (stopBtn_)  stopBtn_->setEnabled(false);
    if (statusLabel_) {
        statusLabel_->setStyleSheet("color: gray;");
        statusLabel_->setText("Idle");
    }
    emit streamStopped();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::addDemodulator() {
    if (panels_.size() >= kMaxDemods) {
        QMessageBox::information(this, "Demodulators",
            QString("Maximum %1 demodulators allowed.").arg(kMaxDemods));
        return;
    }

    const int slot = panels_.size();
    auto* panel = new DemodulatorPanel(slot, this);

    panel->attachToController(ctrl_);
    panel->setCenterFreqMHz(centerFreqMHz());
    panel->setSampleRateHz(device_ ? device_->sampleRate() : 0.0);

    connect(panel, &DemodulatorPanel::removeRequested,
            this,  &RadioMonitorPage::removeDemodulator);
    connect(panel, &DemodulatorPanel::vfoChanged,
            this,  [this](int, double, double) { updateFilterBands(); });

    // Insert before the stretch item at the end of panelsLayout_.
    const int insertAt = panelsLayout_->count() - 1;
    panelsLayout_->insertWidget(insertAt, panel);
    panels_.append(panel);

    // Companion green VFO band overlay on the spectrum.
    auto* band = new QCPItemRect(fftPlot_);
    band->setBrush(QBrush(QColor(0, 200, 80, 40)));
    band->setPen(QPen(QColor(0, 200, 80, 120), 1.0));
    band->topLeft->setType(QCPItemPosition::ptPlotCoords);
    band->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    band->setVisible(false);
    vfoBands_.append(band);

    // If stream is already running, attach immediately and ensure the panel
    // has the current session's recording context so its checkboxes work.
    if (isStreaming()) {
        const double centerHz     = centerFreqMHz() * 1e6;
        const QString combinedSrc = FileNaming::combinedSource(activeChannels_);
        const bool filteredAllowed =
            recordCheck_->isChecked() && recordingSettings_.recordFiltered
            && !recordingSettings_.outputDir.isEmpty();
        const bool audioAllowed =
            recordCheck_->isChecked() && recordingSettings_.recordAudio
            && !recordingSettings_.outputDir.isEmpty();
        panel->setRecordingContext(recordingSettings_.outputDir,
                                   sessionTimestamp_, combinedSrc, centerHz,
                                   filteredAllowed, audioAllowed);
        panel->onStreamStarted();
    }

    updateFilterBands();
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::removeDemodulator(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= panels_.size()) return;

    DemodulatorPanel* panel = panels_.takeAt(slotIndex);
    panel->detachFromController();
    panelsLayout_->removeWidget(panel);
    panel->deleteLater();

    if (slotIndex < vfoBands_.size()) {
        QCPItemRect* band = vfoBands_.takeAt(slotIndex);
        fftPlot_->removeItem(band);
    }

    // No renumbering — slot index stays stable per panel instance.
    updateFilterBands();
    fftPlot_->replot(QCustomPlot::rpQueuedReplot);
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::updateFilterBands() {
    if (!fftPlot_) return;

    for (int i = 0; i < panels_.size() && i < vfoBands_.size(); ++i) {
        auto* panel = panels_[i];
        auto* band  = vfoBands_[i];

        const QString mode = panel->currentMode();
        if (mode.isEmpty()) { band->setVisible(false); continue; }

        const double bwMHz = panel->currentBwMHz();
        const double vfo   = panel->vfoFreqMHz();
        band->topLeft->setCoords    (vfo - bwMHz, 10.0);
        band->bottomRight->setCoords(vfo + bwMHz, -130.0);
        band->setVisible(true);
    }
    fftPlot_->replot(QCustomPlot::rpQueuedReplot);
}

// ---------------------------------------------------------------------------
void RadioMonitorPage::onFftReady(FftFrame frame) {
    fftPlot_->graph(0)->setData(frame.freqMHz, frame.powerDb);

    if (centerLine_) {
        const double mhz = centerFreqMHz();
        centerLine_->start->setCoords(mhz, -130.0);
        centerLine_->end->setCoords  (mhz,   10.0);
    }

    if (!plotUserZoomed_ && !frame.freqMHz.isEmpty()) {
        QSignalBlocker b(fftPlot_->xAxis);
        fftPlot_->xAxis->setRange(frame.freqMHz.first(), frame.freqMHz.last());
    }

    fftDirty_ = true;
}

void RadioMonitorPage::replotIfDirty() {
    if (!fftDirty_ || !fftPlot_->isVisible()) return;
    fftDirty_ = false;
    fftPlot_->replot(QCustomPlot::rpQueuedReplot);
}

void RadioMonitorPage::updateMetrics() {
    for (auto* p : panels_) p->updateMetrics();
}

// ---------------------------------------------------------------------------
// Recording settings
// ---------------------------------------------------------------------------
void RadioMonitorPage::openRecordingSettings() {
    RecordingSettingsDialog dlg(recordingSettings_, this);
    if (dlg.exec() != QDialog::Accepted) return;

    recordingSettings_ = dlg.settings();
    saveRecordingSettings();

    // If the stream is live, re-broadcast the context so panels can react to
    // freshly enabled/disabled filtered/audio toggles.
    if (ctrl_ && ctrl_->isStreaming()) {
        const double centerHz     = centerFreqMHz() * 1e6;
        const QString combinedSrc = FileNaming::combinedSource(activeChannels_);
        pushRecordingContextToPanels(sessionTimestamp_, combinedSrc, centerHz);
    }
}

void RadioMonitorPage::pushRecordingContextToPanels(const QString& timestamp,
                                                    const QString& combinedSource,
                                                    double         centerFreqHz)
{
    const bool filteredAllowed =
        recordCheck_->isChecked() && recordingSettings_.recordFiltered
        && !recordingSettings_.outputDir.isEmpty();
    const bool audioAllowed =
        recordCheck_->isChecked() && recordingSettings_.recordAudio
        && !recordingSettings_.outputDir.isEmpty();

    if ((filteredAllowed || audioAllowed) && !recordingSettings_.outputDir.isEmpty())
        QDir().mkpath(recordingSettings_.outputDir);

    for (auto* p : panels_) {
        p->setRecordingContext(recordingSettings_.outputDir, timestamp,
                               combinedSource, centerFreqHz,
                               filteredAllowed, audioAllowed);
    }
}

void RadioMonitorPage::loadRecordingSettings() {
    QSettings s;
    const QString defaultDir = QDir(QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation)).filePath("stand_recordings");
    recordingSettings_.outputDir =
        s.value("recording/outputDir", defaultDir).toString();
    recordingSettings_.recordRawPerChannel =
        s.value("recording/rawPerChannel", false).toBool();
    recordingSettings_.recordCombined =
        s.value("recording/combined", false).toBool();
    recordingSettings_.recordFiltered =
        s.value("recording/filtered", false).toBool();
    recordingSettings_.recordAudio =
        s.value("recording/audio", false).toBool();
    recordingSettings_.rawFormat = static_cast<RecordingSettings::RawFormat>(
        s.value("recording/rawFormat",
                static_cast<int>(RecordingSettings::RawFormat::Float32)).toInt());
}

void RadioMonitorPage::saveRecordingSettings() const {
    QSettings s;
    s.setValue("recording/outputDir",     recordingSettings_.outputDir);
    s.setValue("recording/rawPerChannel", recordingSettings_.recordRawPerChannel);
    s.setValue("recording/combined",      recordingSettings_.recordCombined);
    s.setValue("recording/filtered",      recordingSettings_.recordFiltered);
    s.setValue("recording/audio",         recordingSettings_.recordAudio);
    s.setValue("recording/rawFormat",     static_cast<int>(recordingSettings_.rawFormat));
}
