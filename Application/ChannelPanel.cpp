#include "ChannelPanel.h"
#include "ClassifierController.h"
#include "../Hardware/DeviceController.h"
#include "../Core/IDevice.h"
#include "qcustomplot.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>

// ---------------------------------------------------------------------------
static QColor channelColor(const ChannelDescriptor& ch) {
    if (ch.direction == ChannelDescriptor::TX)
        return {220, 60, 60};   // red
    if (ch.channelIndex == 0)
        return {0, 180, 255};   // blue  — RX0
    if (ch.channelIndex == 1)
        return {255, 160, 0};   // amber — RX1
    return {0, 200, 80};        // green — others
}

// ---------------------------------------------------------------------------
ChannelPanel::ChannelPanel(const Config& cfg,
                           IDevice*          device,
                           DeviceController* controller,
                           QWidget*          parent)
    : QWidget(parent)
    , cfg_(cfg)
    , device_(device)
    , controller_(controller)
{
    const QString name = cfg_.displayName.toLower();
    rawPath_ = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
               + "/capture_" + name + "_i16.raw";
    wavPath_ = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)
               + "/station_" + name + ".wav";
    buildUi();
}

ChannelPanel::~ChannelPanel() {
    delete classifierCtrl_;
}

// ---------------------------------------------------------------------------
void ChannelPanel::setRxController(RxController* ctrl) {
    if (ctrl_ == ctrl) return;

    if (ctrl_)
        ctrl_->disconnect(this);

    delete classifierCtrl_;
    classifierCtrl_ = nullptr;

    ctrl_ = ctrl;
    if (!ctrl_) return;

    connect(ctrl_, &RxController::fftReady,    this, &ChannelPanel::onFftReady);
    connect(ctrl_, &RxController::demodStatus, this,
            [this](const QString& msg, bool isError) {
                if (!demodStatusLabel_) return;
                demodStatusLabel_->setStyleSheet(
                    isError ? "color: #ff4444; font-size: 11px;"
                            : "color: #00cc44; font-size: 11px;");
                demodStatusLabel_->setText(msg);
            });

    classifierCtrl_ = new ClassifierController(ctrl_, this);

    if (classifierLabel_) {
        connect(classifierCtrl_, &ClassifierController::classificationReady,
                this, [this](const QString& type, double confidence) {
                    classifierLabel_->setStyleSheet("color: #00cc44; font-size: 11px;");
                    classifierLabel_->setText(
                        QString("%1  %2%").arg(type).arg(confidence * 100.0, 0, 'f', 0));
                });
        connect(classifierCtrl_, &ClassifierController::classifierStarted, this, [this]() {
            classifierLabel_->setStyleSheet("color: #00cc44; font-size: 11px;");
            classifierLabel_->setText("Running");
        });
        connect(classifierCtrl_, &ClassifierController::classifierStopped, this, [this]() {
            classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");
            classifierLabel_->setText("Stopped");
        });
        connect(classifierCtrl_, &ClassifierController::classifierError,
                this, [this](const QString& msg) {
                    classifierLabel_->setStyleSheet("color: #ff4444; font-size: 11px;");
                    classifierLabel_->setText("Error: " + msg);
                });
    }
}

