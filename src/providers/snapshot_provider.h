#pragma once
#include "provider.h"
#include <QHash>
#include <memory>

namespace rcx {

// Page-based snapshot provider.
//
// During async refresh the controller reads pages for the main struct and
// every reachable pointer target.  Compose reads entirely from this page
// table — no fallback to the real provider, no blocking I/O on the UI
// thread.  Pages that were never fetched (truly invalid pointers) simply
// read as zeros.
class SnapshotProvider : public Provider {
    std::shared_ptr<Provider> m_real;
    QHash<uint64_t, QByteArray> m_pages;   // page-aligned addr → 4096-byte page
    int m_mainExtent = 0;                  // logical size of the main struct range

    static constexpr uint64_t kPageSize = 4096;
    static constexpr uint64_t kPageMask = ~(kPageSize - 1);

public:
    using PageMap = QHash<uint64_t, QByteArray>;

    SnapshotProvider(std::shared_ptr<Provider> real, PageMap pages, int mainExtent)
        : m_real(std::move(real))
        , m_pages(std::move(pages))
        , m_mainExtent(mainExtent) {}

    bool read(uint64_t addr, void* buf, int len) const override {
        if (len <= 0) return false;
        char* out = static_cast<char*>(buf);
        uint64_t cur = addr;
        int remaining = len;
        while (remaining > 0) {
            uint64_t pageAddr = cur & kPageMask;
            int pageOff = static_cast<int>(cur - pageAddr);
            int chunk = qMin(remaining, static_cast<int>(kPageSize - pageOff));
            auto it = m_pages.constFind(pageAddr);
            if (it != m_pages.constEnd()) {
                std::memcpy(out, it->constData() + pageOff, chunk);
            } else if (m_real) {
                // Fall through to the real provider for pages the async
                // refresh didn't pre-fetch. Required by the auto-RTTI
                // hint: walkRttiItanium peeks at vtable[-8] / type_info
                // bytes that live in module .rdata, which the controller's
                // collectPointerRanges only fetches for *expanded*
                // typed pointers — collapsed ones (the common case for
                // a Class* field) leave those pages out of the snapshot.
                // A handful of qword reads on the UI thread is fine; the
                // alternative is the RTTI feature silently doing nothing.
                if (!m_real->read(cur, out, chunk))
                    std::memset(out, 0, chunk);
            } else {
                std::memset(out, 0, chunk);
            }
            out += chunk;
            cur += chunk;
            remaining -= chunk;
        }
        return true;
    }

    bool isReadable(uint64_t addr, int len) const override {
        if (len <= 0) return (len == 0);
        uint64_t end = addr + static_cast<uint64_t>(len);
        if (end < addr) return false;   // overflow
        for (uint64_t p = addr & kPageMask; p < end; p += kPageSize) {
            if (!m_pages.contains(p)) {
                // Page not in snapshot — defer to the real provider's
                // bounds check (e.g. ProcessMemoryProvider returns true
                // whenever its handle is open, so RTTI fall-through reads
                // can proceed). Without this fall-through, callers that
                // gate on isReadable() would never invoke read() and
                // miss out on the read-fallback path above.
                if (m_real && m_real->isReadable(addr, len)) return true;
                return false;
            }
        }
        return true;
    }

    int size() const override { return m_mainExtent; }
    bool isWritable() const override { return m_real ? m_real->isWritable() : false; }
    bool isLive() const override { return m_real ? m_real->isLive() : false; }
    QString name() const override { return m_real ? m_real->name() : QString(); }
    QString kind() const override { return m_real ? m_real->kind() : QStringLiteral("File"); }
    int pointerSize() const override { return m_real ? m_real->pointerSize() : 8; }
    uint64_t base() const override { return m_real ? m_real->base() : 0; }
    QString getSymbol(uint64_t addr) const override {
        return m_real ? m_real->getSymbol(addr) : QString();
    }
    uint64_t symbolToAddress(const QString& n) const override {
        return m_real ? m_real->symbolToAddress(n) : 0;
    }
    // Forward module enumeration to the real provider — without this,
    // compose's auto-RTTI detect (which calls findOwningModule on every
    // candidate vtable address) gets an empty module list and refuses
    // to walk anything. The real provider already cached its module
    // list at attach time, so this is a cheap copy.
    QVector<ModuleEntry> enumerateModules() const override {
        return m_real ? m_real->enumerateModules() : QVector<ModuleEntry>{};
    }
    QVector<MemoryRegion> enumerateRegions() const override {
        return m_real ? m_real->enumerateRegions() : QVector<MemoryRegion>{};
    }
    uint64_t peb() const override { return m_real ? m_real->peb() : 0; }
    QVector<ThreadInfo> tebs() const override {
        return m_real ? m_real->tebs() : QVector<ThreadInfo>{};
    }

    bool write(uint64_t addr, const void* buf, int len) override {
        if (!m_real) return false;
        bool ok = m_real->write(addr, buf, len);
        if (ok) patchPages(addr, buf, len);
        return ok;
    }

    // Replace the entire page table (called after async read completes)
    void updatePages(PageMap pages, int mainExtent) {
        m_pages = std::move(pages);
        m_mainExtent = mainExtent;
    }

    // Patch specific bytes in existing pages (called after user writes a value)
    void patchPages(uint64_t addr, const void* buf, int len) {
        const char* src = static_cast<const char*>(buf);
        uint64_t cur = addr;
        int remaining = len;
        while (remaining > 0) {
            uint64_t pageAddr = cur & kPageMask;
            int pageOff = static_cast<int>(cur - pageAddr);
            int chunk = qMin(remaining, static_cast<int>(kPageSize - pageOff));
            auto it = m_pages.find(pageAddr);
            if (it != m_pages.end()) {
                std::memcpy(it->data() + pageOff, src, chunk);
            }
            src += chunk;
            cur += chunk;
            remaining -= chunk;
        }
    }

    const PageMap& pages() const { return m_pages; }
};

} // namespace rcx
