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
#include <fstream>





void Device::init_device() {
    if (LMS_Open(&device, device_id, nullptr))
        error();

    if (LMS_Init(device) != 0)
        error();

    if (LMS_EnableChannel(device, LMS_CH_RX, 0, true) != 0)
        error();

    if (LMS_SetLOFrequency(device, LMS_CH_RX, 0, 102e6) != 0)
        error();

    if (LMS_SetSampleRate(device, 30000000, 2) != 0)
        error();

    init_stream();

    is_init = true;
}

void Device::init_stream() {
    streamId.channel = 0; //channel number
    streamId.fifoSize = 1024 * 1024; //fifo size in samples
    streamId.throughputVsLatency = 1.0; //optimize for max throughput
    streamId.isTx = false; //RX channel
    streamId.dataFmt = lms_stream_t::LMS_FMT_I12; //12-bit integers
    if (LMS_SetupStream(device, &streamId) != 0)
        error();
}

void Device::stream() {

    std::ofstream out("capture_iq_i16.raw", std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot open output file");
    }

    if (LMS_StartStream(&streamId)!=0) {
        throw std::runtime_error("Failed to start stream");
    };
    auto t1 = std::chrono::high_resolution_clock::now();
    while (std::chrono::high_resolution_clock::now() - t1 < std::chrono::seconds(5)) //run for 5 seconds
    {
        //Receive samples
        int samplesRead = LMS_RecvStream(&streamId, buffer, sampleCnt, nullptr, 1000);
        if (samplesRead > 0) {
            // Пишем в файл: 2 * samplesRead значений int16
            const std::size_t n_iq_values = static_cast<std::size_t>(samplesRead) * 2;
            out.write(reinterpret_cast<const char*>(buffer),
                      n_iq_values * sizeof(int16_t));
        }
        else if (samplesRead < 0) {
            break;
        }

        printf("Received %d samples\n", samplesRead);
    }
    LMS_StopStream(&streamId);
    out.flush();
    out.close();
}


void Device::calibrate(double sampleRateHz) {}

void Device::set_sample_rate(double sampleRateHz) {
    if (sampleRateHz <= 0) {
        throw std::invalid_argument("Sample rate must be greater than zero");
    }


    if (LMS_SetSampleRate(device, sampleRateHz, 0) != 0) {
        throw std::runtime_error("Failed to set sample rate");
    }

    currentSampleRate = sampleRateHz;
}

double Device::get_sample_rate() const {
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
Device::Device(const lms_info_str_t& id) {
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
        LMS_StopStream(&streamId);
        LMS_DestroyStream(device, &streamId);
        LMS_Close(device);
        device = nullptr;
    }
}


std::string Device::GetDeviceSerial(const lms_info_str_t infoStr) {
    std::string str(infoStr);
    std::istringstream ss(str);
    std::string token;

    while (std::getline(ss, token, ',')) {
        token.erase(token.begin(), std::ranges::find_if(token.begin(), token.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));

        const std::string prefix = "serial=";
        if (token.compare(0, prefix.size(), prefix) == 0) {
            return token.substr(prefix.size());
        }
    }

    return {};
}