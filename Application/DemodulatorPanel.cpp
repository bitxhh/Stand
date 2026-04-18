#include "DemodulatorPanel.h"
#include "CombinedRxController.h"
#include "../Audio/FmAudioOutput.h"
#include "../Core/FileNaming.h"
#include "../DSP/AudioFileHandler.h"
#include "../DSP/BandpassHandler.h"
#include "../DSP/BaseDemodHandler.h"
#include "../DSP/DemodRegistry.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>

DemodulatorPanel::DemodulatorPanel(int slotIndex, QWidget* parent)
    : QWidget(parent)
    , slotIndex_(slotIndex)
{
    buildUi();
}

DemodulatorPanel::~DemodulatorPanel() {
    detachFromController();
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::buildUi() {
    setObjectName("DemodulatorPanel");
    setStyleSheet(
        "QWidget#DemodulatorPanel { "
        "background-color: #2a2a2a; border: 1px solid #444; border-radius: 4px; "
        "}");

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(6, 4, 6, 4);
    outer->setSpacing(2);

    // ── Row 1: slot label, mode, VFO, BW/De-emph, volume, remove ─────────────
    auto* row1  = new QWidget(this);
    auto* hlay1 = new QHBoxLayout(row1);
    hlay1->setContentsMargins(0, 0, 0, 0);

    auto* slotLabel = new QLabel(QString("Demod %1").arg(slotIndex_ + 1), row1);
    slotLabel->setStyleSheet("font-weight: 600; color: #cccccc;");
    slotLabel->setFixedWidth(60);

    auto* modeLabel = new QLabel("Mode:", row1);
    modeCombo_ = new QComboBox(row1);
    modeCombo_->addItem("Off", 0);
    modeCombo_->addItem("FM",  1);
    modeCombo_->addItem("AM",  2);
    modeCombo_->setFixedWidth(60);

    auto* vfoLabel = new QLabel("VFO (MHz):", row1);
    vfoSpin_ = new QDoubleSpinBox(row1);
    vfoSpin_->setRange(30.0, 3800.0);
    vfoSpin_->setDecimals(3);
    vfoSpin_->setSingleStep(0.025);
    vfoSpin_->setValue(102.0);
    vfoSpin_->setFixedWidth(100);
    vfoSpin_->setEnabled(false);

    fmBwLabel_ = new QLabel("BW (kHz):", row1);
    fmBwSpin_  = new QDoubleSpinBox(row1);
    fmBwSpin_->setRange(50.0, 250.0);
    fmBwSpin_->setDecimals(0);
    fmBwSpin_->setSingleStep(10.0);
    fmBwSpin_->setValue(150.0);
    fmBwSpin_->setFixedWidth(70);
    fmBwSpin_->setToolTip("WBFM filter bandwidth. Broadcast: 100–150 kHz");

    fmDeemphLabel_ = new QLabel("De-emph:", row1);
    fmDeemphCombo_ = new QComboBox(row1);
    fmDeemphCombo_->addItem("EU  50 \u00b5s", 50e-6);
    fmDeemphCombo_->addItem("US  75 \u00b5s", 75e-6);
    fmDeemphCombo_->setCurrentIndex(1);
    fmDeemphCombo_->setToolTip("Europe / Russia: 50 \u00b5s\nUSA / Japan: 75 \u00b5s");

    amBwLabel_ = new QLabel("BW (kHz):", row1);
    amBwSpin_  = new QDoubleSpinBox(row1);
    amBwSpin_->setRange(1.0, 20.0);
    amBwSpin_->setDecimals(1);
    amBwSpin_->setSingleStep(1.0);
    amBwSpin_->setValue(5.0);
    amBwSpin_->setFixedWidth(70);
    amBwSpin_->setToolTip("AM broadcast: 4–5 kHz, SSB: 2–3 kHz");

    auto* volLabel = new QLabel("Vol:", row1);
    volumeSlider_  = new QSlider(Qt::Horizontal, row1);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(80);
    volumeSlider_->setFixedWidth(80);
    volumeLabel_ = new QLabel("80%", row1);
    volumeLabel_->setFixedWidth(36);

    filteredCheck_ = new QCheckBox(tr("Rec filt"), row1);
    filteredCheck_->setToolTip(
        tr("Record filtered I/Q around this demodulator's VFO.\n"
           "Enable 'Filtered I/Q' in recording settings to activate."));
    filteredCheck_->setEnabled(false);

    audioCheck_ = new QCheckBox(tr("Rec audio"), row1);
    audioCheck_->setToolTip(
        tr("Record demodulated audio (mono float32 WAV).\n"
           "Enable 'Audio' in recording settings to activate."));
    audioCheck_->setEnabled(false);

    removeButton_ = new QPushButton("\u2715", row1);
    removeButton_->setFixedWidth(26);
    removeButton_->setToolTip("Remove demodulator");

    hlay1->addWidget(slotLabel);
    hlay1->addWidget(modeLabel);
    hlay1->addWidget(modeCombo_);
    hlay1->addSpacing(8);
    hlay1->addWidget(vfoLabel);
    hlay1->addWidget(vfoSpin_);
    hlay1->addSpacing(8);
    hlay1->addWidget(fmBwLabel_);
    hlay1->addWidget(fmBwSpin_);
    hlay1->addWidget(fmDeemphLabel_);
    hlay1->addWidget(fmDeemphCombo_);
    hlay1->addWidget(amBwLabel_);
    hlay1->addWidget(amBwSpin_);
    hlay1->addSpacing(8);
    hlay1->addWidget(volLabel);
    hlay1->addWidget(volumeSlider_);
    hlay1->addWidget(volumeLabel_);
    hlay1->addSpacing(8);
    hlay1->addWidget(filteredCheck_);
    hlay1->addWidget(audioCheck_);
    hlay1->addStretch();
    hlay1->addWidget(removeButton_);

    outer->addWidget(row1);

    // ── Row 2: status + SNR bar ──────────────────────────────────────────────
    auto* row2  = new QWidget(this);
    auto* hlay2 = new QHBoxLayout(row2);
    hlay2->setContentsMargins(0, 0, 0, 0);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet("color: gray; font-size: 11px;");

    levelLabel_ = new QLabel(
        "\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF", this);
    levelLabel_->setStyleSheet("color: gray; font-size: 10px;");

    hlay2->addWidget(statusLabel_, 1);
    hlay2->addWidget(levelLabel_);
    outer->addWidget(row2);

    // Initially hide mode-specific controls (mode = Off).
    fmBwLabel_->hide();      fmBwSpin_->hide();
    fmDeemphLabel_->hide();  fmDeemphCombo_->hide();
    amBwLabel_->hide();      amBwSpin_->hide();

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DemodulatorPanel::onModeChanged);

    connect(vfoSpin_, &QDoubleSpinBox::valueChanged, this, [this](double mhz) {
        if (demodHandler_) {
            const double offsetHz = (mhz - centerFreqMHz_) * 1e6;
            demodHandler_->setOffset(offsetHz);
        }
        emitVfoChanged();
    });

    connect(fmBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
        if (demodHandler_) demodHandler_->setParam("Bandwidth", bwKHz * 1000.0);
        emitVfoChanged();
    });
    connect(amBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
        if (demodHandler_) demodHandler_->setParam("Bandwidth", bwKHz * 1000.0);
        emitVfoChanged();
    });
    connect(fmDeemphCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (demodHandler_)
            demodHandler_->setParam("De-emphasis", fmDeemphCombo_->currentData().toDouble());
    });

    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int v) {
        volumeLabel_->setText(QString("%1%").arg(v));
        volume_ = static_cast<float>(v) / 100.0f;
        if (audioOut_) audioOut_->setVolume(volume_);
    });

    connect(removeButton_, &QPushButton::clicked, this, [this]() {
        emit removeRequested(slotIndex_);
    });

    connect(filteredCheck_, &QCheckBox::toggled,
            this, [this](bool) { updateFilteredRecording(); });
    connect(audioCheck_, &QCheckBox::toggled,
            this, [this](bool) { updateAudioRecording(); });
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::attachToController(CombinedRxController* ctrl) {
    if (ctrl_ == ctrl) return;
    detachFromController();
    ctrl_ = ctrl;
    // If mode is already active, rebuild the handler against the new controller.
    if (ctrl_ && modeCombo_ && modeCombo_->currentIndex() != 0)
        applyDemod();
}

