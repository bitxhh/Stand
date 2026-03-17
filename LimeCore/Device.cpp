#include "Device.h"
#include "LimeException.h"
#include "Logger.h"

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

    init_stream();

    is_init = true;
    LOG_INFO("Device initialized: " + serial);
}

void Device::init_stream() {
    LOG_DEBUG("Setting up RX stream for: " + serial);

    streamId.channel              = 0;
    streamId.fifoSize             = 1024 * 1024;
    streamId.throughputVsLatency  = 1.0f;
    streamId.isTx                 = false;
    streamId.dataFmt              = lms_stream_t::LMS_FMT_I16;

    if (LMS_SetupStream(device, &streamId) != 0)
        throwLimeError("LMS_SetupStream failed");

    LOG_DEBUG("RX stream ready for: " + serial);
}

// ---------------------------------------------------------------------------
// Stream  (blocking — run in a worker thread, not on the UI thread)
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

    if (LMS_SetSampleRate(device, sampleRateHz, 0) != 0)
        throwLimeError("LMS_SetSampleRate failed");

    currentSampleRate = sampleRateHz;
    LOG_INFO("Sample rate set: " + std::to_string(sampleRateHz) + " Hz");
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

    LOG_INFO("Calibrating RX channel on " + serial + " at " + std::to_string(sampleRateHz) + " Hz");

    if (LMS_Calibrate(device, LMS_CH_RX, 0, sampleRateHz, 0) != 0)
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
    , isRunning(other.isRunning)
    , streamId(other.streamId)
{
    std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));
    other.device = nullptr;
    std::memset(other.device_id, 0, sizeof(lms_info_str_t));
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device) {
            LMS_StopStream(&streamId);
            LMS_DestroyStream(device, &streamId);
            LMS_Close(device);
        }
        device            = other.device;
        serial            = std::move(other.serial);
        currentSampleRate = other.currentSampleRate;
        is_init           = other.is_init;
        isCalibrated      = other.isCalibrated;
        isRunning         = other.isRunning;
        streamId          = other.streamId;
        std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));

        other.device = nullptr;
        std::memset(other.device_id, 0, sizeof(lms_info_str_t));
    }
    return *this;
}

Device::~Device() {
    if (device != nullptr) {
        LOG_INFO("Closing device: " + serial);
        isRunning = false;
        LMS_StopStream(&streamId);
        LMS_DestroyStream(device, &streamId);
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
