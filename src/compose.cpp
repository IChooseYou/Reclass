#include "core.h"
#include <algorithm>

namespace rcx {

namespace {

// Scintilla fold constants (avoid including Scintilla headers in core)
constexpr int SC_FOLDLEVELBASE       = 0x400;
constexpr int SC_FOLDLEVELHEADERFLAG = 0x2000;
constexpr uint64_t kGoldenRatio      = 0x9E3779B97F4A7C15ULL;

struct ComposeState {
    QString            text;
    QVector<LineMeta>  meta;
    QSet<uint64_t>     visiting;   // cycle detection for struct recursion
    QSet<qulonglong>   ptrVisiting; // cycle guard for pointer expansions
    int                currentLine = 0;
    int                typeW       = kColType;  // global type column width (fallback)
    int                nameW       = kColName;  // global name column width (fallback)
    bool               baseEmitted = false;     // only first root struct shows base address

    // Precomputed for O(1) lookups
    QHash<uint64_t, QVector<int>> childMap;
    QVector<int64_t>              absOffsets;  // indexed by node index

    // Per-scope column widths (containerId -> width for direct children)
    QHash<uint64_t, int> scopeTypeW;
    QHash<uint64_t, int> scopeNameW;

    int effectiveTypeW(uint64_t scopeId) const {
        return scopeTypeW.value(scopeId, typeW);
    }
    int effectiveNameW(uint64_t scopeId) const {
        return scopeNameW.value(scopeId, nameW);
    }

