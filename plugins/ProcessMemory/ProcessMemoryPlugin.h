#pragma once
#include "../../src/iplugin.h"
#include "../../src/core.h"

#include <cstdint>

/**
 * Process memory provider
 * Reads/writes memory from a live process using platform APIs
 */
class ProcessMemoryProvider : public rcx::Provider
{
public:
    ProcessMemoryProvider(uint32_t pid, const QString& processName);
    ~ProcessMemoryProvider() override;

    // Required overrides
    bool read(uint64_t addr, void* buf, int len) const override;
    int size() const override;

    // Optional overrides
    bool write(uint64_t addr, const void* buf, int len) override;
    bool isWritable() const override { return m_writable; }
    QString name() const override { return m_processName; }
    QString kind() const override { return QStringLiteral("LocalProcess"); }
    QString getSymbol(uint64_t addr) const override;
    uint64_t symbolToAddress(const QString& name) const override;

    bool isLive() const override { return true; }
    uint64_t base() const override { return m_base; }
    int pointerSize() const override { return m_pointerSize; }
    QVector<rcx::MemoryRegion> enumerateRegions() const override;
    bool isReadable(uint64_t, int len) const override {
#ifdef _WIN32
        return m_handle && len >= 0;
#elif defined(__linux__)
        return m_fd >= 0 && len >= 0;
#endif
    }

    // Process-specific helpers
    uint32_t pid() const { return m_pid; }
    void refreshModules() { m_modules.clear(); cacheModules(); }
    uint64_t peb() const override { return m_peb; }
    QVector<ThreadInfo> tebs() const override;
    QVector<ModuleEntry> enumerateModules() const override;

private:
    void cacheModules();

private:
#ifdef _WIN32
    void* m_handle;
#elif defined(__linux__)
    int m_fd;
#endif
    uint32_t m_pid;
    QString m_processName;
    bool m_writable;
    uint64_t m_base;
    int m_pointerSize = 8;
    uint64_t m_peb = 0;

    struct ModuleInfo {
        QString  name;
        QString  fullPath;
        uint64_t base;
        uint64_t size;
    };
    QVector<ModuleInfo> m_modules;
};

/**
 * Plugin that provides ProcessMemoryProvider
 */
class ProcessMemoryPlugin : public IProviderPlugin
{
public:
    std::string Name() const override { return "Process Memory"; }
    std::string Version() const override { return "1.0.0"; }
    std::string Author() const override { return "Reclass"; }
    std::string Description() const override { return "Read and write memory from local running processes"; }
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
extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
