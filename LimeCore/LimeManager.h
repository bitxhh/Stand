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

private:
    static bool ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs);

    void add_device(const lms_info_str_t& id);
    void remove_device_at_index(std::size_t index);

    // Owning vectors ensure automatic cleanup via RAII without having to track dynamic allocations manually.
    //std::vector<lms_info_str_t> device_ids_;
    std::vector<std::shared_ptr<Device>> devices_;
    mutable std::mutex devicesMutex_;
};