// ---------------------------------------------------------------------------
void ChannelPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // ── Title ─────────────────────────────────────────────────────────────────
    const QColor col = channelColor(cfg_.channel);
    auto* titleLabel = new QLabel(cfg_.displayName, this);
    titleLabel->setStyleSheet(
        QString("font-weight: 600; font-size: 11px; color: %1;").arg(col.name()));
    layout->addWidget(titleLabel);

    // ── FFT plot ──────────────────────────────────────────────────────────────
    fftPlot_ = new QCustomPlot(this);
    fftPlot_->setMinimumHeight(180);
    setupFftPlot();
    layout->addWidget(fftPlot_, 1);

    // ── Frequency row ─────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        auto* label = new QLabel("Freq (MHz):", row);
        label->setFixedWidth(72);

        freqSpinBox_ = new QDoubleSpinBox(row);
        freqSpinBox_->setRange(cfg_.freqMinMHz, cfg_.freqMaxMHz);
        freqSpinBox_->setDecimals(3);
        freqSpinBox_->setSingleStep(0.1);
        freqSpinBox_->setValue(cfg_.freqDefaultMHz);
        freqSpinBox_->setFixedWidth(110);

        freqSlider_ = new QSlider(Qt::Horizontal, row);
        freqSlider_->setRange(static_cast<int>(cfg_.freqMinMHz),
                              static_cast<int>(cfg_.freqMaxMHz));
        freqSlider_->setValue(static_cast<int>(cfg_.freqDefaultMHz));

        auto* applyBtn = new QPushButton("Apply", row);
        applyBtn->setFixedWidth(60);

        hlay->addWidget(label);
        hlay->addWidget(freqSpinBox_);
        hlay->addWidget(freqSlider_, 1);
        hlay->addWidget(applyBtn);
        layout->addWidget(row);

        // Slider ↔ spinbox sync
        connect(freqSlider_, &QSlider::valueChanged, this, [this](int v) {
            QSignalBlocker b(freqSpinBox_);
            freqSpinBox_->setValue(static_cast<double>(v));
        });
        connect(freqSpinBox_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            {
                QSignalBlocker b(freqSlider_);
                freqSlider_->setValue(static_cast<int>(v));
            }
            if (demodVfoSpin_) {
                const double sr   = device_->sampleRate();
                const double half = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
                demodVfoSpin_->setRange(v - half, v + half);
            }
            updateFilterBand(modeCombo_ && modeCombo_->currentIndex() != 0);
        });

        connect(applyBtn,     &QPushButton::clicked,            this, &ChannelPanel::applyFrequency);
        connect(freqSlider_,  &QSlider::sliderReleased,         this, &ChannelPanel::applyFrequency);
        connect(freqSpinBox_, &QDoubleSpinBox::editingFinished, this, &ChannelPanel::applyFrequency);
    }

    // ── Gain row ──────────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        auto* label = new QLabel("Gain:", row);
        label->setFixedWidth(36);

        gainSlider_ = new QSlider(Qt::Horizontal, row);
        gainSlider_->setRange(0, 68);
        gainSlider_->setValue(0);
        gainSlider_->setToolTip(cfg_.displayName + " total gain 0\u201368 dB");

        gainValueLabel_ = new QLabel("0 dB", row);
        gainValueLabel_->setFixedWidth(52);

        hlay->addWidget(label);
        hlay->addWidget(gainSlider_);
        hlay->addWidget(gainValueLabel_);
        layout->addWidget(row);

        connect(gainSlider_, &QSlider::valueChanged, this, [this](int v) {
            gainValueLabel_->setText(QString("%1 dB").arg(v));
        });
        connect(gainSlider_, &QSlider::sliderReleased, this, [this]() {
            if (controller_->isInitialized()) {
                controller_->setGainChannel(cfg_.channel, gainSlider_->value());
                emit gainChanged(cfg_.channel, gainSlider_->value());
            }
        });
    }

    // ── Demodulator row ───────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        auto* modeLabel = new QLabel("Mode:", row);

        modeCombo_ = new QComboBox(row);
        modeCombo_->addItem("Off", 0);
        modeCombo_->addItem("FM",  1);
        modeCombo_->addItem("AM",  2);
        modeCombo_->setFixedWidth(60);

        fmBwLabel_ = new QLabel("BW (kHz):", row);
        fmBwSpin_  = new QDoubleSpinBox(row);
        fmBwSpin_->setRange(50.0, 250.0);
        fmBwSpin_->setDecimals(0);
        fmBwSpin_->setSingleStep(10.0);
        fmBwSpin_->setValue(100.0);
        fmBwSpin_->setFixedWidth(70);
        fmBwSpin_->setToolTip("One-sided filter bandwidth (kHz).\nWBFM broadcast: 100\u2013150 kHz");

        fmDeemphLabel_ = new QLabel("De-emph:", row);
        fmDeemphCombo_ = new QComboBox(row);
        fmDeemphCombo_->addItem("EU  50 \u00b5s", 50e-6);
        fmDeemphCombo_->addItem("US  75 \u00b5s", 75e-6);
        fmDeemphCombo_->setToolTip("Europe / Russia: 50 \u00b5s\nUSA, Canada, Japan: 75 \u00b5s");

        amBwLabel_ = new QLabel("BW (kHz):", row);
        amBwSpin_  = new QDoubleSpinBox(row);
        amBwSpin_->setRange(1.0, 20.0);
        amBwSpin_->setDecimals(1);
        amBwSpin_->setSingleStep(1.0);
        amBwSpin_->setValue(5.0);
        amBwSpin_->setFixedWidth(70);
        amBwSpin_->setToolTip("Audio bandwidth (kHz).\nAM broadcast: 4\u20135 kHz\nSSB / amateur: 2\u20133 kHz");

        volLabel_       = new QLabel("Vol:", row);
        demodVolSlider_ = new QSlider(Qt::Horizontal, row);
        demodVolSlider_->setRange(0, 100);
        demodVolSlider_->setValue(80);
        demodVolSlider_->setFixedWidth(80);
        demodVolLabel_  = new QLabel("80%", row);
        demodVolLabel_->setFixedWidth(34);

        hlay->addWidget(modeLabel);
        hlay->addWidget(modeCombo_);
        hlay->addSpacing(8);
        hlay->addWidget(fmBwLabel_);
        hlay->addWidget(fmBwSpin_);
        hlay->addWidget(fmDeemphLabel_);
        hlay->addWidget(fmDeemphCombo_);
        hlay->addWidget(amBwLabel_);
        hlay->addWidget(amBwSpin_);
        hlay->addSpacing(8);
        hlay->addWidget(volLabel_);
        hlay->addWidget(demodVolSlider_);
        hlay->addWidget(demodVolLabel_);
        hlay->addStretch();
        layout->addWidget(row);

        // Initially hide mode-specific + shared controls (mode = Off)
        fmBwLabel_->hide();     fmBwSpin_->hide();
        fmDeemphLabel_->hide(); fmDeemphCombo_->hide();
        amBwLabel_->hide();     amBwSpin_->hide();
        volLabel_->hide();
        demodVolSlider_->hide(); demodVolLabel_->hide();

        connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ChannelPanel::onModeChanged);

        connect(demodVolSlider_, &QSlider::valueChanged, this, [this](int v) {
            demodVolLabel_->setText(QString("%1%").arg(v));
            if (ctrl_) ctrl_->setVolume(static_cast<float>(v) / 100.0f);
        });
        connect(fmBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
            updateFilterBand(modeCombo_->currentIndex() != 0);
            if (ctrl_) ctrl_->setDemodParam("Bandwidth", bwKHz * 1000.0);
        });
        connect(amBwSpin_, &QDoubleSpinBox::valueChanged, this, [this](double bwKHz) {
            updateFilterBand(modeCombo_->currentIndex() != 0);
            if (ctrl_) ctrl_->setDemodParam("Bandwidth", bwKHz * 1000.0);
        });
        connect(fmDeemphCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
            if (ctrl_) ctrl_->setDemodParam("De-emphasis",
                                            fmDeemphCombo_->currentData().toDouble());
        });
    }

    // ── VFO row ───────────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        auto* vfoLabel = new QLabel("VFO:", row);
        vfoLabel->setToolTip("Tune demodulator to a station within the capture band.\n"
                             "Click anywhere on the spectrum to jump here.");

        demodVfoSpin_ = new QDoubleSpinBox(row);
        demodVfoSpin_->setRange(cfg_.freqMinMHz, cfg_.freqMaxMHz);
        demodVfoSpin_->setDecimals(3);
        demodVfoSpin_->setSingleStep(0.1);
        demodVfoSpin_->setValue(cfg_.freqDefaultMHz);
        demodVfoSpin_->setFixedWidth(110);
        demodVfoSpin_->setEnabled(false);
        demodVfoSpin_->setToolTip("Station frequency (MHz). Edit or click the spectrum.");

        auto* hint = new QLabel("\u2190 click spectrum to tune", row);
        hint->setStyleSheet("color: gray; font-size: 10px;");

        hlay->addWidget(vfoLabel);
        hlay->addWidget(demodVfoSpin_);
        hlay->addWidget(hint);
        hlay->addStretch();
        layout->addWidget(row);

        connect(demodVfoSpin_, &QDoubleSpinBox::valueChanged, this, [this](double vfoMHz) {
            updateFilterBand(modeCombo_ && modeCombo_->currentIndex() != 0);
            if (ctrl_) {
                const double offsetHz = (vfoMHz - freqSpinBox_->value()) * 1e6;
                ctrl_->setDemodOffset(offsetHz);
            }
        });
    }

    // ── Signal level indicator ────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        demodLevelLabel_ = new QLabel(
            "\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF", row);
        demodLevelLabel_->setStyleSheet("color: gray; font-size: 10px;");
        demodLevelLabel_->setToolTip(
            "Signal level after demodulation.\n"
            "Gray = weak / no station\n"
            "Green = good signal");

        hlay->addWidget(demodLevelLabel_);
        hlay->addStretch();
        layout->addWidget(row);
    }

    // ── Demod status label ────────────────────────────────────────────────────
    demodStatusLabel_ = new QLabel(this);
    demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");
    layout->addWidget(demodStatusLabel_);

    // ── Recording row ─────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        recordCheckBox_ = new QCheckBox("Record raw", row);
        recordCheckBox_->setToolTip("Record raw I/Q int16 samples to .raw file");

        wavCheckBox_ = new QCheckBox("Export WAV", row);
        wavCheckBox_->setToolTip("Export filtered I/Q to .wav file");

        auto* recSettingsBtn = new QPushButton("Settings\u2026", row);
        recSettingsBtn->setFixedWidth(72);
        connect(recSettingsBtn, &QPushButton::clicked, this, &ChannelPanel::openRecordSettings);

        hlay->addWidget(recordCheckBox_);
        hlay->addSpacing(12);
        hlay->addWidget(wavCheckBox_);
        hlay->addSpacing(12);
        hlay->addWidget(recSettingsBtn);
        hlay->addStretch();
        layout->addWidget(row);
    }

    // ── Classifier row ────────────────────────────────────────────────────────
    {
        auto* row  = new QWidget(this);
        auto* hlay = new QHBoxLayout(row);
        hlay->setContentsMargins(0, 0, 0, 0);

        classifierCheck_ = new QCheckBox("Classifier", row);
        classifierCheck_->setToolTip(
            "Enable Python NN signal classifier.\n"
            "Requires Python + classifier_service.py.\n"
            "Path configured via STAND_PYTHON_EXE / STAND_CLASSIFIER_SCRIPT env vars\n"
            "or defaults: python3 / Python/classifier_service.py");

        classifierLabel_ = new QLabel("Unavailable", row);
        classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");

        hlay->addWidget(classifierCheck_);
        hlay->addSpacing(8);
        hlay->addWidget(classifierLabel_);
        hlay->addStretch();
        layout->addWidget(row);

        connect(classifierCheck_, &QCheckBox::toggled, this, [this](bool on) {
            if (!classifierCtrl_) return;
            if (on) {
                const QString pyExe  = qEnvironmentVariable("STAND_PYTHON_EXE", "python3");
                const QString script = qEnvironmentVariable("STAND_CLASSIFIER_SCRIPT",
                                           QCoreApplication::applicationDirPath()
                                           + "/../Python/classifier_service.py");
                if (classifierLabel_) {
                    classifierLabel_->setStyleSheet("color: gray; font-size: 11px;");
                    classifierLabel_->setText("Starting\u2026");
                }
                classifierCtrl_->start(pyExe, script);
            } else {
                classifierCtrl_->stop();
            }
        });
    }
}

