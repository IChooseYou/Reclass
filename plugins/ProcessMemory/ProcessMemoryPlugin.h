#pragma once
#include "../../src/iplugin.h"
#include "../../src/core.h"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>

/**
 * Windows process memory provider
 * Reads/writes memory from a live process using Win32 API
 */
class ProcessMemoryProvider : public rcx::Provider {
public:
    ProcessMemoryProvider(DWORD pid, const QString& processName);
    ~ProcessMemoryProvider() override;
    
    // Required overrides
    bool read(uint64_t addr, void* buf, int len) const override;
    int size() const override { return m_handle ? INT_MAX : NULL; } // Process memory has no fixed size
    
    // Optional overrides
    bool write(uint64_t addr, const void* buf, int len) override;
    bool isWritable() const override { return m_writable; }
    QString name() const override { return m_processName; }
    QString kind() const override { return QStringLiteral("LocalProcess"); }
    QString getSymbol(uint64_t addr) const override;
    
    // Process-specific helpers
    DWORD pid() const { return m_pid; }
    uint64_t baseAddress() const { return m_base; }
    void refreshModules() { m_modules.clear(); cacheModules(); }

private:
    void cacheModules();
    
private:
    HANDLE m_handle;
    DWORD m_pid;
    QString m_processName;
    bool m_writable;
    uint64_t m_base;

    struct ModuleInfo {
        QString  name;
        uint64_t base;
        uint64_t size;
    };
    QVector<ModuleInfo> m_modules;
};

/**
 * Plugin that provides ProcessMemoryProvider
 */
class ProcessMemoryPlugin : public IProviderPlugin {
public:
    std::string Name() const override { return "Process Memory"; }
    std::string Version() const override { return "1.0.0"; }
    std::string Author() const override { return "ReclassX"; }
    std::string Description() const override { return "Read and write memory from local running Windows processes"; }
    k_ELoadType LoadType() const override { return k_ELoadTypeAuto; }
    QIcon Icon() const override;
    
    bool canHandle(const QString& target) const override;
    std::unique_ptr<rcx::Provider> createProvider(const QString& target, QString* errorMsg) override;
    uint64_t getInitialBaseAddress(const QString& target) const override;
    bool selectTarget(QWidget* parent, QString* target) override;
    
    // Optional: provide custom process list
    bool providesProcessList() const override { return true; }
    QVector<PluginProcessInfo> enumerateProcesses() override;
};

// Plugin export
extern "C" __declspec(dllexport) IPlugin* CreatePlugin();
