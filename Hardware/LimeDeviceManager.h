#pragma once

#include "../Core/IDeviceManager.h"
#include "LimeDevice.h"

#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// LimeDeviceManager — реализация IDeviceManager для LimeSuite.
//
// Хранит список LimeDevice (как shared_ptr<IDevice>).
// refresh() использует LMS_GetDeviceList — добавляет новые устройства,
// удаляет исчезнувшие, не трогает уже существующие объекты.
// ---------------------------------------------------------------------------
class LimeDeviceManager : public IDeviceManager {
    Q_OBJECT

public:
    explicit LimeDeviceManager(QObject* parent = nullptr);
    ~LimeDeviceManager() override = default;

    // IDeviceManager
    void refresh() override;
    [[nodiscard]] QList<std::shared_ptr<IDevice>> devices() const override;

    // Стабильные sample rates для LimeSDR по USB.
    // Совпадают с LimeDevice::kSupportedRates — вынесены сюда для удобства UI.
    [[nodiscard]] static const QList<double>& sampleRates();

private:
    static bool idsEqual(const lms_info_str_t& a, const lms_info_str_t& b);

    mutable std::mutex              mutex_;
    QList<std::shared_ptr<IDevice>> devices_;
};
