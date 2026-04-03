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
#include <cmath>
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
// Internal: select RX antenna path for a given LO frequency.
// LMS_Init does not set an antenna — leaving it at PATH_NONE means the RX
// input is disconnected and only internal noise is received.
// ---------------------------------------------------------------------------
static int antennaForFrequency(double hz) {
    if (hz < 1.5e9) return LMS_PATH_LNAW;  // 0 – 1.5 GHz: wideband (FM, DAB, ADS-B …)
    return LMS_PATH_LNAH;                   // > 1.5 GHz: high-band
}

static const char* antennaName(int path) {
    switch (path) {
        case LMS_PATH_LNAH: return "LNAH (>1.5 GHz)";
        case LMS_PATH_LNAL: return "LNAL (300 MHz–1.5 GHz)";
        case LMS_PATH_LNAW: return "LNAW (<1.5 GHz wideband)";
        default:            return "NONE";
    }
}

// ---------------------------------------------------------------------------
// LMS_SetLPFBW internally modifies G_TIA_RFE.  ExtIO_LimeSDR works around
// this by forcing TIA=3 (max) before the call and restoring afterwards.
// ---------------------------------------------------------------------------
static void setLpfBwProtected(lms_device_t* h, double bw, int tiaValue) {
    // Temporarily set TIA to max so LMS_SetLPFBW's internal calibration works
    LMS_WriteParam(h, LMS7_G_TIA_RFE, 3);
    if (LMS_SetLPFBW(h, LMS_CH_RX, 0, bw) != 0)
        LOG_WARN("LMS_SetLPFBW(" + std::to_string(bw) + ") failed");
    // Restore user TIA
    LMS_WriteParam(h, LMS7_G_TIA_RFE, tiaValue);
}

// ---------------------------------------------------------------------------
// PGA compensation register from LMS7002M datasheet.
// ExtIO_LimeSDR always writes RCC_CTL_PGA_RBB alongside G_PGA_RBB to keep
// the PGA baseband filter response correct at every gain setting.
// ---------------------------------------------------------------------------
static int rccCtlForPga(int pga) {
    return static_cast<int>(
        (430.0 * std::pow(0.65, static_cast<double>(pga) / 10.0) - 110.35)
        / 20.4516 + 16.0);
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

    // ── Log hardware capabilities ────────────────────────────────────────────
    lms_range_t loRange{};
    if (LMS_GetLOFrequencyRange(handle_, LMS_CH_RX, &loRange) == 0)
        LOG_INFO("LO range: " + std::to_string(loRange.min / 1e6) + " – "
                 + std::to_string(loRange.max / 1e6) + " MHz");

    lms_range_t lpfRange{};
    if (LMS_GetLPFBWRange(handle_, LMS_CH_RX, &lpfRange) == 0)
        LOG_INFO("LPF BW range: " + std::to_string(lpfRange.min / 1e6) + " – "
                 + std::to_string(lpfRange.max / 1e6) + " MHz");

    // ── Init order matches ExtIO_LimeSDR (known-good): ─────────────────────
    //   SR → channels → antenna → LPF (TIA-protected) → gain → freq → [calibrate later]

    if (LMS_SetSampleRate(handle_, currentSampleRate_, 2) != 0)
        throwLime("LMS_SetSampleRate failed");

    // TX must be enabled before RX — LMS_Calibrate uses internal TX→RX loopback.
    if (LMS_EnableChannel(handle_, LMS_CH_RX, 0, true) != 0)
        throwLime("LMS_EnableChannel RX failed");
    if (LMS_EnableChannel(handle_, LMS_CH_TX, 0, true) != 0)
        throwLime("LMS_EnableChannel TX failed");

    const int antenna = antennaForFrequency(currentFrequency_);
    if (LMS_SetAntenna(handle_, LMS_CH_RX, 0, antenna) != 0)
        throwLime("LMS_SetAntenna failed");
    LOG_INFO("Antenna path: " + std::string(antennaName(antenna))
             + " for " + std::to_string(currentFrequency_ / 1e6) + " MHz");

    // Analog LPF BW = sample rate (ExtIO default).
    // LMS_SetLPFBW internally modifies G_TIA_RFE — must protect it.
    setLpfBwProtected(handle_, currentSampleRate_, kDefaultTia);
    LOG_INFO("Analog LPF BW set to " + std::to_string(currentSampleRate_) + " Hz");

    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, 0, currentFrequency_) != 0)
        throwLime("LMS_SetLOFrequency failed");

    // Warm up WinUSB I/O context from the main thread.
    // LMS_SetupStream must be called at least once on the main thread before
    // LMS_StartStream is called from the worker thread — otherwise WinUSB
    // ReadFile requests fail silently (ret=0) on Windows.
    setupStream();

    setState(DeviceState::Ready);
    LOG_INFO("LimeDevice ready: " + serial_);
}

