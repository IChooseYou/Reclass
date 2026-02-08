#include "providerregistry.h"
#include <QDebug>

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry s_instance;
    return s_instance;
}

void ProviderRegistry::registerProvider(const QString& name, const QString& identifier, IProviderPlugin* plugin) {
    // Check if already registered
    for (const auto& info : m_providers) {
        if (info.identifier == identifier) {
            qWarning() << "ProviderRegistry: Provider already registered:" << identifier;
            return;
        }
    }
    
    m_providers.append(ProviderInfo(name, identifier, plugin));
    qDebug() << "ProviderRegistry: Registered plugin provider:" << name << "(" << identifier << ")";
}

void ProviderRegistry::registerBuiltinProvider(const QString& name, const QString& identifier, BuiltinFactory factory) {
    // Check if already registered
    for (const auto& info : m_providers) {
        if (info.identifier == identifier) {
            qWarning() << "ProviderRegistry: Provider already registered:" << identifier;
            return;
        }
    }
    
    m_providers.append(ProviderInfo(name, identifier, factory));
    qDebug() << "ProviderRegistry: Registered builtin provider:" << name << "(" << identifier << ")";
}

void ProviderRegistry::unregisterProvider(const QString& identifier) {
    for (int i = 0; i < m_providers.size(); ++i) {
        if (m_providers[i].identifier == identifier) {
            qDebug() << "ProviderRegistry: Unregistered provider:" << identifier;
            m_providers.remove(i);
            return;
        }
    }
    qWarning() << "ProviderRegistry: Provider not found:" << identifier;
}

const ProviderRegistry::ProviderInfo* ProviderRegistry::findProvider(const QString& identifier) const {
    for (const auto& info : m_providers) {
        if (info.identifier == identifier) {
            return &info;
        }
    }
    return nullptr;
}

void ProviderRegistry::clear() {
    m_providers.clear();
}
