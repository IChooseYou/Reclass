#pragma once

// ── TimelineFrameProvider: a past moment, as a Provider ──
//
// compose() renders whatever Provider it is handed, so showing the past is a
// provider swap. The one rule that makes it honest: this provider NEVER reads
// live memory. A page that was not captured at that moment reads as zeros
// and reports not-readable — it is shown as not captured, not quietly filled
// in with today's bytes (which is exactly what SnapshotProvider's fall-through
// would do).
//
// Only STATIC facts are forwarded to the real provider: its name, pointer
// size, symbols and module list. None of those read the target's memory
// contents.
//
// A subclass, never new Provider members: Provider is a fragile plugin ABI.

#include "providers/provider.h"
#include "timeline/tl_store.h"

#include <memory>

namespace rcx::tl {

class TimelineFrameProvider final : public Provider {
public:
    TimelineFrameProvider(std::shared_ptr<Provider> real, FramePtr frame, int extent)
        : m_real(std::move(real)), m_frame(std::move(frame)), m_extent(extent) {}

    void setFrame(FramePtr frame) { m_frame = std::move(frame); }
    const FramePtr& frame() const { return m_frame; }
    void setExtent(int extent) { m_extent = extent; }

    PageState stateOf(uint64_t addr) const {
        return m_frame ? m_frame->stateOf(addr) : PageState::NotCovered;
    }

    bool read(uint64_t addr, void* buf, int len) const override {
        if (len <= 0) return false;
        char* out = static_cast<char*>(buf);
        uint64_t cur = addr;
        int remaining = len;
        bool ok = true;
        while (remaining > 0) {
            const uint64_t page = cur & kPageMask;
            const int off = int(cur - page);
            const int chunk = qMin(remaining, int(kPageSize) - off);
            const QByteArray* bytes = nullptr;
            if (m_frame) {
                auto it = m_frame->pages.constFind(page);
                if (it != m_frame->pages.constEnd() && it->size() == int(kPageSize)) bytes = &it.value();
            }
            if (bytes) {
                std::memcpy(out, bytes->constData() + off, size_t(chunk));
            } else {
                std::memset(out, 0, size_t(chunk));
                ok = false;
            }
            out += chunk;
            cur += uint64_t(chunk);
            remaining -= chunk;
            if (cur == 0 && remaining > 0) {   // wrapped past the top of the address space
                std::memset(out, 0, size_t(remaining));
                return false;
            }
        }
        return ok;
    }

    // Not readable only where a read FAILED then. Bytes nobody was watching
    // are not an error: compose carries on — a pointer's target is still laid
    // out at the address it had — and the controller marks those values not
    // captured (notCaptured below). Refusing them here sent compose down its
    // null-pointer path, which draws zeros: a plausible value that never was.
    bool isReadable(uint64_t addr, int len) const override {
        if (len <= 0) return len == 0;
        const uint64_t last = addr + uint64_t(len - 1);
        if (last < addr || !m_frame) return false;
        for (uint64_t p = addr & kPageMask;; p += kPageSize) {
            if (m_frame->stateOf(p) == PageState::Unreadable) return false;
            if (p + (kPageSize - 1) >= last) break;
        }
        return true;
    }

    // Some byte of [addr, addr+len) has no captured value at this moment —
    // never watched, not sampled yet, or lost — rather than a failed read.
    bool notCaptured(uint64_t addr, int len) const {
        if (len <= 0) return false;
        if (!m_frame) return true;
        uint64_t last = addr + uint64_t(len - 1);
        if (last < addr) last = UINT64_MAX;
        for (uint64_t p = addr & kPageMask;; p += kPageSize) {
            const PageState s = m_frame->stateOf(p);
            if (s == PageState::NotCovered || s == PageState::NotYetSampled || s == PageState::Lost)
                return true;
            if (p + (kPageSize - 1) >= last) break;
        }
        return false;
    }

    int  size() const override { return qMax(1, m_extent); }
    // The past is read-only. This alone disables inline value edits
    // (RcxEditor::canWriteMemory reads it).
    bool isWritable() const override { return false; }
    bool write(uint64_t, const void*, int) override { return false; }
    // Reported live so compose renders exactly what it did at the time (the
    // "name this class" chips on null pointers key off it). Value history is
    // gated separately, by the controller knowing it is showing the past.
    bool isLive() const override { return true; }

    QString  name() const override { return m_real ? m_real->name() : QString(); }
    QString  kind() const override { return m_real ? m_real->kind() : QStringLiteral("Process"); }
    int      pointerSize() const override { return m_real ? m_real->pointerSize() : 8; }
    uint64_t base() const override { return m_real ? m_real->base() : 0; }
    QString  getSymbol(uint64_t addr) const override { return m_real ? m_real->getSymbol(addr) : QString(); }
    uint64_t symbolToAddress(const QString& n) const override { return m_real ? m_real->symbolToAddress(n) : 0; }
    QVector<ModuleEntry> enumerateModules() const override {
        return m_real ? m_real->modulesCached() : QVector<ModuleEntry>{};
    }

private:
    std::shared_ptr<Provider> m_real;
    FramePtr m_frame;
    int m_extent = 0;
};

} // namespace rcx::tl
