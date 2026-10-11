#include "providerregistry.h"
#include <QDebug>
#include <QMenu>
#include <QIcon>
#include <QHash>
#include <algorithm>
#include "svgicon.h"
#include "themes/thememanager.h"

ProviderRegistry& ProviderRegistry::instance() {
    static ProviderRegistry s_instance;
    return s_instance;
}

void ProviderRegistry::registerProvider(const QString& name, const QString& identifier,
                                        IProviderPlugin* plugin, const QString& dllFileName) {
    // Check if already registered
    for (const auto& info : m_providers) {
        if (info.identifier == identifier) {
            qWarning() << "ProviderRegistry: Provider already registered:" << identifier;
            return;
        }
    }

    m_providers.append(ProviderInfo(name, identifier, plugin, dllFileName));
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
            m_providers.removeAt(i);
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

QVector<rcx::SourceEntry> ProviderRegistry::sourceActions() {
    QVector<rcx::SourceEntry> entries;
    auto append = [&entries](const QString& id, const QString& name, const QString& dll, const QString& description) {
        rcx::SourceEntry entry;
        entry.entryKind = rcx::SourceEntry::ProviderAction;
        entry.providerIdentifier = id;
        entry.displayName = rcx::providerLabel(id, name);
        entry.kindLabel = rcx::kindLabelFor(id);
        entry.iconPath = rcx::iconForProvider(id);
        entry.description = rcx::providerDescription(id, description);
        entry.searchAliases = name;
        entry.dllFileName = dll;
        entries.append(entry);
    };
    append(QStringLiteral("File"), QStringLiteral("Open File"), {}, {});
    for (const auto& p : instance().providers()) {
        if (p.identifier == QStringLiteral("File")) continue;
        append(p.identifier, p.name, p.dllFileName,
               p.plugin ? QString::fromStdString(p.plugin->Description()) : QString());
    }
    std::stable_sort(entries.begin(), entries.end(), [](const rcx::SourceEntry& a, const rcx::SourceEntry& b) {
        const int ao = rcx::providerOrder(a.providerIdentifier), bo = rcx::providerOrder(b.providerIdentifier);
        if (ao != bo) return ao < bo;
        return QString::compare(a.displayName, b.displayName, Qt::CaseInsensitive) < 0;
    });
    return entries;
}

void ProviderRegistry::populateSourceMenu(QMenu* menu,
                                         const QVector<SavedSourceDisplay>& savedSources) {
    if (!menu) return;
    const auto& theme = rcx::ThemeManager::instance().current();
    menu->addSection(QStringLiteral("Add source"));
    QMenu* management = nullptr;
    for (const auto& entry : sourceActions()) {
        auto* action = menu->addAction(rcx::themedVsIcon(entry.iconPath, theme.text, 16, menu->devicePixelRatioF()),
                                       entry.displayName + QStringLiteral("\u2026"));
        action->setIconVisibleInMenu(true);
        action->setData(entry.providerIdentifier);
        action->setToolTip(entry.details());
    }
    if (!savedSources.isEmpty()) {
        menu->addSection(QStringLiteral("Saved sources"));
        for (int i = 0; i < savedSources.size(); ++i) {
            auto* action = menu->addAction(savedSources[i].text);
            action->setCheckable(true);
            action->setChecked(savedSources[i].active);
            action->setData(QStringLiteral("#saved:%1").arg(i));
        }
        menu->addSeparator();
        management = menu->addMenu(QStringLiteral("Manage sources"));
        auto* clear = management->addAction(QStringLiteral("Disconnect and clear sources"));
        clear->setData(QStringLiteral("#clear"));
    }
    // Keep each plugin's commands under its own name, separate from selection.
    for (const auto& entry : sourceActions()) {
        const auto* info = instance().findProvider(entry.providerIdentifier);
        if (!info || !info->plugin) continue;
        auto* sub = new QMenu(entry.displayName, menu);
        info->plugin->populatePluginMenu(sub);
        if (sub->isEmpty()) { delete sub; continue; }
        if (!management) management = menu->addMenu(QStringLiteral("Manage sources"));
        management->addMenu(sub);
    }
}
