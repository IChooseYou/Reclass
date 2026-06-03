#include "core.h"
#include "typeinfer.h"
#include "addressparser.h"
#include "profiler.h"
#include "rtti.h"
#include "providers/provider.h"
#include <QRegularExpression>
#include <algorithm>
#include <numeric>

namespace rcx {

// RTTI discovery hook — GUI app sets this so each successful walkRtti()
// surfaces the demangled class name in the unified Symbols panel and in
// the expression parser. Tests leave it nullptr (no-op).
void (*g_rttiDiscoveryHook)(const QString& name, uint64_t address,
                             const QString& moduleName) = nullptr;
QString (*g_nameLookupHook)(uint64_t address, const Provider* active) = nullptr;
void (*g_namesChangedHook)() = nullptr;

namespace {

// ── Value preview for type hints ──
// Formats raw bytes as the suggested type using existing fmt:: functions.

static QString formatPreview(const uint8_t* data, int len, const TypeSuggestion& s) {
    using namespace detail;
    if (s.kinds.isEmpty()) return {};
    NodeKind k = s.kinds[0];
    if (s.kinds.size() == 1) {
        switch (k) {
        case NodeKind::Float:     return fmt::fmtFloat(loadF32(data));
        case NodeKind::Double:    return fmt::fmtDouble(loadF64(data));
        case NodeKind::Int32:     return fmt::fmtInt32((int32_t)loadU32(data));
        case NodeKind::UInt32:    return fmt::fmtUInt32(loadU32(data));
        case NodeKind::Int16:     return fmt::fmtInt16((int16_t)loadU16(data));
        case NodeKind::UInt16:    return fmt::fmtUInt16(loadU16(data));
        case NodeKind::Int64:     return fmt::fmtInt64((int64_t)loadU64(data));
        case NodeKind::UInt64:    return fmt::fmtUInt64(loadU64(data));
        case NodeKind::Pointer64: return fmt::fmtPointer64(loadU64(data));
        case NodeKind::Pointer32: return fmt::fmtPointer32(loadU32(data));
        case NodeKind::Bool:      return fmt::fmtBool(data[0]);
        case NodeKind::UTF8: {
            int n = std::min(len, 8);
            QString s;
            for (int i = 0; i < n && data[i] >= 0x20 && data[i] <= 0x7E; ++i)
                s += QLatin1Char(data[i]);
            return s.isEmpty() ? QString() : (QStringLiteral("\"") + s + QStringLiteral("\""));
        }
        default: return {};
        }
    }
    // Split: show each part
    int partSz = len / s.kinds.size();
    QStringList parts;
    for (int i = 0; i < s.kinds.size(); ++i) {
        TypeSuggestion sub;
        sub.kinds = {s.kinds[i]};
        sub.score = s.score;
        sub.strength = s.strength;
        parts << formatPreview(data + i * partSz, partSz, sub);
    }
    return parts.join(QStringLiteral(", "));
}

// Scintilla fold constants (avoid including Scintilla headers in core)
constexpr int SC_FOLDLEVELBASE       = 0x400;
constexpr int SC_FOLDLEVELHEADERFLAG = 0x2000;
constexpr uint64_t kGoldenRatio      = 0x9E3779B97F4A7C15ULL;

struct ComposeState {
    QString            text;
    QVector<LineMeta>  meta;
    QVector<int>       lineStarts;    // char offset of each line in `text`
    int                maxLineLen = 0;// longest line in chars, trailing spaces excluded
    QSet<uint64_t>     visiting;      // cycle detection for struct recursion
    QSet<qulonglong>   ptrVisiting;   // cycle guard for pointer expansions
    QSet<uint64_t>     virtualPtrRefs; // refIds currently being virtually expanded via pointer deref
    int                currentLine = 0;
    int                typeW       = kColType;  // global type column width (fallback)
    int                nameW       = kColName;  // global name column width (fallback)
    int                offsetHexDigits = 8;     // hex digit tier for offset margin
    bool               baseEmitted = false;     // only first root struct shows base address
    bool               compactColumns = false;  // compact column mode: cap type width, overflow long types
    bool               treeLines      = false;  // draw Unicode tree connectors in indentation
    bool               braceWrap      = false;  // opening brace on its own line
    bool               typeHints      = false;  // show TypeHint chips on hex nodes ("[ptr64]")
    bool               showComments   = true;   // show Comment chips ("/ note") + PDB symbol annotations
    bool               showRtti       = true;   // show Rtti chips ("{RTTI: ClassName}")
    bool               showEnumChips  = true;   // show Enum chips ("(MEMBER)") on int fields with enum refId
    SymbolLookupFn     symbolLookup;             // optional PDB symbol lookup callback
    QVector<bool>      siblingStack;             // per-depth: true = more siblings follow at this level
    uint64_t           currentPtrBase = 0;      // absolute addr of current pointer expansion target

    // ── RTTI auto-detect cache (per compose pass) ──
    // Module list is fetched lazily on first vtable candidate. walkRtti()
    // results are memoized — both successes (avoid re-walk) and failures
    // (avoid re-trying every refresh on the same arbitrary 8-byte word).
    bool                              rttiModulesCached = false;
    QVector<Provider::ModuleEntry>    rttiModules;
    QHash<uint64_t, RttiInfo>         rttiCache;

    // Precomputed for O(1) lookups
    QHash<uint64_t, QVector<int>> childMap;
    mutable QSet<uint64_t>        childMapSorted;  // tracks which entries are sorted
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

