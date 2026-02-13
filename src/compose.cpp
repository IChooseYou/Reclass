#include "core.h"
#include <algorithm>
#include <numeric>

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
    int                offsetHexDigits = 8;     // hex digit tier for offset margin
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
        if (lm.lineKind == LineKind::CommandRow
            || (lm.lineKind == LineKind::Footer && lm.isRootHeader)) {
            // no prefix — flush left
        } else if (lm.foldHead)
            text += lm.foldCollapsed ? QStringLiteral(" \u25B8 ") : QStringLiteral(" \u25BE ");
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
    if (tree.baseAddress == 0) return ptr;
    if (ptr >= tree.baseAddress) return ptr - tree.baseAddress;
    return UINT64_MAX;  // Invalid: ptr below base address
}

static int64_t relOffsetFromRoot(const NodeTree& tree, int idx, uint64_t rootId) {
    int64_t total = 0;
    QSet<uint64_t> visited;
    int cur = idx;
    while (cur >= 0 && cur < tree.nodes.size()) {
        uint64_t nid = tree.nodes[cur].id;
        if (visited.contains(nid)) break;
        visited.insert(nid);
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
        lm.offsetText      = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, isCont, state.offsetHexDigits);
        lm.offsetAddr      = tree.baseAddress + absAddr;
        lm.markerMask      = computeMarkers(node, prov, absAddr, isCont, depth);
        lm.foldLevel       = computeFoldLevel(depth, false);
        lm.effectiveTypeW  = typeW;
        lm.effectiveNameW  = nameW;
        lm.pointerTargetName = ptrTargetName;

        // Set byte count for hex preview lines (used for per-byte change highlighting)
        if (isHexPreview(node.kind)) {
            if (node.kind == NodeKind::Padding) {
                int totalSz = qMax(1, node.arrayLen);
                lm.lineByteCount = qMin(8, totalSz - sub * 8);
            } else {
                lm.lineByteCount = sizeForKind(node.kind);
            }
        }

        QString lineText = fmt::fmtNodeLine(node, prov, absAddr, depth, sub,
                                            /*comment=*/{}, typeW, nameW, ptrTypeOverride);
        state.emitLine(lineText, lm);
    }
}

// Forward declarations (base/rootId default to 0 = use precomputed offsets)
void composeNode(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx, int depth,
                 uint64_t base = 0, uint64_t rootId = 0, bool isArrayChild = false,
                 uint64_t scopeId = 0, int arrayElementIdx = -1,
                 uint64_t arrayContainerAddr = 0);
void composeParent(ComposeState& state, const NodeTree& tree,
                   const Provider& prov, int nodeIdx, int depth,
                   uint64_t base = 0, uint64_t rootId = 0, bool isArrayChild = false,
                   uint64_t scopeId = 0, int arrayElementIdx = -1,
                   uint64_t arrayContainerAddr = 0);

