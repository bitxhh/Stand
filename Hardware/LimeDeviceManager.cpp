#include "LimeDeviceManager.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
LimeDeviceManager::LimeDeviceManager(QObject* parent)
    : IDeviceManager(parent)
{
    refresh();
}

// ---------------------------------------------------------------------------
// IDeviceManager: refresh
// ---------------------------------------------------------------------------
void LimeDeviceManager::refresh() {
    const int count = LMS_GetDeviceList(nullptr);
    if (count < 0)
        throw std::runtime_error("LMS_GetDeviceList failed");

    std::vector<lms_info_str_t> found(static_cast<std::size_t>(count));
    if (count > 0 && LMS_GetDeviceList(found.data()) < 0)
        throw std::runtime_error("LMS_GetDeviceList (read) failed");

    std::lock_guard lock(mutex_);
    bool changed = false;

    // Удаляем устройства, которые больше не присутствуют
    for (auto it = devices_.begin(); it != devices_.end();) {
        auto* lime = static_cast<LimeDevice*>(it->get());
        const bool stillPresent = std::any_of(found.begin(), found.end(),
            [&](const lms_info_str_t& id) {
                return idsEqual(id, lime->limeInfo());
            });

        if (!stillPresent) {
            LOG_INFO("LimeDeviceManager: device removed: "
                     + lime->id().toStdString());
            it = devices_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    // Добавляем вновь обнаруженные устройства
    for (const auto& id : found) {
        const bool known = std::any_of(devices_.begin(), devices_.end(),
            [&](const std::shared_ptr<IDevice>& d) {
                return idsEqual(id, static_cast<LimeDevice*>(d.get())->limeInfo());
            });

        if (!known) {
            auto dev = std::make_shared<LimeDevice>(id);
            LOG_INFO("LimeDeviceManager: device found: "
                     + dev->id().toStdString());
            devices_.append(std::move(dev));
            changed = true;
        }
    }

    if (changed)
        emit devicesChanged();
}

// ---------------------------------------------------------------------------
// IDeviceManager: devices
// ---------------------------------------------------------------------------
QList<std::shared_ptr<IDevice>> LimeDeviceManager::devices() const {
    std::lock_guard lock(mutex_);
    return devices_;
}

// ---------------------------------------------------------------------------
// Static: supported sample rates
// ---------------------------------------------------------------------------
const QList<double>& LimeDeviceManager::sampleRates() {
    return LimeDevice::kSupportedRates;
}

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------
bool LimeDeviceManager::idsEqual(const lms_info_str_t& a, const lms_info_str_t& b) {
    return std::strncmp(a, b, sizeof(lms_info_str_t)) == 0;
}
