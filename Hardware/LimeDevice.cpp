#include "LimeDevice.h"
#include "LimeException.h"
#include "Logger.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

// ---------------------------------------------------------------------------
// Supported sample rates — стабильны на LimeSDR USB и Mini по USB.
// Все дают целое D1 = round(SR / 250 000) для FM IF 250 кГц.
// ---------------------------------------------------------------------------
const QList<double> LimeDevice::kSupportedRates = {
    2'500'000,
    4'000'000,
    5'000'000,
    8'000'000,
    10'000'000,
    15'000'000,
    20'000'000,
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
[[noreturn]] static void throwLime(const std::string& ctx) {
    const char* msg = LMS_GetLastErrorMessage();
    const std::string detail = ctx + (msg ? std::string(": ") + msg : "");
    LOG_ERROR(detail);
    throw LimeException(detail);
}

static std::string parseSerial(const lms_info_str_t& infoStr) {
    std::string src(infoStr);
    std::istringstream ss(src);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(),
                    std::ranges::find_if(token, [](unsigned char c) {
                        return !std::isspace(c);
                    }));
        constexpr std::string_view prefix = "serial=";
        if (token.starts_with(prefix))
            return token.substr(prefix.size());
    }
    return {};
}

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------
LimeDevice::LimeDevice(const lms_info_str_t& id, QObject* parent)
    : IDevice(parent)
{
    std::memcpy(deviceId_, id, sizeof(lms_info_str_t));
    serial_ = parseSerial(id);
    LOG_DEBUG("LimeDevice created: " + serial_);
}

LimeDevice::~LimeDevice() {
    if (handle_) {
        LOG_INFO("LimeDevice closing: " + serial_);
        if (streamReady_) teardownStream();
        LMS_Close(handle_);
        handle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// IDevice: идентификация
// ---------------------------------------------------------------------------
QString LimeDevice::id() const {
    return QString::fromStdString(serial_);
}

QString LimeDevice::name() const {
    return QString("LimeSDR [%1]").arg(QString::fromStdString(serial_));
}

// ---------------------------------------------------------------------------
// IDevice: состояние
// ---------------------------------------------------------------------------
void LimeDevice::setState(DeviceState s) {
    if (state_ == s) return;
    state_ = s;
    emit stateChanged(s);
}

// ---------------------------------------------------------------------------
// IDevice: init
// ---------------------------------------------------------------------------
void LimeDevice::init() {
    LOG_INFO("LimeDevice init: " + serial_);
    setState(DeviceState::Connected);

    // Retry loop: после пересборки Windows держит USB-дескриптор.
    // До 6 попыток с нарастающей паузой (~9 с суммарно).
    static constexpr int kMaxRetries = 6;
    for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
        if (handle_) { LMS_Close(handle_); handle_ = nullptr; }

        if (LMS_Open(&handle_, deviceId_, nullptr) != 0) {
            if (attempt == kMaxRetries)
                throwLime("LMS_Open failed after " + std::to_string(kMaxRetries) + " attempts");
            const int ms = attempt * 500;
            LOG_WARN("LMS_Open attempt " + std::to_string(attempt)
                     + " failed — retry in " + std::to_string(ms) + " ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            continue;
        }

        if (LMS_Init(handle_) == 0) break;

        if (attempt == kMaxRetries) {
            LMS_Close(handle_); handle_ = nullptr;
            throwLime("LMS_Init failed after " + std::to_string(kMaxRetries) + " attempts");
        }
        const int ms = attempt * 500;
        LOG_WARN("LMS_Init attempt " + std::to_string(attempt)
                 + " failed — retry in " + std::to_string(ms) + " ms");
        LMS_Close(handle_); handle_ = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    if (LMS_EnableChannel(handle_, LMS_CH_RX, 0, true) != 0)
        throwLime("LMS_EnableChannel failed");

    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, 0, currentFrequency_) != 0)
        throwLime("LMS_SetLOFrequency failed");

    if (LMS_SetSampleRate(handle_, currentSampleRate_, 2) != 0)
        throwLime("LMS_SetSampleRate failed");

    setState(DeviceState::Ready);
    LOG_INFO("LimeDevice ready: " + serial_);
}

// ---------------------------------------------------------------------------
// IDevice: calibrate
// ---------------------------------------------------------------------------
void LimeDevice::calibrate() {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot calibrate — device not initialized");

    const double calBw = std::max(currentSampleRate_, 2.5e6);
    LOG_INFO("Calibrating " + serial_ + " at " + std::to_string(calBw) + " Hz");

    if (LMS_Calibrate(handle_, LMS_CH_RX, 0, calBw, 0) != 0)
        throwLime("LMS_Calibrate failed");

    LOG_INFO("Calibration done: " + serial_);
}

// ---------------------------------------------------------------------------
// IDevice: setSampleRate / sampleRate
// ---------------------------------------------------------------------------
void LimeDevice::setSampleRate(double hz) {
    if (hz <= 0.0)
        throw LimeParameterException("Sample rate must be > 0");

    LOG_INFO("setSampleRate " + std::to_string(hz) + " Hz on " + serial_);

    if (LMS_SetSampleRate(handle_, hz, 2) != 0)
        throwLime("LMS_SetSampleRate failed");

    currentSampleRate_ = hz;
    emit sampleRateChanged(hz);
    LOG_INFO("Sample rate set: " + std::to_string(hz) + " Hz");
}

double LimeDevice::sampleRate() const {
    if (!handle_) return currentSampleRate_;
    double hostRate = 0.0, rfRate = 0.0;
    if (LMS_GetSampleRate(handle_, LMS_CH_RX, 0, &hostRate, &rfRate) != 0)
        return currentSampleRate_;
    return hostRate;
}

QList<double> LimeDevice::supportedSampleRates() const {
    return kSupportedRates;
}

// ---------------------------------------------------------------------------
// IDevice: setFrequency / frequency
// ---------------------------------------------------------------------------
void LimeDevice::setFrequency(double hz) {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot set frequency — device not initialized");

    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, 0, hz) != 0)
        throwLime("LMS_SetLOFrequency failed");

    currentFrequency_ = hz;
}

