#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QHash>
#include <QSet>
#include <cstdint>
#include <array>
#include <memory>
#include <variant>
#include <QDateTime>

#include "providers/provider.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"

namespace rcx {

// ── Node kind enum ──

enum class NodeKind : uint8_t {
    Hex8, Hex16, Hex32, Hex64, Hex128,
    Int8, Int16, Int32, Int64, Int128,
    UInt8, UInt16, UInt32, UInt64, UInt128,
    Float16, Float, Double, Bool,
    Pointer32, Pointer64,
    FuncPtr32, FuncPtr64,
    Vec2, Vec3, Vec4, Mat4x4,
    UTF8, UTF16,
    Struct, Array
};

} // namespace rcx (temporarily close for qHash)
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
inline uint qHash(rcx::NodeKind key, uint seed = 0) { return qHash(static_cast<int>(key), seed); }
#endif
namespace rcx { // reopen

// ── Kind flags (replaces repeated Hex switches) ──

enum KindFlags : uint32_t {
    KF_None       = 0,
    KF_HexPreview = 1 << 0,  // Hex8..Hex128 (ASCII+hex layout)
    KF_Container  = 1 << 1,  // Struct/Array
    KF_String     = 1 << 2,  // UTF8/UTF16
    KF_Vector     = 1 << 3,  // Vec2/3/4
};

// ── Unified kind metadata table (single source of truth) ──

struct KindMeta {
    NodeKind    kind;
    const char* name;      // UI/JSON name: "Hex64", "UInt16"
    const char* typeName;  // display name: "Hex64", "uint16_t"
    int         size;      // byte size (0 = dynamic: Struct/Array)
    int         lines;     // display line count
    int         align;     // natural alignment
    uint32_t    flags;     // KindFlags bitmask
};

inline constexpr KindMeta kKindMeta[] = {
    // kind                name         typeName      sz  ln  al  flags
    {NodeKind::Hex8,      "Hex8",      "hex8",        1,  1,  1, KF_HexPreview},
    {NodeKind::Hex16,     "Hex16",     "hex16",       2,  1,  2, KF_HexPreview},
    {NodeKind::Hex32,     "Hex32",     "hex32",       4,  1,  4, KF_HexPreview},
    {NodeKind::Hex64,     "Hex64",     "hex64",       8,  1,  8, KF_HexPreview},
    {NodeKind::Hex128,    "Hex128",    "hex128",     16,  1, 16, KF_HexPreview},
    {NodeKind::Int8,      "Int8",      "int8_t",      1,  1,  1, KF_None},
    {NodeKind::Int16,     "Int16",     "int16_t",     2,  1,  2, KF_None},
    {NodeKind::Int32,     "Int32",     "int32_t",     4,  1,  4, KF_None},
    {NodeKind::Int64,     "Int64",     "int64_t",     8,  1,  8, KF_None},
    {NodeKind::Int128,    "Int128",    "int128_t",   16,  1,  8, KF_None},
    {NodeKind::UInt8,     "UInt8",     "uint8_t",     1,  1,  1, KF_None},
    {NodeKind::UInt16,    "UInt16",    "uint16_t",    2,  1,  2, KF_None},
    {NodeKind::UInt32,    "UInt32",    "uint32_t",    4,  1,  4, KF_None},
    {NodeKind::UInt64,    "UInt64",    "uint64_t",    8,  1,  8, KF_None},
    {NodeKind::UInt128,   "UInt128",   "uint128_t",  16,  1,  8, KF_None},
    {NodeKind::Float16,   "Float16",   "float16",     2,  1,  2, KF_None},
    {NodeKind::Float,     "Float",     "float",       4,  1,  4, KF_None},
    {NodeKind::Double,    "Double",    "double",      8,  1,  8, KF_None},
    {NodeKind::Bool,      "Bool",      "bool",        1,  1,  1, KF_None},
    {NodeKind::Pointer32, "Pointer32", "ptr32",       4,  1,  4, KF_None},
    {NodeKind::Pointer64, "Pointer64", "ptr64",       8,  1,  8, KF_None},
    {NodeKind::FuncPtr32, "FuncPtr32", "fnptr32",     4,  1,  4, KF_None},
    {NodeKind::FuncPtr64, "FuncPtr64", "fnptr64",     8,  1,  8, KF_None},
    {NodeKind::Vec2,      "Vec2",      "vec2",        8,  1,  4, KF_Vector},
    {NodeKind::Vec3,      "Vec3",      "vec3",       12,  1,  4, KF_Vector},
    {NodeKind::Vec4,      "Vec4",      "vec4",       16,  1,  4, KF_Vector},
    {NodeKind::Mat4x4,    "Mat4x4",    "mat4x4",     64,  4,  4, KF_None},
    {NodeKind::UTF8,      "UTF8",      "str",         1,  1,  1, KF_String},
    {NodeKind::UTF16,     "UTF16",     "wstr",        2,  1,  2, KF_String},
    {NodeKind::Struct,    "Struct",    "struct",      0,  1,  1, KF_Container},
    {NodeKind::Array,     "Array",     "array",       0,  1,  1, KF_Container},
};

static_assert(std::size(kKindMeta) == static_cast<size_t>(NodeKind::Array) + 1,
              "kKindMeta table must match NodeKind enum");

inline constexpr const KindMeta* kindMeta(NodeKind k) {
    auto i = static_cast<unsigned>(k);
    if (i < std::size(kKindMeta)) return &kKindMeta[i];
    return nullptr;
}

inline constexpr int sizeForKind(NodeKind k)  { auto* m = kindMeta(k); return m ? m->size  : 0; }
inline constexpr int linesForKind(NodeKind k)  { auto* m = kindMeta(k); return m ? m->lines : 1; }
inline constexpr int alignmentFor(NodeKind k)  { auto* m = kindMeta(k); return m ? m->align : 1; }

inline const char* kindToString(NodeKind k) {
    auto* m = kindMeta(k);
    return m ? m->name : "Unknown";
}

inline NodeKind kindFromString(const QString& s) {
    for (const auto& m : kKindMeta)
        if (s == m.name) return m.kind;
    return NodeKind::Hex8;
}

inline NodeKind kindFromTypeName(const QString& s, bool* ok = nullptr) {
    for (const auto& m : kKindMeta) {
        if (s == m.typeName) {
            if (ok) *ok = true;
            return m.kind;
        }
    }
    if (ok) *ok = false;
    return NodeKind::Hex8;
}

inline constexpr uint32_t flagsFor(NodeKind k) {
    const auto* m = kindMeta(k);
    return m ? m->flags : 0;
}
inline constexpr bool isHexNode(NodeKind k) {
    return k >= NodeKind::Hex8 && k <= NodeKind::Hex128;
}
inline constexpr bool isHexPreview(NodeKind k) {
    return isHexNode(k);
}
inline constexpr bool isVectorKind(NodeKind k) {
    return k == NodeKind::Vec2 || k == NodeKind::Vec3 || k == NodeKind::Vec4;
}
inline constexpr bool isMatrixKind(NodeKind k) {
    return k == NodeKind::Mat4x4;
}
inline constexpr bool isFuncPtr(NodeKind k) {
    return k == NodeKind::FuncPtr32 || k == NodeKind::FuncPtr64;
}
inline constexpr bool isPointerKind(NodeKind k) {
    return k == NodeKind::Pointer32 || k == NodeKind::Pointer64;
}
inline constexpr bool isContainerKind(NodeKind k) {
    return k == NodeKind::Struct || k == NodeKind::Array;
}
inline constexpr bool isStringKind(NodeKind k) {
    return k == NodeKind::UTF8 || k == NodeKind::UTF16;
}
// The everyday "common" primitive set shown by default in the type chooser
// (the rest are reachable via the chooser's "Show all" toggle, or by typing
// in its filter which always searches every type). Chosen from a frequency
// sweep of the bundled example .rcx files: this set covers ~99% of real
// nodes while keeping the default list short.
inline constexpr bool isCommonKind(NodeKind k) {
    switch (k) {
    case NodeKind::Hex8:  case NodeKind::Hex16: case NodeKind::Hex32:
    case NodeKind::Hex64: case NodeKind::Hex128:
    case NodeKind::UInt8: case NodeKind::UInt16:
    case NodeKind::UInt32: case NodeKind::UInt64:
    case NodeKind::Int32: case NodeKind::Int64:
    case NodeKind::Pointer64:
    case NodeKind::Float: case NodeKind::Double:
    case NodeKind::Bool:
        return true;
    default:
        return false;
    }
}
// Hex types, pointer types, function pointers, and containers are not meaningful
// primitive-pointer targets — dereferencing them produces the same output as void*.
inline constexpr bool isValidPrimitivePtrTarget(NodeKind k) {
    if (isHexNode(k)) return false;
    if (isPointerKind(k)) return false;
    if (isFuncPtr(k)) return false;
    if (k == NodeKind::Struct || k == NodeKind::Array) return false;
    return true;
}

inline QStringList allTypeNamesForUI(bool /*stripBrackets*/ = false) {
    QStringList out;
    out.reserve(std::size(kKindMeta));
    for (const auto& m : kKindMeta)
        out << QString::fromLatin1(m.typeName);
    return out;
}

// ── Marker vocabulary ──

enum Marker : int {
    M_CONT      = 0,
    M_PTR0      = 2,
    M_CYCLE     = 3,
    M_ERR       = 4,
    M_STRUCT_BG = 5,
    M_HOVER     = 6,
    M_SELECTED  = 7,
    M_CMD_ROW   = 8,
    M_ACCENT    = 9,
    M_FOCUS     = 10,  // Presentation mode: AI focus glow
};

// ── Array length clamp ──
// Also bounded by the 20-bit kArrayElemMask encoding below; kept up here so
// Node::fromJson (declared later) can reference it when clamping arrayLen.
static constexpr int kMaxArrayLen = 1000000;

// ── Bitfield member (name + bit position + width within a container) ──

struct BitfieldMember {
    QString name;
    uint8_t bitOffset = 0;  // position from LSB within the container
    uint8_t bitWidth  = 1;  // number of bits (1..64)
};

// ── Node ──

struct Node {
    uint64_t id         = 0;
    NodeKind kind       = NodeKind::Hex8;
    QString  name;
    QString  structTypeName;  // Struct/Array: optional type name (e.g., "IMAGE_DOS_HEADER")
    QString  classKeyword;    // "struct", "class", or "enum" (empty = "struct")
    uint64_t parentId   = 0;   // 0 = root (no parent)
    int      offset     = 0;
    bool     isRelative = false;   // Pointer: target = base + value (RVA) instead of absolute
    int      arrayLen   = 1;   // Array: element count
    int      strLen     = 64;
    bool     collapsed  = true;
    uint64_t refId      = 0;       // Pointer32/64: id of Struct to expand at *ptr
    NodeKind elementKind = NodeKind::UInt8;  // Array: element type; Pointer with ptrDepth>0: target type
    int      ptrDepth   = 0;   // Pointer: 0=struct/void ptr, 1=primitive*, 2=primitive**
    int      viewIndex  = 0;   // Array: current view offset (transient)
    QVector<QPair<QString, int64_t>> enumMembers; // Enum: name→value pairs
    QVector<BitfieldMember> bitfieldMembers;       // Bitfield: per-bit member definitions
    QString  comment;          // User annotation (displayed as "// text" in comment column)
    bool     bigEndian  = false;   // Scalar value is big-endian (swap on display/parse)