    void emitLine(const QString& lineText, LineMeta lm) {
        if (currentLine > 0) text += '\n';
        // 3-char fold indicator column: " - " expanded, " + " collapsed, "   " other
        // CommandRow has no fold prefix (flush left)
        if (lm.lineKind == LineKind::CommandRow) {
            // no prefix
        } else if (lm.foldHead)
            text += lm.foldCollapsed ? QStringLiteral(" + ") : QStringLiteral(" - ");
        else
            text += QStringLiteral("   ");
        text += lineText;
        meta.append(lm);
        currentLine++;
    }
};

int computeFoldLevel(int depth, bool isHead) {
    int level = SC_FOLDLEVELBASE + depth;
    if (isHead) level |= SC_FOLDLEVELHEADERFLAG;
    return level;
}

uint32_t computeMarkers(const Node& node, const Provider& /*prov*/,
                        uint64_t /*addr*/, bool isCont, int /*depth*/) {
    uint32_t mask = 0;
    if (isCont)                          mask |= (1u << M_CONT);
    if (node.kind == NodeKind::Padding)  mask |= (1u << M_PAD);
    // No ambient validation markers — errors only shown during inline editing.
    return mask;
}

static QString resolvePointerTarget(const NodeTree& tree, uint64_t refId) {
    if (refId == 0) return {};
    int refIdx = tree.indexOfId(refId);
    if (refIdx < 0) return {};
    const Node& ref = tree.nodes[refIdx];
    return ref.structTypeName.isEmpty() ? ref.name : ref.structTypeName;
}

static inline uint64_t ptrToProviderAddr(const NodeTree& tree, uint64_t ptr) {
    if (tree.baseAddress && ptr >= tree.baseAddress) return ptr - tree.baseAddress;
    return ptr;
}

static int64_t relOffsetFromRoot(const NodeTree& tree, int idx, uint64_t rootId) {
    int64_t total = 0;
    QSet<int> visited;
    int cur = idx;
    while (cur >= 0 && cur < tree.nodes.size()) {
        if (visited.contains(cur)) break;
        visited.insert(cur);
        const Node& n = tree.nodes[cur];
        if (n.id == rootId) break;
        total += n.offset;
        if (n.parentId == 0) break;
        cur = tree.indexOfId(n.parentId);
    }
    return total;
}

static inline uint64_t resolveAddr(const ComposeState& state,
                                   const NodeTree& tree,
                                   int nodeIdx,
                                   uint64_t base, uint64_t rootId) {
    if (rootId != 0)
        return base + relOffsetFromRoot(tree, nodeIdx, rootId);
    return state.absOffsets[nodeIdx];
}

void composeLeaf(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx,
                 int depth, uint64_t absAddr, uint64_t scopeId) {
    const Node& node = tree.nodes[nodeIdx];

    // Get per-scope widths (falls back to global if no scope entry)
    int typeW = state.effectiveTypeW(scopeId);
    int nameW = state.effectiveNameW(scopeId);

    // Line count: padding wraps at 8 bytes per line
    int numLines;
    if (node.kind == NodeKind::Padding) {
        int totalBytes = qMax(1, node.arrayLen);
        numLines = (totalBytes + 7) / 8;
    } else {
        numLines = linesForKind(node.kind);
    }

    // Resolve pointer target name for display
    QString ptrTypeOverride;
    QString ptrTargetName;
    if (node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64) {
        ptrTargetName = resolvePointerTarget(tree, node.refId);
        ptrTypeOverride = fmt::pointerTypeName(node.kind, ptrTargetName);
    }

    for (int sub = 0; sub < numLines; sub++) {
        bool isCont = (sub > 0);

        LineMeta lm;
        lm.nodeIdx        = nodeIdx;
        lm.nodeId          = node.id;
        lm.subLine         = sub;
        lm.depth           = depth;
        lm.isContinuation  = isCont;
        lm.lineKind        = isCont ? LineKind::Continuation : LineKind::Field;
        lm.nodeKind        = node.kind;
        lm.offsetText      = fmt::fmtOffsetMargin(absAddr, isCont);
        lm.markerMask      = computeMarkers(node, prov, absAddr, isCont, depth);
        lm.foldLevel       = computeFoldLevel(depth, false);
        lm.effectiveTypeW  = typeW;
        lm.effectiveNameW  = nameW;
        lm.pointerTargetName = ptrTargetName;

        QString lineText = fmt::fmtNodeLine(node, prov, absAddr, depth, sub,
                                            /*comment=*/{}, typeW, nameW, ptrTypeOverride);
        state.emitLine(lineText, lm);
    }
}

// Forward declarations (base/rootId default to 0 = use precomputed offsets)
void composeNode(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx, int depth,
                 uint64_t base = 0, uint64_t rootId = 0, bool isArrayChild = false,
                 uint64_t scopeId = 0, int arrayElementIdx = -1);
void composeParent(ComposeState& state, const NodeTree& tree,
                   const Provider& prov, int nodeIdx, int depth,
                   uint64_t base = 0, uint64_t rootId = 0, bool isArrayChild = false,
                   uint64_t scopeId = 0, int arrayElementIdx = -1);

void composeParent(ComposeState& state, const NodeTree& tree,
                   const Provider& prov, int nodeIdx, int depth,
                   uint64_t base, uint64_t rootId, bool isArrayChild,
                   uint64_t scopeId, int arrayElementIdx) {
    const Node& node = tree.nodes[nodeIdx];
    uint64_t absAddr = resolveAddr(state, tree, nodeIdx, base, rootId);

    // Cycle detection
    if (state.visiting.contains(node.id)) {
        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::Field;
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false);
        lm.nodeKind   = node.kind;
        lm.markerMask = (1u << M_CYCLE) | (1u << M_ERR);
        lm.foldLevel  = computeFoldLevel(depth, false);
        state.emitLine(fmt::indent(depth) + QStringLiteral("/* CYCLE: ") +
                       node.name + QStringLiteral(" */"), lm);
        return;
    }
    state.visiting.insert(node.id);

    // Array element separator: show [N] to indicate which element this is
    if (isArrayChild && arrayElementIdx >= 0) {
        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::ArrayElementSeparator;
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false);
        lm.nodeKind   = node.kind;
        lm.foldLevel  = computeFoldLevel(depth, false);
        lm.markerMask = 0;
        lm.arrayElementIdx = arrayElementIdx;
        state.emitLine(fmt::indent(depth) + QStringLiteral("[%1]").arg(arrayElementIdx), lm);
    }

