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
        for (uint64_t p = addr & kPageMask; p < end; p += kPageSize) {
            if (!m_pages.contains(p)) return false;
        }
        return true;
    }

    int size() const override { return m_mainExtent; }
    bool isWritable() const override { return m_real ? m_real->isWritable() : false; }
    bool isLive() const override { return m_real ? m_real->isLive() : false; }
    QString name() const override { return m_real ? m_real->name() : QString(); }
    QString kind() const override { return m_real ? m_real->kind() : QStringLiteral("File"); }
    QString getSymbol(uint64_t addr) const override {
        return m_real ? m_real->getSymbol(addr) : QString();
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