    // Leaf-only byte size. Returns 0 for Struct (container, unless it's a
    // bitfield which has a fixed container size) and Array-of-Struct/Array
    // because those need tree context to span child nodes. For any node that
    // might be a container, call totalByteSize(tree) instead — that dispatches
    // to tree.structSpan() automatically and returns the real footprint.
    int byteSize() const {
        switch (kind) {
        case NodeKind::UTF8:    return strLen;
        case NodeKind::UTF16:   return qMin(strLen, INT_MAX / 2) * 2;
        case NodeKind::Array: {
            int elemSz = sizeForKind(elementKind);
            if (elemSz <= 0) return 0;
            return qMin(arrayLen, INT_MAX / elemSz) * elemSz;
        }
        case NodeKind::Struct:
            if (classKeyword == QStringLiteral("bitfield")) {
                int sz = sizeForKind(elementKind);
                return sz > 0 ? sz : 4;
            }
            return 0;
        default: return sizeForKind(kind);
        }
    }

    // Container-aware byte size. Walks nested children for Struct/Array via
    // NodeTree::structSpan, falls through to byteSize() for leaves. Defined
    // out-of-line below NodeTree because it needs the full type to call
    // structSpan().
    int totalByteSize(const struct NodeTree& tree) const;

    QJsonObject toJson() const {
        QJsonObject o;
        o["id"]        = QString::number(id);
        o["kind"]      = kindToString(kind);
        o["name"]      = name;
        if (!structTypeName.isEmpty())
            o["structTypeName"] = structTypeName;
        if (!classKeyword.isEmpty() && classKeyword != QStringLiteral("struct"))
            o["classKeyword"] = classKeyword;
        o["parentId"]  = QString::number(parentId);
        o["offset"]    = offset;
        if (isRelative)
            o["isRelative"] = true;
        o["arrayLen"]  = arrayLen;
        o["strLen"]    = strLen;
        o["collapsed"] = collapsed;
        o["refId"]     = QString::number(refId);
        o["elementKind"] = kindToString(elementKind);
        if (ptrDepth > 0)
            o["ptrDepth"] = ptrDepth;
        if (!enumMembers.isEmpty()) {
            QJsonArray arr;
            for (const auto& m : enumMembers) {
                QJsonObject em;
                em["name"] = m.first;
                em["value"] = QString::number(m.second);
                arr.append(em);
            }
            o["enumMembers"] = arr;
        }
        if (!bitfieldMembers.isEmpty()) {
            QJsonArray arr;
            for (const auto& m : bitfieldMembers) {
                QJsonObject bm;
                bm["name"] = m.name;
                bm["bitOffset"] = m.bitOffset;
                bm["bitWidth"] = m.bitWidth;
                arr.append(bm);
            }
            o["bitfieldMembers"] = arr;
        }
        if (!comment.isEmpty())
            o["comment"] = comment;
        if (bigEndian)
            o["bigEndian"] = true;
        return o;
    }
    static Node fromJson(const QJsonObject& o) {
        Node n;
        n.id        = o["id"].toString("0").toULongLong();
        n.kind      = kindFromString(o["kind"].toString());
        n.name      = o["name"].toString();
        n.structTypeName = o["structTypeName"].toString();
        n.classKeyword = o["classKeyword"].toString();
        n.parentId  = o["parentId"].toString("0").toULongLong();
        n.offset    = o["offset"].toInt(0);
        n.isRelative = o["isRelative"].toBool(false);
        n.arrayLen  = qBound(1, o["arrayLen"].toInt(1), kMaxArrayLen);
        n.strLen    = qBound(1, o["strLen"].toInt(64), 1000000);
        n.collapsed = true;  // Always load collapsed; user expands as needed
        n.refId     = o["refId"].toString("0").toULongLong();
        n.elementKind = kindFromString(o["elementKind"].toString("UInt8"));
        n.ptrDepth  = qBound(0, o["ptrDepth"].toInt(0), 2);
        if (o.contains("enumMembers")) {
            QJsonArray arr = o["enumMembers"].toArray();
            for (const auto& v : arr) {
                QJsonObject em = v.toObject();
                n.enumMembers.emplaceBack(em["name"].toString(),
                                        em["value"].toString("0").toLongLong());
            }
        }
        if (o.contains("bitfieldMembers")) {
            QJsonArray arr = o["bitfieldMembers"].toArray();
            for (const auto& v : arr) {
                QJsonObject bm = v.toObject();
                BitfieldMember m;
                m.name = bm["name"].toString();
                m.bitOffset = (uint8_t)qBound(0, bm["bitOffset"].toInt(0), 255);
                m.bitWidth = (uint8_t)qBound(1, bm["bitWidth"].toInt(1), 64);
                n.bitfieldMembers.append(m);
            }
        }
        n.comment = o["comment"].toString();
        n.bigEndian = o["bigEndian"].toBool(false);
        return n;
    }

