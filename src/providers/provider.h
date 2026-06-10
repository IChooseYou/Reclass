#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>
#include <cstring>

namespace rcx {

// Classification of a memory region. Used by the scanner to skip
// uninteresting page types (e.g. value scans default to Private only —
// game state lives in heap/VirtualAlloc, not in DLL .rdata).
enum class RegionType : uint8_t {
    Image   = 0,   // Loaded module (PE/ELF/Mach-O): code + .rdata + .data
    Mapped  = 1,   // Memory-mapped file or shared section
    Private = 2,   // Heap, stack, VirtualAlloc — where mutable user data lives
};

struct MemoryRegion {
    uint64_t   base       = 0;
    uint64_t   size       = 0;
    bool       readable   = true;
    bool       writable   = false;
    bool       executable = false;
    QString    moduleName;
    // type lives last so legacy positional initializers
    // {base, size, r, w, x, name} still compile and just default to Private.
    RegionType type       = RegionType::Private;
};

struct VtopResult {
    uint64_t physical = 0;
    uint64_t pml4e = 0, pdpte = 0, pde = 0, pte = 0;
    uint8_t  pageSize = 0;  // 0=4KB, 1=2MB, 2=1GB
    bool     valid = false;
};

class Provider {
public:
    virtual ~Provider() = default;

    // --- Subclasses MUST implement these two ---
    virtual bool read(uint64_t addr, void* buf, int len) const = 0;
    virtual int  size() const = 0;

    // --- Optional overrides ---
    virtual bool write(uint64_t addr, const void* buf, int len) {
        Q_UNUSED(addr); Q_UNUSED(buf); Q_UNUSED(len);
        return false;
    }
    virtual bool isWritable() const { return false; }

    // Human-readable label for this source.
    // Examples: "notepad.exe", "dump.bin", "tcp://10.0.0.1:1337"
    virtual QString name() const { return {}; }

    // Whether data can change externally (e.g. live process, network socket).
    // Auto-refresh is only active for live providers.
    virtual bool isLive() const { return false; }

    // Category tag for the command row Source span.
    // Examples: "File", "Process", "Socket"
    virtual QString kind() const { return QStringLiteral("File"); }

    // Native pointer size of the target (4 for 32-bit, 8 for 64-bit).
    // Providers should override this to report the target's architecture.
    virtual int pointerSize() const { return 8; }

    // Initial base address discovered by the provider (e.g. main module base).
    // Used by the controller to set tree.baseAddress on first attach.
    // For file/buffer providers this is always 0.
    virtual uint64_t base() const { return 0; }

    // Resolve an absolute address to a symbol name.
    // Returns empty string if no symbol is known.
    // Example: "ntdll.dll+0x1A30"
    // BufferProvider: "" (no symbols in flat files)
    virtual QString getSymbol(uint64_t addr) const {
        Q_UNUSED(addr);
        return {};
    }

    // Resolve a module/symbol name to its address (reverse of getSymbol).
    // Returns 0 if the name is not found.
    virtual uint64_t symbolToAddress(const QString& name) const {
        Q_UNUSED(name);
        return 0;
    }

    // Enumerate committed/readable memory regions.
    // Used by the scan engine to know what address ranges to scan.
    // Default: returns empty (scan engine falls back to [0, size())).
    virtual QVector<MemoryRegion> enumerateRegions() const { return {}; }

    // Process Environment Block address (x64 PEB VA in target process).
    // Only meaningful for live process providers. Returns 0 if unavailable.
    virtual uint64_t peb() const { return 0; }

    struct ThreadInfo { uint64_t tebAddress; uint32_t threadId; };
    virtual QVector<ThreadInfo> tebs() const { return {}; }

    struct ModuleEntry { QString name; QString fullPath; uint64_t base; uint64_t size; };
    virtual QVector<ModuleEntry> enumerateModules() const { return {}; }