// ---------------------------------------------------------------------------
void ChannelPanel::setupFftPlot() {
    fftPlot_->addGraph();
    fftPlot_->graph(0)->setPen(QPen(channelColor(cfg_.channel), 1.2));

    fftPlot_->xAxis->setLabel(
        QString("Frequency (MHz) \u2014 %1").arg(cfg_.displayName));
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

    // Center frequency marker (red dashed)
    centerLine_ = new QCPItemLine(fftPlot_);
    centerLine_->setPen(QPen(QColor(255, 60, 60), 1.2, Qt::DashLine));
    centerLine_->setAntialiased(false);
    centerLine_->start->setCoords(cfg_.freqDefaultMHz, -130.0);
    centerLine_->end->setCoords  (cfg_.freqDefaultMHz,   10.0);
    centerLine_->start->setType(QCPItemPosition::ptPlotCoords);
    centerLine_->end->setType  (QCPItemPosition::ptPlotCoords);

    // Filter band rect (green, semi-transparent; visible when demod active)
    vfoBand_ = new QCPItemRect(fftPlot_);
    vfoBand_->setBrush(QBrush(QColor(0, 200, 80, 40)));
    vfoBand_->setPen(QPen(QColor(0, 200, 80, 120), 1.0));
    vfoBand_->topLeft->setType(QCPItemPosition::ptPlotCoords);
    vfoBand_->bottomRight->setType(QCPItemPosition::ptPlotCoords);
    vfoBand_->topLeft->setCoords    (cfg_.freqDefaultMHz - 0.1, 10.0);
    vfoBand_->bottomRight->setCoords(cfg_.freqDefaultMHz + 0.1, -130.0);
    vfoBand_->setVisible(false);

    // X-axis zoom: clamp to data range, track user zoom flag
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

    // Y-axis: clamp to valid dBFS range
    connect(fftPlot_->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& newRange) {
        constexpr double yMin = -130.0, yMax = 10.0;
        if (newRange.lower < yMin || newRange.upper > yMax) {
            QSignalBlocker b(fftPlot_->yAxis);
            fftPlot_->yAxis->setRange(std::max(newRange.lower, yMin),
                                      std::min(newRange.upper, yMax));
        }
    });

    // Double-click: reset zoom to full capture band
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

    // Click on spectrum → tune VFO to that frequency (demod must be active)
    connect(fftPlot_, &QCustomPlot::mousePress, this, [this](QMouseEvent* event) {
        if (!modeCombo_ || modeCombo_->currentIndex() == 0) return;
        if (!demodVfoSpin_) return;
        const double clickedMHz = fftPlot_->xAxis->pixelToCoord(event->pos().x());
        const double lo   = freqSpinBox_->value();
        const double sr   = device_->sampleRate();
        const double half = (sr > 0 ? sr / 2.0 : 2e6) / 1e6;
        demodVfoSpin_->setValue(std::clamp(clickedMHz, lo - half, lo + half));
    });
}

