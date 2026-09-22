#pragma once
#include "provider.h"
#include <QHash>
#include <QSet>
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

    // Pages we never have to re-read for the lifetime of this snapshot.
    // Populated by the controller after a refresh once it's seen the page
    // belongs to a process module's executable image (.text/.rdata/etc.).
    // Module memory is read-only at runtime, so re-syscalling it every
    // 200 ms is pure waste — the cost is highest for RTTI-decorated rows
    // because each one chases a vtable pointer into module memory.
    QSet<uint64_t> m_permanentPages;

    // Pages whose last read FAILED. They read as zeros and report
    // not-readable, and they never fall through to the real provider: the
    // refresh loop already asked and was refused, and merging a zero-filled
    // stand-in (what readBytes used to hand back) made unreadable memory
    // look like real zero bytes — the unreadable strike never showed.
    QSet<uint64_t> m_failedPages;

    // While set, every page a read or readability check touches is noted
    // here. The controller records what compose ACTUALLY reads — RTTI
    // vtables, dereferenced targets, anything its own range walk misses —
    // and asks for those pages on the next tick, so the capture set is what
    // the view reads rather than a guess at it.
    mutable QSet<uint64_t>* m_touchSink = nullptr;
    mutable uint64_t        m_lastTouched = ~uint64_t(0);

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
        bool ok = true;
        while (remaining > 0) {
            uint64_t pageAddr = cur & kPageMask;
            int pageOff = static_cast<int>(cur - pageAddr);
            int chunk = qMin(remaining, static_cast<int>(kPageSize - pageOff));
            noteTouched(pageAddr);
            auto it = m_pages.constFind(pageAddr);
            if (it != m_pages.constEnd()) {
                std::memcpy(out, it->constData() + pageOff, chunk);
            } else if (m_failedPages.contains(pageAddr)) {
                std::memset(out, 0, chunk);
                ok = false;
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
                if (!m_real->read(cur, out, chunk)) {
                    std::memset(out, 0, chunk);
                    ok = false;
                }
            } else {
                std::memset(out, 0, chunk);
            }
            out += chunk;
            cur += chunk;
            remaining -= chunk;
        }
        return ok;
    }

    bool isReadable(uint64_t addr, int len) const override {
        if (len <= 0) return (len == 0);
        uint64_t end = addr + static_cast<uint64_t>(len);
        if (end < addr) return false;   // overflow
        for (uint64_t p = addr & kPageMask; p < end; p += kPageSize) {
            noteTouched(p);
            if (m_failedPages.contains(p)) return false;
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

    // Merge freshly-read pages into the existing snapshot rather than
    // wholesale replacing it. Used by the per-tick refresh once we
    // started skipping pages (permanent / stable / out-of-viewport):
    // an unread page should retain its previous bytes, not vanish.
    void mergePages(const PageMap& fresh, int mainExtent) {
        for (auto it = fresh.constBegin(); it != fresh.constEnd(); ++it) {
            m_pages.insert(it.key(), it.value());
            m_failedPages.remove(it.key());
        }
        m_mainExtent = mainExtent;
    }

    // Pages whose read just failed: drop any bytes held for them so they
    // read as not-readable instead of as whatever was there last time.
    void markFailed(const QVector<uint64_t>& pageAddrs) {
        for (uint64_t p : pageAddrs) {
            const uint64_t page = p & kPageMask;
            m_failedPages.insert(page);
            m_pages.remove(page);
        }
    }
    bool isFailed(uint64_t pageAddr) const {
        return m_failedPages.contains(pageAddr & kPageMask);
    }

    void beginTouchRecording(QSet<uint64_t>* sink) {
        m_touchSink = sink;
        m_lastTouched = ~uint64_t(0);
    }
    void endTouchRecording() { m_touchSink = nullptr; }

private:
    void noteTouched(uint64_t pageAddr) const {
        if (!m_touchSink || pageAddr == m_lastTouched) return;
        m_touchSink->insert(pageAddr);
        m_lastTouched = pageAddr;
    }

public:

    // Mark a page as immutable for the lifetime of this snapshot.
    // The controller calls this once it has classified the page as
    // belonging to a read-only module section.
    void markPermanent(uint64_t pageAddr) {
        m_permanentPages.insert(pageAddr & kPageMask);
    }
    bool isPermanent(uint64_t pageAddr) const {
        return m_permanentPages.contains(pageAddr & kPageMask);
    }
    void clearPermanent() { m_permanentPages.clear(); }

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
    const QSet<uint64_t>& permanentPages() const { return m_permanentPages; }
};

} // namespace rcx