double LimeDevice::frequency() const {
    return currentFrequency_;
}

// ---------------------------------------------------------------------------
// IDevice: setGain / gain (unified dB)
// ---------------------------------------------------------------------------
void LimeDevice::setGain(double dB) {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot set gain — device not initialized");

    dB = std::clamp(dB, 0.0, kMaxGainDb);

    if (LMS_SetGaindB(handle_, LMS_CH_RX, 0, static_cast<unsigned>(dB)) != 0)
        throwLime("LMS_SetGaindB failed");

    currentGainDb_ = dB;
    LOG_INFO("Gain set: " + std::to_string(dB) + " dB on " + serial_);
}

double LimeDevice::gain() const {
    return currentGainDb_;
}

// ---------------------------------------------------------------------------
// LimeSDR-специфично: раздельные ступени LNA / TIA / PGA
// Используется только внутри LimeAdvancedWidget.
// ---------------------------------------------------------------------------
void LimeDevice::setLimeGain(int lna, int tia, int pga) {
    if (state_ < DeviceState::Ready) return;

    constexpr double kLnaDb[] = {0.0, 5.0, 10.0, 15.0, 20.0, 25.5};
    constexpr double kTiaDb[] = {0.0, 9.0, 12.0};

    lna = std::clamp(lna, 0, 5);
    tia = std::clamp(tia, 0, 2);
    pga = std::clamp(pga, 0, 31);

    const double totalDb = kLnaDb[lna] + kTiaDb[tia] + static_cast<double>(pga);

    LOG_DEBUG("setLimeGain: LNA=" + std::to_string(lna)
              + " TIA=" + std::to_string(tia)
              + " PGA=" + std::to_string(pga)
              + " total=" + std::to_string(totalDb) + " dB");

    if (LMS_SetGaindB(handle_, LMS_CH_RX, 0, static_cast<unsigned>(totalDb)) != 0)
        throwLime("LMS_SetGaindB failed");

    currentGainDb_ = totalDb;
}

// ---------------------------------------------------------------------------
// IDevice: стрим
// ---------------------------------------------------------------------------
void LimeDevice::setupStream() {
    if (streamReady_) teardownStream();

    if (LMS_EnableChannel(handle_, LMS_CH_RX, 0, true) != 0)
        throwLime("LMS_EnableChannel (pre-stream) failed");

    streamId_                     = {};
    streamId_.channel             = 0;
    streamId_.fifoSize            = 1024 * 1024;
    streamId_.throughputVsLatency = 1.0f;
    streamId_.isTx                = false;
    streamId_.dataFmt             = lms_stream_t::LMS_FMT_I16;

    if (LMS_SetupStream(handle_, &streamId_) != 0)
        throwLime("LMS_SetupStream failed");

    streamReady_ = true;
    LOG_DEBUG("RX stream ready: " + serial_);
}

void LimeDevice::teardownStream() {
    if (!streamReady_) return;
    LMS_StopStream(&streamId_);
    LMS_DestroyStream(handle_, &streamId_);
    streamId_    = {};
    streamReady_ = false;
    LOG_DEBUG("RX stream torn down: " + serial_);
}

void LimeDevice::startStream() {
    setupStream();

    if (LMS_StartStream(&streamId_) != 0) {
        teardownStream();
        throwLime("LMS_StartStream failed");
    }
    setState(DeviceState::Streaming);
    LOG_INFO("LMS_StartStream OK: " + serial_);
}

