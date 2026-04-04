#pragma once

#include "../Core/ISyncController.h"

#include <QMap>
#include <QObject>

class LimeDevice;

// ---------------------------------------------------------------------------
// LimeSyncController — ISyncController для LimeSDR (LimeSuite API).
//
// Phase 6: стабы с логированием.
// Реальная реализация требует:
//   • LMS_SetClockFreq(LMS_CLOCK_EXTREF) для reference
//   • LMS_GPIODirWrite / LMS_GPIOWrite для trigger
//   • lms_stream_meta_t::waitForTimestamp для timed start
//
// Все методы безопасны для вызова из main thread.
// ---------------------------------------------------------------------------
class LimeSyncController : public QObject, public ISyncController {
    Q_OBJECT

public:
    explicit LimeSyncController(QObject* parent = nullptr);
    ~LimeSyncController() override;

    // ── ISyncController ──────────────────────────────────────────────────
    void setReferenceSource(IDevice* device, ReferenceSource src) override;
    [[nodiscard]] ReferenceSource referenceSource(const IDevice* device) const override;
    [[nodiscard]] bool isExternalReferenceDetected(const IDevice* device) const override;

    [[nodiscard]] uint64_t getHardwareTimestamp(const IDevice* device) const override;
    [[nodiscard]] int64_t  timestampOffset(const IDevice* master,
                                           const IDevice* slave) const override;

    void armTrigger(const QList<IDevice*>& devices) override;
    void fireTrigger(uint64_t targetTimestamp = 0) override;
    [[nodiscard]] bool isArmed() const override;

signals:
    void referenceSourceChanged(IDevice* device, ReferenceSource src);
    void triggerArmed(int deviceCount);
    void triggerFired(uint64_t targetTimestamp);
    void syncError(const QString& message);

private:
    struct PerDevice {
        ReferenceSource refSource{ReferenceSource::Internal};
    };
    QMap<const IDevice*, PerDevice> state_;
    QList<IDevice*> armedDevices_;
    bool armed_{false};
};