    // Header line (skip for array element structs - condensed display)
    if (!isArrayChild) {
        // Get per-scope widths for this header's parent scope
        int typeW = state.effectiveTypeW(scopeId);
        int nameW = state.effectiveNameW(scopeId);

        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::Header;
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false);
        lm.nodeKind   = node.kind;
        lm.foldHead      = true;
        lm.foldCollapsed = node.collapsed;
        lm.foldLevel  = computeFoldLevel(depth, true);
        lm.markerMask = (1u << M_STRUCT_BG);
        lm.isRootHeader = (node.parentId == 0 && node.kind == NodeKind::Struct && !state.baseEmitted);
        if (lm.isRootHeader) state.baseEmitted = true;
        lm.effectiveTypeW = typeW;
        lm.effectiveNameW = nameW;

        QString headerText;
        if (node.kind == NodeKind::Array) {
            // Array header with navigation: "uint32_t[16]  name  {" (no brace when collapsed)
            lm.isArrayHeader = true;
            lm.elementKind   = node.elementKind;
            lm.arrayViewIdx  = node.viewIndex;
            lm.arrayCount    = node.arrayLen;
            headerText = fmt::fmtArrayHeader(node, depth, node.viewIndex, node.collapsed, typeW, nameW);
        } else {
            // All structs (root and nested) use the same header format
            headerText = fmt::fmtStructHeader(node, depth, node.collapsed, typeW, nameW);
        }
        state.emitLine(headerText, lm);
    }

    if (!node.collapsed || isArrayChild) {
        QVector<int> children = state.childMap.value(node.id);
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return tree.nodes[a].offset < tree.nodes[b].offset;
        });

        // For arrays, render children as condensed (no header/footer for struct elements)
        bool childrenAreArrayElements = (node.kind == NodeKind::Array);
        int elementIdx = 0;
        for (int childIdx : children) {
            // Pass this container's id as the scope for children (for per-scope widths)
            // For array elements, also pass the element index for [N] separator
            composeNode(state, tree, prov, childIdx, depth + 1, base, rootId,
                        childrenAreArrayElements, node.id,
                        childrenAreArrayElements ? elementIdx++ : -1);
        }
    }

    // Footer line: skip when collapsed (only header shows) or for array element structs
    if (!isArrayChild && !node.collapsed) {
        LineMeta lm;
        lm.nodeIdx   = nodeIdx;
        lm.nodeId    = node.id;
        lm.depth     = depth;
        lm.lineKind   = LineKind::Footer;
        lm.nodeKind   = node.kind;
        lm.offsetText.clear();
        lm.foldLevel  = computeFoldLevel(depth, false);
        lm.markerMask = 0;
        int sz = tree.structSpan(node.id, &state.childMap);
        state.emitLine(fmt::fmtStructFooter(node, depth, sz), lm);
    }

    state.visiting.remove(node.id);
}

