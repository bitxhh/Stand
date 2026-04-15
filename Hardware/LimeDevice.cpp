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
// Supported sample rates
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

static int antennaForFrequency(double hz) {
    if (hz < 1.5e9) return LMS_PATH_LNAW;
    return LMS_PATH_LNAH;
}

static const char* antennaName(int path) {
    switch (path) {
        case LMS_PATH_LNAH: return "LNAH (>1.5 GHz)";
        case LMS_PATH_LNAL: return "LNAL (300 MHz–1.5 GHz)";
        case LMS_PATH_LNAW: return "LNAW (<1.5 GHz wideband)";
        default:            return "NONE";
    }
}

// Set analog LPF BW for one RX channel, protecting TIA from internal reset.
// LMS_SetLPFBW corrupts G_TIA_RFE — restore it after.
static void setLpfBwProtected(lms_device_t* h, int ch, double bw, int tiaValue) {
    LMS_WriteParam(h, LMS7_G_TIA_RFE, 3);
    if (LMS_SetLPFBW(h, LMS_CH_RX, ch, bw) != 0)
        LOG_WARN("LMS_SetLPFBW(ch=" + std::to_string(ch)
                 + ", bw=" + std::to_string(bw) + ") failed");
    LMS_WriteParam(h, LMS7_G_TIA_RFE, tiaValue);
}