    // Resolved class keyword (never empty)
    QString resolvedClassKeyword() const {
        return classKeyword.isEmpty() ? QStringLiteral("struct") : classKeyword;
    }
    bool isUnion() const { return resolvedClassKeyword() == QStringLiteral("union"); }
    bool isBitfield() const { return classKeyword == QStringLiteral("bitfield"); }
    bool isEnum() const { return resolvedClassKeyword() == QStringLiteral("enum"); }
};

// ── Bookmark (named address, persists with the project) ──

struct Bookmark {
    QString  name;
    QString  addressFormula;  // e.g. "<game.exe>+0x12340" — survives rebases
    QJsonObject toJson() const {
        QJsonObject o;
        o["name"] = name;
        o["addressFormula"] = addressFormula;
        return o;
    }
    static Bookmark fromJson(const QJsonObject& o) {
        Bookmark b;
        b.name = o["name"].toString();
        b.addressFormula = o["addressFormula"].toString();
        return b;
    }
};

class Provider;

// RTTI discovery hook — fired by compose.cpp every time walkRtti() turns
// up a new vtable. Defined in compose.cpp. The GUI app (main.cpp) wires
// this to rcx::RttiNameProvider::instance().push(...) so the discovery
// participates in the unified Symbols panel + expression parser. Test
// targets leave it nullptr (silent no-op).
extern void (*g_rttiDiscoveryHook)(const QString& name, uint64_t address,
                                    const QString& moduleName);

// Unified address→name lookup hook — main.cpp wires this to the
// NameRegistry so controller.cpp can label addresses from any registered
// source (PDB / RTTI / bookmarks / plugin). Tests leave nullptr; the
// controller falls back to SymbolStore directly when this is null.
extern QString (*g_nameLookupHook)(uint64_t address, const Provider* active);

// Named-source-changed nudge — main.cpp wires this to NameRegistry's
// emitChanged() so that "Bookmark this address..." in the editor refreshes
// the unified Symbols panel without dragging NameRegistry into the core
// (and test) targets.
extern void (*g_namesChangedHook)();

// ── NodeTree ──

struct NodeTree {
    QVector<Node> nodes;
    uint64_t      baseAddress = 0x00400000;
    QString       baseAddressFormula;  // e.g. "<ReClass.exe> + 0x100"
    int           pointerSize = 8;    // 4 for 32-bit targets, 8 for 64-bit
    // Save-file "auto-open" tag — names the class (by structTypeName, or
    // by Node.name as fallback) that should be the active view when the
    // project is loaded. Non-binding: if the class isn't found we fall
    // back to the first root struct. The tutorial / selfTest sets this
    // to "RcxEditor" plus a live baseAddress so Continue lands on the
    // demo immediately.
    QString       initialClass;
    QVector<Bookmark> bookmarks;       // user-named addresses
    uint64_t      m_nextId    = 1;
    mutable QHash<uint64_t, int> m_idCache;
    mutable QHash<uint64_t, QVector<int>> m_childCache;
    // Bumped on every structural mutation (add/remove/parent change). Caches
    // keyed on this counter (generator output, type popup entries, etc.) can
    // skip rebuilds when the tree hasn't changed shape between refreshes.
    quint64       m_generation = 1;
    quint64 generation() const { return m_generation; }
    void   bumpGeneration() { ++m_generation; }

    int addNode(const Node& n) {
        Node copy = n;
        if (copy.id == 0) copy.id = m_nextId++;
        else if (copy.id >= m_nextId) m_nextId = copy.id + 1;
        int idx = nodes.size();
        nodes.append(copy);
        if (!m_idCache.isEmpty())
            m_idCache[copy.id] = idx;
        if (!m_childCache.isEmpty())
            m_childCache[copy.parentId].append(idx);
        ++m_generation;
        return idx;
    }

    // Reserve a unique ID atomically (for use before pushing undo commands)
    uint64_t reserveId() { return m_nextId++; }

    void invalidateIdCache() const { m_idCache.clear(); m_childCache.clear(); }
    // For mutators that change tree shape (parentId, offsets, kind, structTypeName,
    // refId — anything the generator/popup-cache fingerprints). Caller is responsible
    // for invoking this; controller's applyCommand path bumps where needed.
    void touch() { ++m_generation; }

    // Validate tree structure. Returns true if clean. If repair=true, fixes
    // detected issues in place (orphaned subtrees re-rooted at parentId=0,
    // cycles broken by re-rooting the deepest node in the cycle, duplicate
    // ids re-numbered). Reports counts via *out fields when provided.
    struct ValidateReport {
        int orphans   = 0;  // nodes whose parentId doesn't exist
        int cycles    = 0;  // cycles broken
        int duplicates = 0; // duplicate ids re-numbered
        QString summary() const {
            return QStringLiteral("orphans=%1 cycles=%2 duplicates=%3")
                .arg(orphans).arg(cycles).arg(duplicates);
        }
        bool clean() const { return orphans == 0 && cycles == 0 && duplicates == 0; }
    };
    ValidateReport validate(bool repair = true) {
        ValidateReport r;
        if (nodes.isEmpty()) return r;

        // Pass 1: dedupe ids
        {
            QHash<uint64_t, int> seen;
            for (int i = 0; i < nodes.size(); i++) {
                Node& n = nodes[i];
                if (n.id == 0 || seen.contains(n.id)) {
                    r.duplicates++;
                    if (repair) n.id = m_nextId++;
                }
                seen.insert(n.id, i);
                if (n.id >= m_nextId) m_nextId = n.id + 1;
            }
        }
        invalidateIdCache();

        // Pass 2: orphan detection — parentId points to nonexistent node
        for (int i = 0; i < nodes.size(); i++) {
            Node& n = nodes[i];
            if (n.parentId != 0 && indexOfId(n.parentId) < 0) {
                r.orphans++;
                if (repair) n.parentId = 0;
            }
        }
        invalidateIdCache();

        // Pass 3: cycle detection — walk parent chain from each node
        for (int i = 0; i < nodes.size(); i++) {
            QSet<uint64_t> visited;
            int cur = i;
            while (cur >= 0 && cur < nodes.size()) {
                uint64_t nid = nodes[cur].id;
                if (visited.contains(nid)) {
                    // Cycle. Break by re-rooting the offending node.
                    r.cycles++;
                    if (repair) nodes[cur].parentId = 0;
                    break;
                }
                visited.insert(nid);
                if (nodes[cur].parentId == 0) break;
                cur = indexOfId(nodes[cur].parentId);
            }
        }
        invalidateIdCache();
        return r;
    }

    // Sibling overlap detection. Returns pairs of (nodeId, nodeId) where
    // two non-static, non-union siblings have intersecting [offset,
    // offset+span) ranges. The order within a pair is deterministic:
    // the lower-offset node first, then the offender. Unions are
    // excluded because their children DELIBERATELY overlap at offset 0;
    // static fields are excluded because their absolute-address
    // computation makes them sibling-independent. Bitfield containers
    // count their declared elementKind size, not the sum of bit widths.
    //
    // This isn't run by validate() because there's no safe auto-repair:
    // the user has to choose which field to keep / shrink. Surfacing
    // the result is the editor's job (margin warning glyph, follow-up).
    struct OverlapPair {
        uint64_t aId;       // lower-offset sibling
        uint64_t bId;       // overlapping sibling
        uint64_t parentId;  // common parent
    };
    QVector<OverlapPair> findOverlaps() const {
        QVector<OverlapPair> out;
        if (nodes.isEmpty()) return out;

        // Build child map once. childrenOf() lazily fills m_childCache;
        // we want the same data without the mutable cache side effect
        // (this is a const method).
        QHash<uint64_t, QVector<int>> childMap;
        for (int i = 0; i < nodes.size(); ++i)
            childMap[nodes[i].parentId].append(i);

        for (auto it = childMap.constBegin(); it != childMap.constEnd(); ++it) {
            uint64_t parentId = it.key();
            // Root-level structs (parentId == 0) aren't real siblings —
            // they're independent top-level classes that happen to share
            // the synthetic "0" parent. Their offsets are arbitrary and
            // reporting them as overlapping would be noise.
            if (parentId == 0) continue;
            // Skip union containers entirely — overlapping children at
            // offset 0 is the whole point of unions.
            int pi = indexOfId(parentId);
            if (pi >= 0 && nodes[pi].isUnion()) continue;

            // Collect non-static siblings with their [start, end) range.
            // Sort by start so a single forward pass catches every overlap.
            struct Range { int idx; int64_t start; int64_t end; };
            QVector<Range> ranges;
            ranges.reserve(it.value().size());
            for (int ci : it.value()) {
                const Node& n = nodes[ci];
                int sz = (n.kind == NodeKind::Struct || n.kind == NodeKind::Array)
                    ? structSpan(n.id) : n.byteSize();
                // Zero-sized nodes (unfinished containers etc.) can't
                // overlap with anything; treat them as points and skip.
                if (sz <= 0) continue;
                ranges.push_back({ci, (int64_t)n.offset, (int64_t)n.offset + sz});
            }
            std::sort(ranges.begin(), ranges.end(),
                [](const Range& a, const Range& b) { return a.start < b.start; });

            // Walk forward — for each range, every later range whose
            // start < this end is an overlap. We track the maximum end
            // seen so far so a long range that swallows several shorter
            // ones still reports each pair.
            for (int i = 0; i + 1 < ranges.size(); ++i) {
                for (int j = i + 1; j < ranges.size(); ++j) {
                    if (ranges[j].start >= ranges[i].end) break;
                    OverlapPair p;
                    p.aId = nodes[ranges[i].idx].id;
                    p.bId = nodes[ranges[j].idx].id;
                    p.parentId = parentId;
                    out.append(p);
                }
            }
        }
        return out;
    }

