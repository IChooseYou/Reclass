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
        if (lm.foldHead)
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

uint32_t computeMarkers(const Node& node, const Provider& prov,
                        uint64_t addr, bool isCont, int depth) {
    uint32_t mask = 0;
    if (isCont)                          mask |= (1u << M_CONT);
    if (node.kind == NodeKind::Padding)  mask |= (1u << M_PAD);

    if (prov.isValid()) {
        int sz = node.byteSize();
        if (sz > 0 && !prov.isReadable(addr, sz)) {
            mask |= (1u << M_ERR);
        } else if (sz > 0) {
            if (node.kind == NodeKind::Pointer32 && prov.readU32(addr) == 0)
                mask |= (1u << M_PTR0);
            if (node.kind == NodeKind::Pointer64 && prov.readU64(addr) == 0)
                mask |= (1u << M_PTR0);
        }
    }
    return mask;
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
        lm.offsetText      = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, isCont);
        lm.markerMask      = computeMarkers(node, prov, absAddr, isCont, depth);
        lm.foldLevel       = computeFoldLevel(depth, false);
        lm.effectiveTypeW  = typeW;
        lm.effectiveNameW  = nameW;

        QString lineText = fmt::fmtNodeLine(node, prov, absAddr, depth, sub,
                                            /*comment=*/{}, typeW, nameW);
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
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false);
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
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false);
        lm.nodeKind   = node.kind;
        lm.foldLevel  = computeFoldLevel(depth, false);
        lm.markerMask = 0;
        lm.arrayElementIdx = arrayElementIdx;
        state.emitLine(fmt::indent(depth) + QStringLiteral("[%1]").arg(arrayElementIdx), lm);
    }

    // Header line (skip for array element structs - condensed display)
    if (!isArrayChild) {
        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::Header;
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false);
        lm.nodeKind   = node.kind;
        lm.foldHead      = true;
        lm.foldCollapsed = node.collapsed;
        lm.foldLevel  = computeFoldLevel(depth, true);
        lm.markerMask = (1u << M_STRUCT_BG);
        lm.isRootHeader = (node.parentId == 0 && node.kind == NodeKind::Struct);

        QString headerText;
        if (node.kind == NodeKind::Array) {
            // Array header with navigation: "uint32_t[16]  name  { <0/16>"
            lm.isArrayHeader = true;
            lm.elementKind   = node.elementKind;
            lm.arrayViewIdx  = node.viewIndex;
            lm.arrayCount    = node.arrayLen;
            headerText = fmt::fmtArrayHeader(node, depth, node.viewIndex);
        } else if (lm.isRootHeader) {
            // Root structs show base address
            headerText = fmt::fmtStructHeaderWithBase(node, depth, tree.baseAddress);
        } else {
            // Nested structs show normal header
            headerText = fmt::fmtStructHeader(node, depth);
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

    // Footer line (skip for array element structs - condensed display)
    if (!isArrayChild) {
        LineMeta lm;
        lm.nodeIdx   = nodeIdx;
        lm.nodeId    = node.id;
        lm.depth     = depth;
        lm.lineKind   = LineKind::Footer;
        lm.nodeKind   = node.kind;
        lm.offsetText = QStringLiteral("  ---");
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

    // Pointer deref expansion
    if ((node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64)
        && node.refId != 0) {
        {
            LineMeta lm;
            lm.nodeIdx    = nodeIdx;
            lm.nodeId     = node.id;
            lm.depth      = depth;
            lm.lineKind   = LineKind::Field;
            lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false);
            lm.nodeKind   = node.kind;
            lm.foldHead      = true;
            lm.foldCollapsed = node.collapsed;
            lm.foldLevel  = computeFoldLevel(depth, true);
            lm.markerMask = computeMarkers(node, prov, absAddr, false, depth);
            lm.effectiveTypeW = typeW;
            lm.effectiveNameW = nameW;
            state.emitLine(fmt::fmtNodeLine(node, prov, absAddr, depth, 0, {}, typeW, nameW), lm);
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
                                composeParent(state, tree, prov, refIdx,
                                              depth + 1, pBase, ref.id);
                        }
                        state.ptrVisiting.remove(key);
                    }
                }
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

    // Compute effective type column width from longest type name
    int maxTypeLen = kMinTypeW;
    for (const Node& node : tree.nodes) {
        QString typeName;
        if (node.kind == NodeKind::Array) {
            // Array type: "int32_t[10]", "char[64]", etc.
            typeName = fmt::arrayTypeName(node.elementKind, node.arrayLen);
        } else {
            typeName = fmt::typeNameRaw(node.kind);
        }
        maxTypeLen = qMax(maxTypeLen, typeName.size());
    }
    state.typeW = qBound(kMinTypeW, maxTypeLen + 1, kMaxTypeW);

    // Compute effective name column width from longest name
    int maxNameLen = kMinNameW;
    for (const Node& node : tree.nodes) {
        // Skip hex/padding (they show ASCII preview, not name column)
        if (isHexPreview(node.kind)) continue;
        // Skip containers (struct/array headers have different layout)
        if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) continue;
        maxNameLen = qMax(maxNameLen, node.name.size());
    }
    state.nameW = qBound(kMinNameW, maxNameLen + 1, kMaxNameW);

    // Pre-compute per-scope widths (each container gets widths based on direct children only)
    for (int i = 0; i < tree.nodes.size(); i++) {
        const Node& container = tree.nodes[i];
        if (container.kind != NodeKind::Struct && container.kind != NodeKind::Array)
            continue;

        int scopeMaxType = kMinTypeW;
        int scopeMaxName = kMinNameW;

        for (int childIdx : state.childMap.value(container.id)) {
            const Node& child = tree.nodes[childIdx];

            // Type width
            QString childTypeName;
            if (child.kind == NodeKind::Array) {
                childTypeName = fmt::arrayTypeName(child.elementKind, child.arrayLen);
            } else {
                childTypeName = fmt::typeNameRaw(child.kind);
            }
            scopeMaxType = qMax(scopeMaxType, childTypeName.size());

            // Name width (skip hex/padding and containers)
            if (!isHexPreview(child.kind) &&
                child.kind != NodeKind::Struct && child.kind != NodeKind::Array) {
                scopeMaxName = qMax(scopeMaxName, child.name.size());
            }
        }

        state.scopeTypeW[container.id] = qBound(kMinTypeW, scopeMaxType + 1, kMaxTypeW);
        state.scopeNameW[container.id] = qBound(kMinNameW, scopeMaxName + 1, kMaxNameW);
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