void composeParent(ComposeState& state, const NodeTree& tree,
                   const Provider& prov, int nodeIdx, int depth,
                   uint64_t base, uint64_t rootId, bool isArrayChild,
                   uint64_t scopeId, int arrayElementIdx,
                   uint64_t arrayContainerAddr) {
    const Node& node = tree.nodes[nodeIdx];
    uint64_t absAddr = resolveAddr(state, tree, nodeIdx, base, rootId);

    // Cycle detection
    if (state.visiting.contains(node.id)) {
        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::Field;
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = tree.baseAddress + absAddr;
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
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = tree.baseAddress + absAddr;
        lm.nodeKind   = node.kind;
        lm.foldLevel  = computeFoldLevel(depth, false);
        lm.markerMask = 0;
        lm.arrayElementIdx = arrayElementIdx;
        uint64_t relOff = absAddr - arrayContainerAddr;
        QString relOffHex = QString::number(relOff, 16).toUpper();
        state.emitLine(fmt::indent(depth) + QStringLiteral("[%1] +0x%2").arg(arrayElementIdx).arg(relOffHex), lm);
    }

    // Detect root header: first root-level struct — suppressed from display
    // (CommandRow already shows the root class type + name)
    bool isRootHeader = (node.parentId == 0 && node.kind == NodeKind::Struct && !state.baseEmitted);
    if (isRootHeader)
        state.baseEmitted = true;

    // Header line (skip for array element structs and root struct)
    // Root struct header is on CommandRow (type + name + {)
    if (!isArrayChild && !isRootHeader) {
        // Get per-scope widths for this header's parent scope
        int typeW = state.effectiveTypeW(scopeId);
        int nameW = state.effectiveNameW(scopeId);

        LineMeta lm;
        lm.nodeIdx    = nodeIdx;
        lm.nodeId     = node.id;
        lm.depth      = depth;
        lm.lineKind   = LineKind::Header;
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = tree.baseAddress + absAddr;
        lm.nodeKind   = node.kind;
        lm.isRootHeader = false;
        lm.foldHead      = true;
        lm.foldCollapsed = node.collapsed;
        lm.foldLevel  = computeFoldLevel(depth, true);
        lm.markerMask = (1u << M_STRUCT_BG);
        lm.effectiveTypeW = typeW;
        lm.effectiveNameW = nameW;

        QString headerText;
        if (node.kind == NodeKind::Array) {
            // Array header with navigation: "uint32_t[16]  name  {" (no brace when collapsed)
            lm.isArrayHeader = true;
            lm.elementKind   = node.elementKind;
            lm.arrayViewIdx  = node.viewIndex;
            lm.arrayCount    = node.arrayLen;
            QString elemStructName = (node.elementKind == NodeKind::Struct)
                ? resolvePointerTarget(tree, node.refId) : QString();
            headerText = fmt::fmtArrayHeader(node, depth, node.viewIndex, node.collapsed, typeW, nameW, elemStructName);
        } else {
            // All structs (root and nested) use the same header format
            headerText = fmt::fmtStructHeader(node, depth, node.collapsed, typeW, nameW);
        }
        state.emitLine(headerText, lm);
    }

    if (!node.collapsed || isArrayChild || isRootHeader) {
        QVector<int> children = state.childMap.value(node.id);
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return tree.nodes[a].offset < tree.nodes[b].offset;
        });

        int childDepth = depth + 1;

        // Primitive arrays with no child nodes: synthesize element lines dynamically
        if (node.kind == NodeKind::Array && children.isEmpty()
            && node.elementKind != NodeKind::Struct && node.elementKind != NodeKind::Array) {
            int elemSize = sizeForKind(node.elementKind);
            int eTW = state.effectiveTypeW(node.id);
            int eNW = state.effectiveNameW(node.id);
            for (int i = 0; i < node.arrayLen; i++) {
                uint64_t elemAddr = absAddr + i * elemSize;

                // Type override: "float[0]", "uint32_t[1]", etc.
                QString elemTypeStr = fmt::typeNameRaw(node.elementKind)
                                    + QStringLiteral("[%1]").arg(i);

                Node elem;
                elem.kind = node.elementKind;
                elem.name = QString();  // no name for array elements
                elem.offset = node.offset + i * elemSize;
                elem.parentId = node.id;
                elem.id = 0;

                LineMeta lm;
                lm.nodeIdx    = nodeIdx;
                lm.nodeId     = node.id;
                lm.depth      = childDepth;
                lm.lineKind   = LineKind::Field;
                lm.nodeKind   = node.elementKind;
                lm.isArrayElement = true;
                lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + elemAddr, false, state.offsetHexDigits);
                lm.offsetAddr = tree.baseAddress + elemAddr;
                lm.markerMask = computeMarkers(elem, prov, elemAddr, false, childDepth);
                lm.foldLevel  = computeFoldLevel(childDepth, false);
                lm.effectiveTypeW = eTW;
                lm.effectiveNameW = eNW;

                state.emitLine(fmt::fmtNodeLine(elem, prov, elemAddr, childDepth, 0,
                                                {}, eTW, eNW, elemTypeStr), lm);
            }
        }

        // Struct arrays with refId but no child nodes: synthesize by expanding the
        // referenced struct for each element (like repeated pointer deref)
        if (node.kind == NodeKind::Array && children.isEmpty()
            && node.elementKind == NodeKind::Struct && node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                int elemSize = tree.structSpan(node.refId, &state.childMap);
                if (elemSize <= 0) elemSize = 1;
                for (int i = 0; i < node.arrayLen; i++) {
                    uint64_t elemBase = absAddr + (uint64_t)i * elemSize;
                    // Use base offset that maps refStruct's children to the right provider address
                    composeParent(state, tree, prov, refIdx, childDepth, elemBase, node.refId,
                                  /*isArrayChild=*/true, node.id, i, absAddr);
                }
            }
        }

        // Embedded struct with refId but no child nodes: expand referenced struct's
        // children at this node's offset (single instance, like array with count=1)
        if (node.kind == NodeKind::Struct && children.isEmpty() && node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                QVector<int> refChildren = state.childMap.value(node.refId);
                std::sort(refChildren.begin(), refChildren.end(), [&](int a, int b) {
                    return tree.nodes[a].offset < tree.nodes[b].offset;
                });
                for (int childIdx : refChildren) {
                    // Skip self-referential children (e.g. struct Ball has a field of type Ball)
                    if (state.visiting.contains(tree.nodes[childIdx].id))
                        continue;
                    composeNode(state, tree, prov, childIdx, childDepth,
                                absAddr, node.refId, false, node.id);
                }
            }
        }

        // For arrays, render children as condensed (no header/footer for struct elements)
        bool childrenAreArrayElements = (node.kind == NodeKind::Array);
        int elementIdx = 0;
        for (int childIdx : children) {
            // Pass this container's id as the scope for children (for per-scope widths)
            // For array elements, also pass the element index for [N] separator
            composeNode(state, tree, prov, childIdx, childDepth, base, rootId,
                        childrenAreArrayElements, node.id,
                        childrenAreArrayElements ? elementIdx++ : -1,
                        childrenAreArrayElements ? absAddr : 0);
        }
    }

    // Footer line: skip when collapsed or for array element structs
    if (!isArrayChild && (!node.collapsed || isRootHeader)) {
        LineMeta lm;
        lm.nodeIdx   = nodeIdx;
        lm.nodeId    = node.id;
        lm.depth     = depth;
        lm.lineKind   = LineKind::Footer;
        lm.nodeKind   = node.kind;
        lm.isRootHeader = isRootHeader;  // root footer: flush left (no fold prefix)
        lm.foldLevel  = computeFoldLevel(depth, false);
        lm.markerMask = 0;
        int sz = tree.structSpan(node.id, &state.childMap);
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr + sz, false, state.offsetHexDigits);
        lm.offsetAddr = tree.baseAddress + absAddr + sz;
        state.emitLine(fmt::fmtStructFooter(node, depth, sz), lm);
    }

    state.visiting.remove(node.id);
}

