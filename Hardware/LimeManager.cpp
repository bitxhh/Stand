#include "LimeManager.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>


bool LimeManager::ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs) {
    return std::strncmp(lhs, rhs, sizeof(lms_info_str_t)) == 0;
}

LimeManager::LimeManager() {
    refresh_devices();
}

LimeManager::~LimeManager() {
    devices_.clear();
}

void LimeManager::add_device(const lms_info_str_t& id) {
    devices_.emplace_back(std::make_shared<Device>(id));
}

void LimeManager::remove_device_at_index(std::size_t index) {
    if (index < devices_.size()) {
        devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void LimeManager::refresh_devices() {
    std::lock_guard lock(devicesMutex_);

    int deviceCount = LMS_GetDeviceList(nullptr);
    if (deviceCount < 0) {
        throw std::runtime_error("Failed to get device list");
    }

    std::vector<lms_info_str_t> current(static_cast<std::size_t>(deviceCount));
    if (deviceCount > 0 && LMS_GetDeviceList(current.data()) < 0) {
        throw std::runtime_error("Failed to read device list");
    }

    // Remove devices that are no longer present.
    for (std::size_t i = 0; i < devices_.size();) {
        const bool still_connected = std::any_of(current.begin(), current.end(), [&](const lms_info_str_t& id) {
            return ids_equal(id, devices_[i]->GetInfo());
        });

        if (!still_connected) {
            remove_device_at_index(i);
        } else {
            ++i;
        }
    }

    // Add newly discovered devices.
    for (const auto& id : current) {
        const bool already_known = std::any_of(devices_.begin(), devices_.end(), [&](const std::shared_ptr<Device>& device) {
            return ids_equal(device->GetInfo(), id);
        });

        if (!already_known) {
            add_device(id);
        }
    }
}

std::vector<std::shared_ptr<Device>> LimeManager::get_devices() const {
    std::lock_guard lock(devicesMutex_);
    return devices_;
}

std::vector<std::string> LimeManager::get_device_ids() const {
    std::lock_guard lock(devicesMutex_);
    std::vector<std::string> ids;
    ids.reserve(devices_.size());
    for (const auto& device : devices_) {
        ids.emplace_back(device->GetInfo());
    }
    return ids;
}
