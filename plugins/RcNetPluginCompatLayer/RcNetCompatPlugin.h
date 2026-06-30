#pragma once
#include "../../src/iplugin.h"
#include "ReClassNET_Plugin.hpp"

#include <QLibrary>
#include <memory>

#ifdef HAS_CLR_BRIDGE
#include "ClrHost.h"
#endif

/**
 * ReclassX plugin that loads ReClass.NET plugin DLLs
 * and exposes them as ReclassX providers.
 *
 * Supports both native DLLs (C exports) and, when built with
 * HAS_CLR_BRIDGE, managed .NET assemblies via in-process CLR hosting.
 *
 * Target string format: "dllpath|pid:processname"
 */
class RcNetCompatPlugin : public IProviderPlugin
{
public:
    // Plugin metadata
    std::string Name() const override { return "ReClass.NET Compat Layer"; }
    std::string Version() const override { return "1.0.0"; }
    std::string Author() const override { return "REECLASS"; }
    std::string Description() const override {
        return "Loads ReClass.NET native and .NET plugin DLLs as REECLASS data sources";
    }
    k_ELoadType LoadType() const override { return k_ELoadTypeAuto; }
    QIcon Icon() const override;

    // IProviderPlugin interface
    bool canHandle(const QString& target) const override;
    std::unique_ptr<rcx::Provider> createProvider(const QString& target, QString* errorMsg) override;
    uint64_t getInitialBaseAddress(const QString& target) const override;
    bool selectTarget(QWidget* parent, QString* target) override;

    // Override process enumeration -- we enumerate via the loaded DLL
    bool providesProcessList() const override { return true; }
    QVector<PluginProcessInfo> enumerateProcesses() override;

private:
    bool loadPlugin(const QString& path, QString* errorMsg = nullptr);
    bool loadNativeDll(const QString& path, QString* errorMsg = nullptr);
    void unloadNativeDll();

#ifdef HAS_CLR_BRIDGE
    bool loadManagedDll(const QString& path, QString* errorMsg = nullptr);
    std::unique_ptr<ClrHost> m_clrHost;
#endif

    std::unique_ptr<QLibrary> m_lib;
    RcNetFunctions            m_fns;
    QString                   m_dllPath;
    bool                      m_isManaged = false;
};

// Plugin export
extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
