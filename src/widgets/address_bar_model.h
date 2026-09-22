#pragma once

// The address bar's model: the value type the controller pushes to the bar
// once per refresh, and the pure NodeTree queries any widget over the drill
// path needs — siblings of a crumb, the dotted path as text
// and back. Core-only on purpose (core.h, no QWidget/QPainter): the
// test_address_bar_model target builds it without the controller or the
// editor, and the bar itself (src/widgets/address_bar.h, P2) consumes it
// without owning any tree logic.

#include "core.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <functional>

namespace rcx {

// Source liveness on the bar's chip. Mirrors RcxController::SourceStatus by
// numeric value (None=0, Static=1, Live=2, Stale=3, Disconnected=4); the
// controller static_asserts the two stay aligned. Kept as plain ints here so
// this header does not have to include controller.h.
namespace liveness {
inline constexpr int None         = 0;
inline constexpr int Static       = 1;
inline constexpr int Live         = 2;
inline constexpr int Stale        = 3;
inline constexpr int Disconnected = 4;
} // namespace liveness

// One snapshot of everything the bar paints. Pushed by the controller per
// refresh; the bar early-returns on operator== so a live tick with nothing
// new costs no relayout.
struct AddressBarState {
    QString        sourceName;        // Provider::name(); empty = "Select source"
    QString        sourceKindId;      // Provider::kind() — picks the chip icon
    int            liveness = liveness::None;
    uint64_t       baseAddress = 0;   // NodeTree::baseAddress
    QString        baseFormula;       // NodeTree::baseAddressFormula (full, never elided)
    uint64_t       resolvedBase = 0;  // what the formula evaluates to right now
    QVector<Crumb> crumbs;            // the trail, dotted production shape
    QString        trailPath;         // the trail as one dotted path (trailPathText):
                                      // what the bar's path edit opens on. Carried
                                      // in the state — it changes exactly when the
                                      // crumbs do, so a callback would be no cheaper
    uint64_t       viewRootId = 0;    // RcxController::viewRootId(); 0 = show-all, where
                                      // crumbs[0] names the FIRST root but none is "current"
    bool           canBack = false;
    bool           canForward = false;
    bool           canUp = false;
    // The timeline's buttons beside the address — Record / Stop, and Back to
    // live while looking back at a recording — present only while the
    // timeline is switched on for a live source. Values are live otherwise:
    // there is no pausing them.
    bool           timeline = false;
    bool           canRecord = false;    // a live source the timeline can record
    bool           recording = false;
    // Looking back at a recording: "Back to live" appears.
    bool           past = false;