    int indexOfId(uint64_t id) const {
        if (m_idCache.isEmpty() && !nodes.isEmpty()) {
            for (int i = 0; i < nodes.size(); i++)
                m_idCache[nodes[i].id] = i;
        }
        return m_idCache.value(id, -1);
    }

    QVector<int> childrenOf(uint64_t parentId) const {
        if (m_childCache.isEmpty() && !nodes.isEmpty()) {
            for (int i = 0; i < nodes.size(); i++)
                m_childCache[nodes[i].parentId].append(i);
        }
        return m_childCache.value(parentId);
    }

    // Inverse of fieldPath: resolve a dot-path back to a node id. Returns 0
    // on miss. First segment must match a top-level struct's structTypeName
    // or name; subsequent segments match children by name. Matching is
    // exact (case-sensitive). Empty paths and unknown segments return 0
    // without partial matches — callers can decide whether to retry.
    uint64_t nodeIdForPath(const QString& path,
                           QChar sep = QLatin1Char('.')) const {
        if (path.isEmpty() || nodes.isEmpty()) return 0;
        QStringList segs = path.split(sep, Qt::SkipEmptyParts);
        if (segs.isEmpty()) return 0;

        // First segment matches a top-level struct (parentId == 0).
        int cur = -1;
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes[i].parentId != 0) continue;
            const Node& n = nodes[i];
            if (n.structTypeName == segs[0] || n.name == segs[0]) {
                cur = i;
                break;
            }
        }
        if (cur < 0) return 0;

        // Remaining segments — walk children by exact name match.
        for (int s = 1; s < segs.size(); ++s) {
            uint64_t parentId = nodes[cur].id;
            int next = -1;
            for (int i = 0; i < nodes.size(); ++i) {
                if (nodes[i].parentId != parentId) continue;
                if (nodes[i].name == segs[s]) { next = i; break; }
            }
            if (next < 0) return 0;
            cur = next;
        }
        return nodes[cur].id;
    }

    // Dot-separated path from root struct to this node:
    // "Player.Stats.Health". Used by Copy Path and by external tooling
    // (MCP) to identify a node textually. Walks parentId chain; bails on
    // cycles or missing nodes with a best-effort prefix. The top-level
    // class is included if it has a structTypeName, omitted otherwise so
    // an anonymous root doesn't produce a leading dot.
    QString fieldPath(uint64_t id, QChar sep = QLatin1Char('.')) const {
        QStringList parts;
        QSet<uint64_t> seen;
        uint64_t cur = id;
        while (cur != 0 && !seen.contains(cur)) {
            seen.insert(cur);
            int idx = indexOfId(cur);
            if (idx < 0) break;
            const Node& n = nodes[idx];
            QString label = !n.name.isEmpty() ? n.name : n.structTypeName;
            if (label.isEmpty() && n.parentId != 0) label = QStringLiteral("?");
            if (!label.isEmpty()) parts.prepend(label);
            cur = n.parentId;
        }
        return parts.join(sep);
    }

    // Collect node + all descendants (iterative, cycle-safe)
    QVector<int> subtreeIndices(uint64_t nodeId) const {
        int idx = indexOfId(nodeId);
        if (idx < 0) return {};
        // Reuse cached childMap instead of rebuilding from scratch each call
        if (m_childCache.isEmpty() && !nodes.isEmpty()) {
            for (int i = 0; i < nodes.size(); i++)
                m_childCache[nodes[i].parentId].append(i);
        }
        const auto& childMap = m_childCache;
        // BFS with visited guard
        QVector<int> result;
        QSet<uint64_t> visited;
        QVector<uint64_t> stack;
        stack.append(nodeId);
        result.append(idx);
        visited.insert(nodeId);
        while (!stack.isEmpty()) {
            uint64_t pid = stack.takeLast();
            for (int ci : childMap.value(pid)) {
                uint64_t cid = nodes[ci].id;
                if (!visited.contains(cid)) {
                    visited.insert(cid);
                    result.append(ci);
                    stack.append(cid);
                }
            }
        }
        return result;
    }

    int depthOf(int idx) const {
        int d = 0;
        QSet<uint64_t> visited;
        int cur = idx;
        while (cur >= 0 && cur < nodes.size() && nodes[cur].parentId != 0) {
            uint64_t nid = nodes[cur].id;
            if (visited.contains(nid)) break;
            visited.insert(nid);
            cur = indexOfId(nodes[cur].parentId);
            if (cur < 0) break;
            d++;
        }
        return d;
    }

    // Returns the node's offset relative to the root (summed parent offsets).
    // CAN RETURN NEGATIVE when a parent-chain walk produces a negative
    // intermediate, e.g. a malformed tree with a node carrying a negative
    // offset. Callers must check the sign before adding to baseAddress;
    // casting to uint64_t on a negative result silently wraps into
    // high addresses and reads arbitrary memory. Prefer absoluteAddress()
    // when you just need "this node's address in the live process".
    int64_t computeOffset(int idx) const {
        int64_t total = 0;
        QSet<uint64_t> visited;
        int cur = idx;
        while (cur >= 0 && cur < nodes.size()) {
            uint64_t nid = nodes[cur].id;
            if (visited.contains(nid)) break;
            visited.insert(nid);
            total += nodes[cur].offset;
            if (nodes[cur].parentId == 0) break;
            cur = indexOfId(nodes[cur].parentId);
        }
        return total;
    }

    // Convenience wrapper around computeOffset() that folds in baseAddress
    // and handles the negative case safely: if computeOffset returns < 0,
    // *ok is set to false and baseAddress alone is returned. Use this at
    // callsites that only care about "what address does this node live at".
    uint64_t absoluteAddress(int idx, bool* ok = nullptr) const {
        int64_t off = computeOffset(idx);
        if (off < 0) {
            if (ok) *ok = false;
            return baseAddress;
        }
        if (ok) *ok = true;
        return baseAddress + static_cast<uint64_t>(off);
    }

    int structSpan(uint64_t structId,
                   const QHash<uint64_t, QVector<int>>* childMap = nullptr,
                   QSet<uint64_t>* visited = nullptr) const {
        QSet<uint64_t> localVisited;
        if (!visited) visited = &localVisited;

        if (visited->contains(structId)) return 0;  // Cycle detected
        visited->insert(structId);

        int idx = indexOfId(structId);
        if (idx < 0) return 0;

        const Node& node = nodes[idx];
        int declaredSize = node.byteSize();

        // Short-circuit: non-container nodes have no children to traverse
        if (!isContainerKind(node.kind) && node.refId == 0)
            return declaredSize;

        int maxEnd = 0;
        QVector<int> kids = childMap ? childMap->value(structId) : childrenOf(structId);
        for (int ci : kids) {
            const Node& c = nodes[ci];
            int sz = (c.kind == NodeKind::Struct || c.kind == NodeKind::Array)
                ? structSpan(c.id, childMap, visited) : c.byteSize();
            int64_t end = (int64_t)c.offset + sz;
            if (end > maxEnd) maxEnd = (int)qMin(end, (int64_t)INT_MAX);
        }

        // Embedded struct reference: no own children but refId points to a struct definition
        if (kids.isEmpty() && node.kind == NodeKind::Struct && node.refId != 0)
            maxEnd = qMax(maxEnd, structSpan(node.refId, childMap, visited));

        return qMax(declaredSize, maxEnd);
    }

    // Batch selection normalizers
    QSet<uint64_t> normalizePreferAncestors(const QSet<uint64_t>& ids) const;
    QSet<uint64_t> normalizePreferDescendants(const QSet<uint64_t>& ids) const;

    QJsonObject toJson() const {
        QJsonObject o;
        o["baseAddress"] = QString::number(baseAddress, 16);
        if (!baseAddressFormula.isEmpty())
            o["baseAddressFormula"] = baseAddressFormula;
        if (!initialClass.isEmpty())
            o["initialClass"] = initialClass;
        if (pointerSize != 8)
            o["pointerSize"] = pointerSize;
        o["nextId"]      = QString::number(m_nextId);
        QJsonArray arr;
        for (const auto& n : nodes) arr.append(n.toJson());
        o["nodes"] = arr;
        if (!bookmarks.isEmpty()) {
            QJsonArray ba;
            for (const auto& b : bookmarks) ba.append(b.toJson());
            o["bookmarks"] = ba;
        }
        return o;
    }

    static NodeTree fromJson(const QJsonObject& o) {
        NodeTree t;
        t.baseAddress = o["baseAddress"].toString("400000").toULongLong(nullptr, 16);
        t.baseAddressFormula = o["baseAddressFormula"].toString();
        t.initialClass       = o["initialClass"].toString();
        t.pointerSize = o["pointerSize"].toInt(8);
        t.m_nextId    = o["nextId"].toString("1").toULongLong();
        QJsonArray arr = o["nodes"].toArray();
        t.nodes.reserve(arr.size());
        for (const auto& v : arr) {
            Node n = Node::fromJson(v.toObject());
            t.nodes.append(n);
            if (n.id >= t.m_nextId) t.m_nextId = n.id + 1;
        }
        QJsonArray ba = o["bookmarks"].toArray();
        t.bookmarks.reserve(ba.size());
        for (const auto& v : ba)
            t.bookmarks.append(Bookmark::fromJson(v.toObject()));
        return t;
    }

};

