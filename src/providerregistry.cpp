#include "providerregistry.h"
#include <QDebug>
#include <QMenu>
#include <QIcon>
#include <QHash>
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

void ProviderRegistry::populateSourceMenu(QMenu* menu,
                                          const QVector<SavedSourceDisplay>& savedSources)
{
    static const QHash<QString, QString> s_providerIcons = {
        {QStringLiteral("processmemory"),          QStringLiteral(":/vsicons/server-process.svg")},
        {QStringLiteral("remoteprocessmemory"),    QStringLiteral(":/vsicons/remote.svg")},
        {QStringLiteral("windbgmemory"),           QStringLiteral(":/vsicons/debug.svg")},
        {QStringLiteral("RC.netcompatlayer"), QStringLiteral(":/vsicons/plug.svg")},
    };

    // The :/vsicons Codicons bake a light-grey #C5C5C5 ink, chosen for dark
    // chrome. A plain QIcon(path) renders that ink verbatim, so on the light
    // theme these glyphs were near-invisible on a near-white menu while the
    // item TEXT (which comes from the palette) was correctly black. Tint them
    // to the live menu-text colour. populateSourceMenu re-runs on every
    // aboutToShow, so this follows a theme switch with no extra wiring, and
    // themedVsIcon caches on (path, tint, size, dpr) so the rebuild is free.
    const QColor ink = rcx::ThemeManager::instance().current().text;
    const qreal  dpr = menu ? menu->devicePixelRatioF() : qreal(1);
    auto themed = [&ink, dpr](const QString& path) {
        return rcx::themedVsIcon(path, ink, 16, dpr);
    };

    // File source
    auto* fileAct = menu->addAction(themed(QStringLiteral(":/vsicons/file-binary.svg")),
                                    QStringLiteral("File"));
    fileAct->setIconVisibleInMenu(true);
    fileAct->setData(QStringLiteral("File"));

    // Registered providers
    const auto& providers = instance().providers();
    for (const auto& prov : providers) {
        auto it = s_providerIcons.constFind(prov.identifier);
        const QIcon icon = themed(it != s_providerIcons.constEnd() ? *it
                                  : QStringLiteral(":/vsicons/extensions.svg"));

        QString label = prov.dllFileName.isEmpty()
            ? prov.name
            : QStringLiteral("%1  (%2)").arg(prov.name, prov.dllFileName);

        auto* act = menu->addAction(icon, label);
        act->setIconVisibleInMenu(true);
        act->setData(prov.name);  // routing key for selectSource()

        // Plugin-specific actions (e.g. "Unload Driver" when loaded)
        if (prov.plugin)
            prov.plugin->populatePluginMenu(menu);
    }

    // Saved sources
    if (!savedSources.isEmpty()) {
        menu->addSeparator();
        for (int i = 0; i < savedSources.size(); i++) {
            auto* act = menu->addAction(savedSources[i].text);
            act->setCheckable(true);
            act->setChecked(savedSources[i].active);
            act->setData(QStringLiteral("#saved:%1").arg(i));
        }
        menu->addSeparator();
        auto* clearAct = menu->addAction(
            themed(QStringLiteral(":/vsicons/clear-all.svg")),
            QStringLiteral("Clear All"));
        clearAct->setIconVisibleInMenu(true);
        clearAct->setData(QStringLiteral("#clear"));
    }
}
