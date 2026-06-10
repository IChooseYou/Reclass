#pragma once
#include "iplugin.h"
#include <QVector>
#include <QString>
#include <QLibrary>
#include <memory>

/**
 * Manages plugin loading and lifecycle
 */
class PluginManager
{
public:
    PluginManager() = default;
    ~PluginManager();
    
    // Load plugins from the "Plugins" folder
    void LoadPlugins();
    
    // Get all loaded plugins
    const QVector<IPlugin*>& plugins() const { return m_plugins; }

    // Provider plugins REJECTED at load (ABI mismatch / missing token), so the
    // UI can show which were skipped and why instead of the reason being
    // stderr-only. Repopulated each LoadPlugins().
    struct RejectedPlugin { QString path; QString reason; };
    const QVector<RejectedPlugin>& rejectedPlugins() const { return m_rejected; }
    
    // Get plugins of a specific type
    QVector<IProviderPlugin*> providerPlugins() const;
    
    // Find plugin by name
    IPlugin* FindPlugin(const QString& name) const;
    
    // Load a single plugin from path
    bool LoadPluginFromPath(const QString& path);
    
    // Unload a specific plugin by name
    bool UnloadPlugin(const QString& name);
    
    // Unload all plugins
    void UnloadPlugins();
    
private:
    struct PluginEntry
    {
        QLibrary* library;
        IPlugin* plugin;
    };
    
    QVector<PluginEntry> m_entries;
    QVector<IPlugin*> m_plugins; // Non-owning pointers for quick access
    QVector<RejectedPlugin> m_rejected; // ABI-rejected provider plugins (for the UI)
    
    bool LoadPlugin(const QString& path);
};
