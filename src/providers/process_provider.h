#pragma once
#include "provider.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

namespace rcx {

class ProcessProvider : public Provider {
    HANDLE   m_handle   = nullptr;
    uint64_t m_base     = 0;
    int      m_size     = 0;
    QString  m_name;

    struct ModuleInfo {
        QString  name;
        uint64_t base;
        uint64_t size;
    };
    QVector<ModuleInfo> m_modules;

public:
    ProcessProvider(HANDLE proc, uint64_t base, int regionSize, const QString& name)
        : m_handle(proc), m_base(base), m_size(regionSize), m_name(name)
    {
        cacheModules();
    }

    ~ProcessProvider() override {
        if (m_handle) CloseHandle(m_handle);
    }

    ProcessProvider(const ProcessProvider&) = delete;
    ProcessProvider& operator=(const ProcessProvider&) = delete;

    int size() const override { return m_size; }

    bool read(uint64_t addr, void* buf, int len) const override {
        SIZE_T got = 0;
        BOOL ok = ReadProcessMemory(m_handle,
            (LPCVOID)(m_base + addr), buf, len, &got);
        return ok && (int)got == len;
    }

    bool isWritable() const override { return true; }

    bool write(uint64_t addr, const void* buf, int len) override {
        SIZE_T got = 0;
        BOOL ok = WriteProcessMemory(m_handle,
            (LPVOID)(m_base + addr), buf, len, &got);
        return ok && (int)got == len;
    }

    QString name() const override { return m_name; }
    QString kind() const override { return QStringLiteral("Process"); }

    // getSymbol takes an absolute virtual address and resolves it to
    // "module.dll+0xOFFSET" using the cached module list.
    QString getSymbol(uint64_t absAddr) const override {
        for (const auto& mod : m_modules) {
            if (absAddr >= mod.base && absAddr < mod.base + mod.size) {
                uint64_t offset = absAddr - mod.base;
                return QStringLiteral("%1+0x%2")
                    .arg(mod.name)
                    .arg(offset, 0, 16, QChar('0'));
            }
        }
        return {};
    }

    HANDLE handle() const { return m_handle; }
    uint64_t baseAddress() const { return m_base; }
    void refreshModules() { m_modules.clear(); cacheModules(); }

private:
    void cacheModules() {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!EnumProcessModulesEx(m_handle, mods, sizeof(mods),
                                   &needed, LIST_MODULES_ALL))
            return;
        int count = qMin((int)(needed / sizeof(HMODULE)), 1024);
        m_modules.reserve(count);
        for (int i = 0; i < count; ++i) {
            MODULEINFO mi{};
            WCHAR modName[MAX_PATH];
            if (GetModuleInformation(m_handle, mods[i], &mi, sizeof(mi))
                && GetModuleBaseNameW(m_handle, mods[i], modName, MAX_PATH))
            {
                m_modules.append({
                    QString::fromWCharArray(modName),
                    (uint64_t)mi.lpBaseOfDll,
                    (uint64_t)mi.SizeOfImage
                });
            }
        }
    }
};

} // namespace rcx
#endif // _WIN32
