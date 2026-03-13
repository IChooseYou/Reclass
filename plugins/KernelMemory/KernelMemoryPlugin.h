#pragma once
#include "../../src/iplugin.h"
#include "../../src/core.h"
#include "rcx_drv_protocol.h"

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────
// Provider variants
// ─────────────────────────────────────────────────────────────────────────

/**
 * Kernel-mode process memory provider.
 * Reads/writes target process virtual memory via IOCTL_RCX_READ/WRITE_MEMORY.
 */
class KernelProcessProvider : public rcx::Provider
{
public:
    KernelProcessProvider(void* driverHandle, uint32_t pid, const QString& processName);
    ~KernelProcessProvider() override = default;

    bool read(uint64_t addr, void* buf, int len) const override;
    int  size() const override;

    bool write(uint64_t addr, const void* buf, int len) override;
    bool isWritable() const override { return true; }
    QString name() const override { return m_processName; }
    QString kind() const override { return QStringLiteral("KernelProcess"); }
    QString getSymbol(uint64_t addr) const override;
    uint64_t symbolToAddress(const QString& name) const override;

    bool isLive() const override { return true; }
    uint64_t base() const override { return m_base; }
    int pointerSize() const override { return m_pointerSize; }
    QVector<rcx::MemoryRegion> enumerateRegions() const override;
    bool isReadable(uint64_t, int len) const override { return m_driverHandle && len >= 0; }

    uint32_t pid() const { return m_pid; }
    uint64_t peb() const override { return m_peb; }
    QVector<ThreadInfo> tebs() const override;

    // ── Paging / address translation ──
    bool hasKernelPaging() const override { return true; }
    uint64_t getCr3() const override;
    rcx::VtopResult translateAddress(uint64_t va) const override;
    QVector<uint64_t> readPageTable(uint64_t physAddr, int startIdx = 0, int count = 512) const override;
    void* driverHandle() const { return m_driverHandle; }

private:
    void queryPeb();
    void cacheModules();

    void*    m_driverHandle;
    uint32_t m_pid;
    QString  m_processName;
    uint64_t m_base = 0;
    int      m_pointerSize = 8;
    uint64_t m_peb = 0;
    mutable uint64_t m_cr3Cache = 0;

    struct ModuleInfo {
        QString  name;
        uint64_t base;
        uint64_t size;
    };
    QVector<ModuleInfo> m_modules;
};

/**
 * Kernel-mode physical memory provider.
 * Reads/writes raw physical addresses via IOCTL_RCX_READ/WRITE_PHYS.
 */
class KernelPhysProvider : public rcx::Provider
{
public:
    KernelPhysProvider(void* driverHandle, uint64_t baseAddr);
    ~KernelPhysProvider() override = default;

    bool read(uint64_t addr, void* buf, int len) const override;
    int  size() const override { return m_driverHandle ? 0x10000 : 0; }

    bool write(uint64_t addr, const void* buf, int len) override;
    bool isWritable() const override { return true; }
    QString name() const override { return QStringLiteral("Physical Memory"); }
    QString kind() const override { return QStringLiteral("Physical"); }

    bool isLive() const override { return true; }
    uint64_t base() const override { return m_baseAddr; }
    bool isReadable(uint64_t, int len) const override { return m_driverHandle && len >= 0; }

    void setBaseAddr(uint64_t addr) { m_baseAddr = addr; }
    void* driverHandle() const { return m_driverHandle; }

private:
    void*    m_driverHandle;
    uint64_t m_baseAddr;
};

// ─────────────────────────────────────────────────────────────────────────
// Plugin
// ─────────────────────────────────────────────────────────────────────────

class KernelMemoryPlugin : public IProviderPlugin
{
public:
    KernelMemoryPlugin();
    ~KernelMemoryPlugin() override;

    std::string Name() const override { return "Kernel Memory"; }
    std::string Version() const override { return "1.0.0"; }
    std::string Author() const override { return "Reclass"; }
    std::string Description() const override { return "Read and write memory via kernel driver (IOCTL)"; }
    k_ELoadType LoadType() const override { return k_ELoadTypeManual; }
    QIcon Icon() const override;

    bool canHandle(const QString& target) const override;
    std::unique_ptr<rcx::Provider> createProvider(const QString& target, QString* errorMsg) override;
    uint64_t getInitialBaseAddress(const QString& target) const override;
    bool selectTarget(QWidget* parent, QString* target) override;

    bool providesProcessList() const override { return true; }
    QVector<PluginProcessInfo> enumerateProcesses() override;
    void populatePluginMenu(QMenu* menu) override;

private:
    bool ensureDriverLoaded(QString* errorMsg = nullptr);
    void unloadDriver();   // close handle only — service stays running
    void stopDriver();     // full cleanup: close handle + stop + delete service
    QString driverPath() const;

#ifdef _WIN32
    HANDLE m_driverHandle = INVALID_HANDLE_VALUE;
#endif
    bool m_driverLoaded = false;
};

// Plugin export
extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