// Out-of-line because it calls NodeTree::structSpan which needs the
// complete type. Declared on Node above so callers can just write
// node.totalByteSize(tree) regardless of kind — no more "is it a
// container? oh right, use structSpan" branching at call sites.
inline int Node::totalByteSize(const NodeTree& tree) const {
    if (kind == NodeKind::Struct || kind == NodeKind::Array)
        return tree.structSpan(id);
    return byteSize();
}

// Enumerate every root struct's display name. Used by the unsaved-changes
// dialog so a multi-class document (one .rcx file with several top-level
// structs — common when you press `+` to add a new class to the existing
// project) shows ALL its class names rather than just whichever the
// active tab happens to be viewing. Returns ["Untitled"] for an empty
// tree so callers always get at least one label.
inline QStringList rootClassNames(const NodeTree& tree) {
    QStringList out;
    for (const auto& n : tree.nodes) {
        if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
        QString name = !n.structTypeName.isEmpty() ? n.structTypeName : n.name;
        if (name.isEmpty()) name = QStringLiteral("Untitled");
        if (!out.contains(name)) out.append(name);
    }
    if (out.isEmpty()) out.append(QStringLiteral("Untitled"));
    return out;
}

// ── Value History (ring buffer for heatmap) ──

struct ValueHistory {
    static constexpr int kCapacity = 10;
    std::array<QString, kCapacity> values;
    std::array<qint64, kCapacity> timestamps{};  // msec since epoch
    int count = 0;   // total unique values recorded
    int head  = 0;   // next write position in ring

    void record(const QString& v) {
        if (count > 0) {
            int last = (head + kCapacity - 1) % kCapacity;
            if (values[last] == v) return;  // no change
        }
        values[head] = v;
        timestamps[head] = QDateTime::currentMSecsSinceEpoch();
        head = (head + 1) % kCapacity;
        if (count < INT_MAX) count++;
    }

    void clear() {
        count = 0;
        head = 0;
    }

    int uniqueCount() const { return qMin(count, kCapacity); }

    // 0=static, 1=cold(2 unique), 2=warm(3-4), 3=hot(5+)
    int heatLevel() const {
        if (count <= 1) return 0;
        if (count == 2) return 1;
        if (count <= 4) return 2;
        return 3;
    }

    QString last() const {
        if (count == 0) return {};
        return values[(head + kCapacity - 1) % kCapacity];
    }

    // Iterate from oldest to newest (up to uniqueCount entries)
    template<typename Fn>
    void forEach(Fn&& fn) const {
        int n = uniqueCount();
        int start = (head + kCapacity - n) % kCapacity;
        for (int i = 0; i < n; i++)
            fn(values[(start + i) % kCapacity]);
    }

    // Iterate with timestamps from newest to oldest
    template<typename Fn>
    void forEachWithTime(Fn&& fn) const {
        int n = uniqueCount();
        for (int i = 0; i < n; i++) {
            int idx = (head + kCapacity - 1 - i) % kCapacity;
            fn(values[idx], timestamps[idx]);
        }
    }
};

// ── LineMeta ──

enum class LineKind : uint8_t {
    CommandRow,   // line 0: source + address + root class type + name
    Blank,        // (unused — kept for enum stability)
    Header, Field, Continuation, Footer, ArrayElementSeparator
};

static constexpr uint64_t kCommandRowId   = UINT64_MAX;
static constexpr int      kCommandRowLine = 0;
static constexpr int      kFirstDataLine  = 1;
static constexpr uint64_t kFooterIdBit    = 0x8000000000000000ULL;
static constexpr uint64_t kArrayElemBit   = 0x4000000000000000ULL;  // marks array element selection
static constexpr uint64_t kArrayElemShift = 42;                     // bits 42-61 hold element index
static constexpr uint64_t kArrayElemMask  = 0x3FFFFC0000000000ULL;  // 20 bits → max 1048575 elements
static_assert(kMaxArrayLen <= (1 << 20), "kMaxArrayLen must fit in kArrayElemMask (20 bits)");

// Encode an array element selection ID: nodeId | kArrayElemBit | (elemIdx << 42)
inline uint64_t makeArrayElemSelId(uint64_t nodeId, int elemIdx) {
    Q_ASSERT(elemIdx >= 0);
    return nodeId | kArrayElemBit | ((uint64_t)(elemIdx & 0xFFFFF) << kArrayElemShift);
}
inline int arrayElemIdxFromSelId(uint64_t selId) {
    return (int)((selId & kArrayElemMask) >> kArrayElemShift);
}

// Member selection encoding (enum/bitfield members) — mirrors the array
// element pattern, but the flag bit sits one position LOWER (bit 61 vs the
// array's bit 62), so the value field is 19 bits (42-60), NOT 20. The mask
// must therefore EXCLUDE bit 61 (= kMemberBit): a 20-bit mask (0x3FFFFC…,
// reaching bit 61) would read the flag bit back into the decoded subLine
// and inflate every result by 2^19 (524288), so memberSubFromSelId never
// matched the real subLine and member rows were never highlighted.
// (The strip mask used for node lookup is kMemberBit | kMemberSubMask =
// bits 42-61, unchanged by this narrowing.)
static constexpr uint64_t kMemberBit      = 0x2000000000000000ULL;  // bit 61
static constexpr uint64_t kMemberSubShift = 42;
static constexpr uint64_t kMemberSubMask  = 0x1FFFFC0000000000ULL;  // bits 42-60 (19 bits)

inline uint64_t makeMemberSelId(uint64_t nodeId, int subLine) {
    return nodeId | kMemberBit | ((uint64_t)(subLine & 0x7FFFF) << kMemberSubShift);
}
inline int memberSubFromSelId(uint64_t selId) {
    return (int)((selId & kMemberSubMask) >> kMemberSubShift);
}

// What kind of selection an encoded selId represents. The flag bits are
// NOT independent: the 20-bit array index field (bits 42-61) reaches bit
// 61 (= kMemberBit) for indices >= 2^19, so a high array element id also
// has the member bit set. Disambiguate by PRIORITY (footer 63 > array 62
// > member 61) — this is the single source of truth; never test the flag
// bits independently (a `selId & kMemberBit` check would misclassify a
// high-index array element as a member). Decode the index/subLine only
// after classifying: arrayElemIdxFromSelId reads the full 20-bit field, so
// the array index round-trips correctly even with bit 61 set.
enum class SelKind : uint8_t { Plain, Footer, ArrayElem, Member };
inline SelKind selKindOf(uint64_t selId) {
    if (selId & kFooterIdBit)  return SelKind::Footer;
    if (selId & kArrayElemBit) return SelKind::ArrayElem;
    if (selId & kMemberBit)    return SelKind::Member;
    return SelKind::Plain;
}

// ── Tail chips ──
// Unified model for "things that hang off the right side of a row":
// the user comment, RTTI hint, type-inference hint, enum-value name,
// and the discoverable AddComment placeholder. Each chip records its
// own [startCol, endCol) in the line, the rendered text (with prefix
// glyph already baked in), and any kind-specific payload the click
// handler needs. One source of truth — replaces the parallel
// typeHint*/rttiHint*/commentStart fields on LineMeta.
enum class ChipKind : uint8_t {
    Enum = 0,    // (RGBA) — int field with refId→enum
    TypeHint,    // [ptr64] / "0x… [float×2]" — type-inference suggestion
    Rtti,        // {RTTI: ClassName} — auto-detected vtable
    Symbol,      // Reclass.exe+0x123 — provider-resolved PDB symbol for a
                 // pointer's *value* (separate from Node::comment because
                 // it's auto-derived from the provider, not user-authored)
    Comment,     // user-authored Node::comment text
    AddComment   // ghost placeholder when no Comment exists (focus row only)
};