void composeNode(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx, int depth,
                 uint64_t base, uint64_t rootId, bool isArrayChild,
                 uint64_t scopeId, int arrayElementIdx,
                 uint64_t arrayContainerAddr) {
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

        // Emit merged fold header: "Type* Name {" (expanded) or "Type* Name -> val" (collapsed)
        {
            LineMeta lm;
            lm.nodeIdx    = nodeIdx;
            lm.nodeId     = node.id;
            lm.depth      = depth;
            lm.lineKind   = node.collapsed ? LineKind::Field : LineKind::Header;
            lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress + absAddr, false, state.offsetHexDigits);
            lm.offsetAddr = tree.baseAddress + absAddr;
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
            uint64_t ptrVal = 0;
            if (prov.isValid() && sz > 0 && prov.isReadable(absAddr, sz)) {
                ptrVal = (node.kind == NodeKind::Pointer32)
                    ? (uint64_t)prov.readU32(absAddr) : prov.readU64(absAddr);
                if (ptrVal != 0) {
                    uint64_t pBase = ptrToProviderAddr(tree, ptrVal);
                    if (pBase == UINT64_MAX) ptrVal = 0;  // ptr below base: invalid
                }
            }

            // Determine if pointer target is actually readable
            uint64_t pBase = (ptrVal != 0) ? ptrToProviderAddr(tree, ptrVal) : 0;
            bool ptrReadable = (ptrVal != 0) && prov.isReadable(pBase, 1);

            // For invalid/unreadable pointers: use NullProvider (shows zeros)
            // and reset margin offsets (unsigned wrap cancels baseAddress)
            static NullProvider s_nullProv;
            const Provider& childProv = ptrReadable ? prov : static_cast<const Provider&>(s_nullProv);
            if (!ptrReadable)
                pBase = (uint64_t)0 - tree.baseAddress;

            qulonglong key = pBase ^ (node.refId * kGoldenRatio);
            if (!state.ptrVisiting.contains(key)) {
                state.ptrVisiting.insert(key);
                int refIdx = tree.indexOfId(node.refId);
                if (refIdx >= 0) {
                    const Node& ref = tree.nodes[refIdx];
                    if (ref.kind == NodeKind::Struct || ref.kind == NodeKind::Array)
                        composeParent(state, tree, childProv, refIdx,
                                      depth, pBase, ref.id,
                                      /*isArrayChild=*/true);
                }
                state.ptrVisiting.remove(key);
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
        composeParent(state, tree, prov, nodeIdx, depth, base, rootId, isArrayChild, scopeId, arrayElementIdx, arrayContainerAddr);
    } else {
        composeLeaf(state, tree, prov, nodeIdx, depth, absAddr, scopeId);
    }
}

} // anonymous namespace

