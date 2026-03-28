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

    // Tested stable rates for LimeSDR Mini over USB.
    // Lower bound: 2.5 MS/s — hardware artefacts reported below this on the Mini.
    // Upper bound: 20 MS/s — 25/30 MS/s work in Release but drop out in Debug
    //              builds because the CPU cannot drain the USB FIFO fast enough.
    // All values produce integer decimation ratios for the standard FM IF (250 kHz).
    static constexpr int kSampleRateCount = 7;
    float_type sampleRates[kSampleRateCount] =
    {
        2'500'000,    //  2.5 MS/s  — minimum stable on Mini; FM mono fine
        4'000'000,    //  4   MS/s  — fits 2 FM stations in view
        5'000'000,    //  5   MS/s
        8'000'000,    //  8   MS/s  — good AMC dataset capture rate
        10'000'000,   // 10   MS/s  — 10 MHz visible span
        15'000'000,   // 15   MS/s  — Release build only recommended
        20'000'000,   // 20   MS/s  — Release build only recommended
    };



private:
    static bool ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs);

    void add_device(const lms_info_str_t& id);
    void remove_device_at_index(std::size_t index);

    // Owning vectors ensure automatic cleanup via RAII without having to track dynamic allocations manually.
    //std::vector<lms_info_str_t> device_ids_;
    std::vector<std::shared_ptr<Device>> devices_;
    mutable std::mutex devicesMutex_;
};