// ---------------------------------------------------------------------------
void ChannelPanel::updateFilterBand(bool visible) {
    if (!vfoBand_) return;
    vfoBand_->setVisible(visible);
    if (visible) {
        const double vfoMHz = demodVfoSpin_ ? demodVfoSpin_->value() : freqSpinBox_->value();
        double bwMHz = 100.0 / 1000.0;
        const int mode = modeCombo_ ? modeCombo_->currentIndex() : 0;
        if      (mode == 1 && fmBwSpin_)  bwMHz = fmBwSpin_->value() / 1000.0;
        else if (mode == 2 && amBwSpin_)  bwMHz = amBwSpin_->value() / 1000.0;
        vfoBand_->topLeft->setCoords    (vfoMHz - bwMHz, 10.0);
        vfoBand_->bottomRight->setCoords(vfoMHz + bwMHz, -130.0);
    }
    fftPlot_->replot(QCustomPlot::rpQueuedReplot);
}

// ---------------------------------------------------------------------------
void ChannelPanel::onModeChanged(int index) {
    const bool active = (index != 0);
    const bool isFm   = (index == 1);
    const bool isAm   = (index == 2);

    fmBwLabel_->setVisible(isFm);     fmBwSpin_->setVisible(isFm);
    fmDeemphLabel_->setVisible(isFm); fmDeemphCombo_->setVisible(isFm);
    amBwLabel_->setVisible(isAm);     amBwSpin_->setVisible(isAm);
    volLabel_->setVisible(active);
    demodVolSlider_->setVisible(active);
    demodVolLabel_->setVisible(active);
    if (demodVfoSpin_) demodVfoSpin_->setEnabled(active);

    updateFilterBand(active);

    if (!ctrl_) return;
    ctrl_->teardownDemod();
    if (demodStatusLabel_) demodStatusLabel_->setText("");

    if (!active || !ctrl_->isStreaming()) return;

    const double offsetHz = demodVfoSpin_
                            ? (demodVfoSpin_->value() - freqSpinBox_->value()) * 1e6
                            : 0.0;
    const QString mode = isFm ? "FM" : (isAm ? "AM" : "");
    if (mode.isEmpty()) return;

    ctrl_->setDemodMode(mode, offsetHz);
    if (isFm) {
        ctrl_->setDemodParam("Bandwidth",   fmBwSpin_->value() * 1000.0);
        ctrl_->setDemodParam("De-emphasis", fmDeemphCombo_->currentData().toDouble());
    } else {
        ctrl_->setDemodParam("Bandwidth",   amBwSpin_->value() * 1000.0);
    }
    ctrl_->setVolume(static_cast<float>(demodVolSlider_->value()) / 100.0f);
    if (demodStatusLabel_) {
        demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");
        demodStatusLabel_->setText(mode + ": waiting for first audio block\u2026");
    }
}

