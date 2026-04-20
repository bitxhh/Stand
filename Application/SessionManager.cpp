#include "SessionManager.h"

SessionManager::SessionManager(QObject* parent)
    : QObject(parent)
{}

bool SessionManager::isInUse(const QString& deviceId) const {
    return inUse_.contains(deviceId);
}

bool SessionManager::hasAny() const {
    return !inUse_.isEmpty();
}

void SessionManager::markInUse(const QString& deviceId) {
    if (!inUse_.contains(deviceId)) {
        inUse_.insert(deviceId);
        emit sessionChanged();
    }
}

void SessionManager::release(const QString& deviceId) {
    if (inUse_.remove(deviceId))
        emit sessionChanged();
}