void composeNode(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx, int depth,
                 uint64_t base, uint64_t rootId, bool isArrayChild,
                 uint64_t scopeId, int arrayElementIdx) {
    const Node& node = tree.nodes[nodeIdx];
    uint64_t absAddr = resolveAddr(state, tree, nodeIdx, base, rootId);

    // Get per-scope widths for this node
    int typeW = state.effectiveTypeW(scopeId);
    int nameW = state.effectiveNameW(scopeId);

    // Pointer deref expansion — single fold header merges pointer + struct header
    if ((node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64)
        && node.refId != 0) {
        QString ptrTargetName = resolvePointerTarget(tree, node.refId);
        QString ptrTypeOverride = fmt::pointerTypeName(node.kind, ptrTargetName);

        // Emit merged fold header: "ptr64<Type> Name {" (expanded) or "ptr64<Type> Name -> val" (collapsed)
        {
            LineMeta lm;
            lm.nodeIdx    = nodeIdx;
            lm.nodeId     = node.id;
            lm.depth      = depth;
            lm.lineKind   = node.collapsed ? LineKind::Field : LineKind::Header;
            lm.offsetText = fmt::fmtOffsetMargin(absAddr, false);
            lm.nodeKind   = node.kind;
            lm.foldHead      = true;
            lm.foldCollapsed = node.collapsed;
            lm.foldLevel  = computeFoldLevel(depth, true);
            lm.markerMask = computeMarkers(node, prov, absAddr, false, depth);
            lm.effectiveTypeW = typeW;
            lm.effectiveNameW = nameW;
            lm.pointerTargetName = ptrTargetName;
            state.emitLine(fmt::fmtPointerHeader(node, depth, node.collapsed,
                                                  prov, absAddr, ptrTypeOverride,
                                                  typeW, nameW), lm);
        }

        if (!node.collapsed) {
            int sz = node.byteSize();
            if (prov.isValid() && sz > 0 && prov.isReadable(absAddr, sz)) {
                uint64_t ptrVal = (node.kind == NodeKind::Pointer32)
                    ? (uint64_t)prov.readU32(absAddr) : prov.readU64(absAddr);
                if (ptrVal != 0) {
                    uint64_t pBase = ptrToProviderAddr(tree, ptrVal);
                    qulonglong key = pBase ^ (node.refId * kGoldenRatio);
                    if (!state.ptrVisiting.contains(key)) {
                        state.ptrVisiting.insert(key);
                        int refIdx = tree.indexOfId(node.refId);
                        if (refIdx >= 0) {
                            const Node& ref = tree.nodes[refIdx];
                            if (ref.kind == NodeKind::Struct || ref.kind == NodeKind::Array)
                                // isArrayChild=true skips header/footer, emits children only
                                // depth (not depth+1): pointer header replaces struct header,
                                // so children should be at depth+1, not depth+2
                                composeParent(state, tree, prov, refIdx,
                                              depth, pBase, ref.id,
                                              /*isArrayChild=*/true);
                        }
                        state.ptrVisiting.remove(key);
                    }
                }
            }

            // Footer for pointer fold
            {
                LineMeta lm;
                lm.nodeIdx   = nodeIdx;
                lm.nodeId    = node.id;
                lm.depth     = depth;
                lm.lineKind  = LineKind::Footer;
                lm.nodeKind  = node.kind;
                lm.offsetText.clear();
                lm.foldLevel = computeFoldLevel(depth, false);
                lm.markerMask = 0;
                state.emitLine(fmt::indent(depth) + QStringLiteral("}"), lm);
            }
        }
        return;
    }

    if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) {
        composeParent(state, tree, prov, nodeIdx, depth, base, rootId, isArrayChild, scopeId, arrayElementIdx);
    } else {
        composeLeaf(state, tree, prov, nodeIdx, depth, absAddr, scopeId);
    }
}

} // anonymous namespace

