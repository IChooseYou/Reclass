#pragma once

// ── One instance of a row ──
//
// A node can be composed in several places at once: a class embedded twice,
// shown through a pointer and a struct array, or reached again through a
// recursive pointer. Every one of those rows carries the same node id. The
// row bands (hover, selection, focus) used to light every row with the id, so
// pointing at one field lit copies of it all over the view and said nothing
// about what was actually under the mouse or selected. They now mark ONE
// instance: the rows of the place the user pointed at or clicked. Edits still
// act on the node, so every copy of it still changes.
//
// An instance is a row plus the extra rows of its value (a matrix's rows 1–3,
// continuation rows of the same node right below it). A selection remembers
// its instance as an anchor — the first row's address and depth, and its
// index among the node's instances — so it survives a refresh that moves the
// rows, and still resolves when the address changes (a pointer that moved).
//
// QtCore only (plus core.h) — unit-tested without a window.

#include "core.h"

#include <QVector>
#include <algorithm>

namespace rcx {

struct SelectionAnchor {
    uint64_t addr = 0;    // the instance's first row's absolute address
    int      depth = 0;   // and its depth
    int      ordinal = 0; // its index among the instances of the same selection id, in document order

    bool operator==(const SelectionAnchor& o) const {
        return addr == o.addr && depth == o.depth && ordinal == o.ordinal;
    }
    bool operator!=(const SelectionAnchor& o) const { return !(*this == o); }
};

// The first row of the instance `line` belongs to: a value's continuation
// rows belong to the row above them. -1 for a line out of range.
inline int instanceStartLine(const QVector<LineMeta>& meta, int line) {
    if (line < 0 || line >= meta.size()) return -1;
    while (line > 0 && meta[line].isContinuation && meta[line - 1].nodeId == meta[line].nodeId)
        --line;
    return line;
}

// The rows of the instance `line` belongs to: its first row and the
// continuation rows right after it.
inline QVector<int> instanceBlock(const QVector<LineMeta>& meta, int line) {
    QVector<int> out;
    const int start = instanceStartLine(meta, line);
    if (start < 0) return out;
    out.append(start);
    for (int i = start + 1; i < meta.size() && meta[i].isContinuation
                            && meta[i].nodeId == meta[start].nodeId; ++i)
        out.append(i);
    return out;
}

// The first rows of every instance of `selId`, in document order. `nodeLines`
// are the rows composed for the selection's node, ascending (the editor's
// node id → lines index); rows of another kind (a footer for a bare id, a
// member row, an array element) are not instances of it.
inline QVector<int> instanceStarts(const QVector<LineMeta>& meta, const QVector<int>& nodeLines,
                                   uint64_t selId) {
    QVector<int> out;
    for (int ln : nodeLines) {
        if (ln < 0 || ln >= meta.size()) continue;
        const LineMeta& lm = meta[ln];
        if (lm.isContinuation || isSyntheticLine(lm)) continue;
        if (selIdForLine(lm) != selId) continue;
        out.append(ln);
    }
    return out;
}

// The anchor of the instance `line` belongs to.
inline SelectionAnchor anchorForLine(const QVector<LineMeta>& meta, int line) {
    SelectionAnchor a;
    const int start = instanceStartLine(meta, line);
    if (start < 0) return a;
    const LineMeta& lm = meta[start];
    a.addr = lm.offsetAddr;
    a.depth = lm.depth;
    const uint64_t sel = selIdForLine(lm);
    for (int i = 0; i < start; ++i) {
        const LineMeta& o = meta[i];
        if (o.nodeId == lm.nodeId && !o.isContinuation && !isSyntheticLine(o)
            && selIdForLine(o) == sel)
            ++a.ordinal;
    }
    return a;
}

// The rows to mark for `selId`.
//  - With `covered` (the rows a byte selection covers, ascending): every
//    instance whose first row is covered — the bytes themselves are selected.
//  - Otherwise ONE instance: the anchor's (same index, address and depth; then
//    the same address and depth; then the same index), else the first.
inline QVector<int> selectionLines(const QVector<LineMeta>& meta, const QVector<int>& nodeLines,
                                   uint64_t selId, const SelectionAnchor* anchor = nullptr,
                                   const QVector<int>* covered = nullptr) {
    const QVector<int> starts = instanceStarts(meta, nodeLines, selId);
    if (starts.isEmpty()) return {};
    if (covered) {
        QVector<int> out;
        for (int s : starts)
            if (std::binary_search(covered->begin(), covered->end(), s))
                out += instanceBlock(meta, s);
        return out;
    }
    int pick = -1;
    if (anchor) {
        auto same = [&](int s) {
            return meta[s].offsetAddr == anchor->addr && meta[s].depth == anchor->depth;
        };
        const bool inRange = anchor->ordinal >= 0 && anchor->ordinal < starts.size();
        if (inRange && same(starts[anchor->ordinal]))
            pick = starts[anchor->ordinal];
        for (int i = 0; pick < 0 && i < starts.size(); ++i)
            if (same(starts[i])) pick = starts[i];
        if (pick < 0 && inRange)
            pick = starts[anchor->ordinal];
    }
    if (pick < 0) pick = starts.first();
    return instanceBlock(meta, pick);
}

} // namespace rcx
