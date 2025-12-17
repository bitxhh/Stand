//
// Created by ilya1 on 11.12.2025.
//
#pragma once
#include "lime/LimeSuite.h"
#include <string>

#include "LimeManager.h"


class Device {
public:
    explicit Device(const lms_info_str_t& id);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    ~Device();

    void init_device();
    void set_sample_rate(double sampleRateHz);
    void calibrate(double sampleRateHz);
    void set_channels(unsigned rxChannelIndex, unsigned txChannelIndex);
    double get_sample_rate();
    [[nodiscard]] bool is_initialized() const { return device != nullptr; }

    enum class FilterPath { Low, High, Wide };
    void set_paths(FilterPath rxPath, FilterPath txPath);
    [[nodiscard]] const std::string& GetSerial() const;
    [[nodiscard]] const lms_info_str_t& GetInfo() const { return device_id; }
    static std::string GetDeviceSerial(const lms_info_str_t infoStr);

    int     isCalibrated = LimeManager::NotCalibrated;
    bool	isRunning = false;
    bool    LPFenable = false;

private:
    lms_device_t* device{nullptr};
    lms_info_str_t device_id{};
    std::string serial;
    unsigned rxChannel{0};
    unsigned txChannel{0};
    double   currentSampleRate{0.0};
    FilterPath rxPath{FilterPath::Wide};
    FilterPath txPath{FilterPath::Wide};
    int16_t* buffer = nullptr;
    LMS_LogHandler log_handler = nullptr;
    lms_stream_t streamId;

    void apply_channel_config();
    static int map_filter_path(bool isTx, FilterPath path);
};
