#include "DemodRegistry.h"
#include "FmDemodHandler.h"
#include "AmDemodHandler.h"

DemodRegistry::DemodRegistry() {
    add(QStringLiteral("FM"), [](double off, QObject* p) -> BaseDemodHandler* {
        return new FmDemodHandler(off, 75e-6, 150'000.0, p);
    });
    add(QStringLiteral("AM"), [](double off, QObject* p) -> BaseDemodHandler* {
        return new AmDemodHandler(off, 5'000.0, p);
    });
}

DemodRegistry& DemodRegistry::instance() {
    static DemodRegistry reg;
    return reg;
}

void DemodRegistry::add(const QString& name, Factory factory) {
    factories_.insert(name, std::move(factory));
}

QStringList DemodRegistry::names() const {
    return factories_.keys();
}

BaseDemodHandler* DemodRegistry::create(const QString& name, double offsetHz,
                                        QObject* parent) const {
    auto it = factories_.find(name);
    if (it == factories_.end()) return nullptr;
    return it.value()(offsetHz, parent);
}