struct LineChip {
    ChipKind kind;
    int      startCol = -1;            // doc-col where chip text begins (post fold prefix)
    int      endCol   = -1;            // doc-col one past chip text end
    QString  text;                     // already includes the prefix glyph (/, [, {, ()
    // Kind-specific payload — only one is meaningful per kind.
    uint64_t          rttiVtableAddr = 0;        // Rtti
    QVector<NodeKind> typeHintKinds;             // TypeHint
    int64_t           enumCurrentValue = 0;      // Enum
    uint64_t          enumRefNodeId    = 0;      // Enum (the top-level enum struct's id)
};

struct LineMeta {
    int      nodeIdx        = -1;
    uint64_t nodeId         = 0;
    int      subLine        = 0;
    int      depth          = 0;
    int      foldLevel      = 0;
    bool     foldHead       = false;
    bool     foldCollapsed  = false;
    bool     isContinuation = false;
    bool     isRootHeader   = false;  // true for top-level struct headers (base address editable)
    bool     isArrayHeader  = false;  // true for array headers (has <idx/count> nav)
    LineKind lineKind       = LineKind::Field;
    NodeKind nodeKind       = NodeKind::Int32;
    NodeKind elementKind    = NodeKind::UInt8;  // Array element type
    int      arrayViewIdx   = 0;   // Array: current view index
    int      arrayCount     = 0;   // Array: total element count
    int      arrayElementIdx = -1; // Index of this element within parent array (-1 if not array element)
    QString  offsetText;
    uint64_t offsetAddr     = 0;     // Raw absolute address (for margin toggle)
    uint64_t ptrBase        = 0;     // Pointer expansion base (non-zero = use for RVA)
    uint32_t markerMask     = 0;
    bool     dataChanged    = false;  // true if any byte in this node changed since last refresh
    bool     unreadable     = false;  // true when the provider is valid but the value's bytes are
                                      // not readable (bad page / freed region) — render the value
                                      // distinctly instead of as a silent "00". NOT set for the
                                      // intentional NullProvider zero-fill of null pointer targets.
    int      heatLevel      = 0;     // 0=static, 1=cold, 2=warm, 3=hot (from ValueHistory)
    QVector<int> changedByteIndices;  // Hex preview: which byte indices (0-based) changed on this line
    int      lineByteCount  = 0;     // Hex preview: actual data byte count on this line
    int      effectiveTypeW = 14;  // Per-line type column width used for rendering
    int      effectiveNameW = 22;  // Per-line name column width used for rendering
    QString  pointerTargetName;    // Resolved target type name for Pointer32/64 (empty = "void")
    bool     isArrayElement  = false;  // true for synthesized primitive array element lines
    bool     isMemberLine   = false;  // true for enum member / bitfield member lines
    int      braceCol       = -1;      // Column of trailing '{' on header lines (-1 = none); avoids per-char IPC scan
    uint64_t parentAddr     = 0;       // Absolute address of enclosing container (for relative offset display)

    // ── Chips (unified annotations after the value column) ──
    // Replaces the parallel typeHint*/rttiHint*/commentStart fields.
    // Compose pushes one entry per applicable annotation; editor reads
    // the array for hit-testing, span lookup, and indicator coloring.
    // Render order is value-relevant first, user-authored last:
    //   Enum, TypeHint, Rtti, Comment, AddComment.
    // AddComment is only emitted when no Comment chip is present and
    // the editor is asking compose to draw a placeholder for the row.
    QVector<LineChip> chips;
};

// Tail-chip lookup — returns first chip of the given kind, or nullptr.
// O(chips.size()) which is tiny (4 max), no caching needed.
inline const LineChip* findChip(const LineMeta& lm, ChipKind kind) {
    for (const auto& c : lm.chips) {
        if (c.kind == kind) return &c;
    }
    return nullptr;
}

inline bool isSyntheticLine(const LineMeta& lm) {
    return lm.lineKind == LineKind::CommandRow;
}

// Encoded selection id for a composed line — the single source of truth
// for the line→selId rule. Footer rows carry the footer bit, array
// elements the array-elem encoding, members the member encoding;
// everything else is the bare nodeId. Used by both the controller's
// click handler (handleNodeClick) and the editor's byte-selection→row
// sync so a byte selection produces exactly the ids a click would.
inline uint64_t selIdForLine(const LineMeta& lm) {
    if (lm.lineKind == LineKind::Footer)
        return lm.nodeId | kFooterIdBit;
    if (lm.isArrayElement && lm.arrayElementIdx >= 0)
        return makeArrayElemSelId(lm.nodeId, lm.arrayElementIdx);
    if (lm.isMemberLine && lm.subLine >= 0)
        return makeMemberSelId(lm.nodeId, lm.subLine);
    return lm.nodeId;
}

// Decode an encoded selId back to its base node id — the inverse of the high-bit
// encoding that selIdForLine / makeArrayElemSelId / makeMemberSelId apply. Strips
// the footer, array-element, and member bits. Routing every decode site through
// this keeps the mask a single source of truth: adding a new selId flag bit can
// then never silently corrupt an un-updated hand-written copy of the mask.
inline uint64_t baseNodeIdFromSelId(uint64_t selId) {
    return selId & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                     | kMemberBit | kMemberSubMask);
}

// ── Layout Info ──

struct LayoutInfo {
    int typeW = 14;  // Effective type column width (default = kColType)
    int nameW = 22;  // Effective name column width (default = kColName)
    int offsetHexDigits = 8;  // Hex digits for offset margin (4/8/12/16)
    uint64_t baseAddress = 0; // Base address for relative offset computation
    bool treeLines = false;   // Whether tree line connectors are embedded in the text
};

// ── ComposeResult ──

struct ComposeResult {
    QString            text;
    QVector<LineMeta>  meta;
    LayoutInfo         layout;
    // Pre-computed by compose so applyDocument doesn't need to re-scan.
    // - maxLineLen: longest line length in chars, ignoring trailing spaces
    //   (drives SCI_SETSCROLLWIDTH).
    // - lineStarts: char offset of the start of each line in `text`.
    //   Size matches meta.size(); applyDocument uses it to slice line
    //   texts in O(1) instead of re-splitting on '\n'.
    int                maxLineLen = 0;
    QVector<int>       lineStarts;
};

// ── Command ──

namespace cmd {
    struct OffsetAdj   { uint64_t nodeId; int oldOffset, newOffset; };
    struct ChangeKind  { uint64_t nodeId; NodeKind oldKind, newKind;
                         QVector<OffsetAdj> offAdjs; };
    struct Rename      { uint64_t nodeId; QString oldName, newName; };
    struct Collapse    { uint64_t nodeId; bool oldState, newState; };
    struct Insert      { Node node; QVector<OffsetAdj> offAdjs; };
    struct Remove      { uint64_t nodeId; QVector<Node> subtree;
                         QVector<OffsetAdj> offAdjs; };
    struct ChangeBase  { uint64_t oldBase, newBase; QString oldFormula, newFormula; };
    struct WriteBytes  { uint64_t addr; QByteArray oldBytes, newBytes; };
    struct ChangeArrayMeta { uint64_t nodeId;
                             NodeKind oldElementKind, newElementKind;
                             int oldArrayLen, newArrayLen; };
    struct ChangePointerRef { uint64_t nodeId;
                              uint64_t oldRefId, newRefId; };
    struct ChangeStructTypeName { uint64_t nodeId; QString oldName, newName; };
    struct ChangeClassKeyword { uint64_t nodeId; QString oldKeyword, newKeyword; };
    struct ChangeOffset { uint64_t nodeId; int oldOffset, newOffset; };
    struct ChangeEnumMembers { uint64_t nodeId;
                               QVector<QPair<QString, int64_t>> oldMembers, newMembers; };
    struct ToggleRelative   { uint64_t nodeId; bool oldVal, newVal; };
    struct ToggleBigEndian  { uint64_t nodeId; bool oldVal, newVal; };
    struct ChangeComment    { uint64_t nodeId; QString oldComment, newComment; };
}

using Command = std::variant<
    cmd::ChangeKind, cmd::Rename, cmd::Collapse,
    cmd::Insert, cmd::Remove, cmd::ChangeBase, cmd::WriteBytes,
    cmd::ChangeArrayMeta, cmd::ChangePointerRef, cmd::ChangeStructTypeName,
    cmd::ChangeClassKeyword, cmd::ChangeOffset, cmd::ChangeEnumMembers,
    cmd::ToggleRelative,
    cmd::ToggleBigEndian, cmd::ChangeComment
>;

// ── Column spans (for inline editing) ──

struct ColumnSpan {
    int  start = 0;   // inclusive column index
    int  end   = 0;   // exclusive column index
    bool valid = false;
};

