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
    Hex8, Hex16, Hex32, Hex64,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double, Bool,
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
    KF_HexPreview = 1 << 0,  // Hex8..Hex64 (ASCII+hex layout)
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
    {NodeKind::Int8,      "Int8",      "int8_t",      1,  1,  1, KF_None},
    {NodeKind::Int16,     "Int16",     "int16_t",     2,  1,  2, KF_None},
    {NodeKind::Int32,     "Int32",     "int32_t",     4,  1,  4, KF_None},
    {NodeKind::Int64,     "Int64",     "int64_t",     8,  1,  8, KF_None},
    {NodeKind::UInt8,     "UInt8",     "uint8_t",     1,  1,  1, KF_None},
    {NodeKind::UInt16,    "UInt16",    "uint16_t",    2,  1,  2, KF_None},
    {NodeKind::UInt32,    "UInt32",    "uint32_t",    4,  1,  4, KF_None},
    {NodeKind::UInt64,    "UInt64",    "uint64_t",    8,  1,  8, KF_None},
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

inline constexpr const KindMeta* kindMeta(NodeKind k) {
    for (const auto& m : kKindMeta)
        if (m.kind == k) return &m;
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
    return k >= NodeKind::Hex8 && k <= NodeKind::Hex64;
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
// Hex types, pointer types, function pointers, and containers are not meaningful
// primitive-pointer targets — dereferencing them produces the same output as void*.
inline constexpr bool isValidPrimitivePtrTarget(NodeKind k) {
    if (isHexNode(k)) return false;
    if (k == NodeKind::Pointer32 || k == NodeKind::Pointer64) return false;
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
    bool     isStatic   = false;   // static field — excluded from struct layout
    QString  offsetExpr;           // C/C++ expression → absolute address (static fields only)
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

    // Note: Returns 0 for Array-of-Struct/Array. Use tree.structSpan() for accurate size.
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
        if (isStatic)
            o["isStatic"] = true;
        if (!offsetExpr.isEmpty())
            o["offsetExpr"] = offsetExpr;
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
        n.isStatic  = o["isStatic"].toBool(o["isHelper"].toBool(false));
        n.offsetExpr = o["offsetExpr"].toString();
        n.isRelative = o["isRelative"].toBool(false);
        n.arrayLen  = qBound(1, o["arrayLen"].toInt(1), 1000000);
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
        return n;
    }

    // Resolved class keyword (never empty)
    QString resolvedClassKeyword() const {
        return classKeyword.isEmpty() ? QStringLiteral("struct") : classKeyword;
    }

    // NOTE: isStringArray() was checking UInt8/UInt16 instead of UTF8/UTF16.
    // Currently unused — commented out until a caller needs it.
    // bool isStringArray() const {
    //     return kind == NodeKind::Array &&
    //            (elementKind == NodeKind::UTF8 || elementKind == NodeKind::UTF16);
    // }
};

// ── NodeTree ──

struct NodeTree {
    QVector<Node> nodes;
    uint64_t      baseAddress = 0x00400000;
    QString       baseAddressFormula;  // e.g. "<ReClass.exe> + 0x100"
    int           pointerSize = 8;    // 4 for 32-bit targets, 8 for 64-bit
    uint64_t      m_nextId    = 1;
    mutable QHash<uint64_t, int> m_idCache;
    mutable QHash<uint64_t, QVector<int>> m_childCache;

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
        return idx;
    }

    // Reserve a unique ID atomically (for use before pushing undo commands)
    uint64_t reserveId() { return m_nextId++; }

    void invalidateIdCache() const { m_idCache.clear(); m_childCache.clear(); }

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

    // Collect node + all descendants (iterative, cycle-safe)
    QVector<int> subtreeIndices(uint64_t nodeId) const {
        int idx = indexOfId(nodeId);
        if (idx < 0) return {};
        // Build parent→children map
        QHash<uint64_t, QVector<int>> childMap;
        for (int i = 0; i < nodes.size(); i++)
            childMap[nodes[i].parentId].append(i);
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

        int maxEnd = 0;
        QVector<int> kids = childMap ? childMap->value(structId) : childrenOf(structId);
        for (int ci : kids) {
            const Node& c = nodes[ci];
            if (c.isStatic) continue;  // static fields don't affect struct size
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
        if (pointerSize != 8)
            o["pointerSize"] = pointerSize;
        o["nextId"]      = QString::number(m_nextId);
        QJsonArray arr;
        for (const auto& n : nodes) arr.append(n.toJson());
        o["nodes"] = arr;
        return o;
    }

    static NodeTree fromJson(const QJsonObject& o) {
        NodeTree t;
        t.baseAddress = o["baseAddress"].toString("400000").toULongLong(nullptr, 16);
        t.baseAddressFormula = o["baseAddressFormula"].toString();
        t.pointerSize = o["pointerSize"].toInt(8);
        t.m_nextId    = o["nextId"].toString("1").toULongLong();
        QJsonArray arr = o["nodes"].toArray();
        t.nodes.reserve(arr.size());
        for (const auto& v : arr) {
            Node n = Node::fromJson(v.toObject());
            t.nodes.append(n);
            if (n.id >= t.m_nextId) t.m_nextId = n.id + 1;
        }
        return t;
    }

};

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

// Encode an array element selection ID: nodeId | kArrayElemBit | (elemIdx << 42)
inline uint64_t makeArrayElemSelId(uint64_t nodeId, int elemIdx) {
    Q_ASSERT(elemIdx >= 0);
    return nodeId | kArrayElemBit | ((uint64_t)(elemIdx & 0xFFFFF) << kArrayElemShift);
}
inline int arrayElemIdxFromSelId(uint64_t selId) {
    return (int)((selId & kArrayElemMask) >> kArrayElemShift);
}

