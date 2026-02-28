#include "core.h"
#include "addressparser.h"
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
    QSet<uint64_t>     visiting;      // cycle detection for struct recursion
    QSet<qulonglong>   ptrVisiting;   // cycle guard for pointer expansions
    QSet<uint64_t>     virtualPtrRefs; // refIds currently being virtually expanded via pointer deref
    int                currentLine = 0;
    int                typeW       = kColType;  // global type column width (fallback)
    int                nameW       = kColName;  // global name column width (fallback)
    int                offsetHexDigits = 8;     // hex digit tier for offset margin
    bool               baseEmitted = false;     // only first root struct shows base address
    bool               compactColumns = false;  // compact column mode: cap type width, overflow long types
    uint64_t           currentPtrBase = 0;      // absolute addr of current pointer expansion target

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


static const QVector<int>& childIndices(const ComposeState& state, uint64_t parentId) {
    static const QVector<int> kEmpty;
    auto it = state.childMap.constFind(parentId);
    return it == state.childMap.constEnd() ? kEmpty : it.value();
}

void composeLeaf(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx,
                 int depth, uint64_t absAddr, uint64_t scopeId) {
    const Node& node = tree.nodes[nodeIdx];

    // Get per-scope widths (falls back to global if no scope entry)
    int typeW = state.effectiveTypeW(scopeId);
    int nameW = state.effectiveNameW(scopeId);

    int numLines = linesForKind(node.kind);

    // Resolve pointer target name for display
    QString ptrTypeOverride;
    QString ptrTargetName;
    if (node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64) {
        if (node.ptrDepth > 0 && isValidPrimitivePtrTarget(node.elementKind)) {
            // Primitive pointer: e.g. "int32*" or "f64**"
            const auto* meta = kindMeta(node.elementKind);
            QString baseName = meta ? QString::fromLatin1(meta->typeName)
                                    : QStringLiteral("void");
            QString stars = (node.ptrDepth >= 2) ? QStringLiteral("**") : QStringLiteral("*");
            ptrTypeOverride = baseName + stars;
        } else {
            ptrTargetName = resolvePointerTarget(tree, node.refId);
            ptrTypeOverride = fmt::pointerTypeName(node.kind, ptrTargetName);
        }
    }

    // Detect type overflow in compact mode (for effectiveTypeW)
    QString rawType = ptrTypeOverride.isEmpty() ? fmt::typeNameRaw(node.kind) : ptrTypeOverride;
    bool typeOverflow = state.compactColumns && rawType.size() > typeW;
    int lineTypeW = typeOverflow ? rawType.size() : typeW;

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
        lm.offsetText      = fmt::fmtOffsetMargin(absAddr, isCont, state.offsetHexDigits);
        lm.offsetAddr      = absAddr;
        lm.ptrBase         = state.currentPtrBase;
        lm.markerMask      = computeMarkers(node, prov, absAddr, isCont, depth);
        lm.foldLevel       = computeFoldLevel(depth, false);
        lm.effectiveTypeW  = lineTypeW;
        lm.effectiveNameW  = nameW;
        lm.pointerTargetName = ptrTargetName;

        // Set byte count for hex preview lines (used for per-byte change highlighting)
        if (isHexPreview(node.kind)) {
            lm.lineByteCount = sizeForKind(node.kind);
        }

        QString lineText = fmt::fmtNodeLine(node, prov, absAddr, depth, sub,
                                            /*comment=*/{}, typeW, nameW, ptrTypeOverride,
                                            state.compactColumns);
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
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = absAddr;
        lm.ptrBase    = state.currentPtrBase;
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
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = absAddr;
        lm.ptrBase    = state.currentPtrBase;
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
        lm.offsetText = fmt::fmtOffsetMargin(absAddr, false, state.offsetHexDigits);
        lm.offsetAddr = absAddr;
        lm.ptrBase    = state.currentPtrBase;
        lm.nodeKind   = node.kind;
        lm.isRootHeader = false;
        lm.foldHead      = true;
        lm.foldCollapsed = node.collapsed;
        lm.foldLevel  = computeFoldLevel(depth, true);
        lm.markerMask = (1u << M_STRUCT_BG);

        QString headerText;
        if (node.kind == NodeKind::Array) {
            // Array header with navigation: "uint32_t[16]  name  {" (no brace when collapsed)
            lm.isArrayHeader = true;
            lm.elementKind   = node.elementKind;
            lm.arrayViewIdx  = node.viewIndex;
            lm.arrayCount    = node.arrayLen;
            QString elemStructName = (node.elementKind == NodeKind::Struct)
                ? resolvePointerTarget(tree, node.refId) : QString();
            QString rawType = fmt::arrayTypeName(node.elementKind, node.arrayLen, elemStructName);
            bool overflow = state.compactColumns && rawType.size() > typeW;
            lm.effectiveTypeW = overflow ? rawType.size() : typeW;
            lm.effectiveNameW = nameW;
            headerText = fmt::fmtArrayHeader(node, depth, node.viewIndex, node.collapsed, typeW, nameW, elemStructName, state.compactColumns);
        } else {
            // All structs (root and nested) use the same header format
            QString rawType = fmt::structTypeName(node);
            bool overflow = state.compactColumns && rawType.size() > typeW;
            lm.effectiveTypeW = overflow ? rawType.size() : typeW;
            lm.effectiveNameW = nameW;
            headerText = fmt::fmtStructHeader(node, depth, node.collapsed, typeW, nameW, state.compactColumns);
        }
        state.emitLine(headerText, lm);
    }

    if (!node.collapsed || isArrayChild || isRootHeader) {
        // Enum with members: render name = value lines instead of offset-based fields
        if (node.resolvedClassKeyword() == QStringLiteral("enum") && !node.enumMembers.isEmpty()) {
            int childDepth = depth + 1;
            int maxNameLen = 4;
            for (const auto& m : node.enumMembers)
                maxNameLen = qMax(maxNameLen, (int)m.first.size());

            // Build display order sorted by value
            QVector<int> order(node.enumMembers.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                return node.enumMembers[a].second < node.enumMembers[b].second;
            });

            for (int oi = 0; oi < order.size(); oi++) {
                int mi = order[oi];
                const auto& m = node.enumMembers[mi];
                LineMeta lm;
                lm.nodeIdx    = nodeIdx;
                lm.nodeId     = node.id;
                lm.subLine    = mi;
                lm.depth      = childDepth;
                lm.lineKind   = LineKind::Field;
                lm.isMemberLine = true;
                lm.nodeKind   = NodeKind::UInt32;
                lm.foldLevel  = computeFoldLevel(childDepth, false);
                lm.markerMask = 0;
                lm.offsetText = fmt::fmtOffsetMargin(absAddr, true, state.offsetHexDigits);
                lm.offsetAddr = absAddr;
                lm.ptrBase    = state.currentPtrBase;
                state.emitLine(fmt::fmtEnumMember(m.first, m.second, childDepth, maxNameLen), lm);
            }

            // Footer
            if (!isArrayChild) {
                LineMeta lm;
                lm.nodeIdx   = nodeIdx;
                lm.nodeId    = node.id;
                lm.depth     = depth;
                lm.lineKind  = LineKind::Footer;
                lm.nodeKind  = node.kind;
                lm.isRootHeader = isRootHeader;
                lm.foldLevel = computeFoldLevel(depth, false);
                lm.markerMask = 0;
                lm.offsetText = fmt::fmtOffsetMargin(absAddr, false, state.offsetHexDigits);
                lm.offsetAddr = absAddr;
                lm.ptrBase    = state.currentPtrBase;
                state.emitLine(fmt::fmtStructFooter(node, depth, 0), lm);
            }

            state.visiting.remove(node.id);
            return;
        }

        // Bitfield with members: render name : width = value lines
        if (node.resolvedClassKeyword() == QStringLiteral("bitfield")
            && !node.bitfieldMembers.isEmpty()) {
            int childDepth = depth + 1;
            int maxNameLen = 4;
            for (const auto& m : node.bitfieldMembers)
                maxNameLen = qMax(maxNameLen, (int)m.name.size());

            for (int mi = 0; mi < node.bitfieldMembers.size(); mi++) {
                const auto& m = node.bitfieldMembers[mi];
                uint64_t bitVal = fmt::extractBits(prov, absAddr, node.elementKind,
                                                   m.bitOffset, m.bitWidth);
                LineMeta lm;
                lm.nodeIdx    = nodeIdx;
                lm.nodeId     = node.id;
                lm.subLine    = mi;
                lm.depth      = childDepth;
                lm.lineKind   = LineKind::Field;
                lm.isMemberLine = true;
                lm.nodeKind   = node.elementKind;
                lm.foldLevel  = computeFoldLevel(childDepth, false);
                lm.markerMask = 0;
                lm.offsetText = fmt::fmtOffsetMargin(absAddr, true, state.offsetHexDigits);
                lm.offsetAddr = absAddr;
                lm.ptrBase    = state.currentPtrBase;
                state.emitLine(fmt::fmtBitfieldMember(m.name, m.bitWidth, bitVal,
                                                       childDepth, maxNameLen), lm);
            }

            // Footer
            if (!isArrayChild) {
                LineMeta lm;
                lm.nodeIdx   = nodeIdx;
                lm.nodeId    = node.id;
                lm.depth     = depth;
                lm.lineKind  = LineKind::Footer;
                lm.nodeKind  = node.kind;
                lm.isRootHeader = isRootHeader;
                lm.foldLevel = computeFoldLevel(depth, false);
                lm.markerMask = 0;
                int sz = sizeForKind(node.elementKind);
                lm.offsetText = fmt::fmtOffsetMargin(absAddr + sz, false, state.offsetHexDigits);
                lm.offsetAddr = absAddr + sz;
                lm.ptrBase    = state.currentPtrBase;
                state.emitLine(fmt::fmtStructFooter(node, depth, sz), lm);
            }

            state.visiting.remove(node.id);
            return;
        }

        const QVector<int>& allChildren = childIndices(state, node.id);

        // Split children into regular nodes and static fields (static fields render at the end)
        QVector<int> regular, staticIdxs;
        for (int ci : allChildren) {
            if (tree.nodes[ci].isStatic)
                staticIdxs.append(ci);
            else
                regular.append(ci);
        }

        int childDepth = depth + 1;

        // Primitive arrays with no child nodes: synthesize element lines dynamically
        if (node.kind == NodeKind::Array && regular.isEmpty()
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
                lm.arrayElementIdx = i;
                lm.offsetText = fmt::fmtOffsetMargin(elemAddr, false, state.offsetHexDigits);
                lm.offsetAddr = elemAddr;
                lm.ptrBase    = state.currentPtrBase;
                lm.markerMask = computeMarkers(elem, prov, elemAddr, false, childDepth);
                lm.foldLevel  = computeFoldLevel(childDepth, false);
                bool elemOverflow = state.compactColumns && elemTypeStr.size() > eTW;
                lm.effectiveTypeW = elemOverflow ? elemTypeStr.size() : eTW;
                lm.effectiveNameW = eNW;

                state.emitLine(fmt::fmtNodeLine(elem, prov, elemAddr, childDepth, 0,
                                                {}, eTW, eNW, elemTypeStr,
                                                state.compactColumns), lm);
            }
        }

        // Struct arrays with refId but no child nodes: synthesize by expanding the
        // referenced struct for each element (like repeated pointer deref)
        if (node.kind == NodeKind::Array && regular.isEmpty()
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
        if (node.kind == NodeKind::Struct && regular.isEmpty() && node.refId != 0) {
            int refIdx = tree.indexOfId(node.refId);
            if (refIdx >= 0) {
                const QVector<int>& refChildren = childIndices(state, node.refId);
                // Use the referenced struct's scope widths (children come from there)
                uint64_t refScopeId = node.refId;
                for (int childIdx : refChildren) {
                    const Node& child = tree.nodes[childIdx];
                    // Self-referential child → show as collapsed struct (non-expandable)
                    if (state.visiting.contains(child.id)) {
                        int typeW = state.effectiveTypeW(refScopeId);
                        int nameW = state.effectiveNameW(refScopeId);
                        QString rawType = fmt::structTypeName(child);
                        bool overflow = state.compactColumns && rawType.size() > typeW;
                        LineMeta lm;
                        lm.nodeIdx    = nodeIdx;  // parent struct — materialize target
                        lm.nodeId     = child.id;
                        lm.depth      = childDepth;
                        lm.lineKind   = LineKind::Header;
                        lm.offsetText = fmt::fmtOffsetMargin(
                            absAddr + child.offset, false,
                            state.offsetHexDigits);
                        lm.offsetAddr = absAddr + child.offset;
                        lm.ptrBase    = state.currentPtrBase;
                        lm.nodeKind   = child.kind;
                        lm.foldHead      = true;
                        lm.foldCollapsed = true;
                        lm.foldLevel  = computeFoldLevel(childDepth, true);
                        lm.markerMask = (1u << M_STRUCT_BG) | (1u << M_CYCLE);
                        lm.effectiveTypeW = overflow ? rawType.size() : typeW;
                        lm.effectiveNameW = nameW;
                        state.emitLine(fmt::fmtStructHeader(child, childDepth,
                            /*collapsed=*/true, typeW, nameW, state.compactColumns), lm);
                        continue;
                    }
                    composeNode(state, tree, prov, childIdx, childDepth,
                                absAddr, node.refId, false, refScopeId);
                }
            }
        }

        // For arrays, render children as condensed (no header/footer for struct elements)
        bool childrenAreArrayElements = (node.kind == NodeKind::Array);
        int elementIdx = 0;
        for (int childIdx : regular) {
            // Pass this container's id as the scope for children (for per-scope widths)
            // For array elements, also pass the element index for [N] separator
            composeNode(state, tree, prov, childIdx, childDepth, base, rootId,
                        childrenAreArrayElements, node.id,
                        childrenAreArrayElements ? elementIdx++ : -1,
                        childrenAreArrayElements ? absAddr : 0);
        }

        // ── Static fields: render after regular children, before footer ──
        if (!staticIdxs.isEmpty() && !node.collapsed) {
            // Build identifier resolver for static field expressions
            auto makeResolver = [&](uint64_t parentAbsAddr) {
                AddressParserCallbacks cbs;
                cbs.resolveIdentifier = [&tree, &prov, &regular, parentAbsAddr]
                        (const QString& name, bool* ok) -> uint64_t {
                    if (name == QStringLiteral("base")) {
                        *ok = true;
                        return parentAbsAddr;
                    }
                    // Find sibling field by name, read its value
                    for (int ci : regular) {
                        const Node& sib = tree.nodes[ci];
                        if (sib.name == name) {
                            int sz = sib.byteSize();
                            uint64_t sibAddr = parentAbsAddr + sib.offset;
                            if (sz > 0 && prov.isValid() && prov.isReadable(sibAddr, sz)) {
                                *ok = true;
                                if (sz == 1) return (uint64_t)prov.readU8(sibAddr);
                                if (sz == 2) return (uint64_t)prov.readU16(sibAddr);
                                if (sz == 4) return (uint64_t)prov.readU32(sibAddr);
                                return prov.readU64(sibAddr);
                            }
                            *ok = false;
                            return 0;
                        }
                    }
                    *ok = false;
                    return 0;
                };
                cbs.readPointer = [&prov](uint64_t addr, bool* ok) -> uint64_t {
                    if (prov.isValid() && prov.isReadable(addr, 8)) {
                        *ok = true;
                        return prov.readU64(addr);
                    }
                    *ok = false;
                    return 0;
                };
                return cbs;
            };

            auto cbs = makeResolver(absAddr);

            for (int si : staticIdxs) {
                const Node& sf = tree.nodes[si];

                // Evaluate expression → absolute address
                uint64_t staticAddr = 0;
                bool exprOk = false;
                if (!sf.offsetExpr.isEmpty()) {
                    auto result = AddressParser::evaluate(sf.offsetExpr, 8, &cbs);
                    exprOk = result.ok;
                    if (result.ok)
                        staticAddr = result.value;
                }

                // Resolve type name
                QString typeName;
                if (sf.kind == NodeKind::Struct)
                    typeName = fmt::structTypeName(sf);
                else if (sf.kind == NodeKind::Pointer64 || sf.kind == NodeKind::Pointer32)
                    typeName = fmt::pointerTypeName(sf.kind, resolvePointerTarget(tree, sf.refId));
                else
                    typeName = fmt::typeNameRaw(sf.kind);

                bool isCollapsed = sf.collapsed;

                // ── Header line: "static <type> <name> {" or collapsed: "static <type> <name> { return <expr>; }"
                QString headerLine;
                if (isCollapsed) {
                    QString exprPart;
                    if (!sf.offsetExpr.isEmpty()) {
                        if (exprOk)
                            exprPart = QStringLiteral("return %1 } \u2192 0x%2")
                                .arg(sf.offsetExpr)
                                .arg(QString::number(staticAddr, 16).toUpper());
                        else
                            exprPart = QStringLiteral("return %1 }  (error)").arg(sf.offsetExpr);
                    } else {
                        exprPart = QStringLiteral("}");
                    }
                    headerLine = fmt::indent(childDepth)
                        + QStringLiteral("static ") + typeName
                        + QStringLiteral(" ") + sf.name
                        + QStringLiteral(" { ") + exprPart;
                } else {
                    headerLine = fmt::indent(childDepth)
                        + QStringLiteral("static ") + typeName
                        + QStringLiteral(" ") + sf.name
                        + QStringLiteral(" {");
                }

                LineMeta lm;
                lm.nodeIdx    = si;
                lm.nodeId     = sf.id;
                lm.depth      = childDepth;
                lm.lineKind   = LineKind::Header;
                lm.nodeKind   = sf.kind;
                lm.foldHead      = true;
                lm.foldCollapsed = isCollapsed;
                lm.isStaticLine  = true;
                lm.foldLevel  = computeFoldLevel(childDepth, true);
                lm.markerMask = (1u << M_STRUCT_BG);
                lm.offsetText = QStringLiteral("~") + QString::number(staticAddr, 16)
                    .toUpper().rightJustified(state.offsetHexDigits - 1, '0');
                lm.offsetAddr = staticAddr;
                lm.ptrBase    = state.currentPtrBase;
                lm.effectiveTypeW = typeName.size() + 7; // "static " prefix
                lm.effectiveNameW = sf.name.size();
                state.emitLine(headerLine, lm);

                // ── Body + children (only when expanded) ──
                if (!isCollapsed) {
                    // Body line: "   return <expr>    → 0xADDR"
                    {
                        QString bodyLine;
                        if (!sf.offsetExpr.isEmpty()) {
                            if (exprOk)
                                bodyLine = fmt::indent(childDepth + 1)
                                    + QStringLiteral("return %1").arg(sf.offsetExpr);
                            else
                                bodyLine = fmt::indent(childDepth + 1)
                                    + QStringLiteral("return %1  (error)").arg(sf.offsetExpr);
                        } else {
                            bodyLine = fmt::indent(childDepth + 1)
                                + QStringLiteral("return 0");
                        }

                        // Right-align resolved address
                        if (exprOk && !sf.offsetExpr.isEmpty()) {
                            bodyLine += QStringLiteral("  \u2192 0x")
                                + QString::number(staticAddr, 16).toUpper();
                        }

                        LineMeta blm;
                        blm.nodeIdx    = si;
                        blm.nodeId     = sf.id;
                        blm.depth      = childDepth + 1;
                        blm.lineKind   = LineKind::Field;
                        blm.nodeKind   = sf.kind;
                        blm.isStaticLine = true;
                        blm.foldLevel  = computeFoldLevel(childDepth + 1, false);
                        blm.markerMask = 0;
                        blm.offsetText = QString(state.offsetHexDigits, QChar(' '));
                        blm.offsetAddr = staticAddr;
                        blm.ptrBase    = state.currentPtrBase;
                        state.emitLine(bodyLine, blm);
                    }

                    // If struct/array, compose children at evaluated address
                    if (exprOk && (sf.kind == NodeKind::Struct || sf.kind == NodeKind::Array)) {
                        const QVector<int>& staticKids = childIndices(state, sf.id);
                        for (int sci : staticKids) {
                            composeNode(state, tree, prov, sci, childDepth + 1,
                                        staticAddr, sf.id, false, sf.id);
                        }
                    }

                    // Footer line: "};"
                    {
                        LineMeta flm;
                        flm.nodeIdx   = si;
                        flm.nodeId    = sf.id;
                        flm.depth     = childDepth;
                        flm.lineKind  = LineKind::Footer;
                        flm.nodeKind  = sf.kind;
                        flm.isStaticLine = true;
                        flm.foldLevel = computeFoldLevel(childDepth, false);
                        flm.markerMask = 0;
                        if (exprOk && (sf.kind == NodeKind::Struct || sf.kind == NodeKind::Array)) {
                            int sSpan = tree.structSpan(sf.id, &state.childMap);
                            flm.offsetText = fmt::fmtOffsetMargin(staticAddr + sSpan, false,
                                                                   state.offsetHexDigits);
                            flm.offsetAddr = staticAddr + sSpan;
                        } else {
                            flm.offsetText = QString(state.offsetHexDigits, QChar(' '));
                            flm.offsetAddr = staticAddr;
                        }
                        flm.ptrBase    = state.currentPtrBase;
                        state.emitLine(fmt::indent(childDepth) + QStringLiteral("};"), flm);
                    }
                }
            }
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
        lm.offsetText = fmt::fmtOffsetMargin(absAddr + sz, false, state.offsetHexDigits);
        lm.offsetAddr = absAddr + sz;
        lm.ptrBase    = state.currentPtrBase;
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

        // Check if this pointer has materialized children (from materializeRefChildren)
        const QVector<int>& ptrChildren = childIndices(state, node.id);
        bool hasMaterialized = !ptrChildren.isEmpty();

        // Force collapsed if this refId is already being virtually expanded
        // (prevents infinite recursion in virtual expansion mode).
        // Materialized children bypass this — they are real tree nodes with
        // independent collapsed state, so recursion is bounded by the tree.
        bool forceCollapsed = !hasMaterialized
                              && state.virtualPtrRefs.contains(node.refId);
        bool effectiveCollapsed = node.collapsed || forceCollapsed;

        // Emit merged fold header: "Type* Name {" (expanded) or "Type* Name -> val" (collapsed)
        {
            LineMeta lm;
            lm.nodeIdx    = nodeIdx;
            lm.nodeId     = node.id;
            lm.depth      = depth;
            lm.lineKind   = effectiveCollapsed ? LineKind::Field : LineKind::Header;
            lm.offsetText = fmt::fmtOffsetMargin(absAddr, false, state.offsetHexDigits);
            lm.offsetAddr = absAddr;
            lm.ptrBase    = state.currentPtrBase;
            lm.nodeKind   = node.kind;
            lm.foldHead      = true;
            lm.foldCollapsed = effectiveCollapsed;
            lm.foldLevel  = computeFoldLevel(depth, true);
            lm.markerMask = computeMarkers(node, prov, absAddr, false, depth);
            if (forceCollapsed) lm.markerMask |= (1u << M_CYCLE);
            bool ptrOverflow = state.compactColumns && ptrTypeOverride.size() > typeW;
            lm.effectiveTypeW = ptrOverflow ? ptrTypeOverride.size() : typeW;
            lm.effectiveNameW = nameW;
            lm.pointerTargetName = ptrTargetName;
            state.emitLine(fmt::fmtPointerHeader(node, depth, effectiveCollapsed,
                                                  prov, absAddr, ptrTypeOverride,
                                                  typeW, nameW, state.compactColumns), lm);
        }

        if (!effectiveCollapsed) {
            int sz = node.byteSize();
            uint64_t ptrVal = 0;
            if (prov.isValid() && sz > 0 && prov.isReadable(absAddr, sz)) {
                ptrVal = (node.kind == NodeKind::Pointer32)
                    ? (uint64_t)prov.readU32(absAddr) : prov.readU64(absAddr);
                if (ptrVal != 0) {
                    // Treat sentinel values as invalid pointers
                    if (ptrVal == UINT64_MAX || (node.kind == NodeKind::Pointer32 && ptrVal == 0xFFFFFFFF))
                        ptrVal = 0;
                }
            }

            // Pointer target address is used directly (absolute)
            uint64_t pBase = ptrVal;
            bool ptrReadable = (ptrVal != 0) && prov.isReadable(pBase, 1);

            // For invalid/unreadable pointers: use NullProvider (shows zeros)
            static NullProvider s_nullProv;
            const Provider& childProv = ptrReadable ? prov : static_cast<const Provider&>(s_nullProv);
            if (!ptrReadable)
                pBase = 0;

            uint64_t savedPtrBase = state.currentPtrBase;
            state.currentPtrBase = pBase;

            if (hasMaterialized) {
                // Render materialized children at the pointer target address.
                // These are real tree nodes with independent state — use rootId
                // so resolveAddr computes offsets relative to the pointer target.
                for (int childIdx : ptrChildren) {
                    composeNode(state, tree, childProv, childIdx, depth + 1,
                                pBase, node.id, false, node.id);
                }
            } else {
                // Virtual expansion via ref struct definition.
                // Temporarily remove the ref struct from visiting so composeParent
                // doesn't hit the struct-level cycle guard. The ptrVisiting mechanism
                // handles actual address-level pointer cycles, and virtualPtrRefs
                // prevents infinite virtual recursion (inner self-referential pointers
                // are force-collapsed with M_CYCLE for the user to materialize).
                qulonglong key = pBase ^ (node.refId * kGoldenRatio);
                if (!state.ptrVisiting.contains(key)) {
                    state.ptrVisiting.insert(key);
                    int refIdx = tree.indexOfId(node.refId);
                    if (refIdx >= 0) {
                        const Node& ref = tree.nodes[refIdx];
                        if (ref.kind == NodeKind::Struct || ref.kind == NodeKind::Array) {
                            bool wasVisiting = state.visiting.remove(node.refId);
                            state.virtualPtrRefs.insert(node.refId);
                            composeParent(state, tree, childProv, refIdx,
                                          depth, pBase, ref.id,
                                          /*isArrayChild=*/true);
                            state.virtualPtrRefs.remove(node.refId);
                            if (wasVisiting) state.visiting.insert(node.refId);
                        }
                    }
                    state.ptrVisiting.remove(key);
                }
            }

            state.currentPtrBase = savedPtrBase;

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

ComposeResult compose(const NodeTree& tree, const Provider& prov, uint64_t viewRootId,
                      bool compactColumns) {
    ComposeState state;
    state.compactColumns = compactColumns;

    // Precompute parent→children map
    for (int i = 0; i < tree.nodes.size(); i++)
        state.childMap[tree.nodes[i].parentId].append(i);

    for (auto it = state.childMap.begin(); it != state.childMap.end(); ++it) {
        QVector<int>& children = it.value();
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return tree.nodes[a].offset < tree.nodes[b].offset;
        });
    }

    // Precompute absolute offsets (baseAddress + structure-relative offset)
    state.absOffsets.resize(tree.nodes.size());
    for (int i = 0; i < tree.nodes.size(); i++)
        state.absOffsets[i] = tree.baseAddress + tree.computeOffset(i);

    // Compute hex digit tier from max absolute address
    {
        uint64_t maxAddr = tree.baseAddress;
        for (int i = 0; i < tree.nodes.size(); i++) {
            uint64_t addr = (uint64_t)state.absOffsets[i];
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
    const int typeCap = state.compactColumns ? kCompactTypeW : kMaxTypeW;
    int maxTypeLen = kMinTypeW;
    for (const Node& node : tree.nodes) {
        maxTypeLen = qMax(maxTypeLen, (int)nodeTypeName(node).size());
    }
    state.typeW = qBound(kMinTypeW, maxTypeLen, typeCap);

    // Compute effective name column width from longest name
    // Include struct/array names - they now use columnar layout too
    int maxNameLen = kMinNameW;
    for (const Node& node : tree.nodes) {
        // Skip hex (they show ASCII preview, not name column)
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

            // Name width (skip hex, but include containers)
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

        state.scopeTypeW[container.id] = qBound(kMinTypeW, scopeMaxType, typeCap);
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

            // Name width (skip hex, include containers)
            if (!isHexPreview(child.kind)) {
                rootMaxName = qMax(rootMaxName, (int)child.name.size());
            }
        }
        state.scopeTypeW[0] = qBound(kMinTypeW, rootMaxType, typeCap);
        state.scopeNameW[0] = qBound(kMinNameW, rootMaxName, kMaxNameW);
    }

    // Emit CommandRow as line 0 (combined: source + address + root class type + name)
    const QString cmdRowText = QStringLiteral("[\u25B8] source\u25BE  0x0  struct NoName {");
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
        lm.ptrBase    = state.currentPtrBase;
        lm.markerMask = 0;
        lm.effectiveTypeW = state.typeW;
        lm.effectiveNameW = state.nameW;
        state.emitLine(cmdRowText, lm);
    }

    const QVector<int>& roots = childIndices(state, 0);

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


} // namespace rcx