    // Set sibling-continuation flag for children at the given depth.
    // childDepth is the depth of the children being iterated.
    void setTreeSibling(int childDepth, bool hasMoreSiblings) {
        if (!treeLines) return;
        int d = childDepth - 1;
        while (siblingStack.size() <= d) siblingStack.append(false);
        siblingStack[d] = hasMoreSiblings;
    }

    void emitLine(const QString& lineText, LineMeta&& lm) {
        // Record this line's char offset in text + the global '\n' if any.
        if (currentLine > 0) text += '\n';
        lineStarts.append(text.size());
        int lineStartChars = text.size();
        // 3-char fold indicator column: " - " expanded, " + " collapsed, "   " other
        // CommandRow has no fold prefix (flush left)
        if (lm.lineKind == LineKind::CommandRow
            || (lm.lineKind == LineKind::Footer && lm.isRootHeader)) {
            // no prefix — flush left
        } else if (lm.foldHead)
            text += lm.foldCollapsed ? QStringLiteral(" \u25B8 ") : QStringLiteral(" \u25BE ");
        else
            text += QStringLiteral("   ");

        // Replace leading indent spaces with Unicode tree connectors
        if (treeLines && lm.depth > 0) {
            QString treeIndent;
            int D = lm.depth;
            bool isFooter = (lm.lineKind == LineKind::Footer);
            for (int d = 0; d < D; d++) {
                bool active = (d < siblingStack.size() && siblingStack[d]);
                if (isFooter || d < D - 1) {
                    // Ancestor continuation or footer's own level
                    treeIndent += active ? QStringLiteral("\u2502 ")
                                        : QStringLiteral("  ");
                } else {
                    // This node's own connector (non-footer only)
                    treeIndent += active ? QStringLiteral("\u251C ")
                                        : QStringLiteral("\u2514 ");
                }
            }
            text += treeIndent + lineText.mid(D * kTreeIndent);
        } else {
            text += lineText;
        }

        // Auto-detect trailing '{' for braceCol (avoids per-char IPC scan in editor).
        // Stored in Scintilla document-column space — LineGeometry handles the
        // prefix offset so flush-left lines (CommandRow, root footer) get 0
        // and Header lines get kFoldCol added automatically.
        if (lm.braceCol < 0 && (lm.lineKind == LineKind::Header
                                 || lm.lineKind == LineKind::CommandRow)) {
            LineGeometry geom = LineGeometry::forLine(lm);
            int len = lineText.size();
            for (int p = len - 1; p >= 0; --p) {
                QChar ch = lineText[p];
                if (ch == ' ' || ch == '\t') continue;
                if (ch == '{') lm.braceCol = geom.documentColumn(p);
                break;
            }
        }

        meta.append(std::move(lm));
        currentLine++;

        // Track the longest line length (excluding trailing spaces) so
        // applyDocument can set SCI_SETSCROLLWIDTH without re-scanning.
        int lineEnd = text.size();
        int len = lineEnd - lineStartChars;
        // Trim trailing spaces in-place by counting backwards.
        int lastNonSpace = len;
        while (lastNonSpace > 0 && text[lineStartChars + lastNonSpace - 1] == ' ')
            --lastNonSpace;
        if (lastNonSpace > maxLineLen) maxLineLen = lastNonSpace;
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


static const QVector<int>& childIndices(ComposeState& state, uint64_t parentId) {
    static const QVector<int> kEmpty;
    auto it = state.childMap.find(parentId);
    if (it == state.childMap.end()) return kEmpty;
    // Lazy sort: only sort children on first access
    if (!state.childMapSorted.contains(parentId)) {
        auto& children = it.value();
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return state.absOffsets[a] < state.absOffsets[b];
        });
        state.childMapSorted.insert(parentId);
    }
    return it.value();
}

// Resolve RTTI for a candidate vtable address, cached per compose pass.
// Module enumeration runs at most once per pass; values that don't land
// inside any known module short-circuit before walkRtti is even called.
// Negative results (ok=false) are cached too — prevents the parser from
// being re-run on the same arbitrary qword every refresh tick.
//
// maxVtableSlots=0 is honored by walkRtti's `for (slot = 0; slot < cap; ++slot)`
// loop (rtti.cpp:236) — it yields the demangled class name without
// enumerating method addresses, which is all the inline hint needs.
static const RttiInfo& rttiForVtable(ComposeState& state, const Provider& prov,
                                     uint64_t candidateAddr) {
    auto it = state.rttiCache.find(candidateAddr);
    if (it != state.rttiCache.end()) return it.value();

    if (!state.rttiModulesCached) {
        state.rttiModules = prov.enumerateModules();
        state.rttiModulesCached = true;
    }

    RttiInfo info;  // ok=false default — caches negative results
    bool inModule = false;
    for (const auto& m : state.rttiModules) {
        if (candidateAddr >= m.base && candidateAddr < m.base + m.size) {
            inModule = true; break;
        }
    }
    if (inModule) {
        // Try MSVC RTTI first (signature-validated, lower false-positive
        // risk). Fall back to Itanium ABI for GCC/Clang/MinGW binaries —
        // most C++ class instances on Linux/macOS, and any Reclass.exe
        // self-attach (Reclass is MinGW-built) hit this path.
        info = walkRtti(prov, candidateAddr, /*ptrSize=*/8, /*maxVtableSlots=*/0);
        if (!info.ok)
            info = walkRttiItanium(prov, candidateAddr, /*ptrSize=*/8, /*maxVtableSlots=*/0);
        if (info.ok && !info.demangledName.isEmpty() && g_rttiDiscoveryHook) {
            // Surface the discovery in the unified Symbols panel + make it
            // resolve in the expression parser. The hook is set by the
            // GUI app (main.cpp), nullptr in headless test builds.
            g_rttiDiscoveryHook(info.demangledName, info.vtableAddress,
                                info.moduleName);
        }
    }
    return state.rttiCache.insert(candidateAddr, info).value();
}

void composeLeaf(ComposeState& state, const NodeTree& tree,
                 const Provider& prov, int nodeIdx,
                 int depth, uint64_t absAddr, uint64_t scopeId) {
    const Node& node = tree.nodes[nodeIdx];

    // Resolve parent's absolute address for relative offset display
    uint64_t parentAbsAddr = 0;
    if (depth > 0) {
        int pi = tree.indexOfId(node.parentId);
        if (pi >= 0 && pi < state.absOffsets.size())
            parentAbsAddr = state.absOffsets[pi];
    }

    // Get per-scope widths (falls back to global if no scope entry)
    int typeW = state.effectiveTypeW(scopeId);
    int nameW = state.effectiveNameW(scopeId);

    int numLines = linesForKind(node.kind);

    // Resolve pointer target name for display
    QString ptrTypeOverride;
    QString ptrTargetName;
    if (node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64) {
        if (node.ptrDepth > 0 && node.refId == 0 && isValidPrimitivePtrTarget(node.elementKind)) {
            // Primitive pointer: e.g. "int32*" or "f64**"
            const auto* meta = kindMeta(node.elementKind);
            QString baseName = meta ? QString::fromLatin1(meta->typeName)
                                    : QStringLiteral("void");
            QString stars = (node.ptrDepth >= 2) ? QStringLiteral("**") : QStringLiteral("*");
            ptrTypeOverride = baseName + stars;
        } else {
            ptrTargetName = resolvePointerTarget(tree, node.refId);
            QString stars = QString(node.ptrDepth + 1, QChar('*'));
            ptrTypeOverride = (ptrTargetName.isEmpty() ? QStringLiteral("void") : ptrTargetName) + stars;
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
        lm.parentAddr      = parentAbsAddr;
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

        // ── Chip block: Enum, TypeHint, Rtti, Comment ──
        // Each chip appends "  <text>" to lineText and records its
        // [startCol, endCol) in lm.chips. One emit pattern, one source
        // of truth for the editor's hit-test, indicator passes, and
        // inline-comment span lookup. Legacy commentStart / typeHint*
        // / rttiHint* fields are populated alongside while step 5 of
        // the migration finishes; remove them then.
        if (sub == 0) {
            // Defensive sanitizer for any chip text: chips occupy a
            // single Scintilla row, no exceptions. Any \r/\n/\t in the
            // source (Node::comment, PDB symbol with embedded newlines,
            // typeHint with weird payload) would split the chip across
            // logical rows without LineMeta for the continuations —
            // phantom rows, broken offset margins, broken hit-tests.
            // Collapse all of it to a middle-dot separator + squashed
            // runs of spaces. Multi-line chips can come back later as
            // an explicit feature; for now, single row only.
            auto sanitizeChip = [](QString s) -> QString {
                if (s.contains(QChar('\n')) || s.contains(QChar('\r'))
                    || s.contains(QChar('\t'))) {
                    s.replace(QChar('\r'), QChar(' '));
                    s.replace(QChar('\t'), QChar(' '));
                    s.replace(QChar('\n'), QStringLiteral(" · "));
                    static const QRegularExpression rxRun(QStringLiteral("  +"));
                    s.replace(rxRun, QStringLiteral(" "));
                }
                return s;
            };
            auto pushChip = [&](ChipKind kind, const QString& rawText,
                                const std::function<void(LineChip&)>& fillPayload = {}) {
                QString text = sanitizeChip(rawText);
                LineChip c;
                c.kind = kind;
                c.text = text;
                // Trim the value-column padding so chips sit next to
                // value, then append "  " + text to the Scintilla buffer.
                // Scintilla owns paint/scroll/zoom/resize natively — no
                // overlay widget, no sync code, no race.
                while (lineText.endsWith(QLatin1Char(' '))) lineText.chop(1);
                lineText += QStringLiteral("  ") + text;
                c.startCol = LineGeometry::forLine(lm)
                                 .documentColumn(lineText.size() - text.size());
                c.endCol   = LineGeometry::forLine(lm)
                                 .documentColumn(lineText.size());
                if (fillPayload) fillPayload(c);
                lm.chips.push_back(std::move(c));
            };

            // 1. Enum — int field whose refId points at a top-level enum.
            if (state.showEnumChips
                && node.refId != 0
                && (node.kind == NodeKind::UInt8 || node.kind == NodeKind::UInt16
                 || node.kind == NodeKind::UInt32 || node.kind == NodeKind::UInt64
                 || node.kind == NodeKind::Int8  || node.kind == NodeKind::Int16
                 || node.kind == NodeKind::Int32 || node.kind == NodeKind::Int64)
                && prov.isReadable(absAddr, node.byteSize())) {
                int refIdx = tree.indexOfId(node.refId);
                if (refIdx >= 0) {
                    const Node& refNode = tree.nodes[refIdx];
                    if (refNode.isEnum() && !refNode.enumMembers.isEmpty()) {
                        int64_t v = 0;
                        switch (node.kind) {
                        case NodeKind::UInt8:  v = (int64_t)prov.readU8 (absAddr); break;
                        case NodeKind::UInt16: v = (int64_t)prov.readU16(absAddr); break;
                        case NodeKind::UInt32: v = (int64_t)prov.readU32(absAddr); break;
                        case NodeKind::UInt64: v = (int64_t)prov.readU64(absAddr); break;
                        case NodeKind::Int8:   v = (int8_t) prov.readU8 (absAddr); break;
                        case NodeKind::Int16:  v = (int16_t)prov.readU16(absAddr); break;
                        case NodeKind::Int32:  v = (int32_t)prov.readU32(absAddr); break;
                        case NodeKind::Int64:  v = (int64_t)prov.readU64(absAddr); break;
                        default: break;
                        }
                        QString memberName;
                        for (const auto& m : refNode.enumMembers) {
                            if (m.second == v) { memberName = m.first; break; }
                        }
                        if (!memberName.isEmpty()) {
                            QString chipText = QStringLiteral("(") + memberName
                                             + QStringLiteral(")");
                            pushChip(ChipKind::Enum, chipText, [&](LineChip& c) {
                                c.enumCurrentValue = v;
                                c.enumRefNodeId    = node.refId;
                            });
                        }
                    }
                }
            }

            // (TypeHint chips were removed — the inline "[ptr64]" /
            // "[int32_t×2]" annotations on hex rows competed with the
            // actual values for visual attention and added noise without
            // earning their keep. Inference still drives the right-click
            // "Convert to inferred type" suggestion path, just not the
            // inline chip.)

            // 3. RTTI / Symbol — annotation chips.
            //    Priority (per user: "RTTI always more accurate"):
            //      1. RTTI resolved  → overlay chip with demangled name only,
            //                          NO "Reclass.exe+0x..." symbol suffix.
            //                          Click → create class of that name.
            //      2. Pointer with value 0 → overlay "(Name class…)" CTA chip.
            //                          Click → rename current tab's root struct.
            //                          Only on Pointer32/Pointer64 (not Hex64)
            //                          to keep raw uninitialized hex rows quiet.
            //      3. Symbol only    → inline Symbol chip (legacy, addr→name
            //                          via PDB). Falls back when RTTI fails.
            {
                QString rttiName;
                uint64_t rttiVtable = 0;
                bool isNullPointer = false;
                if (state.showRtti
                    && (node.kind == NodeKind::Hex64 || node.kind == NodeKind::Pointer64)
                    && prov.isReadable(absAddr, 8)) {
                    uint64_t candidate = prov.readU64(absAddr);
                    if (candidate == 0
                        && (node.kind == NodeKind::Pointer64
                            || node.kind == NodeKind::Pointer32)) {
                        // Null vtable slot — chip becomes a "name this class"
                        // call-to-action. Excluded for raw Hex64 because every
                        // zero-byte row would otherwise sprout a chip.
                        isNullPointer = true;
                    } else if (candidate != 0 && candidate != UINT64_MAX) {
                        const RttiInfo& info = rttiForVtable(state, prov, candidate);
                        if (info.ok && !info.demangledName.isEmpty()) {
                            rttiName = info.demangledName;
                            rttiVtable = candidate;
                        }
                    }
                }

                QString ptrSym;
                if ((node.kind == NodeKind::Pointer32 || node.kind == NodeKind::Pointer64
                  || node.kind == NodeKind::FuncPtr32 || node.kind == NodeKind::FuncPtr64)
                    && prov.isReadable(absAddr, node.byteSize())) {
                    uint64_t pv = 0;
                    if (node.kind == NodeKind::Pointer64 || node.kind == NodeKind::FuncPtr64)
                        pv = prov.readU64(absAddr);
                    else
                        pv = (uint64_t)prov.readU32(absAddr);
                    if (pv != 0) ptrSym = prov.getSymbol(pv);
                }

                if (!rttiName.isEmpty()) {
                    // Plain demangled name, inline. Indicator pass styles
                    // it as a clickable pill. Symbol suppressed when RTTI
                    // resolves (annotation merge — RTTI supersedes the
                    // .exe+0x... fallback).
                    pushChip(ChipKind::Rtti, rttiName, [&](LineChip& c) {
                        c.rttiVtableAddr = rttiVtable;
                    });
                } else if (isNullPointer) {
                    pushChip(ChipKind::Rtti, QStringLiteral("(Name class…)"),
                             [](LineChip& c) { c.rttiVtableAddr = 0; });
                } else if (!ptrSym.isEmpty()) {
                    pushChip(ChipKind::Symbol, ptrSym);
                }
            }

            // 3b. TypeHint — type-inference annotation on hex preview nodes.
            //     Click target is wired to convert the field to the inferred
            //     type (handled in MainWindow). Only fires when inference
            //     confidence is strong (strength >= 3), so stable zero-byte
            //     runs and randomness don't bait users into wrong conversions.
            if (isHexNode(node.kind)) {
                const int sz = sizeForKind(node.kind);
                QByteArray b = prov.isReadable(absAddr, sz)
                    ? prov.readBytes(absAddr, sz) : QByteArray(sz, '\0');
                auto suggestions = inferTypes(
                    reinterpret_cast<const uint8_t*>(b.constData()), sz);
                if (!suggestions.isEmpty() && suggestions[0].strength >= 3) {
                    QString hint = formatHint(suggestions[0]);
                    QVector<NodeKind> kinds = suggestions[0].kinds;
                    pushChip(ChipKind::TypeHint, hint, [&](LineChip& c) {
                        c.typeHintKinds = kinds;
                    });
                }
            }

            // 4. Comment — user-authored Node::comment, falls back to
            //    state.symbolLookup() for the field's OWN address (not
            //    the pointer's value — that's the Symbol chip above).
            if (state.showComments) {
                QString commentText;
                if (!node.comment.isEmpty())
                    commentText = node.comment;
                else if (state.symbolLookup) {
                    QString sym = state.symbolLookup(absAddr);
                    if (!sym.isEmpty())
                        commentText = sym;
                }
                if (!commentText.isEmpty()) {
                    // Defensive: collapse embedded \r/\n/\t so the chip text
                    // stays on one Scintilla row. Without this, a stored
                    // multi-line comment turns into phantom rows with no
                    // LineMeta backing them.
                    if (commentText.contains(QChar('\n'))
                        || commentText.contains(QChar('\r'))
                        || commentText.contains(QChar('\t'))) {
                        commentText.replace(QChar('\r'), QChar(' '));
                        commentText.replace(QChar('\t'), QChar(' '));
                        commentText.replace(QChar('\n'), QStringLiteral(" · "));
                        static const QRegularExpression rxRun(QStringLiteral("  +"));
                        commentText.replace(rxRun, QStringLiteral(" "));
                    }
                    // Comment chip is a green pill — no glyph prefix,
                    // the chip pill + text color already mark it as a
                    // user comment. (The earlier "/ " marker doubled the
                    // signal and looked awkward next to the cleaner
                    // [TypeHint] / {RTTI} / (Enum) glyph chips.)
                    pushChip(ChipKind::Comment, commentText);
                }
            }
        }

        state.emitLine(lineText, std::move(lm));
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
        state.emitLine(fmt::indent(depth) + QStringLiteral("\u21BB ") +
                       node.name + QStringLiteral("  (circular reference)"), std::move(lm));
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
        lm.markerMask = (1u << M_STRUCT_BG);
        lm.arrayElementIdx = arrayElementIdx;
        uint64_t relOff = absAddr - arrayContainerAddr;
        QString relOffHex = QString::number(relOff, 16).toUpper();
        state.emitLine(fmt::indent(depth) + QStringLiteral("[%1] +0x%2").arg(arrayElementIdx).arg(relOffHex), std::move(lm));
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
        // Brace wrapping: move trailing '{' to its own line
        if (state.braceWrap && !node.collapsed && headerText.endsWith(QChar('{'))) {
            headerText.chop(1);
            // Remove trailing separator spaces
            while (headerText.endsWith(' ')) headerText.chop(1);
            state.emitLine(headerText, std::move(lm));
            // Emit standalone brace line (Footer lineKind so tree connectors align with closing })
            LineMeta braceLm;
            braceLm.nodeIdx   = nodeIdx;
            braceLm.nodeId    = node.id;
            braceLm.depth     = depth;
            braceLm.lineKind  = LineKind::Footer;
            braceLm.foldLevel = computeFoldLevel(depth, true);
            braceLm.markerMask = (1u << M_STRUCT_BG);
            state.emitLine(fmt::indent(depth) + QStringLiteral("{"), std::move(braceLm));
        } else {
            state.emitLine(headerText, std::move(lm));
        }
    }

    if (!node.collapsed || isArrayChild || isRootHeader) {
        // Enum with members: render name = value lines instead of offset-based fields
        if (node.isEnum() && !node.enumMembers.isEmpty()) {
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
                state.setTreeSibling(childDepth, oi < order.size() - 1);
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
                state.emitLine(fmt::fmtEnumMember(m.first, m.second, childDepth, maxNameLen), std::move(lm));
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
                state.emitLine(fmt::fmtStructFooter(node, depth, 0), std::move(lm));
            }

            state.visiting.remove(node.id);
            return;
        }

        // Bitfield with members: render name : width = value lines
        if (node.isBitfield()
            && !node.bitfieldMembers.isEmpty()) {
            int childDepth = depth + 1;
            int maxNameLen = 4;
            for (const auto& m : node.bitfieldMembers)
                maxNameLen = qMax(maxNameLen, (int)m.name.size());

            for (int mi = 0; mi < node.bitfieldMembers.size(); mi++) {
                state.setTreeSibling(childDepth, mi < node.bitfieldMembers.size() - 1);
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
                                                       childDepth, maxNameLen), std::move(lm));
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
                state.emitLine(fmt::fmtStructFooter(node, depth, sz), std::move(lm));
            }

            state.visiting.remove(node.id);
            return;
        }