// Member selection encoding (enum/bitfield members) — mirrors array element pattern
static constexpr uint64_t kMemberBit      = 0x2000000000000000ULL;
static constexpr uint64_t kMemberSubShift = 42;
static constexpr uint64_t kMemberSubMask  = 0x3FFFFC0000000000ULL;

inline uint64_t makeMemberSelId(uint64_t nodeId, int subLine) {
    return nodeId | kMemberBit | ((uint64_t)(subLine & 0xFFFFF) << kMemberSubShift);
}
inline int memberSubFromSelId(uint64_t selId) {
    return (int)((selId & kMemberSubMask) >> kMemberSubShift);
}

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
    int      heatLevel      = 0;     // 0=static, 1=cold, 2=warm, 3=hot (from ValueHistory)
    QVector<int> changedByteIndices;  // Hex preview: which byte indices (0-based) changed on this line
    int      lineByteCount  = 0;     // Hex preview: actual data byte count on this line
    int      effectiveTypeW = 14;  // Per-line type column width used for rendering
    int      effectiveNameW = 22;  // Per-line name column width used for rendering
    QString  pointerTargetName;    // Resolved target type name for Pointer32/64 (empty = "void")
    bool     isArrayElement  = false;  // true for synthesized primitive array element lines
    bool     isMemberLine   = false;  // true for enum member / bitfield member lines
    bool     isStaticLine   = false;  // true for static field node lines
    QString  typeHint;                 // Type inference hint text (e.g. "Float×2") — only set for hex nodes when hints enabled
    int      typeHintStart  = -1;      // Character offset where hint text starts in line text (-1 = none)
    QVector<NodeKind> typeHintKinds;   // Suggested kinds from inference (empty = no hint)
    int      commentStart   = -1;      // Character offset where "// comment" starts in line text (-1 = none)
    bool     commentPlaceholder = false; // true when "  //" is a placeholder (no real comment)
};

inline bool isSyntheticLine(const LineMeta& lm) {
    return lm.lineKind == LineKind::CommandRow;
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
    struct ChangeOffsetExpr { uint64_t nodeId; QString oldExpr, newExpr; };
    struct ToggleStatic     { uint64_t nodeId; bool oldVal, newVal; };
    struct ToggleRelative   { uint64_t nodeId; bool oldVal, newVal; };
    struct ChangeComment    { uint64_t nodeId; QString oldComment, newComment; };
}

using Command = std::variant<
    cmd::ChangeKind, cmd::Rename, cmd::Collapse,
    cmd::Insert, cmd::Remove, cmd::ChangeBase, cmd::WriteBytes,
    cmd::ChangeArrayMeta, cmd::ChangePointerRef, cmd::ChangeStructTypeName,
    cmd::ChangeClassKeyword, cmd::ChangeOffset, cmd::ChangeEnumMembers,
    cmd::ChangeOffsetExpr, cmd::ToggleStatic, cmd::ToggleRelative,
    cmd::ChangeComment
>;

// ── Column spans (for inline editing) ──

struct ColumnSpan {
    int  start = 0;   // inclusive column index
    int  end   = 0;   // exclusive column index
    bool valid = false;
};

enum class EditTarget { Name, Type, Value, BaseAddress, Source, ArrayIndex, ArrayCount,
                        ArrayElementType, ArrayElementCount, PointerTarget,
                        RootClassType, RootClassName, TypeSelector, StaticExpr, Comment };

// Column layout constants (shared with format.cpp span computation)
inline constexpr int kFoldCol     = 3;   // 3-char fold indicator prefix per line
inline constexpr int kTreeIndent  = 2;   // chars per nesting-level indent (tree connectors)
inline constexpr int kColType     = 14;  // Max type column width (fits "uint64_t[999]")
inline constexpr int kColName     = 22;
inline constexpr int kColValue    = 96;
inline constexpr int kColComment  = 28;  // "// Enter=Save Esc=Cancel" fits
inline constexpr int kColBaseAddr = 12;  // "0x" + up to 10 hex digits (40-bit address)
inline constexpr int kSepWidth    = 1;
inline constexpr int kMinTypeW    = 7;   // Minimum type column width (fits "uint8_t")
inline constexpr int kMaxTypeW    = 128; // Maximum type column width
inline constexpr int kMinNameW    = 8;   // Minimum name column width (matches ASCII preview)
inline constexpr int kMaxNameW    = 128; // Maximum name column width
inline constexpr int kCompactTypeW    = 20; // Type column cap for compact column mode

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

// Static field expression span: locates text between "return " and "→" / "(error)" / end
inline ColumnSpan staticExprSpanFor(const LineMeta& /*lm*/, const QString& lineText) {
    int ret = lineText.indexOf(QLatin1String("return "));
    if (ret < 0) return {};
    int exprStart = ret + 7;
    // End: before arrow, before "(error)", or line end
    int exprEnd = lineText.size();
    int arrow = lineText.indexOf(QChar(0x2192), exprStart);
    if (arrow > exprStart) exprEnd = arrow;
    int err = lineText.indexOf(QLatin1String("(error)"), exprStart);
    if (err > exprStart && err < exprEnd) exprEnd = err;
    // Also stop at " }" for collapsed format
    int brace = lineText.indexOf(QLatin1String(" }"), exprStart);
    if (brace > exprStart && brace < exprEnd) exprEnd = brace;
    while (exprEnd > exprStart && lineText[exprEnd - 1] == ' ') exprEnd--;
    return {exprStart, exprEnd, true};
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
    int xOffset    = 0;  // horizontal scroll in pixels
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
                      SymbolLookupFn symbolLookup = {});

} // namespace rcx
