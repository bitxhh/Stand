#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <functional>

class BaseDemodHandler;
class QObject;

// ---------------------------------------------------------------------------
// DemodRegistry — factory for demodulator handlers.
//
// Usage:
//   auto& reg = DemodRegistry::instance();
//   QStringList modes = reg.names();          // → ["FM", "AM", ...]
//   auto* handler = reg.create("FM", offsetHz, parent);
//   auto descs = handler->paramDescriptors(); // → UI auto-builds widgets
// ---------------------------------------------------------------------------
class DemodRegistry {
public:
    using Factory = std::function<BaseDemodHandler*(double offsetHz, QObject* parent)>;

    static DemodRegistry& instance();

    void add(const QString& name, Factory factory);
    [[nodiscard]] QStringList names() const;
    BaseDemodHandler* create(const QString& name, double offsetHz,
                             QObject* parent = nullptr) const;

private:
    DemodRegistry();
    QMap<QString, Factory> factories_;
};
