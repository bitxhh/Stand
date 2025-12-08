#include "LimeManager.h"

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
    if (device_id[0] == '\0') {
        throw std::runtime_error("Device ID error");
    }

    if (device != nullptr) {
        return;
    }

    if (LMS_Open(&device, device_id, nullptr) != 0) {
        throw std::runtime_error("Failed to open device");
    }

    if (LMS_Init(device) != 0) {
        LMS_Close(device);
        throw std::runtime_error("Failed to init device");
    }
}

void Device::calibrate(double sampleRateHz) {
    set_sample_rate(sampleRateHz);

    if (LMS_Calibrate(device, false, 0, sampleRateHz, 0) != 0) {
        throw std::runtime_error("Failed to calibrate RX channel");
    }

    if (LMS_Calibrate(device, true, 0, sampleRateHz, 0) != 0) {
        throw std::runtime_error("Failed to calibrate TX channel");
    }
}

void Device::set_sample_rate(double sampleRateHz) {
    if (sampleRateHz <= 0) {
        throw std::invalid_argument("Sample rate must be greater than zero");
    }

    init_device();

    if (LMS_SetSampleRate(device, sampleRateHz, 0) != 0) {
        throw std::runtime_error("Failed to set sample rate");
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

bool LimeManager::ids_equal(const lms_info_str_t& lhs, const lms_info_str_t& rhs) {
    return std::strncmp(lhs, rhs, sizeof(lms_info_str_t)) == 0;
}

LimeManager::LimeManager() {
    refresh_devices();
}

LimeManager::~LimeManager() {
    devices_.clear();
    // device_ids_.clear(); // Удалено
}

void LimeManager::add_device(const lms_info_str_t& id) {
    // device_ids_.emplace_back(); // Удалено
    // std::memcpy(device_ids_.back(), id, sizeof(lms_info_str_t)); // Удалено
    devices_.emplace_back(std::make_shared<Device>(id));
}

void LimeManager::remove_device_at_index(std::size_t index) {
    if (index < devices_.size()) {
        devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(index));
    }
    // Удалена логика очистки device_ids_
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