    // Memoized view of enumerateModules(). enumerateModules() can be an
    // expensive syscall (a Toolhelp module snapshot of a process with hundreds
    // of DLLs — a big game like DayZ). RTTI resolution looks up the owning
    // module several times per pointer candidate AND runs on every refresh, so
    // calling the uncached syscall there stalled attach on module-heavy targets
    // for seconds (kernel providers, with a different module path, didn't lag).
    // A fresh Provider is created on every (re)attach, so this per-instance
    // cache self-refreshes; call invalidateModuleCache() if the target loads or
    // unloads modules mid-session and RTTI must re-resolve.
    const QVector<ModuleEntry>& modulesCached() const {
        if (!m_moduleCacheValid) {
            m_moduleCache = enumerateModules();
            m_moduleCacheValid = true;
        }
        return m_moduleCache;
    }
    void invalidateModuleCache() const { m_moduleCacheValid = false; }

    // --- Kernel paging capabilities (override in kernel providers) ---
    virtual bool hasKernelPaging() const { return false; }
    virtual uint64_t getCr3() const { return 0; }
    virtual VtopResult translateAddress(uint64_t va) const {
        Q_UNUSED(va); return {};
    }
    virtual QVector<uint64_t> readPageTable(uint64_t physAddr,
                                            int startIdx = 0,
                                            int count = 512) const {
        Q_UNUSED(physAddr); Q_UNUSED(startIdx); Q_UNUSED(count);
        return {};
    }

    // --- Derived convenience (non-virtual, never override) ---

    bool isValid() const { return size() > 0; }

    virtual bool isReadable(uint64_t addr, int len) const {
        if (len <= 0) return (len == 0);
        uint64_t ulen = (uint64_t)len;
        return addr <= (uint64_t)size() && ulen <= (uint64_t)size() - addr;
    }

    template<typename T>
    T readAs(uint64_t addr) const {
        T v{};
        read(addr, &v, sizeof(T));
        return v;
    }

    uint8_t  readU8 (uint64_t a) const { return readAs<uint8_t>(a);  }
    uint16_t readU16(uint64_t a) const { return readAs<uint16_t>(a); }
    uint32_t readU32(uint64_t a) const { return readAs<uint32_t>(a); }
    uint64_t readU64(uint64_t a) const { return readAs<uint64_t>(a); }
    float    readF32(uint64_t a) const { return readAs<float>(a);    }
    double   readF64(uint64_t a) const { return readAs<double>(a);   }

    QByteArray readBytes(uint64_t addr, int len) const {
        if (len <= 0) return {};
        QByteArray buf(len, Qt::Uninitialized);
        if (!read(addr, buf.data(), len))
            buf.fill('\0');
        return buf;
    }

    bool writeBytes(uint64_t addr, const QByteArray& d) {
        return write(addr, d.constData(), d.size());
    }

private:
    // ── ABI WARNING — Provider is a fragile base class ──
    // Plugin DLLs (ProcessMemory, KernelMemory, WinDbg, …) inherit from
    // Provider and are compiled separately. ANY data member added here grows
    // sizeof(Provider) and shifts every plugin-side subclass member, so the
    // host and a not-rebuilt plugin disagree on the object layout. The host
    // then reads these cache members where the plugin stored the subclass's
    // own data → garbage QVector<ModuleEntry> → access violation the first
    // time modulesCached() runs (RTTI module resolution during compose).
    // If you add/remove/reorder members here, EVERY provider plugin must be
    // rebuilt. The build enforces this via add_dependencies(Reclass <plugins>)
    // in CMakeLists.txt so a partial app-only build can't ship a stale plugin.
    //
    // Per-instance module-list cache backing modulesCached(). Mutable so the
    // lazy fill works through const lookups in the RTTI path.
    mutable QVector<ModuleEntry> m_moduleCache;
    mutable bool                 m_moduleCacheValid = false;
};

} // namespace rcx