enum class EditTarget { Name, Type, Value, BaseAddress, Source, ArrayIndex, ArrayCount,
                        ArrayElementType, ArrayElementCount, PointerTarget,
                        RootClassType, RootClassName, TypeSelector, Comment };

// Column layout constants (shared with format.cpp span computation)
inline constexpr int kFoldCol     = 3;   // 3-char fold indicator prefix per line
inline constexpr int kTreeIndent  = 2;   // chars per nesting-level indent (tree connectors)
inline constexpr int kColType     = 14;  // Max type column width (fits "uint64_t[999]")
inline constexpr int kColName     = 22;
inline constexpr int kColValue    = 96;
inline constexpr int kColComment  = 28;  // "// Enter=Save Esc=Cancel" fits
inline constexpr int kColBaseAddr = 12;  // "0x" + up to 10 hex digits (40-bit address)
inline constexpr int kSepWidth    = 1;
inline constexpr int kMinTypeW    = 9;   // Minimum type column width (fits "uint64_t" + separator)
inline constexpr int kMaxTypeW    = 128; // Maximum type column width
inline constexpr int kMinNameW    = 10;  // Minimum name column width (fits "field_0000")
inline constexpr int kMaxNameW    = 128; // Maximum name column width
inline constexpr int kCompactTypeW    = 20; // Type column cap for compact column mode
// Auto-refresh interval (ms) — used as the *base* rate the adaptive
// loop in RcxController::onRefreshTick widens from. 200 ms feels like
// real-time inspection for live values; the controller automatically
// backs off to ~1.5 s when nothing is changing or the window is
// unfocused, so this number does not gate idle CPU.
inline constexpr int kDefaultRefreshMs = 200;

// LineGeometry — single source of truth for column math on a composed line.
// Bundles the prefix (fold indicator), per-depth indent, and a content
// origin so callers can ask "what is the document column of content position
// N on this line?" without repeating `kFoldCol + depth*kTreeIndent` everywhere.
// Use case: hit-testing, indicator placement, span computation.
struct LineGeometry {
    int prefixWidth   = kFoldCol;        // 0 for CommandRow / root footer (flush-left)
    int indentWidth   = 0;               // depth * kTreeIndent
    int typeColumnWidth = kColType;
    int nameColumnWidth = kColName;

    // Document column for the start of the type column.
    int typeStart() const { return prefixWidth + indentWidth; }
    // Document column for the start of the name column.
    int nameStart() const { return typeStart() + typeColumnWidth + kSepWidth; }
    // Document column for the start of the value column.
    int valueStart() const { return nameStart() + nameColumnWidth + kSepWidth; }
    // Translate a content-space column (lineText index, ignoring prefix) to
    // the corresponding document column in the Scintilla buffer.
    int documentColumn(int contentCol) const { return prefixWidth + contentCol; }

    static LineGeometry forLine(const LineMeta& lm) {
        LineGeometry g;
        bool flushLeft = (lm.lineKind == LineKind::CommandRow)
                     || (lm.lineKind == LineKind::Footer && lm.isRootHeader);
        g.prefixWidth = flushLeft ? 0 : kFoldCol;
        g.indentWidth = lm.depth * kTreeIndent;
        g.typeColumnWidth = lm.effectiveTypeW > 0 ? lm.effectiveTypeW : kColType;
        g.nameColumnWidth = lm.effectiveNameW > 0 ? lm.effectiveNameW : kColName;
        return g;
    }
};

inline ColumnSpan typeSpanFor(const LineMeta& lm, int typeW = kColType) {
    if (lm.lineKind != LineKind::Field || lm.isContinuation || lm.isMemberLine) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    return {ind, ind + typeW, true};
}

inline ColumnSpan nameSpanFor(const LineMeta& lm, int typeW = kColType, int nameW = kColName) {
    if (lm.isContinuation || lm.lineKind != LineKind::Field || lm.isMemberLine) return {};

    int ind = kFoldCol + lm.depth * kTreeIndent;
    int start = ind + typeW + kSepWidth;

    // Hex: ASCII preview occupies the name column (padded to nameW)
    if (isHexPreview(lm.nodeKind))
        return {start, start + nameW, true};

    return {start, start + nameW, true};
}

inline ColumnSpan valueSpanFor(const LineMeta& lm, int /*lineLength*/, int typeW = kColType, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer ||
        lm.lineKind == LineKind::ArrayElementSeparator) return {};
    if (lm.isMemberLine) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;

    // Hex uses nameW for ASCII column (same as regular name column)
    bool isHex = isHexPreview(lm.nodeKind);
    int valWidth = isHex ? 23 : kColValue;

    int prefixW = typeW + nameW + 2 * kSepWidth;

    if (lm.isContinuation) {
        int start = ind + prefixW;
        return {start, start + valWidth, true};
    }
    if (lm.lineKind != LineKind::Field) return {};

    int start = ind + prefixW;
    return {start, start + valWidth, true};
}

// Member line spans (enum "name = value", bitfield "name : N = value")
inline ColumnSpan memberNameSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isMemberLine) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    int eq = lineText.indexOf(QLatin1String(" = "), ind);
    if (eq < 0) return {};
    int nameEnd = eq;
    while (nameEnd > ind && lineText[nameEnd - 1] == ' ') nameEnd--;
    return {ind, nameEnd, true};
}

inline ColumnSpan memberValueSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isMemberLine) return {};
    int eq = lineText.indexOf(QLatin1String(" = "));
    if (eq < 0) return {};
    int valStart = eq + 3;
    int valEnd = lineText.size();
    while (valEnd > valStart && lineText[valEnd - 1] == ' ') valEnd--;
    return {valStart, valEnd, true};
}

inline ColumnSpan commentSpanFor(const LineMeta& lm, int lineLength, int typeW = kColType, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer
        || lm.lineKind == LineKind::CommandRow || lm.lineKind == LineKind::ArrayElementSeparator
        || lm.isContinuation || lm.isMemberLine) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;

    bool isHex = isHexPreview(lm.nodeKind);
    int valWidth = isHex ? 23 : kColValue;

    int prefixW = typeW + nameW + 2 * kSepWidth;
    int start;
    if (lm.isContinuation) {
        start = ind + prefixW + valWidth;
    } else {
        start = ind + prefixW + valWidth;
    }
    return {start, lineLength, start < lineLength};
}

// ── CommandRow spans ──
// Line format: "source▾ · 0x140000000"

inline ColumnSpan commandRowSrcSpan(const QString& lineText) {
    // Source label ends at the ▾ dropdown arrow
    int arrow = lineText.indexOf(QChar(0x25BE));
    if (arrow < 0) return {};
    int start = 0;
    while (start < arrow && !lineText[start].isLetterOrNumber()
           && lineText[start] != '<' && lineText[start] != '\'') start++;
    if (start >= arrow) return {};
    return {start, arrow, true};
}

// ── CommandRow root-class spans ──
// Combined CommandRow format ends with: "  struct ClassName {"

inline int commandRowRootStart(const QString& lineText) {
    int best = -1;
    int i;
    // Match "struct " / "class " / "enum " as whole words before the class name
    i = lineText.lastIndexOf(QStringLiteral("struct "));
    if (i > best) best = i;
    i = lineText.lastIndexOf(QStringLiteral("class "));
    if (i > best) best = i;
    i = lineText.lastIndexOf(QStringLiteral("enum "));
    if (i > best) best = i;
    return best;
}

inline ColumnSpan commandRowAddrSpan(const QString& lineText) {
    // Address starts after the source dropdown arrow (▾)
    int arrow = lineText.indexOf(QChar(0x25BE));
    if (arrow < 0) return {};
    // Skip whitespace after arrow to find the start of the address/formula
    int addrStart = arrow + 1;
    while (addrStart < lineText.size() && lineText[addrStart].isSpace()) addrStart++;
    // If text starts with '<' or '[', it's a formula — use the whole thing.
    // Only look for bare "0x" prefix if the address doesn't start with formula syntax.
    int start;
    if (addrStart < lineText.size()
        && (lineText[addrStart] == '<' || lineText[addrStart] == '[')) {
        start = addrStart;
    } else {
        int oxPos = lineText.indexOf(QStringLiteral("0x"), arrow);
        start = (oxPos >= 0) ? oxPos : addrStart;
    }
    // End at root keyword (struct/class/enum) or end of line
    int rootStart = commandRowRootStart(lineText);
    int end = (rootStart > start) ? rootStart : lineText.size();
    // Trim trailing whitespace
    while (end > start && lineText[end - 1].isSpace()) end--;
    if (end <= start) return {};
    return {start, end, true};
}

