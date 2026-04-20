#pragma once

#include <QObject>
#include <QSet>
#include <QString>

// ---------------------------------------------------------------------------
// SessionManager — tracks which device IDs currently have an open
// DeviceDetailWindow.  DeviceSelectionWindow uses this to gray out
// buttons for devices that are already in use.
// ---------------------------------------------------------------------------
class SessionManager : public QObject {
    Q_OBJECT

public:
    explicit SessionManager(QObject* parent = nullptr);

    [[nodiscard]] bool isInUse(const QString& deviceId) const;
    [[nodiscard]] bool hasAny() const;
    void markInUse(const QString& deviceId);
    void release(const QString& deviceId);

signals:
    void sessionChanged();   // emitted on every markInUse / release

private:
    QSet<QString> inUse_;
};
