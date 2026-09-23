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

// ── Memory watchpoints (optional provider capability) ──
//
// What a provider can DETECT, not what it is asked for. The honest split:
// a provider that only holds a read handle on the target can see writes by
// re-reading (the effect), but never reads, because a read leaves no trace.
// A provider with a real trap source — a hypervisor's EPT, a kernel driver,
// an in-target payload — can report reads, the faulting PC and the thread.
enum class WatchAccess : uint8_t {
    None      = 0,
    Read      = 1,
    Write     = 2,
    ReadWrite = 3,   // Read | Write
    Execute   = 4,
};

inline WatchAccess operator|(WatchAccess a, WatchAccess b) {
    return static_cast<WatchAccess>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool hasAccess(WatchAccess set, WatchAccess want) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(want)) == static_cast<uint8_t>(want)
           && want != WatchAccess::None;
}

// One observed touch of a watched range.
struct WatchHit {
    int         id          = 0;   // the watch that fired
    uint64_t    address     = 0;   // exact address touched (may be inside the range)
    int         size        = 0;   // bytes affected
    WatchAccess access      = WatchAccess::Write;
    uint64_t    pc          = 0;   // faulting instruction; 0 = unknown (sampled, not trapped)
    uint32_t    threadId    = 0;   // 0 = unknown
    qint64      timestampMs = 0;   // wall clock when observed
    QByteArray  oldBytes, newBytes;  // populated when the provider knows them
};

// What a provider's watch implementation can actually do. A caller shows this
// to the user rather than promising behaviour the backend cannot deliver.
struct WatchCaps {
    bool        supported     = false;
    WatchAccess access        = WatchAccess::None;  // detectable accesses
    int         maxWatches    = 0;   // 0 = unlimited
    int         maxRangeBytes = 0;   // 0 = unlimited
    bool        reportsPc     = false;
    bool        reportsThread = false;
    bool        sampled       = true;  // true = polled (can miss/merge); false = trapped
    QString     method;                // "polled-diff", "hardware", "hypervisor", …
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

    // --- Memory watchpoints (optional; default = unsupported) ---
    //
    // Watch an address or range for reads/writes. Every provider picks its own
    // mechanism: shadow-copy polling (see watch_poll.h, the only thing that
    // works from outside a target without being its debugger), a kernel
    // driver, an in-target payload, or a hypervisor's EPT. Callers MUST consult
    // watchCapabilities() first — a backend that cannot see reads says so
    // rather than silently never firing.
    //
    // NOTE (ABI): these are virtuals on a fragile base class. Adding, removing
    // or reordering any of them mis-routes every virtual call in a plugin built
    // against a different provider.h, so RCX_PROVIDER_ABI_VERSION must be
    // bumped alongside (see iplugin.h).

    virtual WatchCaps watchCapabilities() const { return {}; }

    // Arm a watch. Returns a positive id, or -1 if unsupported/refused
    // (range too large, out of hardware slots, access not detectable).
    virtual int addWatch(uint64_t addr, int size, WatchAccess access) {
        Q_UNUSED(addr); Q_UNUSED(size); Q_UNUSED(access);
        return -1;
    }

    virtual bool removeWatch(int id) { Q_UNUSED(id); return false; }
    virtual void clearWatches() {}

    // Drain everything observed since the last call. For a sampled backend
    // this IS the sampling point, so call it on a regular tick; for a trapped
    // backend it drains a queue filled asynchronously.
    virtual QVector<WatchHit> pollWatchHits() { return {}; }

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
