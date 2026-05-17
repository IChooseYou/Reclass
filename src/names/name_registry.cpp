#include "name_registry.h"

namespace rcx {

NameRegistry& NameRegistry::instance() {
    static NameRegistry r;
    return r;
}

void NameRegistry::registerProvider(std::shared_ptr<NameProvider> p) {
    if (!p) return;
    // Replace if a provider with the same id already exists (idempotent).
    QString id = p->id();
    for (int i = 0; i < m_providers.size(); i++) {
        if (m_providers[i]->id() == id) {
            m_providers[i] = std::move(p);
            emit providersChanged();
            return;
        }
    }
    m_providers.append(std::move(p));
    emit providersChanged();
}

void NameRegistry::unregisterProvider(const QString& id) {
    for (int i = 0; i < m_providers.size(); i++) {
        if (m_providers[i]->id() == id) {
            m_providers.removeAt(i);
            emit providersChanged();
            return;
        }
    }
}

QString NameRegistry::nameFor(uint64_t addr, const Provider* active) const {
    if (addr == 0) return {};
    for (const auto& p : m_providers) {
        QString s = p->nameFor(addr, active);
        if (!s.isEmpty()) return s;
    }
    return {};
}

uint64_t NameRegistry::addressFor(const QString& name, const Provider* active) const {
    if (name.isEmpty()) return 0;
    for (const auto& p : m_providers) {
        uint64_t a = p->addressFor(name, active);
        if (a != 0) return a;
    }
    return 0;
}

void NameRegistry::emitChanged() {
    emit providersChanged();
}

} // namespace rcx
