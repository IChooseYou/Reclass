#include "pluginmanager.h"
#include "providerregistry.h"
#include "providers/provider.h"   // sizeof(rcx::Provider) for the ABI guard
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>

PluginManager::~PluginManager()
{
    UnloadPlugins();
}

void PluginManager::LoadPlugins()
{
    m_rejected.clear();  // repopulated below by the ABI guard

    // Probe plugin locations relative to the executable.
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList pluginDirs;
    pluginDirs << (appDir + "/Plugins");
#ifdef __APPLE__
    // In macOS app bundles, plugins may live in Contents/PlugIns or in
    // the top-level build/Plugins directory during local development.
    pluginDirs << QDir::cleanPath(appDir + "/../PlugIns");
#endif

    // Find all DLL files
    QStringList filters;
#ifdef _WIN32
    filters << "*.dll";
#elif defined(__APPLE__)
    filters << "*.dylib";
#else
    filters << "*.so";
#endif

    int totalCandidates = 0;
    bool foundAnyDir = false;
    for (const QString& pluginsDir : pluginDirs)
    {
        QDir dir(pluginsDir);
        if (!dir.exists())
            continue;

        foundAnyDir = true;
        dir.setNameFilters(filters);
        QFileInfoList files = dir.entryInfoList(QDir::Files);
        totalCandidates += files.count();

        qDebug() << "PluginManager: Scanning for plugins in:" << pluginsDir;
        for (const QFileInfo& fileInfo : files)
        {
            // Skip the remote-inject payload binary — it's not a plugin and
            // loading it (especially on Linux) spawns a rogue thread.
            if (fileInfo.baseName().startsWith("rcx_payload"))
                continue;

            LoadPlugin(fileInfo.absoluteFilePath());
        }
    }

    if (!foundAnyDir)
        qWarning() << "PluginManager: Plugins directory not found. Searched:" << pluginDirs;
    else
        qDebug() << "PluginManager: Found" << totalCandidates << "potential plugin(s)";
    
    qDebug() << "PluginManager: Loaded" << m_plugins.count() << "plugin(s)";
}

bool PluginManager::LoadPlugin(const QString& path)
{
    QLibrary* library = new QLibrary(path);
    
    // Load the library
    if (!library->load())
    {
        qWarning() << "PluginManager: Failed to load plugin:" << path;
        qWarning() << "PluginManager: Error" << library->errorString();
        delete library;
        return false;
    }
    
    // Resolve the CreatePlugin function
    CreatePluginFunc CreateFunc = (CreatePluginFunc)library->resolve("CreatePlugin");
    if (!CreateFunc)
    {
        qWarning() << "PluginManager: Plugin" << path << "does not export CreatePlugin()";
        library->unload();
        delete library;
        return false;
    }

    // Create plugin instance
    IPlugin* plugin = CreateFunc();
    if (!plugin)
    {
        qWarning() << "PluginManager: CreatePlugin() returned nullptr for" << path;
        library->unload();
        delete library;
        return false;
    }

    // ── Provider ABI guard (provider plugins only) ──
    // rcx::Provider is a fragile base class shared with plugin DLLs: an ABI-stale
    // provider plugin (e.g. one not rebuilt after a provider.h data-member
    // change) corrupts memory the first time the host touches a base-class
    // member — an access violation deep in the RTTI/module-cache path with no
    // obvious cause. Require provider plugins to export RcxPluginAbiToken() equal
    // to the host's RCX_PROVIDER_ABI_TOKEN before registering them, so a stale one
    // can never create a corrupting Provider. A missing token means the plugin
    // predates the guard / was built against older headers; a mismatched value
    // means a layout disagreement. Either way: refuse it, log how to fix it, and
    // keep the app running with the other plugins. The check runs AFTER
    // construction (safe: the factory object inherits IPlugin, not Provider — the
    // Provider is only made later via createProvider) and is gated on Type() so
    // future non-provider plugins, which never touch Provider, are exempt.
    // (See iplugin.h RCX_DEFINE_PLUGIN_ABI and the CMake add_dependencies note.)
    if (plugin->Type() == IPlugin::ProviderPlugin)
    {
        const uint64_t hostAbi = RCX_PROVIDER_ABI_TOKEN;
        auto* abiFunc = reinterpret_cast<RcxPluginAbiTokenFunc>(
            library->resolve("RcxPluginAbiToken"));
        if (!abiFunc)
        {
            qWarning() << "PluginManager: Provider plugin" << path
                       << "exports no ABI token - built against older/incompatible"
                       << "headers. Rebuild it. Skipping to avoid a memory-corruption crash.";
            m_rejected.push_back({path, QStringLiteral(
                "No ABI token — built against older/incompatible headers. Rebuild the plugin.")});
            delete plugin;
            library->unload();
            delete library;
            return false;
        }
        const uint64_t pluginAbi = abiFunc();
        if (pluginAbi != hostAbi)
        {
            qWarning() << "PluginManager: Provider plugin" << path
                       << "ABI mismatch: plugin (ver" << (pluginAbi >> 32)
                       << "sizeof(Provider)" << (pluginAbi & 0xFFFFFFFFu) << ")"
                       << "vs host (ver" << (hostAbi >> 32)
                       << "sizeof(Provider)" << (hostAbi & 0xFFFFFFFFu) << ")"
                       << "- rebuild the plugin. Skipping to avoid a crash.";
            m_rejected.push_back({path, QStringLiteral(
                "ABI mismatch (plugin ver %1 size %2 vs host ver %3 size %4) — rebuild the plugin.")
                .arg(pluginAbi >> 32).arg(pluginAbi & 0xFFFFFFFFu)
                .arg(hostAbi >> 32).arg(hostAbi & 0xFFFFFFFFu)});
            delete plugin;
            library->unload();
            delete library;
            return false;
        }
    }

    qDebug() << "PluginManager: Loaded plugin:" << plugin->Name().c_str() << plugin->Version().c_str() << "by" << plugin->Author().c_str();
    
    // Store plugin entry
    m_entries.push_back(PluginEntry{library, plugin});
    m_plugins.append(plugin);
    
    // Auto-register providers in global registry
    if (plugin->Type() == IPlugin::ProviderPlugin)
    {
        IProviderPlugin* provider = static_cast<IProviderPlugin*>(plugin);
        QString name = QString::fromStdString(plugin->Name());
        QString identifier = name.toLower().replace(" ", "");
        QString dllFileName = QFileInfo(path).fileName();
        ProviderRegistry::instance().registerProvider(name, identifier, provider, dllFileName);
    }
    
    return true;
}

