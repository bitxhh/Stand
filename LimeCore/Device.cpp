#include "Device.h"
#include "LimeException.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <sstream>
#include <fstream>

// ---------------------------------------------------------------------------
// Internal helper: throws LimeException with the last LimeSuite error string.
// Replaces the old error() that called exit(-10).
// ---------------------------------------------------------------------------
[[noreturn]] static void throwLimeError(const std::string& context) {
    const char* limeMsg = LMS_GetLastErrorMessage();
    const std::string detail = context + (limeMsg ? std::string(": ") + limeMsg : "");
    LOG_ERROR(detail);
    throw LimeException(detail);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void Device::init_device() {
    LOG_INFO("Opening device: " + serial);

    if (LMS_Open(&device, device_id, nullptr) != 0)
        throwLimeError("LMS_Open failed for " + serial);

    if (LMS_Init(device) != 0)
        throwLimeError("LMS_Init failed");

    if (LMS_EnableChannel(device, LMS_CH_RX, 0, true) != 0)
        throwLimeError("LMS_EnableChannel failed");

    if (LMS_SetLOFrequency(device, LMS_CH_RX, 0, 102e6) != 0)
        throwLimeError("LMS_SetLOFrequency failed");

    if (LMS_SetSampleRate(device, 30'000'000, 2) != 0)
        throwLimeError("LMS_SetSampleRate failed");

    // Set up the stream immediately so the streamId is valid.
    // StreamWorker will call LMS_StartStream / LMS_StopStream per session.
    // If the user changes SR later, StreamWorker calls teardown_stream() +
    // setup_stream() before starting to refresh USB packet sizing.
    setup_stream();

    is_init = true;
    LOG_INFO("Device initialized: " + serial);
}

void Device::setup_stream() {
    if (streamReady_) {
        LOG_WARN("setup_stream called while stream already exists — tearing down first");
        teardown_stream();
    }

    LOG_DEBUG("Setting up RX stream for: " + serial);

    // LMS_Calibrate internally resets parts of the RF chain.
    // Re-enable the channel here to guarantee it is active before SetupStream,
    // regardless of what happened between init_device() and now.
    if (LMS_EnableChannel(device, LMS_CH_RX, 0, true) != 0)
        throwLimeError("LMS_EnableChannel (pre-stream) failed");

    streamId                      = {};
    streamId.channel              = 0;
    streamId.fifoSize             = 1024 * 1024;
    streamId.throughputVsLatency  = 1.0f;
    streamId.isTx                 = false;
    streamId.dataFmt              = lms_stream_t::LMS_FMT_I16;

    if (LMS_SetupStream(device, &streamId) != 0)
        throwLimeError("LMS_SetupStream failed");

    streamReady_ = true;
    LOG_DEBUG("RX stream ready for: " + serial);
}

void Device::teardown_stream() {
    if (!streamReady_) return;
    LMS_StopStream(&streamId);
    LMS_DestroyStream(device, &streamId);
    streamId     = {};
    streamReady_ = false;
    LOG_DEBUG("RX stream torn down for: " + serial);
}

// ---------------------------------------------------------------------------
// Stream  (blocking — вызывать из отдельного потока, не из UI)
// ---------------------------------------------------------------------------
void Device::stream() {
    LOG_INFO("Starting RX stream: " + serial);

    std::ofstream out("capture_iq_i16.raw", std::ios::binary);
    if (!out)
        throw LimeStreamException("Cannot open output file capture_iq_i16.raw");

    if (LMS_StartStream(&streamId) != 0)
        throwLimeError("LMS_StartStream failed");

    isRunning = true;
    const auto deadline = std::chrono::high_resolution_clock::now() + std::chrono::seconds(5);

    while (isRunning && std::chrono::high_resolution_clock::now() < deadline) {
        const int samplesRead = LMS_RecvStream(&streamId, buffer, sampleCnt, nullptr, 1000);

        if (samplesRead > 0) {
            const std::size_t nValues = static_cast<std::size_t>(samplesRead) * 2;
            out.write(reinterpret_cast<const char*>(buffer), nValues * sizeof(int16_t));
            LOG_DEBUG("Received " + std::to_string(samplesRead) + " samples from " + serial);
        } else if (samplesRead < 0) {
            LOG_ERROR("LMS_RecvStream returned error for " + serial);
            break;
        }
    }

    LMS_StopStream(&streamId);
    isRunning = false;
    out.flush();

    LOG_INFO("RX stream stopped: " + serial);
}

// ---------------------------------------------------------------------------
// Sample rate
// ---------------------------------------------------------------------------
void Device::set_sample_rate(double sampleRateHz) {
    if (sampleRateHz <= 0.0)
        throw LimeParameterException("Sample rate must be > 0, got " + std::to_string(sampleRateHz));

    LOG_INFO("Setting sample rate to " + std::to_string(sampleRateHz) + " Hz on " + serial);

    // oversampling=2 matches init_device() and keeps the FPGA clock at a known
    // multiple of the host SR. oversampling=0 (auto) can pick large values at
    // low SRs that interfere with USB transfer sizing.
    if (LMS_SetSampleRate(device, sampleRateHz, 2) != 0)
        throwLimeError("LMS_SetSampleRate failed");

    currentSampleRate = sampleRateHz;
    LOG_INFO("Sample rate set: " + std::to_string(sampleRateHz) + " Hz");
}

void Device::set_center_frequency(double freqHz) {
    if (!is_init)
        throw LimeInitException("Cannot set frequency — device not initialized");

    if (freqHz <= 0.0)
        throw LimeParameterException("Center frequency must be > 0, got " + std::to_string(freqHz));

    LOG_INFO("Setting center frequency to " + std::to_string(freqHz) + " Hz on " + serial);

    if (LMS_SetLOFrequency(device, LMS_CH_RX, 0, freqHz) != 0)
        throwLimeError("LMS_SetLOFrequency failed");

    LOG_INFO("Center frequency set: " + std::to_string(freqHz) + " Hz");
}

void Device::set_normalized_gain(double gain) {
    if (!is_init)
        throw LimeInitException("Cannot set gain — device not initialized");

    gain = std::clamp(gain, 0.0, 1.0);

    LOG_INFO("Setting normalized RX gain to " + std::to_string(gain) + " on " + serial);

    if (LMS_SetNormalizedGain(device, LMS_CH_RX, 0, gain) != 0)
        throwLimeError("LMS_SetNormalizedGain failed");

    LOG_INFO("Normalized RX gain set: " + std::to_string(gain));
}

double Device::get_sample_rate() const {
    if (device == nullptr)
        throw LimeInitException("device handle is null — was init_device() called?");

    double hostRate = 0.0;
    double rfRate   = 0.0;

    if (LMS_GetSampleRate(device, LMS_CH_RX, 0, &hostRate, &rfRate) != 0)
        throwLimeError("LMS_GetSampleRate failed");

    return hostRate;
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------
void Device::calibrate(double sampleRateHz) {
    if (!is_init)
        throw LimeInitException("Cannot calibrate — device not initialized");

    // LimeSuite silently clamps calibration bandwidth to 2.5 MHz minimum.
    // Pass at least 2.5 MHz to avoid the confusing "out of range" console message.
    const double calBw = std::max(sampleRateHz, 2.5e6);

    LOG_INFO("Calibrating RX channel on " + serial
             + " at " + std::to_string(calBw) + " Hz"
             + (calBw > sampleRateHz ? " (clamped from " + std::to_string(sampleRateHz) + " Hz)" : ""));

    if (LMS_Calibrate(device, LMS_CH_RX, 0, calBw, 0) != 0)
        throwLimeError("LMS_Calibrate failed");

    isCalibrated = Calibrated;
    LOG_INFO("Calibration complete: " + serial);
}

// ---------------------------------------------------------------------------
// Constructors / destructor
// ---------------------------------------------------------------------------
Device::Device(const lms_info_str_t& id) {
    std::memcpy(device_id, id, sizeof(lms_info_str_t));
    serial = GetDeviceSerial(id);
    LOG_DEBUG("Device object created: " + serial);
}

Device::Device(Device&& other) noexcept
    : device(other.device)
    , serial(std::move(other.serial))
    , currentSampleRate(other.currentSampleRate)
    , is_init(other.is_init)
    , isCalibrated(other.isCalibrated)
    , isRunning(other.isRunning.load())
    , streamId(other.streamId)
{
    std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));
    other.device = nullptr;
    std::memset(other.device_id, 0, sizeof(lms_info_str_t));
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device) {
            if (streamReady_) {
                LMS_StopStream(&streamId);
                LMS_DestroyStream(device, &streamId);
            }
            LMS_Close(device);
        }
        device            = other.device;
        serial            = std::move(other.serial);
        currentSampleRate = other.currentSampleRate;
        is_init           = other.is_init;
        isCalibrated      = other.isCalibrated;
        isRunning         = other.isRunning.load();
        streamReady_      = other.streamReady_;
        streamId          = other.streamId;
        std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));

        other.device      = nullptr;
        other.streamReady_= false;
        std::memset(other.device_id, 0, sizeof(lms_info_str_t));
    }
    return *this;
}

Device::~Device() {
    if (device != nullptr) {
        LOG_INFO("Closing device: " + serial);
        isRunning = false;
        if (streamReady_) {
            LMS_StopStream(&streamId);
            LMS_DestroyStream(device, &streamId);
        }
        LMS_Close(device);
        device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Serial parsing
// ---------------------------------------------------------------------------
std::string Device::GetDeviceSerial(const lms_info_str_t infoStr) {
    std::string str(infoStr);
    std::istringstream ss(str);
    std::string token;

    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::ranges::find_if(token, [](unsigned char c) {
            return !std::isspace(c);
        }));
        constexpr std::string_view prefix = "serial=";
        if (token.starts_with(prefix))
            return token.substr(prefix.size());
    }
    return {};
}