void LimeDevice::stopStream() {
    teardownStream();
    if (state_ == DeviceState::Streaming)
        setState(DeviceState::Ready);
    LOG_INFO("Stream stopped: " + serial_);
}

int LimeDevice::readBlock(int16_t* buffer, int count, int timeoutMs) {
    return LMS_RecvStream(&streamId_, buffer, count, nullptr, timeoutMs);
}

// ---------------------------------------------------------------------------
// createAdvancedWidget — LNA / TIA / PGA controls
// ---------------------------------------------------------------------------
QWidget* LimeDevice::createAdvancedWidget(QWidget* parent) {
    auto* w      = new QWidget(parent);
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel("LimeSDR gain stages", w);
    title->setStyleSheet("font-weight: 600;");
    layout->addWidget(title);

    auto* hint = new QLabel("LNA → TIA → PGA  (max ~68.5 dB total)", w);
    hint->setStyleSheet("color: gray; font-size: 10px;");
    layout->addWidget(hint);

    // ── LNA ──────────────────────────────────────────────────────────────────
    auto* lnaRow = new QWidget(w);
    auto* lnaH   = new QHBoxLayout(lnaRow);
    lnaH->setContentsMargins(0, 0, 0, 0);
    auto* lnaLbl = new QLabel("LNA:", lnaRow);
    lnaLbl->setFixedWidth(36);
    lnaLbl->setToolTip("0/5/10/15/20/25.5 dB");
    auto* lnaSlider = new QSlider(Qt::Horizontal, lnaRow);
    lnaSlider->setRange(0, 5);
    lnaSlider->setValue(0);
    auto* lnaValLbl = new QLabel("0.0 dB", lnaRow);
    lnaValLbl->setFixedWidth(52);
    lnaH->addWidget(lnaLbl);
    lnaH->addWidget(lnaSlider);
    lnaH->addWidget(lnaValLbl);
    layout->addWidget(lnaRow);

    // ── TIA ──────────────────────────────────────────────────────────────────
    auto* tiaRow = new QWidget(w);
    auto* tiaH   = new QHBoxLayout(tiaRow);
    tiaH->setContentsMargins(0, 0, 0, 0);
    auto* tiaLbl = new QLabel("TIA:", tiaRow);
    tiaLbl->setFixedWidth(36);
    tiaLbl->setToolTip("0/9/12 dB");
    auto* tiaCombo = new QComboBox(tiaRow);
    tiaCombo->addItem("0 dB",  0);
    tiaCombo->addItem("9 dB",  1);
    tiaCombo->addItem("12 dB", 2);
    tiaH->addWidget(tiaLbl);
    tiaH->addWidget(tiaCombo);
    tiaH->addStretch();
    layout->addWidget(tiaRow);

    // ── PGA ──────────────────────────────────────────────────────────────────
    auto* pgaRow = new QWidget(w);
    auto* pgaH   = new QHBoxLayout(pgaRow);
    pgaH->setContentsMargins(0, 0, 0, 0);
    auto* pgaLbl = new QLabel("PGA:", pgaRow);
    pgaLbl->setFixedWidth(36);
    pgaLbl->setToolTip("0–31 dB (1 dB/шаг)");
    auto* pgaSlider = new QSlider(Qt::Horizontal, pgaRow);
    pgaSlider->setRange(0, 31);
    pgaSlider->setValue(0);
    auto* pgaValLbl = new QLabel("0 dB", pgaRow);
    pgaValLbl->setFixedWidth(52);
    pgaH->addWidget(pgaLbl);
    pgaH->addWidget(pgaSlider);
    pgaH->addWidget(pgaValLbl);
    layout->addWidget(pgaRow);

    layout->addStretch();

    // ── Wiring ───────────────────────────────────────────────────────────────
    constexpr double lnaDb[] = {0.0, 5.0, 10.0, 15.0, 20.0, 25.5};

    QObject::connect(lnaSlider, &QSlider::valueChanged, lnaValLbl, [=](int v) {
        lnaValLbl->setText(QString("%1 dB").arg(lnaDb[v], 0, 'f', 1));
    });
    QObject::connect(pgaSlider, &QSlider::valueChanged, pgaValLbl, [=](int v) {
        pgaValLbl->setText(QString("%1 dB").arg(v));
    });

    // Применяем при отпускании слайдера или смене TIA
    auto apply = [this, lnaSlider, tiaCombo, pgaSlider]() {
        setLimeGain(lnaSlider->value(),
                    tiaCombo->currentData().toInt(),
                    pgaSlider->value());
    };
    QObject::connect(lnaSlider, &QSlider::sliderReleased, w, apply);
    QObject::connect(pgaSlider, &QSlider::sliderReleased, w, apply);
    QObject::connect(tiaCombo, &QComboBox::currentIndexChanged, w, apply);

    return w;
}
