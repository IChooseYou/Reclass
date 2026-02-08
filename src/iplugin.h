#pragma once
#include <QString>
#include <QIcon>
#include <memory>
#include <string>

// Forward declaration
namespace rcx { class Provider; }

/**
 * Plugin interface for ReclassX
 * 
 * Plugins are loaded from the "Plugins" folder as DLLs.
 * Each plugin must export a C function: extern "C" __declspec(dllexport) IPlugin* CreatePlugin();
 */
class IPlugin {
public:
    virtual ~IPlugin() = default;
    
    // Plugin metadata
    virtual std::string Name() const = 0;
    virtual std::string Version() const = 0;
    virtual std::string Author() const = 0;
    virtual std::string Description() const = 0;
    virtual QIcon       Icon() const { return QIcon(); }
    
    // Plugin type - determines what functionality it provides
    enum k_EType
    {
        // Provides memory/data sources
        ProviderPlugin,

        // In the future we could make plugins that change the main UI
        // for loading different data sources
    };
    virtual k_EType     Type() const = 0;

    // Plugin load type - determines whether and when the plugin is loaded
    // by the PluginManager
    enum k_ELoadType
    {
        // Plugin is automatically loaded on startup
        k_ELoadTypeAuto,

        // Plugin must be loaded manually via 'Manage Plugins'
        k_ELoadTypeManual,
    };
    virtual k_ELoadType LoadType() const = 0;
};

// Forward declarations
class QWidget;
class QTableWidget;

/**
 * Process information structure for custom process lists
 */
struct PluginProcessInfo {
    uint32_t pid;
    QString name;
    QString path;
    QIcon icon;
    
    PluginProcessInfo() : pid(0) {}
    PluginProcessInfo(uint32_t p, const QString& n, const QString& pth = QString(), const QIcon& i = QIcon())
        : pid(p), name(n), path(pth), icon(i) {}
};

/**
 * Provider plugin interface
 * 
 * Plugins that implement this interface can create Provider instances
 * for reading/writing memory from various sources (processes, files, network, etc.)
 */
class IProviderPlugin : public IPlugin {
public:
    k_EType Type() const override { return ProviderPlugin; }
    
    /**
     * Check if this plugin can create a provider for the given target
     * @param target - Target identifier (e.g., PID for process, path for file)
     * @return true if this plugin can handle the target
     */
    virtual bool canHandle(const QString& target) const = 0;
    
    /**
     * Create a provider instance
     * @param target - Target identifier
     * @param errorMsg - Output parameter for error message if creation fails
     * @return Provider instance, or nullptr on failure
     */
    virtual std::unique_ptr<rcx::Provider> createProvider(const QString& target, QString* errorMsg = nullptr) = 0;
    
    /**
     * Get initial base address for the provider (optional)
     * Called after createProvider to set the document's base address
     * @param target - Same target identifier passed to createProvider
     * @return Initial base address, or 0 if not applicable
     */
    virtual uint64_t getInitialBaseAddress(const QString& target) const { Q_UNUSED(target); return 0; }
    
    /**
     * Show a dialog to select a target (e.g., process picker)
     * @param parent - Parent widget for dialog
     * @param target - Output parameter for selected target
     * @return true if user selected a target, false if cancelled
     */
    virtual bool selectTarget(QWidget* parent, QString* target) = 0;
    
    /**
     * Get custom process list (optional)
     * 
     * If implemented, this allows the plugin to override the default process enumeration.
     * Return an empty list to use the default process picker.
     * 
     * @return List of processes to display, or empty list to use default
     */
    virtual QVector<PluginProcessInfo> enumerateProcesses() { return QVector<PluginProcessInfo>(); }
    
    /**
     * Check if this plugin wants to override the process list
     * @return true if enumerateProcesses() should be called
     */
    virtual bool providesProcessList() const { return false; }
};

// Plugin factory function signature
typedef IPlugin* (*CreatePluginFunc)();

#define IPLUGIN_IID "com.reclassx.IPlugin/1.0"