        const QVector<int>& allChildren = childIndices(state, node.id);
        const QVector<int>& regular     = allChildren;

        int childDepth = depth + 1;

        // Primitive arrays with no child nodes: synthesize element lines dynamically
        if (node.kind == NodeKind::Array && regular.isEmpty()
            && node.elementKind != NodeKind::Struct && node.elementKind != NodeKind::Array) {
            int elemSize = sizeForKind(node.elementKind);
            int eTW = state.effectiveTypeW(node.id);
            int eNW = state.effectiveNameW(node.id);
            for (int i = 0; i < node.arrayLen; i++) {
                state.setTreeSibling(childDepth, i < node.arrayLen - 1);
                uint64_t elemAddr = absAddr + (uint64_t)i * elemSize;

                // Type override: "float[0]", "uint32_t[1]", etc.
                QString elemTypeStr = fmt::typeNameRaw(node.elementKind)
                                    + QStringLiteral("[%1]").arg(i);

                Node elem;
                elem.kind = node.elementKind;
                elem.name = QString();  // no name for array elements
                elem.offset = node.offset + (int)((uint64_t)i * elemSize);
                elem.parentId = node.id;
                elem.id = 0;

                LineMeta lm;
                lm.nodeIdx    = nodeIdx;
                lm.nodeId     = node.id;
                lm.depth      = childDepth;
                lm.lineKind   = LineKind::Field;
                lm.nodeKind   = node.elementKind;
                lm.isArrayElement = true;
                lm.parentAddr = absAddr;
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
                                                state.compactColumns), std::move(lm));
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
                    state.setTreeSibling(childDepth, i < node.arrayLen - 1);
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
                for (int rci = 0; rci < refChildren.size(); rci++) {
                    int childIdx = refChildren[rci];
                    state.setTreeSibling(childDepth, rci < refChildren.size() - 1);
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
                            /*collapsed=*/true, typeW, nameW, state.compactColumns), std::move(lm));
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
        for (int ri = 0; ri < regular.size(); ri++) {
            int childIdx = regular[ri];
            bool hasMore = (ri < regular.size() - 1);
            state.setTreeSibling(childDepth, hasMore);
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
        lm.offsetText = fmt::fmtOffsetMargin(absAddr + sz, false, state.offsetHexDigits);
        lm.offsetAddr = absAddr + sz;
        lm.ptrBase    = state.currentPtrBase;
        state.emitLine(fmt::fmtStructFooter(node, depth, sz), std::move(lm));
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
        QString stars = QString(node.ptrDepth + 1, QChar('*'));
        QString ptrTypeOverride = (ptrTargetName.isEmpty() ? QStringLiteral("void") : ptrTargetName) + stars;
        if (node.isRelative)
            ptrTypeOverride += QStringLiteral(" rva");

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
            {
                QString ptrText = fmt::fmtPointerHeader(node, depth, effectiveCollapsed,
                                                         prov, absAddr, ptrTypeOverride,
                                                         typeW, nameW, state.compactColumns);
                // RTTI hint on typed pointer headers: the pointer's value
                // is *the vtable address itself*, not a struct/data pointer.
                // composeLeaf's RTTI block doesn't see this case (typed
                // pointers route through composeNode), so we duplicate the
                // detect-and-attach here. Same per-pass cache, same amber
                // indicator, same `{RTTI: …}` text.
                // Local helper so the typed-pointer header path can push
                // chips with the same documentColumn math used in
                // composeLeaf. Each chip records its [startCol, endCol)
                // in document-column space (post fold-prefix).
                // Defensive sanitizer (mirror of composeLeaf's pushChip):
                // every chip is single-line, period.
                auto sanitizeChip = [](QString s) -> QString {
                    if (s.contains(QChar('\n')) || s.contains(QChar('\r'))
                        || s.contains(QChar('\t'))) {
                        s.replace(QChar('\r'), QChar(' '));
                        s.replace(QChar('\t'), QChar(' '));
                        s.replace(QChar('\n'), QStringLiteral(" · "));
                        static const QRegularExpression rxRun(QStringLiteral("  +"));
                        s.replace(rxRun, QStringLiteral(" "));
                    }
                    return s;
                };
                auto pushPtrChip = [&](ChipKind kind, const QString& rawText,
                                       auto&& fillPayload) {
                    QString chipText = sanitizeChip(rawText);
                    LineChip chip;
                    chip.kind = kind;
                    chip.text = chipText;
                    // Trim padding, append "  " + chip text. Indicator
                    // pass styles it as a clickable pill.
                    while (ptrText.endsWith(QLatin1Char(' '))) ptrText.chop(1);
                    ptrText += QStringLiteral("  ") + chipText;
                    chip.startCol = LineGeometry::forLine(lm)
                        .documentColumn(ptrText.size() - chipText.size());
                    chip.endCol   = LineGeometry::forLine(lm)
                        .documentColumn(ptrText.size());
                    fillPayload(chip);
                    lm.chips.push_back(std::move(chip));
                };

                // RTTI / Symbol — same priority ladder as composeLeaf:
                //   1. RTTI resolved → overlay chip, demangled name only,
                //      Symbol suppressed (RTTI supersedes).
                //   2. Pointer value 0 → overlay "(Name class…)" CTA.
                //   3. Symbol only → inline chip.
                QString rttiName;
                uint64_t rttiVtable = 0;
                QString ptrSym;
                bool isNullPointer = false;
                if (prov.isReadable(absAddr, 8)) {
                    uint64_t candidate = prov.readU64(absAddr);
                    if (candidate == 0) {
                        if (state.showRtti) isNullPointer = true;
                    } else if (candidate != UINT64_MAX) {
                        if (state.showRtti) {
                            const RttiInfo& info = rttiForVtable(state, prov, candidate);
                            if (info.ok && !info.demangledName.isEmpty()) {
                                rttiName = info.demangledName;
                                rttiVtable = candidate;
                            }
                        }
                        ptrSym = prov.getSymbol(candidate);
                    }
                }
                if (!rttiName.isEmpty()) {
                    pushPtrChip(ChipKind::Rtti, rttiName, [&](LineChip& c) {
                        c.rttiVtableAddr = rttiVtable;
                    });
                } else if (isNullPointer) {
                    pushPtrChip(ChipKind::Rtti, QStringLiteral("(Name class…)"),
                                [](LineChip& c) { c.rttiVtableAddr = 0; });
                } else if (!ptrSym.isEmpty()) {
                    pushPtrChip(ChipKind::Symbol, ptrSym, [](LineChip&) {});
                }

                // Comment chip on typed-pointer header. composeLeaf
                // already emits this for raw pointer fields — without
                // it here, user-authored Node::comment is invisible on
                // typed pointers (the live demo's __vptr row showed
                // the RTTI + Symbol chips fine but the "Qt vtable
                // pointer …" comment was just dropped).
                if (state.showComments && !node.comment.isEmpty()) {
                    QString commentText = node.comment;
                    if (commentText.contains(QChar('\n'))
                        || commentText.contains(QChar('\r'))
                        || commentText.contains(QChar('\t'))) {
                        commentText.replace(QChar('\r'), QChar(' '));
                        commentText.replace(QChar('\t'), QChar(' '));
                        commentText.replace(QChar('\n'), QStringLiteral(" · "));
                        static const QRegularExpression rxRun(QStringLiteral("  +"));
                        commentText.replace(rxRun, QStringLiteral(" "));
                    }
                    pushPtrChip(ChipKind::Comment, commentText,
                                [](LineChip&) {});
                }
                if (state.braceWrap && !effectiveCollapsed && ptrText.endsWith(QChar('{'))) {
                    ptrText.chop(1);
                    while (ptrText.endsWith(' ')) ptrText.chop(1);
                    state.emitLine(ptrText, std::move(lm));
                    LineMeta braceLm;
                    braceLm.nodeIdx   = nodeIdx;
                    braceLm.nodeId    = node.id;
                    braceLm.depth     = depth;
                    braceLm.lineKind  = LineKind::Footer;  // tree connectors align with closing }
                    braceLm.foldLevel = computeFoldLevel(depth, true);
                    braceLm.markerMask = lm.markerMask;
                    state.emitLine(fmt::indent(depth) + QStringLiteral("{"), std::move(braceLm));
                } else {
                    state.emitLine(ptrText, std::move(lm));
                }
            }
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

            // Relative pointer (RVA): target = tree.baseAddress + value.
            // Matches PE/COFF/ELF RVA convention — values are offsets
            // from the document's base (imageBase when attached at
            // module load), NOT from the recursion-time parent base.
            // Using `base` (the recursion arg) here made top-level
            // RVA pointers resolve to literal `value` since base=0 at
            // root, which read e.g. NT_HEADERS at 0x78 instead of
            // imageBase+0x78.
            if (node.isRelative && ptrVal != 0)
                ptrVal += tree.baseAddress;

            // Follow extra indirection levels for ** struct pointers
            // ptrDepth=0 → single deref (normal *), ptrDepth=1 → double deref (**)
            for (int d = 0; d < node.ptrDepth && ptrVal != 0; d++) {
                bool is64 = (node.kind == NodeKind::Pointer64);
                int psz = is64 ? 8 : 4;
                if (!prov.isReadable(ptrVal, psz)) { ptrVal = 0; break; }
                ptrVal = is64 ? prov.readU64(ptrVal)
                              : (uint64_t)prov.readU32(ptrVal);
                if (ptrVal == UINT64_MAX || (!is64 && ptrVal == 0xFFFFFFFF))
                    ptrVal = 0;
            }

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
                for (int pci = 0; pci < ptrChildren.size(); pci++) {
                    state.setTreeSibling(depth + 1, pci < ptrChildren.size() - 1);
                    composeNode(state, tree, childProv, ptrChildren[pci], depth + 1,
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
                state.emitLine(fmt::indent(depth) + QStringLiteral("}"), std::move(lm));
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
                      bool compactColumns, bool treeLines, bool braceWrap,
                      bool typeHints, bool showComments,
                      SymbolLookupFn symbolLookup,
                      bool showRtti, bool showEnumChips) {
    PROFILE_SCOPE("compose");
    ComposeState state;
    state.compactColumns = compactColumns;
    state.treeLines = treeLines;
    state.braceWrap = braceWrap;
    state.typeHints = typeHints;
    state.showComments = showComments;
    state.showRtti = showRtti;
    state.showEnumChips = showEnumChips;
    state.symbolLookup = std::move(symbolLookup);

    // Precompute parent→children map
    for (int i = 0; i < tree.nodes.size(); i++)
        state.childMap[tree.nodes[i].parentId].append(i);

    // Children are sorted lazily on first access in childIndices()

    // Pre-allocate output buffers (estimate ~3 lines per node, ~80 chars per line)
    state.meta.reserve(tree.nodes.size() * 3);
    state.text.reserve(tree.nodes.size() * 80);

    // Precompute absolute offsets via BFS (O(N) — avoids per-node parent-chain walk).
    // Treats any node with a missing parentId as a root so orphans don't silently
    // land at offset 0 (which would compose them on top of real root structs).
    {
        PROFILE_SCOPE("compose.absOffsets-BFS");
        state.absOffsets.resize(tree.nodes.size());
        state.absOffsets.fill(0);
        QVector<bool> visited(tree.nodes.size(), false);
        for (int i = 0; i < tree.nodes.size(); i++)
            if (tree.nodes[i].parentId == 0)
                state.absOffsets[i] = tree.nodes[i].offset;
        {
            QVector<int> bfsQueue;
            for (int i : state.childMap.value(0)) {
                bfsQueue.append(i);
                visited[i] = true;
            }
            int front = 0;
            while (front < bfsQueue.size()) {
                int idx = bfsQueue[front++];
                int pi = tree.indexOfId(tree.nodes[idx].parentId);
                state.absOffsets[idx] = (pi >= 0 ? state.absOffsets[pi] : 0)
                                      + tree.nodes[idx].offset;
                for (int ci : state.childMap.value(tree.nodes[idx].id)) {
                    if (!visited[ci]) { visited[ci] = true; bfsQueue.append(ci); }
                }
            }
            // Any node still unvisited is either an orphan (parentId points to
            // nothing) or part of a parent-chain that doesn't reach root.
            // Treat as a top-level root so its offset isn't garbage 0.
            for (int i = 0; i < tree.nodes.size(); i++) {
                if (!visited[i]) {
                    state.absOffsets[i] = tree.nodes[i].offset;
                    visited[i] = true;
                    // Walk descendants to fix their offsets too
                    QVector<int> q; q.append(i);
                    while (!q.isEmpty()) {
                        int p = q.takeLast();
                        for (int ci : state.childMap.value(tree.nodes[p].id)) {
                            if (visited[ci]) continue;
                            visited[ci] = true;
                            state.absOffsets[ci] = state.absOffsets[p] + tree.nodes[ci].offset;
                            q.append(ci);
                        }
                    }
                }
            }
        }
        for (auto& v : state.absOffsets)
            v += tree.baseAddress;
    }

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

    // Pre-compute type name lengths (avoids re-creating temp QStrings in width loops)
    QVector<int> typeNameLens(tree.nodes.size());
    {
    PROFILE_SCOPE("compose.widths");
    for (int i = 0; i < tree.nodes.size(); i++)
        typeNameLens[i] = nodeTypeName(tree.nodes[i]).size();

    // Compute effective column widths from longest type/name in a single pass
    const int typeCap = state.compactColumns ? kCompactTypeW : kMaxTypeW;
    int maxTypeLen = kMinTypeW;
    int maxNameLen = kMinNameW;
    for (int i = 0; i < tree.nodes.size(); i++) {
        maxTypeLen = qMax(maxTypeLen, typeNameLens[i]);
        // Include ALL nodes (even hex) so column width stays stable when cycling types
        maxNameLen = qMax(maxNameLen, (int)tree.nodes[i].name.size());
    }
    state.typeW = qBound(kMinTypeW, maxTypeLen, typeCap);
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
            // Skip struct children — pointer headers shouldn't inflate sibling widths
            if (child.kind == NodeKind::Struct)
                continue;
            scopeMaxType = qMax(scopeMaxType, typeNameLens[childIdx]);

            // Name width: include ALL nodes so width stays stable when cycling types
            scopeMaxName = qMax(scopeMaxName, (int)child.name.size());
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
    {
        int rootMaxType = kMinTypeW;
        int rootMaxName = kMinNameW;
        for (int childIdx : state.childMap.value(0)) {
            const Node& child = tree.nodes[childIdx];
            // Skip struct children — pointer headers shouldn't inflate sibling widths
            if (child.kind == NodeKind::Struct)
                continue;
            rootMaxType = qMax(rootMaxType, typeNameLens[childIdx]);

            // Name width: include ALL nodes so width stays stable when cycling types
            rootMaxName = qMax(rootMaxName, (int)child.name.size());
        }
        state.scopeTypeW[0] = qBound(kMinTypeW, rootMaxType, typeCap);
        state.scopeNameW[0] = qBound(kMinNameW, rootMaxName, kMaxNameW);
    }
    }  // end PROFILE_SCOPE("compose.widths")

    // Emit CommandRow as line 0 (combined: source + address + root class type + name)
    // Placeholder shown only until controller.updateCommandRow rewrites
    // it with the real source / base address / class name. "Untitled"
    // matches MainWindow::rootName's empty-tree fallback at main.cpp:2554.
    const QString cmdRowText = QStringLiteral("[\u25B8] source\u25BE  0x0  struct Untitled {");
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
        state.emitLine(cmdRowText, std::move(lm));
    }

    // Brace wrapping: emit standalone "{" after CommandRow
    // isRootHeader makes it flush left (no fold prefix), matching the root "};' footer
    if (state.braceWrap) {
        LineMeta braceLm;
        braceLm.nodeIdx   = -1;
        braceLm.nodeId    = 0;  // not associated with any node (no hover)
        braceLm.depth     = 0;
        braceLm.lineKind  = LineKind::Footer;
        braceLm.isRootHeader = true;  // flush left — same as root footer
        braceLm.foldLevel = SC_FOLDLEVELBASE;
        braceLm.markerMask = 0;
        state.emitLine(QStringLiteral("{"), std::move(braceLm));
    }

    const QVector<int>& roots = childIndices(state, 0);

    {
        PROFILE_SCOPE("compose.walk-nodes");
        for (int idx : roots) {
            // If viewRootId is set, skip roots that don't match
            if (viewRootId != 0 && tree.nodes[idx].id != viewRootId)
                continue;
            composeNode(state, tree, prov, idx, 0);
        }
    }

    ComposeResult cr;
    cr.text       = std::move(state.text);
    cr.meta       = std::move(state.meta);
    cr.layout     = LayoutInfo{state.typeW, state.nameW, state.offsetHexDigits, tree.baseAddress, treeLines};
    cr.maxLineLen = state.maxLineLen;
    cr.lineStarts = std::move(state.lineStarts);
    return cr;
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
