#pragma once
#include "../../src/providers/provider.h"
#include "ReClassNET_Plugin.hpp"

#include <QString>
#include <QVector>

/**
 * Provider that bridges ReClass.NET native plugin DLL calls
 * to the ReclassX Provider interface.
 */
class RcNetCompatProvider : public rcx::Provider
{
public:
    RcNetCompatProvider(const RcNetFunctions& fns, uint32_t pid,
                        const QString& processName);
    ~RcNetCompatProvider() override;

    // Required overrides
    bool read(uint64_t addr, void* buf, int len) const override;
    int  size() const override;

    // Optional overrides
    bool     write(uint64_t addr, const void* buf, int len) override;
    bool     isWritable() const override { return m_fns.WriteRemoteMemory != nullptr; }
    QString  name() const override { return m_processName; }
    QString  kind() const override { return QStringLiteral("RcNet"); }
    bool     isLive() const override { return true; }
    uint64_t base() const override { return m_base; }
    QString  getSymbol(uint64_t addr) const override;
    uint64_t symbolToAddress(const QString& name) const override;

    // Expose cached modules so RTTI / symbol resolution works (base returns empty).
    QVector<ModuleEntry> enumerateModules() const override {
        QVector<ModuleEntry> result;
        result.reserve(m_modules.size());
        for (const auto& m : m_modules)
            result.push_back(ModuleEntry{m.name, QString(), m.base, m.size});
        return result;
    }

    struct ModuleInfo {
        QString  name;
        uint64_t base;
        uint64_t size;
    };

private:
    void cacheModules();

    RcNetFunctions m_fns;
    RC_Pointer     m_handle = nullptr;
    uint32_t       m_pid;
    QString        m_processName;
    uint64_t       m_base = 0;
    QVector<ModuleInfo> m_modules;
};
