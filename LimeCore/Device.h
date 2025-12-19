//
// Created by ilya1 on 11.12.2025.
//
#pragma once
#include "lime/LimeSuite.h"
#include <string>


class Device {
public:
    //-------------------------------------------
    explicit Device(const lms_info_str_t& id);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    ~Device();
//------------------------------------------------
    void init_device();
    void set_sample_rate(double sampleRateHz);
    void calibrate(double sampleRateHz);
    [[nodiscard]] bool is_initialized() const { return is_init; }

//Getters ------------
    [[nodiscard]] const std::string& GetSerial() const {return serial; };
    [[nodiscard]] const lms_info_str_t& GetInfo() const { return device_id; }
    static std::string GetDeviceSerial(const lms_info_str_t infoStr);
    double get_sample_rate();
    //------------------

    bool    is_init = false;
    int     isCalibrated = NotCalibrated;
    bool	isRunning = false;

private:
    lms_device_t* device = nullptr;
    lms_info_str_t device_id{};
    std::string serial;
    double currentSampleRate ={0.0};

    enum CalibrationStatus {
        Calibrated = 1,
        NotCalibrated = 0,
        CalibrationErr = -1
    };

};
