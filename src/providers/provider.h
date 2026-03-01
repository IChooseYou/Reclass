#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstdint>
#include <cstring>

namespace rcx {

struct MemoryRegion {
    uint64_t base       = 0;
    uint64_t size       = 0;
    bool     readable   = true;
    bool     writable   = false;
    bool     executable = false;
    QString  moduleName;
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
};

} // namespace rcx
