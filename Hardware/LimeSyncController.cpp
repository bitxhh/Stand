#include "LimeSyncController.h"
#include "LimeDevice.h"
#include "Logger.h"

// LimeSuite reference clock IDs (lime/LimeSuite.h)
//   LMS_CLOCK_EXTREF = 0x0002 — external reference oscillator

LimeSyncController::LimeSyncController(QObject* parent)
    : QObject(parent)
{}

LimeSyncController::~LimeSyncController() = default;

// ---------------------------------------------------------------------------
// Reference clock
// ---------------------------------------------------------------------------
void LimeSyncController::setReferenceSource(IDevice* device, ReferenceSource src) {
    state_[device].refSource = src;

    // Real implementation (Phase 6+):
    //   auto* lime = dynamic_cast<LimeDevice*>(device);
    //   if (!lime) { emit syncError("Not a LimeDevice"); return; }
    //   const auto* handle = ... ;  // need access to lms_device_t*
    //   if (src == ReferenceSource::External10MHz)
    //       LMS_SetClockFreq(handle, LMS_CLOCK_EXTREF, 10e6);
    //   else
    //       LMS_SetClockFreq(handle, LMS_CLOCK_EXTREF, 0);  // revert to internal

    LOG_CAT(LogCat::kDeviceLifecycle, LogLevel::Info,
            "SyncController: reference source for device " + device->id().toStdString()
            + " → " + (src == ReferenceSource::External10MHz ? "External 10 MHz" : "Internal"));
    emit referenceSourceChanged(device, src);
}

ISyncController::ReferenceSource
LimeSyncController::referenceSource(const IDevice* device) const {
    auto it = state_.find(device);
    return (it != state_.end()) ? it->refSource : ReferenceSource::Internal;
}

bool LimeSyncController::isExternalReferenceDetected(const IDevice* device) const {
    // Real implementation: read clock status register or try LMS_GetClockFreq.
    // Stub: always false.
    Q_UNUSED(device);
    LOG_WARN("SyncController::isExternalReferenceDetected — stub, returning false");
    return false;
}

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------
uint64_t LimeSyncController::getHardwareTimestamp(const IDevice* device) const {
    // Delegates to IDevice::lastReadTimestamp({RX, 0}).
    // Only meaningful while a stream is running.
    return device->lastReadTimestamp({ChannelDescriptor::RX, 0});
}

int64_t LimeSyncController::timestampOffset(const IDevice* master,
                                             const IDevice* slave) const {
    // Real implementation: read timestamps from both devices as close together
    // as possible (ideally from the same readBlock cycle), then compute
    // difference.  Requires both devices to be streaming.
    //
    // Stub: return 0 (no offset).
    const uint64_t tsMaster = getHardwareTimestamp(master);
    const uint64_t tsSlave  = getHardwareTimestamp(slave);
    const int64_t  offset   = static_cast<int64_t>(tsMaster) - static_cast<int64_t>(tsSlave);

    LOG_CAT(LogCat::kDeviceLifecycle, LogLevel::Info,
            "SyncController::timestampOffset: master=" + std::to_string(tsMaster)
            + " slave=" + std::to_string(tsSlave)
            + " Δ=" + std::to_string(offset) + " samples");
    return offset;
}

// ---------------------------------------------------------------------------
// Trigger (two-phase commit)
// ---------------------------------------------------------------------------
void LimeSyncController::armTrigger(const QList<IDevice*>& devices) {
    // Real implementation:
    //   1. For each device:
    //      a. setupStream() (without LMS_StartStream)
    //      b. Configure GPIO direction: master=output, slaves=input
    //   2. Master waits for all slaves to report "armed" via GPIO read-back
    //
    // Stub: just remember the device list.
    armedDevices_ = devices;
    armed_ = true;
    LOG_CAT(LogCat::kDeviceLifecycle, LogLevel::Info,
            "SyncController::armTrigger: " + std::to_string(devices.size()) + " devices armed");
    emit triggerArmed(devices.size());
}

void LimeSyncController::fireTrigger(uint64_t targetTimestamp) {
    if (!armed_) {
        emit syncError("fireTrigger called but no devices are armed");
        return;
    }

    // Real implementation:
    //   if (targetTimestamp > 0):
    //     For each armed device:
    //       Start stream with meta.waitForTimestamp=true, meta.timestamp=target
    //     Streams begin data flow when their hw counter reaches targetTimestamp.
    //   else (GPIO trigger):
    //     Start all streams (they wait for GPIO edge).
    //     Set master GPIO high → all slaves see rising edge → streaming begins.
    //
    // Stub: just log.
    LOG_CAT(LogCat::kDeviceLifecycle, LogLevel::Info,
            "SyncController::fireTrigger: targetTimestamp=" + std::to_string(targetTimestamp)
            + " devices=" + std::to_string(armedDevices_.size()));

    armedDevices_.clear();
    armed_ = false;
    emit triggerFired(targetTimestamp);
}

bool LimeSyncController::isArmed() const {
    return armed_;
}