inline ColumnSpan commandRowRootTypeSpan(const QString& lineText) {
    int start = commandRowRootStart(lineText);
    if (start < 0) return {};
    int end = start;
    while (end < lineText.size() && lineText[end] != QChar(' ')) end++;
    if (end <= start) return {};
    return {start, end, true};
}

inline ColumnSpan commandRowRootNameSpan(const QString& lineText) {
    int base = commandRowRootStart(lineText);
    if (base < 0) return {};
    int space = lineText.indexOf(' ', base);
    if (space < 0) return {};
    int nameStart = space + 1;
    while (nameStart < lineText.size() && lineText[nameStart].isSpace()) nameStart++;
    if (nameStart >= lineText.size()) return {};
    int nameEnd = lineText.indexOf(QStringLiteral(" {"), nameStart);
    if (nameEnd < 0) nameEnd = lineText.size();
    while (nameEnd > nameStart && lineText[nameEnd - 1].isSpace()) nameEnd--;
    if (nameEnd <= nameStart) return {};
    return {nameStart, nameEnd, true};
}

// ── CommandRow type-selector chevron span ──
// Detects "[▸]" at the start of the command row text

inline ColumnSpan commandRowChevronSpan(const QString& lineText) {
    if (lineText.size() < 3) return {};
    if (lineText[0] == '[' && lineText[1] == QChar(0x25B8) && lineText[2] == ']')
        return {0, qMin(4, (int)lineText.size()), true};  // include trailing space for easier clicking
    return {};
}

// ── Array element type/count spans (within type column of array headers) ──
// Line format: "   int32_t[10]  name  {"
// arrayElemTypeSpan covers "int32_t", arrayElemCountSpan covers "10"

inline ColumnSpan arrayElemTypeSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    // Find '[' in the type portion
    int bracket = lineText.indexOf('[', ind);
    if (bracket <= ind) return {};
    return {ind, bracket, true};
}

inline ColumnSpan arrayElemCountSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    int openBracket = lineText.indexOf('[', ind);
    int closeBracket = lineText.indexOf(']', openBracket);
    if (openBracket < 0 || closeBracket < 0 || closeBracket <= openBracket + 1) return {};
    return {openBracket + 1, closeBracket, true};
}

// Click-area version: includes brackets [N] for hit testing
inline ColumnSpan arrayElemCountClickSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    int openBracket = lineText.indexOf('[', ind);
    int closeBracket = lineText.indexOf(']', openBracket);
    if (openBracket < 0 || closeBracket < 0 || closeBracket <= openBracket + 1) return {};
    return {openBracket, closeBracket + 1, true};
}

// ── Pointer kind/target spans (within type column of pointer fields) ──
// Line format: "   void*          name  -> 0x..."
// pointerTargetSpan covers the target name before '*'

inline ColumnSpan pointerKindSpanFor(const LineMeta& /*lm*/, const QString& /*lineText*/) {
    return {};  // No separate kind span in "Type*" format
}

inline ColumnSpan pointerTargetSpanFor(const LineMeta& lm, const QString& lineText) {
    if ((lm.lineKind != LineKind::Field && lm.lineKind != LineKind::Header) || lm.isContinuation) return {};
    if (lm.nodeKind != NodeKind::Pointer32 && lm.nodeKind != NodeKind::Pointer64) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    int star = lineText.indexOf('*', ind);
    if (star <= ind) return {};
    return {ind, star, true};
}

// ── Array navigation spans ──
// Line format: "uint32_t[16]  name  { <0/16>"

inline ColumnSpan arrayPrevSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isArrayHeader) return {};
    int lt = lineText.lastIndexOf('<');
    if (lt < 0) return {};
    return {lt, lt + 1, true};
}

inline ColumnSpan arrayIndexSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isArrayHeader) return {};
    int lt = lineText.lastIndexOf('<');
    int slash = lineText.indexOf('/', lt);
    if (lt < 0 || slash < 0) return {};
    return {lt + 1, slash, true};
}

inline ColumnSpan arrayCountSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isArrayHeader) return {};
    int slash = lineText.lastIndexOf('/');
    int gt = lineText.indexOf('>', slash);
    if (slash < 0 || gt < 0) return {};
    return {slash + 1, gt, true};
}

inline ColumnSpan arrayNextSpanFor(const LineMeta& lm, const QString& lineText) {
    if (!lm.isArrayHeader) return {};
    int gt = lineText.lastIndexOf('>');
    if (gt < 0) return {};
    return {gt, gt + 1, true};
}

// ── ViewState ──

struct ViewState {
    int scrollLine = 0;
    int cursorLine = 0;
    int cursorCol  = 0;
    int xOffset    = 0;       // horizontal scroll in pixels
    // Anchor the cursor by the node that owned it, so a refresh that
    // shifts line counts (e.g. type change with pointer auto-expansion,
    // sibling insert/delete above) lands the caret back on the SAME
    // node — not the now-different node that occupies the old line
    // number. Falls back to (cursorLine, cursorCol) when 0.
    uint64_t cursorNodeId  = 0;
    int      cursorSubLine = 0;
};

// ── Format function forward declarations ──

namespace fmt {
    using TypeNameFn = QString (*)(NodeKind);
    void setTypeNameProvider(TypeNameFn fn);
    QString typeName(NodeKind kind, int colType = kColType);
    QString typeNameRaw(NodeKind kind);  // Unpadded type name for width calculation
    QString fmtInt8(int8_t v);
    QString fmtInt16(int16_t v);
    QString fmtInt32(int32_t v);
    QString fmtInt64(int64_t v);
    QString fmtUInt8(uint8_t v);
    QString fmtUInt16(uint16_t v);
    QString fmtUInt32(uint32_t v);
    QString fmtUInt64(uint64_t v);
    QString fmtFloat(float v);
    QString fmtDouble(double v);
    QString fmtBool(uint8_t v);
    QString fmtPointer32(uint32_t v);
    QString fmtPointer64(uint64_t v);
    QString fmtNodeLine(const Node& node, const Provider& prov,
                        uint64_t addr, int depth, int subLine = 0,
                        const QString& comment = {}, int colType = kColType, int colName = kColName,
                        const QString& typeOverride = {}, bool compact = false);
    QString fmtOffsetMargin(uint64_t absoluteOffset, bool isContinuation, int hexDigits = 8);
    QString fmtStructHeader(const Node& node, int depth, bool collapsed, int colType = kColType, int colName = kColName, bool compact = false);
    QString fmtStructFooter(const Node& node, int depth, int totalSize = -1);
    QString fmtArrayHeader(const Node& node, int depth, int viewIdx, bool collapsed, int colType = kColType, int colName = kColName, const QString& elemStructName = {}, bool compact = false);
    QString structTypeName(const Node& node);  // Full type string for struct headers
    QString arrayTypeName(NodeKind elemKind, int count, const QString& structName = {});
    QString pointerTypeName(NodeKind kind, const QString& targetName);
    QString fmtPointerHeader(const Node& node, int depth, bool collapsed,
                             const Provider& prov, uint64_t addr,
                             const QString& ptrTypeName, int colType = kColType, int colName = kColName,
                             bool compact = false);
    QString validateBaseAddress(const QString& text);
    QString indent(int depth);
    QString readValue(const Node& node, const Provider& prov,
                      uint64_t addr, int subLine);
    QString editableValue(const Node& node, const Provider& prov,
                          uint64_t addr, int subLine);
    QByteArray parseValue(NodeKind kind, const QString& text, bool* ok);
    QByteArray parseValue(const Node& node, const QString& text, bool* ok);
    QByteArray parseAsciiValue(const QString& text, int expectedSize, bool* ok);
    QString validateValue(NodeKind kind, const QString& text);
    QString fmtEnumMember(const QString& name, int64_t value, int depth, int nameW);
    QString fmtBitfieldMember(const QString& name, uint8_t bitWidth,
                              uint64_t value, int depth, int nameW);
    uint64_t extractBits(const Provider& prov, uint64_t addr,
                         NodeKind containerKind,
                         uint8_t bitOffset, uint8_t bitWidth);
} // namespace fmt

// ── Compose function forward declaration ──

// Optional callback: given an absolute address, return a symbol name (e.g. "nt!PsActiveProcessHead")
// or empty string if no symbol matches. Used for PDB symbol annotations on rows.
using SymbolLookupFn = std::function<QString(uint64_t addr)>;

ComposeResult compose(const NodeTree& tree, const Provider& prov, uint64_t viewRootId = 0,
                      bool compactColumns = false, bool treeLines = false,
                      bool braceWrap = false, bool typeHints = false,
                      bool showComments = true,
                      SymbolLookupFn symbolLookup = {},
                      bool showRtti = true, bool showEnumChips = true);

} // namespace rcx
