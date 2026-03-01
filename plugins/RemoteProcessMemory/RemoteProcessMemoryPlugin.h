#pragma once
#include "../../src/iplugin.h"
#include "../../src/providers/provider.h"

#include <cstdint>
#include <memory>
#include <QMutex>
#include <QHash>
#include <QVector>

struct IpcClient;   /* defined in .cpp */

/* ── Provider ─────────────────────────────────────────────────────── */

class RemoteProcessProvider : public rcx::Provider
{
public:
    struct ModuleInfo { QString name; uint64_t base; uint64_t size; };

    RemoteProcessProvider(uint32_t pid, const QString& processName,
                          std::shared_ptr<IpcClient> ipc);
    ~RemoteProcessProvider() override;

    /* required */
    bool read(uint64_t addr, void* buf, int len) const override;
    int  size() const override;

    /* optional */
    bool     write(uint64_t addr, const void* buf, int len) override;
    bool     isWritable() const override { return m_connected; }
    QString  name() const override { return m_processName; }
    QString  kind() const override { return QStringLiteral("RemoteProcess"); }
    bool     isLive() const override { return true; }
    uint64_t base() const override { return m_base; }
    int      pointerSize() const override { return m_pointerSize; }
    bool     isReadable(uint64_t, int len) const override { return m_connected && len >= 0; }
    QString  getSymbol(uint64_t addr) const override;
    uint64_t symbolToAddress(const QString& n) const override;

    uint32_t pid() const { return m_pid; }

private:
    void cacheModules();

    uint32_t m_pid;
    QString  m_processName;
    bool     m_connected;
    uint64_t m_base;
    int      m_pointerSize = 8;
    mutable std::shared_ptr<IpcClient> m_ipc;
    QVector<ModuleInfo> m_modules;
};

/* ── Plugin ───────────────────────────────────────────────────────── */

class RemoteProcessMemoryPlugin : public IProviderPlugin
{
public:
    RemoteProcessMemoryPlugin();
    ~RemoteProcessMemoryPlugin() override;

    std::string Name() const override        { return "Remote Process Memory"; }
    std::string Version() const override     { return "1.0.0"; }
    std::string Author() const override      { return "Reclass"; }
    std::string Description() const override {
        return "Read/write memory via injected payload (shared-memory IPC)";
    }
    k_ELoadType LoadType() const override { return k_ELoadTypeManual; }
    QIcon Icon() const override;

    bool canHandle(const QString& target) const override;
    std::unique_ptr<rcx::Provider> createProvider(const QString& target,
                                                  QString* errorMsg) override;
    uint64_t getInitialBaseAddress(const QString& target) const override;
    bool selectTarget(QWidget* parent, QString* target) override;

    bool providesProcessList() const override { return true; }
    QVector<PluginProcessInfo> enumerateProcesses() override;

private:
    std::shared_ptr<IpcClient> getOrCreateConnection(
        uint32_t pid, QString* errorMsg);

    mutable QMutex m_connectionsMutex;
    QHash<uint32_t, std::shared_ptr<IpcClient>> m_connections;
};

extern "C" RCX_PLUGIN_EXPORT IPlugin* CreatePlugin();