void DemodulatorPanel::detachFromController() {
    teardownFilteredRecording();
    teardownDemod();
    ctrl_ = nullptr;
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::onModeChanged(int index) {
    const bool active = (index != 0);
    const bool isFm   = (index == 1);
    const bool isAm   = (index == 2);

    fmBwLabel_->setVisible(isFm);     fmBwSpin_->setVisible(isFm);
    fmDeemphLabel_->setVisible(isFm); fmDeemphCombo_->setVisible(isFm);
    amBwLabel_->setVisible(isAm);     amBwSpin_->setVisible(isAm);
    vfoSpin_->setEnabled(active);

    teardownDemod();
    if (statusLabel_) statusLabel_->setText("");

    if (active && ctrl_ && ctrl_->isStreaming())
        applyDemod();

    emitVfoChanged();
}

// ---------------------------------------------------------------------------
bool DemodulatorPanel::recordingDirValid() const {
    return !recordingDir_.isEmpty() && !recordingTimestamp_.isEmpty();
}

void DemodulatorPanel::setRecordingContext(const QString& dir,
                                           const QString& timestamp,
                                           const QString& combinedSource,
                                           double         centerFreqHz,
                                           bool           filteredAllowed,
                                           bool           audioAllowed)
{
    recordingDir_        = dir;
    recordingTimestamp_  = timestamp;
    combinedSource_      = combinedSource;
    recordingCenterHz_   = centerFreqHz;
    filteredAllowed_     = filteredAllowed;
    audioAllowed_        = audioAllowed;

    if (filteredCheck_) {
        filteredCheck_->setEnabled(filteredAllowed);
        if (!filteredAllowed && filteredCheck_->isChecked())
            filteredCheck_->setChecked(false);   // triggers teardown via toggled signal
    }
    if (audioCheck_) {
        audioCheck_->setEnabled(audioAllowed);
        if (!audioAllowed && audioCheck_->isChecked())
            audioCheck_->setChecked(false);
    }

    // Re-evaluate — paths may have become valid / invalid.
    updateFilteredRecording();
    updateAudioRecording();
}

void DemodulatorPanel::updateFilteredRecording() {
    teardownFilteredRecording();

    if (!ctrl_ || !ctrl_->isStreaming()) return;
    if (!filteredCheck_ || !filteredCheck_->isChecked()) return;
    if (!filteredAllowed_ || !recordingDirValid())      return;

    const double bwMHz = currentBwMHz();
    if (bwMHz <= 0.0) return;   // no active mode

    const double bwHz    = bwMHz * 1e6;
    const double bwKHz   = bwMHz * 1e3;
    const double vfoHz   = (vfoFreqMHz() - centerFreqMHz_) * 1e6;
    constexpr double kOutputSR = 250'000.0;   // BandpassExporter default

    const QString suffix = QStringLiteral("bp%1kHz").arg(bwKHz, 0, 'f', 0);
    const QString path = FileNaming::composeWithSuffix(
        recordingDir_, recordingTimestamp_, combinedSource_, suffix,
        recordingCenterHz_, kOutputSR, ".cf32");

    filteredHandler_ = new BandpassHandler(path, vfoHz, bwHz, kOutputSR);
    ctrl_->addExtraHandler(filteredHandler_);
}

void DemodulatorPanel::teardownFilteredRecording() {
    if (!filteredHandler_) return;
    if (ctrl_) ctrl_->removeExtraHandler(filteredHandler_);
    delete filteredHandler_;
    filteredHandler_ = nullptr;
}

void DemodulatorPanel::updateAudioRecording() {
    teardownAudioRecording();

    if (!ctrl_ || !ctrl_->isStreaming() || !demodHandler_) return;
    if (!audioCheck_ || !audioCheck_->isChecked()) return;
    if (!audioAllowed_ || !recordingDirValid())   return;

    const QString mode = currentMode().toLower();   // "fm" / "am"
    if (mode.isEmpty()) return;

    const QString suffix = QStringLiteral("%1%2").arg(mode).arg(slotIndex_);
    const QString dir    = recordingDir_;
    const QString ts     = recordingTimestamp_;
    const QString src    = combinedSource_;
    const double  cf     = recordingCenterHz_;

    auto builder = [dir, ts, src, suffix, cf](double sr) {
        return FileNaming::composeWithSuffix(dir, ts, src, suffix, cf, sr, ".wav");
    };

    audioHandler_ = new AudioFileHandler(builder, this);
    connect(demodHandler_, &BaseDemodHandler::audioReady,
            audioHandler_, &AudioFileHandler::push, Qt::QueuedConnection);
}

void DemodulatorPanel::teardownAudioRecording() {
    if (!audioHandler_) return;
    if (demodHandler_)
        disconnect(demodHandler_, &BaseDemodHandler::audioReady,
                   audioHandler_, &AudioFileHandler::push);
    audioHandler_->close();
    delete audioHandler_;
    audioHandler_ = nullptr;
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::applyDemod() {
    if (!ctrl_) return;
    const int mode = modeCombo_ ? modeCombo_->currentIndex() : 0;
    if (mode == 0) return;

    const QString modeStr = (mode == 1) ? QStringLiteral("FM") : QStringLiteral("AM");
    const double offsetHz = (vfoSpin_->value() - centerFreqMHz_) * 1e6;

    demodHandler_ = DemodRegistry::instance().create(modeStr, offsetHz, this);
    if (!demodHandler_) return;

    // Push current param values into the handler before it's added to pipeline.
    if (mode == 1) {
        demodHandler_->setParam("Bandwidth",   fmBwSpin_->value() * 1000.0);
        demodHandler_->setParam("De-emphasis", fmDeemphCombo_->currentData().toDouble());
    } else {
        demodHandler_->setParam("Bandwidth",   amBwSpin_->value() * 1000.0);
    }

    audioOut_ = new FmAudioOutput(this);
    audioOut_->setVolume(volume_);
    connect(audioOut_, &FmAudioOutput::statusChanged,
            this, [this](const QString& msg, bool isError) {
                if (!statusLabel_) return;
                statusLabel_->setStyleSheet(
                    isError ? "color: #ff4444; font-size: 11px;"
                            : "color: #00cc44; font-size: 11px;");
                statusLabel_->setText(msg);
            });

    connect(demodHandler_, &BaseDemodHandler::audioReady,
            audioOut_,     &FmAudioOutput::push, Qt::QueuedConnection);

    ctrl_->addExtraHandler(demodHandler_);

    if (statusLabel_) {
        statusLabel_->setStyleSheet("color: gray; font-size: 11px;");
        statusLabel_->setText(modeStr + ": waiting for first audio block\u2026");
    }

    // Hook up audio recording now that demodHandler_ emits audioReady.
    updateAudioRecording();
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::teardownDemod() {
    // Close audio WAV before the demod handler goes away (audioReady would
    // otherwise deliver to a deleted object via queued connection).
    teardownAudioRecording();

    if (ctrl_ && demodHandler_)
        ctrl_->removeExtraHandler(demodHandler_);

    delete demodHandler_;
    demodHandler_ = nullptr;

    if (audioOut_) {
        audioOut_->teardown();
        delete audioOut_;
        audioOut_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::setCenterFreqMHz(double mhz) {
    centerFreqMHz_ = mhz;
    if (vfoSpin_) {
        const double half = (sampleRateHz_ > 0 ? sampleRateHz_ / 2.0 : 2e6) / 1e6;
        const double curMHz = vfoSpin_->value();
        QSignalBlocker b(vfoSpin_);
        vfoSpin_->setRange(mhz - half, mhz + half);
        // Keep VFO near the new center when it would otherwise fall outside band.
        const double clamped = std::clamp(curMHz, mhz - half, mhz + half);
        vfoSpin_->setValue(clamped);
    }
    if (demodHandler_) {
        const double offsetHz = (vfoSpin_->value() - centerFreqMHz_) * 1e6;
        demodHandler_->setOffset(offsetHz);
    }
    emitVfoChanged();
}

void DemodulatorPanel::setSampleRateHz(double sr) {
    sampleRateHz_ = sr;
    if (vfoSpin_) {
        const double half = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
        const double cur  = vfoSpin_->value();
        QSignalBlocker b(vfoSpin_);
        vfoSpin_->setRange(centerFreqMHz_ - half, centerFreqMHz_ + half);
        vfoSpin_->setValue(std::clamp(cur, centerFreqMHz_ - half, centerFreqMHz_ + half));
    }
    emitVfoChanged();
}

void DemodulatorPanel::tuneToMHz(double mhz) {
    if (!vfoSpin_) return;
    const double half = (sampleRateHz_ > 0 ? sampleRateHz_ / 2.0 : 2e6) / 1e6;
    const double clamped = std::clamp(mhz, centerFreqMHz_ - half, centerFreqMHz_ + half);
    vfoSpin_->setValue(clamped);   // triggers valueChanged → offset update + vfoChanged signal
}

double DemodulatorPanel::vfoFreqMHz() const {
    return vfoSpin_ ? vfoSpin_->value() : centerFreqMHz_;
}

QString DemodulatorPanel::currentMode() const {
    if (!modeCombo_) return {};
    const int mode = modeCombo_->currentIndex();
    if (mode == 1) return QStringLiteral("FM");
    if (mode == 2) return QStringLiteral("AM");
    return {};
}

double DemodulatorPanel::currentBwMHz() const {
    if (!modeCombo_) return 0.0;
    const int mode = modeCombo_->currentIndex();
    if (mode == 1 && fmBwSpin_) return fmBwSpin_->value() / 1000.0;
    if (mode == 2 && amBwSpin_) return amBwSpin_->value() / 1000.0;
    return 0.0;
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::emitVfoChanged() {
    emit vfoChanged(slotIndex_, vfoFreqMHz(), currentBwMHz());
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::onStreamStarted() {
    // Re-attach the demodulator to the (newly created) combined pipeline.
    if (ctrl_ && modeCombo_ && modeCombo_->currentIndex() != 0)
        applyDemod();

    // Filtered recording is independent of the demod — attach if the user
    // had its checkbox on (audio recording is handled inside applyDemod()).
    updateFilteredRecording();
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::onStreamStopped() {
    if (statusLabel_) statusLabel_->setText("");
    if (levelLabel_)
        levelLabel_->setText("\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF");

    // Close any recording handlers so their files are finalized. Ownership
    // of filteredHandler_ / audioHandler_ lives with the panel — the controller
    // only holds raw pointers in extraHandlers_ and does not delete them.
    teardownAudioRecording();
    teardownFilteredRecording();

    // Handler will be cleaned up by CombinedRxController::performCleanup via the
    // extraHandlers_ list. Drop our pointer so we don't double-delete.
    demodHandler_ = nullptr;
    if (audioOut_) {
        audioOut_->teardown();
        delete audioOut_;
        audioOut_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
void DemodulatorPanel::updateMetrics() {
    if (!levelLabel_ || !demodHandler_) return;

    const double snr   = demodHandler_->snrDb();
    const double ifRms = demodHandler_->ifRms();

    constexpr int    kBlocks = 10;
    constexpr double kSnrMax = 15.0;
    const int filled = static_cast<int>(
        std::clamp(snr / kSnrMax * kBlocks, 0.0, static_cast<double>(kBlocks)));

    QString bar;
    bar.reserve(kBlocks);
    for (int i = 0; i < kBlocks; ++i)
        bar += (i < filled) ? QChar(0x25AE) : QChar(0x25AF);

    const QString color = (snr > 6.0)  ? "#00cc44"
                        : (snr > 2.0)  ? "#ffaa00"
                        :                "#888888";

    levelLabel_->setStyleSheet(
        QString("color: %1; font-size: 10px;").arg(color));
    levelLabel_->setText(
        QString("%1  SNR %2 dB  IF %3")
            .arg(bar)
            .arg(snr,   0, 'f', 1)
            .arg(ifRms, 0, 'f', 3));
}