// ---------------------------------------------------------------------------
// IDevice: calibrate
// ---------------------------------------------------------------------------
void LimeDevice::calibrate() {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot calibrate — device not initialized");

    // CalBW = max(SR, 2.5M) — matches ExtIO_LimeSDR behaviour.
    // ExtIO sets CalibrationBandwidth = max(LPFbandwidth, 2.5e6),
    // and LPF bandwidth = sample rate.
    const double calBw = std::max(currentSampleRate_, 2.5e6);

    // LimeSuite LMS_Calibrate can leave the LNA in internal loopback mode
    // when called at high gain with an antenna connected (MCU error 5).
    // The hardware recovers reliably if gain is set to 0 dB before calibration.
    const double savedGain = currentGainDb_;
    if (savedGain > 0.0) {
        LOG_INFO("Calibrate: lowering gain to 0 dB (was " + std::to_string(savedGain) + " dB)");
        LMS_SetGaindB(handle_, LMS_CH_RX, 0, 0);
    }

    LOG_INFO("Calibrating " + serial_ + " at " + std::to_string(calBw) + " Hz");

    const bool ok = (LMS_Calibrate(handle_, LMS_CH_RX, 0, calBw, 0) == 0);

    // Always restore gain, even if calibration failed
    if (savedGain > 0.0) {
        LOG_INFO("Calibrate: restoring gain to " + std::to_string(savedGain) + " dB");
        LMS_SetGaindB(handle_, LMS_CH_RX, 0, static_cast<unsigned>(savedGain));
    }

    if (!ok)
        throwLime("LMS_Calibrate failed");

    // LMS_Calibrate may reset LPF and TIA — restore both (TIA-protected).
    setLpfBwProtected(handle_, currentSampleRate_, kDefaultTia);

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

    // Analog LPF must track sample rate (ExtIO behaviour).
    setLpfBwProtected(handle_, hz, kDefaultTia);

    emit sampleRateChanged(hz);
    LOG_INFO("Sample rate set: " + std::to_string(hz)
             + " Hz  LPF=" + std::to_string(hz) + " Hz");
}