    bool operator==(const AddressBarState& o) const {
        return sourceName == o.sourceName
            && sourceKindId == o.sourceKindId
            && liveness == o.liveness
            && baseAddress == o.baseAddress
            && baseFormula == o.baseFormula
            && resolvedBase == o.resolvedBase
            && crumbs == o.crumbs
            && trailPath == o.trailPath
            && viewRootId == o.viewRootId
            && canBack == o.canBack
            && canForward == o.canForward
            && canUp == o.canUp
            && timeline == o.timeline
            && canRecord == o.canRecord
            && recording == o.recording
            && past == o.past;
    }
    bool operator!=(const AddressBarState& o) const { return !(*this == o); }
};

// ── Tree queries ──

namespace detail {
// The root a show-all view (viewRootId 0) labels: the first top-level struct,
// the same node rootClassNames() lists first and classLabelOf(0) names.
inline int firstRootStructIdx(const NodeTree& tree) {
    for (int i = 0; i < tree.nodes.size(); ++i)
        if (tree.nodes[i].parentId == 0 && tree.nodes[i].kind == NodeKind::Struct) return i;
    return -1;
}
inline int classIdx(const NodeTree& tree, uint64_t classId) {
    int idx = classId ? tree.indexOfId(classId) : -1;
    return idx >= 0 ? idx : firstRootStructIdx(tree);
}
// The field name a hop shows in a crumb and in the dotted path — the same
// fallback the controller's addressBarState applies to a nameless pointer.
inline QString hopFieldName(const Node& n) {
    return n.name.isEmpty() ? fmt::typeNameRaw(n.kind) : n.name;
}
} // namespace detail

// One entry of a crumb's chevron menu: a drillable field of the class at
// that crumb, in memory (offset) order.
struct SiblingEntry {
    uint64_t id = 0;        // the field node (a pointer or embedded struct)
    QString  field;         // its name (or type-name fallback)
    QString  classLabel;    // label of the class it drills into
    QString  keyword;       // that class's resolved keyword
    bool     expanded = false;   // !collapsed — already open on screen
    bool     current = false;    // the hop the trail goes through
    int      offset = 0;         // Node::offset — the sort key
    // Where the field leads: a pointer's dereferenced target, an embedded
    // struct's or array's own address. 0 = unknown (no source, a null or
    // unreadable pointer, an unknown container frame). The pure query
    // below leaves it 0 — it has no memory; RcxController::siblingsForCrumb
    // fills it from the last compose (expanded hops) or one provider read
    // (collapsed pointers), at menu-open time only.
    uint64_t address = 0;
};

// The drillable children of `classId`: NodeTree::childrenOf filtered by
// drillTargetId, sorted by Node::offset. childrenOf returns tree-insertion
// order, which is not memory order once fields were inserted in the middle;
// a stable sort keeps union members (which all share offset 0) in the order
// they were declared, the order the document shows them in.
inline QVector<SiblingEntry> siblingFieldsOf(const NodeTree& tree, uint64_t classId,
                                             uint64_t currentHopId) {
    QVector<SiblingEntry> out;
    for (int ci : tree.childrenOf(classId)) {
        const Node& n = tree.nodes[ci];
        const uint64_t target = drillTargetId(n);
        if (target == 0) continue;
        SiblingEntry e;
        e.id       = n.id;
        e.field    = detail::hopFieldName(n);
        e.expanded = !n.collapsed;
        e.current  = (n.id == currentHopId);
        e.offset   = n.offset;
        const int ti = tree.indexOfId(target);
        if (ti >= 0) {
            e.classLabel = nodeClassLabel(tree.nodes[ti]);
            e.keyword    = tree.nodes[ti].resolvedClassKeyword();
        }
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const SiblingEntry& a, const SiblingEntry& b) { return a.offset < b.offset; });
    return out;
}

// One root class, id and all: the path edit's root completion rows
// (drillFieldsAt("")) and the type chooser's Root mode are built from these.
struct RootEntry {
    uint64_t id = 0;
    QString  label;
    QString  keyword;
};

// The id-carrying twin of rootClassNames(): same nodes (top-level structs),
// same label rule, same first-wins de-duplication by label, same order — so
// a menu built from this and the unsaved-changes dialog built from that list
// the same classes in the same order. The one difference: an empty tree
// yields no entries here (there is no id to carry) where rootClassNames()
// returns its "Untitled" placeholder.
inline QVector<RootEntry> rootClassEntries(const NodeTree& tree) {
    QVector<RootEntry> out;
    QStringList seen;
    for (const Node& n : tree.nodes) {
        if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
        QString label = !n.structTypeName.isEmpty() ? n.structTypeName : n.name;
        if (label.isEmpty()) label = QStringLiteral("Untitled");
        if (seen.contains(label)) continue;
        seen.append(label);
        out.push_back({ n.id, label, n.resolvedClassKeyword() });
    }
    return out;
}

// What the bar's chevron menus and its path edit read from the tree,
// pulled on demand (a menu opening, a keystroke) — never per refresh. The
// bar has no NodeTree: the controller answers these through the editor.
// Lives here, not in the bar, so editor.h can take one by value without
// pulling the widget header into every consumer.
struct AddressBarTreeQueries {
    // Drillable fields of the class at crumb `level`, the trail's hop
    // flagged current; level == crumbs.size()-1 is the deepest class
    // (nothing current — its chevron drills further).
    std::function<QVector<SiblingEntry>(int)>            siblingsOf;
    // Drillable fields of the class a dotted path lands in; "" lists the
    // roots in the same shape. Empty when the path does not resolve.
    std::function<QVector<SiblingEntry>(const QString&)> fieldsAtPath;
    // resolveDrillPath's words for a typed path, "" when it resolves.
    std::function<QString(const QString&)>               validatePath;
};

