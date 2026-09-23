#pragma once
#include "provider.h"
#include <QDateTime>

namespace rcx {

// ── Shadow-copy watch set ──
//
// The only watch technique available to a provider that merely READS a target
// from outside it.
//
// Why not PAGE_GUARD or hardware breakpoints, the two obvious answers: both
// raise their event *inside* the target. A guard-page touch raises
// STATUS_GUARD_PAGE_VIOLATION and a debug register match raises #DB, and both
// are delivered to the target and its debugger — not to a process that merely
// holds a VM_READ/VM_OPERATION handle. An external provider can arm a guard
// page (VirtualProtectEx needs only PROCESS_VM_OPERATION) and then never hear
// about the hit, while the first touch silently clears the guard bit and the
// watch evaporates. Hardware breakpoints additionally cost a SetThreadContext
// on every thread, cap out at four, and are trivially clobbered by the target.
// Neither is usable without becoming the target's debugger.
//
// What IS observable from outside is the *effect* of a write: re-read the
// range and compare it with a shadow copy. The honest limits of that, which
// callers see through WatchCaps:
//   * WRITES ONLY — a read leaves nothing behind to observe.
//   * SAMPLED, not trapped — two writes between polls read as one, and a value
//     that is changed and restored between polls reads as none.
//   * No faulting PC and no thread id — nothing traps, so there is no context.
//
// A provider with a real trap source (hypervisor EPT, kernel driver, in-target
// payload) should implement the Provider watch virtuals directly and report
// sampled=false instead of using this.
class PolledWatchSet {
public:
    // Capabilities every shadow-diff backend shares. Providers pass their own
    // method name so the UI can say which one is in use.
    static WatchCaps caps(const QString& method = QStringLiteral("polled-diff")) {
        WatchCaps c;
        c.supported     = true;
        c.access        = WatchAccess::Write;  // reads are not observable this way
        c.maxWatches    = 0;                   // unlimited
        c.maxRangeBytes = 0;
        c.reportsPc     = false;
        c.reportsThread = false;
        c.sampled       = true;
        c.method        = method;
        return c;
    }

    // Arms a watch and takes the baseline immediately, so a write that lands
    // before the first poll is still caught. Refuses a read-only watch rather
    // than accepting one that could never fire.
    int add(uint64_t addr, int size, WatchAccess access, const Provider& prov) {
        if (size <= 0) return -1;
        if (!hasAccess(access, WatchAccess::Write)) return -1;
        Entry e;
        e.id      = m_nextId++;
        e.address = addr;
        e.size    = size;
        e.access  = access;
        e.shadow  = QByteArray(size, Qt::Uninitialized);
        if (!prov.read(addr, e.shadow.data(), size)) {
            // Unreadable right now (not yet mapped). Keep the watch but mark
            // the baseline missing, so the first successful read seeds it
            // instead of reporting the whole range as a change.
            e.seeded = false;
            e.shadow.fill('\0');
        }
        m_entries.append(e);
        return e.id;
    }

    bool remove(int id) {
        for (int i = 0; i < m_entries.size(); ++i)
            if (m_entries[i].id == id) { m_entries.remove(i); return true; }
        return false;
    }

    void clear() { m_entries.clear(); }
    int  count() const { return m_entries.size(); }
    bool isEmpty() const { return m_entries.isEmpty(); }

    // Re-read every watched range and report what changed. Narrowed to the
    // actually-changed span, not the whole range, so a one-byte write in a
    // 4 KB watch reports one byte.
    QVector<WatchHit> poll(const Provider& prov) {
        QVector<WatchHit> hits;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (Entry& e : m_entries) {
            QByteArray cur(e.size, Qt::Uninitialized);
            // A failed read is "can't tell", not "changed": leave the shadow
            // alone so the range isn't reported as churning every tick while
            // the target has it unmapped.
            if (!prov.read(e.address, cur.data(), e.size)) continue;
            if (!e.seeded) { e.shadow = cur; e.seeded = true; continue; }
            if (cur == e.shadow) continue;

            int first = 0;
            while (first < e.size && cur[first] == e.shadow[first]) ++first;
            int last = e.size - 1;
            while (last > first && cur[last] == e.shadow[last]) --last;
            const int span = last - first + 1;

            WatchHit h;
            h.id          = e.id;
            h.address     = e.address + (uint64_t)first;
            h.size        = span;
            h.access      = WatchAccess::Write;
            h.timestampMs = now;
            h.oldBytes    = e.shadow.mid(first, span);
            h.newBytes    = cur.mid(first, span);
            hits.append(h);

            e.shadow = cur;
        }
        return hits;
    }

private:
    struct Entry {
        int         id = 0;
        uint64_t    address = 0;
        int         size = 0;
        WatchAccess access = WatchAccess::Write;
        QByteArray  shadow;
        bool        seeded = true;
    };
    QVector<Entry> m_entries;
    int            m_nextId = 1;
};

} // namespace rcx
