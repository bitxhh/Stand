//
// Created by ilya1 on 11.12.2025.
//

#include "Device.h"
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

std::string Device::GetDeviceSerial(const lms_info_str_t infoStr) {
    std::string str(infoStr);
    std::istringstream ss(str);
    std::string token;

    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));

        const std::string prefix = "serial=";
        if (token.compare(0, prefix.size(), prefix) == 0) {
            return token.substr(prefix.size());
        }
    }

    return {};
}

Device::Device(const lms_info_str_t& id) : device(nullptr) {
    std::memcpy(device_id, id, sizeof(lms_info_str_t));
    serial = GetDeviceSerial(id);
}

void Device::init_device() {
    if (device != nullptr) {
        return;
    }

    if (LMS_Open(&device, device_id, nullptr) != 0) {
        device = nullptr;
        throw std::runtime_error("Failed to open device");
    }

    if (LMS_Init(device) != 0) {
        LMS_Close(device);
        device = nullptr;
        throw std::runtime_error("Failed to initialize device");
    }

    apply_channel_config();
}

void Device::calibrate(double sampleRateHz) {
    init_device();

    // Keep the analog filters and calibration bandwidth aligned with the current sample rate
    // similarly to the sequence used in the ExtIO LimeSDR reference implementation.
    set_sample_rate(sampleRateHz);

    const double bandwidth = sampleRateHz / 2.0;
    if (LMS_SetLPFBW(device, false, rxChannel, bandwidth) != 0) {
        throw std::runtime_error("Failed to set RX LPF bandwidth before calibration");
    }

    if (LMS_SetLPFBW(device, true, txChannel, bandwidth) != 0) {
        throw std::runtime_error("Failed to set TX LPF bandwidth before calibration");
    }

    // Use normalized gains to keep the calibration procedure stable across boards.
    if (LMS_SetNormalizedGain(device, false, rxChannel, 0.70) != 0) {
        throw std::runtime_error("Failed to set RX gain before calibration");
    }

    if (LMS_SetNormalizedGain(device, true, txChannel, 0.50) != 0) {
        throw std::runtime_error("Failed to set TX gain before calibration");
    }

    if (LMS_Calibrate(device, false, rxChannel, bandwidth, 0) != 0) {
        throw std::runtime_error("Failed to calibrate RX channel");
    }

    if (LMS_Calibrate(device, true, txChannel, bandwidth, 0) != 0) {
        throw std::runtime_error("Failed to calibrate TX channel");
    }

    isCalibrated = LimeManager::Calibrated;
}

void Device::set_sample_rate(double sampleRateHz) {
    if (sampleRateHz <= 0) {
        throw std::invalid_argument("Sample rate must be greater than zero");
    }

    init_device();

    if (LMS_SetSampleRate(device, sampleRateHz, 0) != 0) {
        throw std::runtime_error("Failed to set sample rate");
    }

    currentSampleRate = sampleRateHz;
}

void Device::set_channels(unsigned rxChannelIndex, unsigned txChannelIndex) {
    rxChannel = rxChannelIndex;
    txChannel = txChannelIndex;

    if (device != nullptr) {
        apply_channel_config();
    }
}

double Device::get_sample_rate() {
    if (device == nullptr) {
        throw std::runtime_error("Device not initialized");
    }

    double hostSampleRate = 0.0;
    double rfSampleRate = 0.0;

    if (LMS_GetSampleRate(device, false, rxChannel, &hostSampleRate, &rfSampleRate) != 0) {
        throw std::runtime_error("Failed to read sample rate");
    }

    return hostSampleRate;
}

void Device::set_paths(FilterPath rxPathSelection, FilterPath txPathSelection) {
    rxPath = rxPathSelection;
    txPath = txPathSelection;

    if (device != nullptr) {
        apply_channel_config();
    }
}

void Device::apply_channel_config() {
    if (device == nullptr) {
        throw std::runtime_error("Device not initialized");
    }

    // Enable only selected channels.
    for (unsigned ch = 0; ch < 2; ++ch) {
        LMS_EnableChannel(device, false, ch, ch == rxChannel);
        LMS_EnableChannel(device, true, ch, ch == txChannel);
    }

    if (LMS_SetAntenna(device, false, rxChannel, map_filter_path(false, rxPath)) != 0) {
        throw std::runtime_error("Failed to select RX filter path");
    }

    if (LMS_SetAntenna(device, true, txChannel, map_filter_path(true, txPath)) != 0) {
        throw std::runtime_error("Failed to select TX filter path");
    }
}

int Device::map_filter_path(bool isTx, FilterPath path) {
    if (isTx) {
        switch (path) {
        case FilterPath::High:
            return LMS_PATH_TX2;
        case FilterPath::Low:
        case FilterPath::Wide:
        default:
            return LMS_PATH_TX1;
        }
    }

    switch (path) {
    case FilterPath::Low:
        return LMS_PATH_LNAL;
    case FilterPath::High:
        return LMS_PATH_LNAH;
    case FilterPath::Wide:
    default:
        return LMS_PATH_LNAW;
    }
}

const std::string& Device::GetSerial() const {
    return serial;
}

Device::Device(Device&& other) noexcept : device(other.device), serial(std::move(other.serial)) {
    std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));
    other.device = nullptr;
    std::memset(other.device_id, 0, sizeof(lms_info_str_t));
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device) {
            LMS_Close(device);
        }
        device = other.device;
        serial = std::move(other.serial);
        std::memcpy(device_id, other.device_id, sizeof(lms_info_str_t));

        other.device = nullptr;
        std::memset(other.device_id, 0, sizeof(lms_info_str_t));
    }
    return *this;
}

Device::~Device() {
    if (device != nullptr) {
        LMS_Close(device);
    }
}