ComposeResult compose(const NodeTree& tree, const Provider& prov, uint64_t viewRootId) {
    ComposeState state;

    // Precompute parent→children map
    for (int i = 0; i < tree.nodes.size(); i++)
        state.childMap[tree.nodes[i].parentId].append(i);

    // Precompute absolute offsets
    state.absOffsets.resize(tree.nodes.size());
    for (int i = 0; i < tree.nodes.size(); i++)
        state.absOffsets[i] = tree.computeOffset(i);

    // Compute hex digit tier from max absolute address
    {
        uint64_t maxAddr = tree.baseAddress;
        for (int i = 0; i < tree.nodes.size(); i++) {
            uint64_t addr = tree.baseAddress + (uint64_t)state.absOffsets[i];
            if (addr > maxAddr) maxAddr = addr;
        }
        if      (maxAddr <= 0xFFFFULL)             state.offsetHexDigits = 4;
        else if (maxAddr <= 0xFFFFFFFFULL)         state.offsetHexDigits = 8;
        else if (maxAddr <= 0xFFFFFFFFFFFFULL)     state.offsetHexDigits = 12;
        else                                        state.offsetHexDigits = 16;
    }

    // Helper: compute the display type string for a node (for width calculation)
    auto nodeTypeName = [&](const Node& n) -> QString {
        if (n.kind == NodeKind::Array) {
            QString sn = (n.elementKind == NodeKind::Struct)
                ? resolvePointerTarget(tree, n.refId) : QString();
            return fmt::arrayTypeName(n.elementKind, n.arrayLen, sn);
        }
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
        // Skip hex/padding (they show ASCII preview, not name column)
        if (isHexPreview(node.kind)) continue;
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

            // Name width (skip hex/padding, but include containers)
            if (!isHexPreview(child.kind)) {
                scopeMaxName = qMax(scopeMaxName, (int)child.name.size());
            }
        }

        // Primitive arrays with no tree children: account for synthesized element types
        // e.g. "uint32_t[0]", "uint32_t[99]" — longest index determines width
        if (container.kind == NodeKind::Array
            && state.childMap.value(container.id).isEmpty()
            && container.elementKind != NodeKind::Struct
            && container.elementKind != NodeKind::Array
            && container.arrayLen > 0) {
            int maxIdx = container.arrayLen - 1;
            QString longestElemType = fmt::typeNameRaw(container.elementKind)
                                    + QStringLiteral("[%1]").arg(maxIdx);
            scopeMaxType = qMax(scopeMaxType, (int)longestElemType.size());
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

            // Name width (skip hex/padding, include containers)
            if (!isHexPreview(child.kind)) {
                rootMaxName = qMax(rootMaxName, (int)child.name.size());
            }
        }
        state.scopeTypeW[0] = qBound(kMinTypeW, rootMaxType, kMaxTypeW);
        state.scopeNameW[0] = qBound(kMinNameW, rootMaxName, kMaxNameW);
    }

    // Emit CommandRow as line 0 (combined: source + address + root class type + name)
    const QString cmdRowText = QStringLiteral("[\u25B8] source\u25BE \u00B7 0x0 \u00B7 struct\u25BE NoName {");
    {
        LineMeta lm;
        lm.nodeIdx   = -1;
        lm.nodeId    = kCommandRowId;
        lm.depth     = 0;
        lm.lineKind  = LineKind::CommandRow;
        lm.foldLevel = SC_FOLDLEVELBASE;
        lm.foldHead  = false;
        lm.offsetText = fmt::fmtOffsetMargin(tree.baseAddress, false, state.offsetHexDigits);
        lm.offsetAddr = tree.baseAddress;
        lm.markerMask = 0;
        lm.effectiveTypeW = state.typeW;
        lm.effectiveNameW = state.nameW;
        state.emitLine(cmdRowText, lm);
    }

    QVector<int> roots = state.childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    for (int idx : roots) {
        // If viewRootId is set, skip roots that don't match
        if (viewRootId != 0 && tree.nodes[idx].id != viewRootId)
            continue;
        composeNode(state, tree, prov, idx, 0);
    }

    return { state.text, state.meta, LayoutInfo{state.typeW, state.nameW, state.offsetHexDigits, tree.baseAddress} };
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

int NodeTree::computeStructAlignment(uint64_t structId) const {
    int idx = indexOfId(structId);
    if (idx < 0) return 1;
    int maxAlign = 1;
    QVector<int> kids = childrenOf(structId);
    for (int ci : kids) {
        const Node& c = nodes[ci];
        if (c.kind == NodeKind::Struct || c.kind == NodeKind::Array) {
            maxAlign = qMax(maxAlign, computeStructAlignment(c.id));
        } else {
            maxAlign = qMax(maxAlign, alignmentFor(c.kind));
        }
    }
    return maxAlign;
}

} // namespace rcx