// PGA compensation register (LMS7002M datasheet).
static int rccCtlForPga(int pga) {
    return static_cast<int>(
        (430.0 * std::pow(0.65, static_cast<double>(pga) / 10.0) - 110.35)
        / 20.4516 + 16.0);
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
        teardownAllStreams();
        LMS_Close(handle_);
        handle_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// IDevice: close — останавливает все стримы, закрывает хэндл, сбрасывает в Connected.
// Вызывается из UI-потока при закрытии DeviceDetailWindow.
// ---------------------------------------------------------------------------
void LimeDevice::close() {
    if (!handle_) {
        setState(DeviceState::Connected);
        return;
    }
    LOG_INFO("LimeDevice::close: " + serial_);
    teardownAllStreams();
    LMS_Close(handle_);
    handle_ = nullptr;
    setState(DeviceState::Connected);
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

    if (LMS_SetSampleRate(handle_, currentSampleRate_, 2) != 0)
        throwLime("LMS_SetSampleRate failed");

    // Enable both RX channels. TX ch0 must be enabled for calibration loopback.
    if (LMS_EnableChannel(handle_, LMS_CH_RX, 0, true) != 0)
        throwLime("LMS_EnableChannel RX0 failed");
    if (LMS_EnableChannel(handle_, LMS_CH_RX, 1, true) != 0)
        throwLime("LMS_EnableChannel RX1 failed");
    if (LMS_EnableChannel(handle_, LMS_CH_TX, 0, true) != 0)
        throwLime("LMS_EnableChannel TX0 failed");

    // Antenna and LO for both RX channels
    for (int ch = 0; ch < 2; ++ch) {
        const int ant = antennaForFrequency(currentFrequency_[ch]);
        if (LMS_SetAntenna(handle_, LMS_CH_RX, ch, ant) != 0)
            throwLime("LMS_SetAntenna RX" + std::to_string(ch) + " failed");
        LOG_INFO("Antenna RX" + std::to_string(ch) + ": "
                 + antennaName(ant) + " for "
                 + std::to_string(currentFrequency_[ch] / 1e6) + " MHz");

        setLpfBwProtected(handle_, ch, currentSampleRate_, kDefaultTia);

        if (LMS_SetLOFrequency(handle_, LMS_CH_RX, ch, currentFrequency_[ch]) != 0)
            throwLime("LMS_SetLOFrequency RX" + std::to_string(ch) + " failed");
    }

    // Warm up WinUSB I/O context for both RX channels (required before worker-thread start).
    for (int ch = 0; ch < 2; ++ch)
        setupStream({ChannelDescriptor::RX, ch});

    setState(DeviceState::Ready);
    LOG_INFO("LimeDevice ready: " + serial_);
}

// ---------------------------------------------------------------------------
// IDevice: calibrate
// ---------------------------------------------------------------------------
void LimeDevice::calibrateChannel(int idx) {
    const double calBw = std::max(currentSampleRate_, 2.5e6);
    const double savedGain = currentGainDb_[idx];

    if (savedGain > 0.0) {
        LOG_INFO("Calibrate ch" + std::to_string(idx)
                 + ": lowering gain to 0 dB (was " + std::to_string(savedGain) + " dB)");
        LMS_SetGaindB(handle_, LMS_CH_RX, idx, 0);
    }

    LOG_INFO("Calibrating ch" + std::to_string(idx)
             + " on " + serial_ + " at " + std::to_string(calBw) + " Hz");

    static constexpr int kMaxCalRetries = 3;
    bool ok = false;
    for (int attempt = 1; attempt <= kMaxCalRetries; ++attempt) {
        ok = (LMS_Calibrate(handle_, LMS_CH_RX, idx, calBw, 0) == 0);
        if (ok) break;
        if (attempt < kMaxCalRetries) {
            LOG_WARN("LMS_Calibrate ch" + std::to_string(idx)
                     + " attempt " + std::to_string(attempt) + " failed — retrying in 200 ms");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    // Всегда восстанавливаем усиление — LMS_Calibrate сбрасывает регистры LNA/PGA/TIA.
    // Даже при savedGain == 0 нужно выставить регистры в известное состояние.
    LOG_INFO("Calibrate ch" + std::to_string(idx)
             + ": restoring gain to " + std::to_string(savedGain) + " dB");
    LMS_SetGaindB(handle_, LMS_CH_RX, idx, static_cast<unsigned>(savedGain));

    // PGA RCC_CTL компенсация — аналогично setGain().
    uint16_t pgaVal = 0;
    LMS_ReadParam(handle_, LMS7_G_PGA_RBB, &pgaVal);
    LMS_WriteParam(handle_, LMS7_RCC_CTL_PGA_RBB, rccCtlForPga(static_cast<int>(pgaVal)));

    if (!ok)
        throwLime("LMS_Calibrate ch" + std::to_string(idx) + " failed after "
                  + std::to_string(kMaxCalRetries) + " attempts");

    setLpfBwProtected(handle_, idx, currentSampleRate_, kDefaultTia);
    LOG_INFO("Calibration ch" + std::to_string(idx) + " done: " + serial_);
}

void LimeDevice::calibrate(const QList<ChannelDescriptor>& channels) {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot calibrate — device not initialized");

    // LMS_Calibrate internally resets FPGA/streaming state, which can invalidate
    // any lms_stream_t handles we already hold. Teardown cleanly BEFORE calibration
    // (handles still valid), then rebuild the warm-up handles afterwards so the next
    // prepareStream/startStream sequence works against fresh handles.
    teardownAllStreams();

    // Determine which channels to calibrate.
    // Empty list = calibrate everything (default, backward-compatible).
    bool calibrateRx[2] = {true, true};
    bool calibrateTx    = true;
    if (!channels.isEmpty()) {
        calibrateRx[0] = calibrateRx[1] = false;
        calibrateTx = false;
        for (const auto& ch : channels) {
            if (ch.direction == ChannelDescriptor::RX && ch.channelIndex < 2)
                calibrateRx[ch.channelIndex] = true;
            else if (ch.direction == ChannelDescriptor::TX)
                calibrateTx = true;
        }
    }

    if (calibrateRx[0]) calibrateChannel(0);
    if (calibrateRx[1]) calibrateChannel(1);

    // TX calibration (non-fatal — some hardware units don't support it cleanly).
    if (calibrateTx) {
        const double calBw = std::max(currentSampleRate_, 2.5e6);
        LOG_INFO("Calibrating TX0 on " + serial_ + " at " + std::to_string(calBw) + " Hz");
        if (LMS_Calibrate(handle_, LMS_CH_TX, 0, calBw, 0) == 0) {
            if (LMS_SetLPFBW(handle_, LMS_CH_TX, 0, currentSampleRate_) != 0)
                LOG_WARN("LMS_SetLPFBW TX0 post-calibrate failed");
            LOG_INFO("TX0 calibration done: " + serial_);
        } else {
            LOG_WARN("LMS_Calibrate TX0 failed — continuing without TX calibration");
        }
    }

    // Re-setup both RX channels to restore the WinUSB I/O warmup context (same as init does).
    for (int ch = 0; ch < 2; ++ch)
        setupStream({ChannelDescriptor::RX, ch});
    LOG_INFO("Stream handles rebuilt after calibration: " + serial_);
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

    // Analog LPF tracks sample rate — RX (TIA-protected) + TX.
    for (int ch = 0; ch < 2; ++ch) {
        setLpfBwProtected(handle_, ch, hz, kDefaultTia);
        if (LMS_SetLPFBW(handle_, LMS_CH_TX, ch, hz) != 0)
            LOG_WARN("LMS_SetLPFBW TX ch" + std::to_string(ch) + " failed");
    }

    emit sampleRateChanged(hz);
    LOG_INFO("Sample rate set: " + std::to_string(hz) + " Hz");
}

double LimeDevice::sampleRate() const {
    return currentSampleRate_;
}

QList<double> LimeDevice::supportedSampleRates() const {
    return kSupportedRates;
}

// ---------------------------------------------------------------------------
// IDevice: setFrequency / frequency — single-channel backward compat
// ---------------------------------------------------------------------------
void LimeDevice::setFrequency(double hz) {
    setFrequency({ChannelDescriptor::RX, 0}, hz);
}

// ── Channel-aware ─────────────────────────────────────────────────────────
void LimeDevice::setFrequency(ChannelDescriptor ch, double hz) {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot set frequency — device not initialized");

    const int idx = ch.channelIndex;

    if (ch.direction == ChannelDescriptor::TX) {
        // TX: use TX antenna path (TX1 = first connector)
        if (LMS_SetAntenna(handle_, LMS_CH_TX, idx, LMS_PATH_TX1) != 0)
            LOG_WARN("LMS_SetAntenna TX" + std::to_string(idx) + " failed");
        if (LMS_SetLOFrequency(handle_, LMS_CH_TX, idx, hz) != 0)
            throwLime("LMS_SetLOFrequency TX failed");
        float_type actualLo = 0;
        if (LMS_GetLOFrequency(handle_, LMS_CH_TX, idx, &actualLo) == 0
            && std::abs(actualLo - hz) > 1e3)
            LOG_WARN("TX LO mismatch TX" + std::to_string(idx)
                     + ": requested " + std::to_string(hz / 1e6)
                     + " MHz, actual " + std::to_string(actualLo / 1e6) + " MHz");
        currentTxFrequency_[idx] = hz;
        LOG_INFO("TX" + std::to_string(idx) + " freq set: "
                 + std::to_string(hz / 1e6) + " MHz on " + serial_);
        return;
    }

    // RX path
    // Streaming retune runs through a synchronous handshake: the worker is
    // parked in checkPauseForRetune(), we perform LMS_StopStream → LO →
    // StartStream (to flush the FPGA FIFO), emit retuned() so DSP handlers
    // can reset their state, then release the worker.
    if (state_ == DeviceState::Streaming) {
        performStreamingRetune(idx, hz);
        return;
    }

    // Не стримим — применяем напрямую
    const int antenna = antennaForFrequency(hz);
    const int prevAntenna = antennaForFrequency(currentFrequency_[idx]);
    if (antenna != prevAntenna) {
        if (LMS_SetAntenna(handle_, LMS_CH_RX, idx, antenna) != 0)
            throwLime("LMS_SetAntenna failed");
        LOG_INFO("Antenna RX" + std::to_string(idx) + " changed: "
                 + antennaName(antenna) + " for " + std::to_string(hz / 1e6) + " MHz");
    }

    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, idx, hz) != 0)
        throwLime("LMS_SetLOFrequency failed");

    float_type actualLo = 0;
    if (LMS_GetLOFrequency(handle_, LMS_CH_RX, idx, &actualLo) == 0
        && std::abs(actualLo - hz) > 1e3) {
        LOG_WARN("LO mismatch RX" + std::to_string(idx)
                 + ": requested " + std::to_string(hz / 1e6)
                 + " MHz, actual " + std::to_string(actualLo / 1e6) + " MHz");
    }

    currentFrequency_[idx] = hz;
    emit retuned(ch, hz);
}

// ---------------------------------------------------------------------------
// IDevice: setGain / gain — single-channel backward compat
// ---------------------------------------------------------------------------
void LimeDevice::setGain(double dB) {
    setGain({ChannelDescriptor::RX, 0}, dB);
}

// ── Channel-aware ─────────────────────────────────────────────────────────
void LimeDevice::setGain(ChannelDescriptor ch, double dB) {
    if (state_ < DeviceState::Ready)
        throw LimeInitException("Cannot set gain — device not initialized");

    const int idx = ch.channelIndex;

    if (ch.direction == ChannelDescriptor::TX) {
        dB = std::clamp(dB, 0.0, kMaxTxGainDb);
        if (LMS_SetGaindB(handle_, LMS_CH_TX, idx, static_cast<unsigned>(dB)) != 0)
            throwLime("LMS_SetGaindB TX failed");
        currentTxGainDb_[idx] = dB;
        LOG_INFO("TX" + std::to_string(idx) + " gain set: "
                 + std::to_string(dB) + " dB on " + serial_);
        return;
    }

    // RX path
    dB = std::clamp(dB, 0.0, kMaxGainDb);
    if (LMS_SetGaindB(handle_, LMS_CH_RX, idx, static_cast<unsigned>(dB)) != 0)
        throwLime("LMS_SetGaindB failed");

    // PGA compensation: LMS_SetGaindB does not write RCC_CTL_PGA_RBB.
    // After calling LMS_SetGaindB(ch=idx), MAC points to channel idx.
    uint16_t pgaVal = 0;
    LMS_ReadParam(handle_, LMS7_G_PGA_RBB, &pgaVal);
    const int rcc = rccCtlForPga(static_cast<int>(pgaVal));
    LMS_WriteParam(handle_, LMS7_RCC_CTL_PGA_RBB, rcc);

    // LMS_SetGaindB may overwrite TIA — restore.
    LMS_WriteParam(handle_, LMS7_G_TIA_RFE, kDefaultTia);

    currentGainDb_[idx] = dB;
    LOG_INFO("Gain RX" + std::to_string(idx) + ": "
             + std::to_string(dB) + " dB  PGA=" + std::to_string(pgaVal)
             + " RCC=" + std::to_string(rcc) + " on " + serial_);
}

// ---------------------------------------------------------------------------
// Stream internals
// ---------------------------------------------------------------------------
void LimeDevice::setupStream(ChannelDescriptor ch) {
    if (streams_.count(ch)) teardownStream(ch);

    // Re-enable the channel before setup.
    const bool isTx = (ch.direction == ChannelDescriptor::TX);
    if (!isTx && LMS_EnableChannel(handle_, LMS_CH_TX, ch.channelIndex, true) != 0)
        LOG_WARN("LMS_EnableChannel TX" + std::to_string(ch.channelIndex) + " (pre-stream) failed");
    if (LMS_EnableChannel(handle_, isTx ? LMS_CH_TX : LMS_CH_RX, ch.channelIndex, true) != 0)
        throwLime("LMS_EnableChannel (pre-stream) failed for ch"
                  + std::to_string(ch.channelIndex));

    lms_stream_t stream{};
    stream.channel             = ch.channelIndex;
    stream.fifoSize            = 1024 * 1024;
    stream.throughputVsLatency = 1.0f;
    stream.isTx                = isTx;
    stream.dataFmt             = lms_stream_t::LMS_FMT_I16;

    if (LMS_SetupStream(handle_, &stream) != 0)
        throwLime("LMS_SetupStream failed for ch" + std::to_string(ch.channelIndex));

    streams_[ch] = stream;
    LOG_DEBUG("Stream ready: ch" + std::to_string(ch.channelIndex)
              + (isTx ? " TX" : " RX") + " on " + serial_);
}

void LimeDevice::teardownStream(ChannelDescriptor ch) {
    auto it = streams_.find(ch);
    if (it == streams_.end()) return;
    LMS_StopStream(&it->second);
    LMS_DestroyStream(handle_, &it->second);
    streams_.erase(it);
    LOG_DEBUG("Stream torn down: ch" + std::to_string(ch.channelIndex)
              + " on " + serial_);
}

void LimeDevice::teardownAllStreams() {
    for (auto& [ch, stream] : streams_) {
        LMS_StopStream(&stream);
        LMS_DestroyStream(handle_, &stream);
    }
    streams_.clear();
}

// ---------------------------------------------------------------------------
// IDevice: prepareStream — setup без старта, вызывается из UI-потока
// ---------------------------------------------------------------------------
void LimeDevice::prepareStream(ChannelDescriptor ch) {
    if (state_ < DeviceState::Ready) return;
    // Teardown если уже был setup от прошлого сеанса
    if (streams_.count(ch)) teardownStream(ch);
    setupStream(ch);
    LOG_DEBUG("prepareStream done: ch" + std::to_string(ch.channelIndex)
              + " on " + serial_);
}

// ---------------------------------------------------------------------------
// IDevice: single-channel stream — backward compat delegates to ch0
// ---------------------------------------------------------------------------
void LimeDevice::startStream() {
    startStream({ChannelDescriptor::RX, 0});
}

void LimeDevice::stopStream() {
    stopStream({ChannelDescriptor::RX, 0});
}

int LimeDevice::readBlock(int16_t* buffer, int count, int timeoutMs) {
    return readBlock({ChannelDescriptor::RX, 0}, buffer, count, timeoutMs);
}

// ---------------------------------------------------------------------------
// IDevice: channel-aware stream
// ---------------------------------------------------------------------------
void LimeDevice::startStream(ChannelDescriptor ch) {
    if (!streams_.count(ch)) {
        // Одиночный канал или prepareStream не вызывался — setup здесь.
        setupStream(ch);
    }
    // Если prepareStream уже вызывался из UI-потока, LMS_SetupStream
    // был выполнен до запуска любого воркера и не прервёт другой канал.

    auto& stream = streams_[ch];
    if (LMS_StartStream(&stream) != 0) {
        teardownStream(ch);
        throwLime("LMS_StartStream failed for ch" + std::to_string(ch.channelIndex));
    }
    setState(DeviceState::Streaming);
    LOG_INFO("LMS_StartStream OK: ch" + std::to_string(ch.channelIndex)
             + " on " + serial_);
}

void LimeDevice::stopStream(ChannelDescriptor ch) {
    teardownStream(ch);
    if (streams_.empty() && state_ == DeviceState::Streaming)
        setState(DeviceState::Ready);
    LOG_INFO("Stream stopped: ch" + std::to_string(ch.channelIndex)
             + " on " + serial_);
}

int LimeDevice::readBlock(ChannelDescriptor ch, int16_t* buffer, int count, int timeoutMs) {
    auto it = streams_.find(ch);
    if (it == streams_.end()) return -1;

    lms_stream_meta_t meta{};
    const int n = LMS_RecvStream(&it->second, buffer, count, &meta, timeoutMs);
    if (n > 0)
        lastTimestamp_[ch] = meta.timestamp;
    return n;
}

// ---------------------------------------------------------------------------
// Retune handshake — see LimeDevice.h for protocol documentation.
// ---------------------------------------------------------------------------
void LimeDevice::checkPauseForRetune(ChannelDescriptor ch) {
    if (ch.direction != ChannelDescriptor::RX) return;
    const int idx = ch.channelIndex;
    if (idx < 0 || idx >= 2) return;

    std::unique_lock lock(retuneMutex_);
    if (!retuneInProgress_[idx]) return;  // fast path — no retune pending

    workerParked_[idx] = true;
    retuneCv_.notify_all();
    retuneCv_.wait(lock, [this, idx] { return !retuneInProgress_[idx]; });
    workerParked_[idx] = false;
}

void LimeDevice::performStreamingRetune(int idx, double hz) {
    // Snapshot key data while not yet parked so we fail fast on a bad idx.
    auto it = streams_.find({ChannelDescriptor::RX, idx});
    if (it == streams_.end()) {
        // No active stream for this channel — apply directly.
        if (LMS_SetLOFrequency(handle_, LMS_CH_RX, idx, hz) != 0)
            throwLime("LMS_SetLOFrequency failed");
        currentFrequency_[idx] = hz;
        emit retuned({ChannelDescriptor::RX, idx}, hz);
        return;
    }

    // ── Phase 1: park the worker ─────────────────────────────────────────────
    {
        std::unique_lock lock(retuneMutex_);
        retuneInProgress_[idx] = true;
        retuneCv_.notify_all();
        // Wait for worker to enter checkPauseForRetune().
        // 1s is well above any sane LMS_RecvStream timeout — if we exceed it
        // the worker is likely blocked or dead; proceed anyway (best-effort).
        if (!retuneCv_.wait_for(lock, std::chrono::seconds(1),
                                [this, idx] { return workerParked_[idx]; })) {
            LOG_WARN("performStreamingRetune RX" + std::to_string(idx)
                     + ": worker did not park within 1s — proceeding");
        }
    }

    // ── Phase 2: mutate LMS state while worker is parked ─────────────────────
    // Antenna switch (safe to call even if value unchanged)
    const int ant = antennaForFrequency(hz);
    if (LMS_SetAntenna(handle_, LMS_CH_RX, idx, ant) != 0)
        LOG_WARN("LMS_SetAntenna (retune) failed: RX" + std::to_string(idx));
    else
        LOG_DEBUG("Antenna (retune) RX" + std::to_string(idx)
                  + " → " + antennaName(ant));

    // Stop → change LO → restart to flush FPGA FIFO.
    // LMS_StopStream / LMS_StartStream only affect this channel's stream;
    // the other channel (dual RX) keeps streaming uninterrupted.
    LMS_StopStream(&it->second);

    bool ok = true;
    if (LMS_SetLOFrequency(handle_, LMS_CH_RX, idx, hz) != 0) {
        LOG_WARN("LMS_SetLOFrequency (retune) failed: RX" + std::to_string(idx));
        ok = false;
    } else {
        float_type actualLo = 0;
        LMS_GetLOFrequency(handle_, LMS_CH_RX, idx, &actualLo);
        if (std::abs(actualLo - hz) > 1e3)
            LOG_WARN("LO mismatch (retune) RX" + std::to_string(idx)
                     + ": requested " + std::to_string(hz / 1e6)
                     + " MHz, actual " + std::to_string(actualLo / 1e6) + " MHz");
        else
            LOG_DEBUG("LO updated: RX" + std::to_string(idx)
                      + " → " + std::to_string(hz / 1e6) + " MHz");
    }

    if (LMS_StartStream(&it->second) != 0) {
        LOG_WARN("LMS_StartStream after retune failed: RX" + std::to_string(idx));
        ok = false;
    }

    if (ok)
        currentFrequency_[idx] = hz;

    // ── Phase 3: notify handlers BEFORE unparking worker ─────────────────────
    // DirectConnection: pipeline_->notifyRetune(hz) runs synchronously here,
    // so handler DSP state (FIR delay line, NCO phase, decimation counter)
    // is reset while the worker is still parked — race-free.
    emit retuned({ChannelDescriptor::RX, idx}, hz);

    // ── Phase 4: release the worker ──────────────────────────────────────────
    {
        std::lock_guard lock(retuneMutex_);
        retuneInProgress_[idx] = false;
    }
    retuneCv_.notify_all();
}

uint64_t LimeDevice::lastReadTimestamp(ChannelDescriptor ch) const {
    auto it = lastTimestamp_.find(ch);
    return (it != lastTimestamp_.end()) ? it->second : 0;
}

// ---------------------------------------------------------------------------
// IDevice: TX write block
// ---------------------------------------------------------------------------
int LimeDevice::writeBlock(ChannelDescriptor ch, const int16_t* buffer,
                            int count, int timeoutMs) {
    auto it = streams_.find(ch);
    if (it == streams_.end()) return -1;
    return LMS_SendStream(&it->second, buffer, count, nullptr,
                          static_cast<unsigned>(timeoutMs));
}

// ---------------------------------------------------------------------------
// createAdvancedWidget — reserved for Path B (direct LNA/TIA/PGA control).
// ---------------------------------------------------------------------------
QWidget* LimeDevice::createAdvancedWidget(QWidget* /*parent*/) {
    return nullptr;
}
