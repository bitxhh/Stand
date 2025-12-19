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





void Device::init_device() {is_init = true;}

void Device::calibrate(double sampleRateHz) {}

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

double Device::get_sample_rate() {
    if (device == nullptr) {
        throw std::runtime_error("Device not initialized");
    }

    double hostSampleRate = 0.0;
    double rfSampleRate = 0.0;

//    if (LMS_GetSampleRate(device, false, rxChannel, &hostSampleRate, &rfSampleRate) != 0) {
//        throw std::runtime_error("Failed to read sample rate");
//    }

    return hostSampleRate;
}


//-----------------------------------------------------------
Device::Device(const lms_info_str_t& id) : device(nullptr) {
    std::memcpy(device_id, id, sizeof(lms_info_str_t));
    serial = GetDeviceSerial(id);
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