// ---------------------------------------------------------------------------
void ChannelPanel::onFftReady(FftFrame frame) {
    fftPlot_->graph(0)->setData(frame.freqMHz, frame.powerDb);

    if (centerLine_) {
        const double mhz = freqSpinBox_ ? freqSpinBox_->value() : cfg_.freqDefaultMHz;
        centerLine_->start->setCoords(mhz, -130.0);
        centerLine_->end->setCoords  (mhz,   10.0);
    }

    if (!plotUserZoomed_ && !frame.freqMHz.isEmpty()) {
        QSignalBlocker b(fftPlot_->xAxis);
        fftPlot_->xAxis->setRange(frame.freqMHz.first(), frame.freqMHz.last());
    }

    fftDirty_ = true;
}

// ---------------------------------------------------------------------------
void ChannelPanel::replotIfDirty() {
    if (!fftDirty_ || !fftPlot_->isVisible()) return;
    fftDirty_ = false;
    fftPlot_->replot(QCustomPlot::rpQueuedReplot);
}

// ---------------------------------------------------------------------------
void ChannelPanel::applyFrequency() {
    if (!controller_->isInitialized()) return;
    const double mhz = freqSpinBox_->value();
    controller_->setFrequencyChannel(cfg_.channel, mhz);
    if (ctrl_) ctrl_->setFftCenterFreq(mhz);
    if (centerLine_) {
        centerLine_->start->setCoords(mhz, -130.0);
        centerLine_->end->setCoords  (mhz,   10.0);
        fftPlot_->replot(QCustomPlot::rpQueuedReplot);
    }
    emit frequencyApplied(cfg_.channel, mhz);
}