ComposeResult compose(const NodeTree& tree, const Provider& prov) {
    ComposeState state;

    // Precompute parent→children map
    for (int i = 0; i < tree.nodes.size(); i++)
        state.childMap[tree.nodes[i].parentId].append(i);

    // Precompute absolute offsets
    state.absOffsets.resize(tree.nodes.size());
    for (int i = 0; i < tree.nodes.size(); i++)
        state.absOffsets[i] = tree.computeOffset(i);

    // Helper: compute the display type string for a node (for width calculation)
    auto nodeTypeName = [&](const Node& n) -> QString {
        if (n.kind == NodeKind::Array)
            return fmt::arrayTypeName(n.elementKind, n.arrayLen);
        if (n.kind == NodeKind::Struct)
            return fmt::structTypeName(n);
        if (n.kind == NodeKind::Pointer32 || n.kind == NodeKind::Pointer64)
            return fmt::pointerTypeName(n.kind, resolvePointerTarget(tree, n.refId));
        return fmt::typeNameRaw(n.kind);
    };

    // Compute effective type column width from longest type name
    // Include struct/array headers which use "struct TypeName" or "type[count]" format
    int maxTypeLen = kMinTypeW;
    for (const Node& node : tree.nodes) {
        maxTypeLen = qMax(maxTypeLen, (int)nodeTypeName(node).size());
    }
    state.typeW = qBound(kMinTypeW, maxTypeLen, kMaxTypeW);

    // Compute effective name column width from longest name
    // Include struct/array names - they now use columnar layout too
    int maxNameLen = kMinNameW;
    for (const Node& node : tree.nodes) {
        // Skip padding (it shows ASCII preview, not name column)
        if (node.kind == NodeKind::Padding) continue;
        maxNameLen = qMax(maxNameLen, (int)node.name.size());
    }
    state.nameW = qBound(kMinNameW, maxNameLen, kMaxNameW);

    // Pre-compute per-scope widths (each container gets widths based on direct children only)
    for (int i = 0; i < tree.nodes.size(); i++) {
        const Node& container = tree.nodes[i];
        if (container.kind != NodeKind::Struct && container.kind != NodeKind::Array)
            continue;

        int scopeMaxType = kMinTypeW;
        int scopeMaxName = kMinNameW;

        for (int childIdx : state.childMap.value(container.id)) {
            const Node& child = tree.nodes[childIdx];
            scopeMaxType = qMax(scopeMaxType, (int)nodeTypeName(child).size());

            // Name width (skip padding, but include hex and containers)
            if (child.kind != NodeKind::Padding) {
                scopeMaxName = qMax(scopeMaxName, (int)child.name.size());
            }
        }

        state.scopeTypeW[container.id] = qBound(kMinTypeW, scopeMaxType, kMaxTypeW);
        state.scopeNameW[container.id] = qBound(kMinNameW, scopeMaxName, kMaxNameW);
    }

    // Compute scope widths for root level (parentId == 0)
    // Include struct/array headers - they now use columnar layout too
    {
        int rootMaxType = kMinTypeW;
        int rootMaxName = kMinNameW;
        for (int childIdx : state.childMap.value(0)) {
            const Node& child = tree.nodes[childIdx];
            rootMaxType = qMax(rootMaxType, (int)nodeTypeName(child).size());

            // Name width (skip padding, include hex and containers)
            if (child.kind != NodeKind::Padding) {
                rootMaxName = qMax(rootMaxName, (int)child.name.size());
            }
        }
        state.scopeTypeW[0] = qBound(kMinTypeW, rootMaxType, kMaxTypeW);
        state.scopeNameW[0] = qBound(kMinNameW, rootMaxName, kMaxNameW);
    }

    // Emit CommandRow as line 0 (synthetic UI line)
    {
        LineMeta lm;
        lm.nodeIdx   = -1;
        lm.nodeId    = kCommandRowId;
        lm.depth     = 0;
        lm.lineKind  = LineKind::CommandRow;
        lm.foldLevel = SC_FOLDLEVELBASE;
        lm.foldHead  = false;
        lm.offsetText.clear();
        lm.markerMask = 0;
        lm.effectiveTypeW = state.typeW;
        lm.effectiveNameW = state.nameW;
        state.emitLine(QStringLiteral("File  Address: 0x0"), lm);
    }

    QVector<int> roots = state.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    for (int idx : roots) {
        composeNode(state, tree, prov, idx, 0);
    }

    return { state.text, state.meta, LayoutInfo{state.typeW, state.nameW} };
}

QSet<uint64_t> NodeTree::normalizePreferAncestors(const QSet<uint64_t>& ids) const {
    QSet<uint64_t> result;
    for (uint64_t id : ids) {
        int idx = indexOfId(id);
        if (idx < 0) continue;
        bool ancestorSelected = false;
        uint64_t cur = nodes[idx].parentId;
        QSet<uint64_t> visited;
        while (cur != 0 && !visited.contains(cur)) {
            visited.insert(cur);
            if (ids.contains(cur)) { ancestorSelected = true; break; }
            int pi = indexOfId(cur);
            if (pi < 0) break;
            cur = nodes[pi].parentId;
        }
        if (!ancestorSelected)
            result.insert(id);
    }
    return result;
}

QSet<uint64_t> NodeTree::normalizePreferDescendants(const QSet<uint64_t>& ids) const {
    QSet<uint64_t> result;
    for (uint64_t id : ids) {
        QVector<int> sub = subtreeIndices(id);
        bool hasSelectedDescendant = false;
        for (int si : sub) {
            uint64_t sid = nodes[si].id;
            if (sid != id && ids.contains(sid)) {
                hasSelectedDescendant = true;
                break;
            }
        }
        if (!hasSelectedDescendant)
            result.insert(id);
    }
    return result;
}

} // namespace rcx
