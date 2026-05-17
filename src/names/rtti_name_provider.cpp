#include "rtti_name_provider.h"
#include "name_registry.h"
#include "themes/thememanager.h"
#include <QColor>
#include <QMutexLocker>

namespace rcx {

RttiNameProvider& RttiNameProvider::instance() {
    static RttiNameProvider p;
    return p;
}

uint32_t RttiNameProvider::accent() const {
    return ThemeManager::instance().current().markerCycle.rgba();
}

QVector<NamedAddress> RttiNameProvider::entries(const Provider* /*active*/) const {
    QMutexLocker lock(&m_lock);
    return m_hits;
}

void RttiNameProvider::push(const QString& name, uint64_t address,
                            const QString& moduleName) {
    if (name.isEmpty() || address == 0) return;
    QString key = name + QStringLiteral("@") + QString::number(address, 16);
    {
        QMutexLocker lock(&m_lock);
        if (m_byKey.contains(key)) return;
        NamedAddress n;
        n.name = name;
        n.address = address;
        n.kind = QStringLiteral("rtti");
        if (!moduleName.isEmpty()) n.source = moduleName;
        m_byKey.insert(key, m_hits.size());
        m_hits.append(std::move(n));
    }
    NameRegistry::instance().emitChanged();
}

void RttiNameProvider::clear() {
    {
        QMutexLocker lock(&m_lock);
        m_hits.clear();
        m_byKey.clear();
    }
    NameRegistry::instance().emitChanged();
}

void RttiNameProvider::clearForModule(const QString& moduleName) {
    if (moduleName.isEmpty()) return;
    {
        QMutexLocker lock(&m_lock);
        QVector<NamedAddress> kept;
        kept.reserve(m_hits.size());
        QHash<QString, int> newKey;
        for (const auto& h : m_hits) {
            if (h.source == moduleName) continue;
            QString key = h.name + QStringLiteral("@") + QString::number(h.address, 16);
            newKey.insert(key, kept.size());
            kept.append(h);
        }
        m_hits = std::move(kept);
        m_byKey = std::move(newKey);
    }
    NameRegistry::instance().emitChanged();
}

} // namespace rcx
