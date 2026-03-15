#include "pluginmanager.h"
#include "providerregistry.h"
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
