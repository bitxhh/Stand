#pragma once

#include <chrono>
#include <cctype>
#include <memory>
#include <mutex>
#include <vector>
#include "Device.h"


using namespace std::chrono_literals;


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

    float_type sampleRates[8] =
    {
        2000000,
        4000000,
        8000000,
        10000000,
        15000000,
        20000000,
        25000000,
        30000000
    };

    enum CalibrationStatus {
        Calibrated = 1,
        NotCalibrated = 0,
        CalibrationErr = -1
    };
    uint16_t LNA = 10;
    uint16_t TIA = 3;
    uint16_t PGA = 12;

private:
    static bool ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs);

    void add_device(const lms_info_str_t& id);
    void remove_device_at_index(std::size_t index);

    // Owning vectors ensure automatic cleanup via RAII without having to track dynamic allocations manually.
    //std::vector<lms_info_str_t> device_ids_;
    std::vector<std::shared_ptr<Device>> devices_;
    mutable std::mutex devicesMutex_;
};