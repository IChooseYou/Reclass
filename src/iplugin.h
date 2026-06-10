#pragma once
#include <QString>
#include <QIcon>
#include <memory>
#include <string>
#include <cstdint>

#ifdef _WIN32
    #define RCX_PLUGIN_EXPORT __declspec(dllexport)
#else
    #define RCX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// Forward declarations
namespace rcx { class Provider; }
class QMenu;

/**
 * Plugin interface for Reclass
 *
 * Plugins are loaded from the "Plugins" folder as shared libraries.
 * Each plugin must export a C function: extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
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
    bool is32Bit = false;

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

    /**
     * Add plugin-specific actions to the source menu (optional).
     * Called each time the source menu is shown. Only add items when relevant
     * (e.g., "Unload Driver" only when the driver is loaded).
     */
    virtual void populatePluginMenu(QMenu*) {}
};

// Plugin factory function signature
typedef IPlugin* (*CreatePluginFunc)();

#define IPLUGIN_IID "com.reclass.IPlugin/1.0"

// ── Provider ABI guard ──
// Provider plugins derive from rcx::Provider, a *fragile base class*: adding a
// data member to it (as modulesCached()'s cache did) changes sizeof(Provider)
// and shifts every plugin-side subclass member. A plugin built against a
// different provider.h than the host then disagrees on object layout and
// corrupts memory the first time the host touches a base-class member — an
// access violation deep in the RTTI/module path with no obvious culprit.
//
// To turn that silent crash into a graceful, diagnosable refusal, every
// provider plugin exports an ABI token (RcxPluginAbiToken) and the host
// (PluginManager) compares it to its own before instantiating the plugin.
// A missing or mismatched token means "rebuild this plugin"; the host logs it
// and skips the plugin instead of loading a memory-corrupting one.
//
// The token folds the two ABI surfaces a plugin can disagree with the host on:
//   * low 32 bits = sizeof(rcx::Provider) — auto-captures DATA-MEMBER changes
//     (the layout shift that caused the original crash);
//   * high 32 bits = RCX_PROVIDER_ABI_VERSION — a hand-bumped counter for
//     changes sizeof can't see, i.e. Provider VTABLE/semantic changes (adding,
//     removing, or reordering a virtual leaves sizeof unchanged but mis-routes
//     every virtual call). Bump it whenever you touch Provider's virtual
//     interface or change the meaning of an existing method.
// The build also enforces a rebuild on ANY provider.h change via
// add_dependencies(Reclass <plugins>) in CMakeLists; this token is the
// belt-and-suspenders that catches an out-of-band / hand-copied stale DLL.
//
// Place RCX_DEFINE_PLUGIN_ABI() once at file scope in each plugin's .cpp,
// where providers/provider.h is fully included (alongside CreatePlugin()).
#define RCX_PROVIDER_ABI_VERSION 1u

// Both sides MUST compute the token identically. Keep this in one place so the
// plugin macro and the host check can't drift apart.
#define RCX_PROVIDER_ABI_TOKEN \
    ((static_cast<uint64_t>(RCX_PROVIDER_ABI_VERSION) << 32) \
     | static_cast<uint64_t>(sizeof(::rcx::Provider)))

typedef uint64_t (*RcxPluginAbiTokenFunc)();

#define RCX_DEFINE_PLUGIN_ABI() \
    extern "C" RCX_PLUGIN_EXPORT uint64_t RcxPluginAbiToken() { \
        return RCX_PROVIDER_ABI_TOKEN; \
    }