// The trail as one dotted path — "RcxEditor.vptr.parent": the view root's
// label, then the field followed at each hop. What the bar's path edit
// opens with, and what resolveDrillPath() turns back into a root + focus
// path. The root label follows classLabelOf: a show-all view (viewRootId 0)
// names the first root struct.
inline QString trailPathText(const NodeTree& tree, uint64_t viewRootId,
                             const QVector<uint64_t>& focusPath) {
    QStringList parts;
    const int ri = detail::classIdx(tree, viewRootId);
    parts << (ri >= 0 ? nodeClassLabel(tree.nodes[ri]) : QStringLiteral("Untitled"));
    for (uint64_t hop : focusPath) {
        const int hi = tree.indexOfId(hop);
        if (hi < 0) break;
        parts << detail::hopFieldName(tree.nodes[hi]);
    }
    return parts.join(QLatin1Char('.'));
}

// Resolve a typed dotted path to a view root + focus path. The first segment
// matches a top-level struct by structTypeName or name, exactly as
// NodeTree::nodeIdForPath does; every later segment matches a child of the
// CURRENT CONTAINER by name and must be drillable, and the container then
// becomes drillTargetId(child) — the pointer's refId class, or the embedded
// struct itself. That hop through refId is what nodeIdForPath (parentId-only)
// cannot express, and it is what makes "RcxEditor.vptr.parent" land in
// QWidgetPrivate's `parent` rather than in a field of RcxEditor.
//
// Tolerant of whitespace, a trailing dot and doubled dots (SkipEmptyParts).
// On a miss *err names the segment ("No class 'x'", "No field 'x' in
// Class", or, for a field that exists but is a plain value, "'x' in Class
// is not a class or a pointer to one") and nothing is written to the out
// parameters. A nameless hop matches its type-name fallback so a path
// produced by trailPathText() always resolves.
inline bool resolveDrillPath(const NodeTree& tree, const QString& text,
                             uint64_t* rootId, QVector<uint64_t>* focusPath,
                             QString* err) {
    auto fail = [&](const QString& why) { if (err) *err = why; return false; };
    QStringList segs;
    for (const QString& raw : text.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        const QString s = raw.trimmed();
        if (!s.isEmpty()) segs << s;
    }
    if (segs.isEmpty()) return fail(QStringLiteral("Empty path"));

    int rootIdx = -1;
    for (int i = 0; i < tree.nodes.size(); ++i) {
        const Node& n = tree.nodes[i];
        if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
        if (n.structTypeName == segs[0] || n.name == segs[0]) { rootIdx = i; break; }
    }
    if (rootIdx < 0) return fail(QStringLiteral("No class '%1'").arg(segs[0]));

    QVector<uint64_t> path;
    uint64_t container = tree.nodes[rootIdx].id;
    for (int s = 1; s < segs.size(); ++s) {
        const int ci = tree.indexOfId(container);
        const QString containerLabel =
            ci >= 0 ? nodeClassLabel(tree.nodes[ci]) : QStringLiteral("Untitled");
        int found = -1;
        for (int k : tree.childrenOf(container)) {
            const Node& n = tree.nodes[k];
            if (n.name == segs[s] || (n.name.isEmpty() && fmt::typeNameRaw(n.kind) == segs[s])) {
                found = k;
                break;
            }
        }
        if (found < 0)
            return fail(QStringLiteral("No field '%1' in %2").arg(segs[s], containerLabel));
        const uint64_t target = drillTargetId(tree.nodes[found]);
        if (target == 0)
            return fail(QStringLiteral("'%1' in %2 is not a class or a pointer to one")
                            .arg(segs[s], containerLabel));
        path.push_back(tree.nodes[found].id);
        container = target;
    }

    if (rootId)    *rootId = tree.nodes[rootIdx].id;
    if (focusPath) *focusPath = path;
    if (err)       err->clear();
    return true;
}

} // namespace rcx
