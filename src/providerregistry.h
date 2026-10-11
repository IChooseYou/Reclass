#pragma once
#include "iplugin.h"
#include "sourceentry.h"
#include <QList>
#include <QString>
#include <functional>

// Forward declarations
namespace rcx { class Provider; }
class QWidget;
class QMenu;

// Lightweight struct for saved source display in menus
struct SavedSourceDisplay {
    QString text;
    bool active = false;
};

/**
 * Global registry for data source providers
 * 
 * Providers register themselves here so they can be listed in the Source picker.
 * Supports both plugin-based providers and built-in providers.
 */
class ProviderRegistry {
public:
    // Factory function for creating built-in providers
    using BuiltinFactory = std::function<bool(QWidget* parent, QString* target)>;
    
    struct ProviderInfo {
        QString name;          // Display name (e.g., "Process Memory")
        QString identifier;    // Unique ID (e.g., "process")
        IProviderPlugin* plugin;  // Plugin (if plugin-based)
        BuiltinFactory factory;   // Factory (if built-in)
        bool isBuiltin;
        QString dllFileName;   // Original DLL/SO filename (plugin-based only)

        ProviderInfo(const QString& n, const QString& id, IProviderPlugin* p,
                     const QString& dll = {})
            : name(n), identifier(id), plugin(p), factory(nullptr),
              isBuiltin(false), dllFileName(dll) {}

        ProviderInfo(const QString& n, const QString& id, BuiltinFactory f)
            : name(n), identifier(id), plugin(nullptr), factory(f), isBuiltin(true) {}
    };
    
    static ProviderRegistry& instance();
    
    // Register a plugin-based provider
    void registerProvider(const QString& name, const QString& identifier, IProviderPlugin* plugin,
                          const QString& dllFileName = {});
    
    // Register a built-in provider with a factory function
    void registerBuiltinProvider(const QString& name, const QString& identifier, BuiltinFactory factory);
    
    // Unregister a provider (called when unloading plugins)
    void unregisterProvider(const QString& identifier);
    
    // Get all registered providers
    const QList<ProviderInfo>& providers() const { return m_providers; }
    
    // Find provider by identifier
    const ProviderInfo* findProvider(const QString& identifier) const;
    
    // Clear all providers
    void clear();

    // Shared labels, descriptions, icons, and stable routing IDs for every
    // source entry point. File is available even without loaded plugins.
    static QVector<rcx::SourceEntry> sourceActions();

    // Native-menu fallback using the same provider catalog as the popup.
    // Provider management commands stay separate from source selection.
    static void populateSourceMenu(QMenu* menu,
                                   const QVector<SavedSourceDisplay>& savedSources = {});

private:
    ProviderRegistry() = default;
    QList<ProviderInfo> m_providers;
};