// ---------------------------------------------------------------------------
void ChannelPanel::applyDemodParams() {
    if (!ctrl_) return;
    const int mode = modeCombo_ ? modeCombo_->currentIndex() : 0;
    if (mode == 0) return;

    const double offsetHz = demodVfoSpin_
                            ? (demodVfoSpin_->value() - freqSpinBox_->value()) * 1e6
                            : 0.0;
    const QString modeStr = (mode == 1) ? "FM" : "AM";
    ctrl_->setDemodMode(modeStr, offsetHz);

    if (mode == 1) {
        ctrl_->setDemodParam("Bandwidth",   fmBwSpin_->value() * 1000.0);
        ctrl_->setDemodParam("De-emphasis", fmDeemphCombo_->currentData().toDouble());
    } else {
        ctrl_->setDemodParam("Bandwidth",   amBwSpin_->value() * 1000.0);
    }
    ctrl_->setVolume(static_cast<float>(demodVolSlider_->value()) / 100.0f);

    if (demodStatusLabel_) {
        demodStatusLabel_->setStyleSheet("color: gray; font-size: 11px;");
        demodStatusLabel_->setText(modeStr + ": waiting for first audio block\u2026");
    }
}

// ---------------------------------------------------------------------------
RxController::StreamConfig ChannelPanel::buildStreamConfig() const {
    RxController::StreamConfig cfg;
    cfg.loFreqMHz = freqSpinBox_ ? freqSpinBox_->value() : cfg_.freqDefaultMHz;
    cfg.recordRaw = recordCheckBox_ && recordCheckBox_->isChecked();
    cfg.rawPath   = rawPath_;
    cfg.exportWav = wavCheckBox_   && wavCheckBox_->isChecked();
    cfg.wavPath   = wavPath_;
    cfg.wavOffset = wavOffset_;
    cfg.wavBw     = wavBw_;

    if (modeCombo_) {
        const int mode = modeCombo_->currentIndex();
        if      (mode == 1) cfg.demodMode = "FM";
        else if (mode == 2) cfg.demodMode = "AM";
        if (mode > 0 && demodVfoSpin_)
            cfg.demodOffsetHz = (demodVfoSpin_->value() - cfg.loFreqMHz) * 1e6;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
void ChannelPanel::onStreamStarted() {
    // Start/Stop buttons are owned by DDW; nothing panel-specific here.
}

void ChannelPanel::onStreamStopped() {
    if (demodStatusLabel_) demodStatusLabel_->setText("");
    if (demodLevelLabel_)
        demodLevelLabel_->setText("\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF\u25AF");
}

// ---------------------------------------------------------------------------
void ChannelPanel::updateMetrics() {
    if (!demodLevelLabel_) return;
    if (!ctrl_ || !ctrl_->demodHandler()) return;

    const double snr   = ctrl_->snrDb();
    const double ifRms = ctrl_->ifRms();

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

    demodLevelLabel_->setStyleSheet(
        QString("color: %1; font-size: 10px;").arg(color));
    demodLevelLabel_->setText(
        QString("%1  SNR %2 dB  IF %3")
            .arg(bar)
            .arg(snr,   0, 'f', 1)
            .arg(ifRms, 0, 'f', 3));
}

void ChannelPanel::openRecordSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Recording Settings — " + cfg_.displayName);
    auto* form = new QVBoxLayout(&dlg);

    // Raw file path
    auto* rawRow = new QHBoxLayout;
    rawRow->addWidget(new QLabel("Raw file:"));
    auto* rawEdit = new QLineEdit(rawPath_);
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
    auto* okBtn     = new QPushButton("OK");
    auto* cancelBtn = new QPushButton("Cancel");
    buttons->addStretch();
    buttons->addWidget(okBtn);
    buttons->addWidget(cancelBtn);
    form->addLayout(buttons);

    connect(okBtn,     &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        rawPath_   = rawEdit->text();
        wavPath_   = wavEdit->text();
        wavOffset_ = offsetSpin->value();
        wavBw_     = bwSpin->value();
    }
}
