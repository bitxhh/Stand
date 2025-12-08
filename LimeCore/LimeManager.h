#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lime/LimeSuite.h>

using namespace std::chrono_literals;

class Device {
public:
    explicit Device(const lms_info_str_t& id);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;

    ~Device();

    void init_device();
    void calibrate(double sampleRateHz);
    [[nodiscard]] const std::string& GetSerial() const;
    [[nodiscard]] const lms_info_str_t& GetInfo() const { return device_id; }
    static std::string GetDeviceSerial(const lms_info_str_t infoStr);

private:
    lms_device_t* device{nullptr};
    lms_info_str_t device_id{};
    std::string serial;
};

class LimeManager {
public:
    LimeManager();
    ~LimeManager();
    LimeManager(const LimeManager&) = delete;
    LimeManager& operator=(const LimeManager&) = delete;

    /**
     * Refresh internal device lists. Adds newly connected devices and removes disconnected ones.
     */
    void refresh_devices();

    [[nodiscard]] std::vector<std::shared_ptr<Device>> get_devices() const;
    [[nodiscard]] std::vector<std::string> get_device_ids() const;

private:
    static bool ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs);

    void add_device(const lms_info_str_t& id);
    void remove_device_at_index(std::size_t index);

    // Owning vectors ensure automatic cleanup via RAII without having to track dynamic allocations manually.
    //std::vector<lms_info_str_t> device_ids_;
    std::vector<std::shared_ptr<Device>> devices_;
    mutable std::mutex devicesMutex_;
};