QVector<IProviderPlugin*> PluginManager::providerPlugins() const
{
    QVector<IProviderPlugin*> result;
    for (IPlugin* plugin : m_plugins)
    {
        if (plugin->Type() == IPlugin::ProviderPlugin)
        {
            result.append(static_cast<IProviderPlugin*>(plugin));
        }
    }
    return result;
}

IPlugin* PluginManager::FindPlugin(const QString& name) const
{
    for (IPlugin* plugin : m_plugins)
    {
        if (QString::fromStdString(plugin->Name()) == name)
        {
            return plugin;
        }
    }
    return nullptr;
}

bool PluginManager::LoadPluginFromPath(const QString& path)
{
    // Check if already loaded
    QFileInfo fileInfo(path);
    QString fileName = fileInfo.fileName();
    
    for (const auto& entry : m_entries)
    {
        if (entry.library->fileName().endsWith(fileName))
        {
            qWarning() << "PluginManager: Plugin already loaded:" << fileName;
            return false;
        }
    }
    
    return LoadPlugin(path);
}

bool PluginManager::UnloadPlugin(const QString& name)
{
    for (int i = 0; i < m_entries.size(); ++i)
    {
        if (QString::fromStdString(m_entries[i].plugin->Name()) == name)
        {
            qDebug() << "PluginManager: Unloading plugin:" << name;
            
            IPlugin* plugin = m_entries[i].plugin;
            
            // Unregister provider from global registry
            if (plugin->Type() == IPlugin::ProviderPlugin)
            {
                QString identifier = name.toLower().replace(" ", "");
                ProviderRegistry::instance().unregisterProvider(identifier);
            }
            
            // Delete plugin instance
            delete plugin;
            
            // Unload library
            m_entries[i].library->unload();
            delete m_entries[i].library;
            
            // Remove from lists
            m_entries.remove(i);
            m_plugins.remove(i);
            
            return true;
        }
    }
    
    qWarning() << "PluginManager: Plugin not found:" << name;
    return false;
}

void PluginManager::UnloadPlugins()
{
    // Clear provider registry
    ProviderRegistry::instance().clear();
    
    // Delete plugin instances and unload libraries
    for (int i = 0; i < m_entries.size(); ++i) {
        delete m_entries[i].plugin;
        m_entries[i].library->unload();
        delete m_entries[i].library;
    }
    
    m_entries.clear();
    m_plugins.clear();
}