double LimeDevice::sampleRate() const {
    // LMS_GetSampleRate returns RF (ADC) rate, not the host-side rate.
    // Return the cached host rate that was set via setSampleRate().
    return currentSampleRate_;
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

    const int antenna = antennaForFrequency(hz);
    const int prevAntenna = antennaForFrequency(currentFrequency_);
    if (antenna != prevAntenna) {
        if (LMS_SetAntenna(handle_, LMS_CH_RX, 0, antenna) != 0)
            throwLime("LMS_SetAntenna failed");
        LOG_INFO("Antenna path changed: " + std::string(antennaName(antenna))
                 + " for " + std::to_string(hz / 1e6) + " MHz");
    }

    if (state_ == DeviceState::Streaming) {
        // During streaming: don't call LMS_SetLOFrequency from the UI thread —
        // it would contend with LMS_RecvStream on the same USB handle and freeze
        // the UI for up to one readBlock timeout. Instead, post the value as a
        // pending change; readBlock() picks it up and applies it on the worker thread.
        currentFrequency_ = hz;
        pendingFrequency_.store(hz);
        return;
    }

    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, 0, hz) != 0)
        throwLime("LMS_SetLOFrequency failed");

    // Readback actual LO — PLL may not lock at requested frequency
    float_type actualLo = 0;
    if (LMS_GetLOFrequency(handle_, LMS_CH_RX, 0, &actualLo) == 0
        && std::abs(actualLo - hz) > 1e3) {
        LOG_WARN("LO mismatch: requested " + std::to_string(hz / 1e6)
                 + " MHz, actual " + std::to_string(actualLo / 1e6) + " MHz");
    }

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

    // LMS_SetGaindB distributes gain across LNA+PGA but does not write
    // RCC_CTL_PGA_RBB — the PGA compensation register from the LMS7002M
    // datasheet.  Without it, PGA baseband filter shape degrades.
    // Read back the actual PGA value and write the matching RCC.
    uint16_t pgaVal = 0;
    LMS_ReadParam(handle_, LMS7_G_PGA_RBB, &pgaVal);
    const int rcc = rccCtlForPga(static_cast<int>(pgaVal));
    LMS_WriteParam(handle_, LMS7_RCC_CTL_PGA_RBB, rcc);

    // LMS_SetGaindB may also overwrite TIA — restore to default.
    LMS_WriteParam(handle_, LMS7_G_TIA_RFE, kDefaultTia);

    currentGainDb_ = dB;
    LOG_INFO("Gain set: " + std::to_string(dB) + " dB  PGA=" + std::to_string(pgaVal)
             + " RCC=" + std::to_string(rcc) + " TIA=" + std::to_string(kDefaultTia)
             + " on " + serial_);
}

double LimeDevice::gain() const {
    return currentGainDb_;
}


// ---------------------------------------------------------------------------
// IDevice: стрим
// ---------------------------------------------------------------------------
void LimeDevice::setupStream() {
    if (streamReady_) teardownStream();

    // Re-enable TX then RX — LMS_Calibrate resets parts of the RF chain.
    if (LMS_EnableChannel(handle_, LMS_CH_TX, 0, true) != 0)
        throwLime("LMS_EnableChannel TX (pre-stream) failed");

    if (LMS_EnableChannel(handle_, LMS_CH_RX, 0, true) != 0)
        throwLime("LMS_EnableChannel RX (pre-stream) failed");

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
    // Always teardown+setup before each start so USB packet sizing matches
    // the current sample rate (may have changed since init or last stream).
    // This mirrors the old StreamWorker pattern that fixed "TransferPacket
    // Read failed" on Windows.
    if (streamReady_) teardownStream();
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
    // Apply pending LO frequency change on the worker thread — safe because this
    // is the same thread that owns the LMS_RecvStream call, so no USB contention.
    const double freq = pendingFrequency_.exchange(kNoFreqPending);
    if (freq != kNoFreqPending) {
        if (LMS_SetLOFrequency(handle_, LMS_CH_RX, 0, freq) != 0) {
            LOG_WARN("LMS_SetLOFrequency (deferred) failed: " + serial_);
        } else {
            float_type actualLo = 0;
            LMS_GetLOFrequency(handle_, LMS_CH_RX, 0, &actualLo);
            if (std::abs(actualLo - freq) > 1e3)
                LOG_WARN("LO mismatch (deferred): requested " + std::to_string(freq / 1e6)
                         + " MHz, actual " + std::to_string(actualLo / 1e6) + " MHz");
            else
                LOG_DEBUG("LO updated to " + std::to_string(freq / 1e6) + " MHz");
        }
    }

    return LMS_RecvStream(&streamId_, buffer, count, nullptr, timeoutMs);
}

// ---------------------------------------------------------------------------
// createAdvancedWidget — зарезервировано для Пути B (прямое управление
// LNA/TIA/PGA через LMS_WriteParam). Пока возвращает nullptr.
// ---------------------------------------------------------------------------
QWidget* LimeDevice::createAdvancedWidget(QWidget* /*parent*/) {
    return nullptr;
}
