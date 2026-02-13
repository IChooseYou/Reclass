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
#include <memory>
#include <variant>

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
    Vec2, Vec3, Vec4, Mat4x4,
    UTF8, UTF16,
    Padding,
    Struct, Array
};

} // namespace rcx (temporarily close for qHash)
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
inline uint qHash(rcx::NodeKind key, uint seed = 0) { return ::qHash(static_cast<uint8_t>(key), seed); }
#endif
namespace rcx { // reopen

// ── Kind flags (replaces repeated Hex/Padding switches) ──

enum KindFlags : uint32_t {
    KF_None       = 0,
    KF_HexPreview = 1 << 0,  // Hex8..Hex64 + Padding (ASCII+hex layout)
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
    {NodeKind::Vec2,      "Vec2",      "vec2",        8,  1,  4, KF_Vector},
    {NodeKind::Vec3,      "Vec3",      "vec3",       12,  1,  4, KF_Vector},
    {NodeKind::Vec4,      "Vec4",      "vec4",       16,  1,  4, KF_Vector},
    {NodeKind::Mat4x4,    "Mat4x4",    "mat4x4",     64,  4,  4, KF_None},
    {NodeKind::UTF8,      "UTF8",      "char[]",      1,  1,  1, KF_String},
    {NodeKind::UTF16,     "UTF16",     "wchar_t[]",   2,  1,  2, KF_String},
    {NodeKind::Padding,   "Padding",   "pad",         1,  1,  1, KF_HexPreview},
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
inline constexpr bool isHexPreview(NodeKind k) {
    return flagsFor(k) & KF_HexPreview;
}
inline constexpr bool isHexNode(NodeKind k) {
    return k >= NodeKind::Hex8 && k <= NodeKind::Hex64;
}
inline constexpr bool isVectorKind(NodeKind k) {
    return k == NodeKind::Vec2 || k == NodeKind::Vec3 || k == NodeKind::Vec4;
}
inline constexpr bool isMatrixKind(NodeKind k) {
    return k == NodeKind::Mat4x4;
}

inline QStringList allTypeNamesForUI(bool stripBrackets = false) {
    QStringList out;
    out.reserve(std::size(kKindMeta));
    for (const auto& m : kKindMeta) {
        QString t = QString::fromLatin1(m.typeName);
        if (stripBrackets) t.remove(QStringLiteral("[]"));
        out << t;
    }
    out.sort(Qt::CaseInsensitive);
    out.removeDuplicates();
    return out;
}

// ── Marker vocabulary ──

enum Marker : int {
    M_CONT      = 0,
    M_PAD       = 1,
    M_PTR0      = 2,
    M_CYCLE     = 3,
    M_ERR       = 4,
    M_STRUCT_BG = 5,
    M_HOVER     = 6,
    M_SELECTED  = 7,
    M_CMD_ROW   = 8,
    M_ACCENT    = 9,
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
    int      arrayLen   = 1;   // Array: element count
    int      strLen     = 64;
    bool     collapsed  = false;
    uint64_t refId      = 0;       // Pointer32/64: id of Struct to expand at *ptr
    NodeKind elementKind = NodeKind::UInt8;  // Array: element type
    int      viewIndex  = 0;   // Array: current view offset (transient)

    // Note: Returns 0 for Array-of-Struct/Array. Use tree.structSpan() for accurate size.
    int byteSize() const {
        switch (kind) {
        case NodeKind::UTF8:    return strLen;
        case NodeKind::UTF16:   return strLen * 2;
        case NodeKind::Padding: return qMax(1, arrayLen);
        case NodeKind::Array:   return arrayLen * sizeForKind(elementKind);
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
        o["arrayLen"]  = arrayLen;
        o["strLen"]    = strLen;
        o["collapsed"] = collapsed;
        o["refId"]     = QString::number(refId);
        o["elementKind"] = kindToString(elementKind);
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
        n.arrayLen  = o["arrayLen"].toInt(1);
        n.strLen    = o["strLen"].toInt(64);
        n.collapsed = o["collapsed"].toBool(false);
        n.refId     = o["refId"].toString("0").toULongLong();
        n.elementKind = kindFromString(o["elementKind"].toString("UInt8"));
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
    uint64_t      m_nextId    = 1;
    mutable QHash<uint64_t, int> m_idCache;

    int addNode(const Node& n) {
        Node copy = n;
        if (copy.id == 0) copy.id = m_nextId++;
        else if (copy.id >= m_nextId) m_nextId = copy.id + 1;
        int idx = nodes.size();
        nodes.append(copy);
        if (!m_idCache.isEmpty())
            m_idCache[copy.id] = idx;
        return idx;
    }

    // Reserve a unique ID atomically (for use before pushing undo commands)
    uint64_t reserveId() { return m_nextId++; }

    void invalidateIdCache() const { m_idCache.clear(); }

    int indexOfId(uint64_t id) const {
        if (m_idCache.isEmpty() && !nodes.isEmpty()) {
            for (int i = 0; i < nodes.size(); i++)
                m_idCache[nodes[i].id] = i;
        }
        return m_idCache.value(id, -1);
    }

    QVector<int> childrenOf(uint64_t parentId) const {
        QVector<int> result;
        for (int i = 0; i < nodes.size(); i++) {
            if (nodes[i].parentId == parentId) result.append(i);
        }
        return result;
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
            int sz = (c.kind == NodeKind::Struct || c.kind == NodeKind::Array)
                ? structSpan(c.id, childMap, visited) : c.byteSize();
            int end = c.offset + sz;
            if (end > maxEnd) maxEnd = end;
        }

        // Embedded struct reference: no own children but refId points to a struct definition
        if (kids.isEmpty() && node.kind == NodeKind::Struct && node.refId != 0)
            maxEnd = qMax(maxEnd, structSpan(node.refId, childMap, visited));

        return qMax(declaredSize, maxEnd);
    }

    // Compute natural alignment of a struct (max alignment of direct children)
    int computeStructAlignment(uint64_t structId) const;

    // Batch selection normalizers
    QSet<uint64_t> normalizePreferAncestors(const QSet<uint64_t>& ids) const;
    QSet<uint64_t> normalizePreferDescendants(const QSet<uint64_t>& ids) const;

    QJsonObject toJson() const {
        QJsonObject o;
        o["baseAddress"] = QString::number(baseAddress, 16);
        o["nextId"]      = QString::number(m_nextId);
        QJsonArray arr;
        for (const auto& n : nodes) arr.append(n.toJson());
        o["nodes"] = arr;
        return o;
    }

    static NodeTree fromJson(const QJsonObject& o) {
        NodeTree t;
        t.baseAddress = o["baseAddress"].toString("400000").toULongLong(nullptr, 16);
        t.m_nextId    = o["nextId"].toString("1").toULongLong();
        QJsonArray arr = o["nodes"].toArray();
        for (const auto& v : arr) {
            Node n = Node::fromJson(v.toObject());
            t.nodes.append(n);
            if (n.id >= t.m_nextId) t.m_nextId = n.id + 1;
        }
        return t;
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
    uint32_t markerMask     = 0;
    bool     dataChanged    = false;  // true if any byte in this node changed since last refresh
    QVector<int> changedByteIndices;  // Hex preview: which byte indices (0-based) changed on this line
    int      lineByteCount  = 0;     // Hex preview: actual data byte count on this line
    int      effectiveTypeW = 14;  // Per-line type column width used for rendering
    int      effectiveNameW = 22;  // Per-line name column width used for rendering
    QString  pointerTargetName;    // Resolved target type name for Pointer32/64 (empty = "void")
    bool     isArrayElement  = false;  // true for synthesized primitive array element lines
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
    struct ChangeBase  { uint64_t oldBase, newBase; };
    struct WriteBytes  { uint64_t addr; QByteArray oldBytes, newBytes; };
    struct ChangeArrayMeta { uint64_t nodeId;
                             NodeKind oldElementKind, newElementKind;
                             int oldArrayLen, newArrayLen; };
    struct ChangePointerRef { uint64_t nodeId;
                              uint64_t oldRefId, newRefId; };
    struct ChangeStructTypeName { uint64_t nodeId; QString oldName, newName; };
    struct ChangeClassKeyword { uint64_t nodeId; QString oldKeyword, newKeyword; };
    struct ChangeOffset { uint64_t nodeId; int oldOffset, newOffset; };
}

using Command = std::variant<
    cmd::ChangeKind, cmd::Rename, cmd::Collapse,
    cmd::Insert, cmd::Remove, cmd::ChangeBase, cmd::WriteBytes,
    cmd::ChangeArrayMeta, cmd::ChangePointerRef, cmd::ChangeStructTypeName,
    cmd::ChangeClassKeyword, cmd::ChangeOffset
>;

// ── Column spans (for inline editing) ──

struct ColumnSpan {
    int  start = 0;   // inclusive column index
    int  end   = 0;   // exclusive column index
    bool valid = false;
};

enum class EditTarget { Name, Type, Value, BaseAddress, Source, ArrayIndex, ArrayCount,
                        ArrayElementType, ArrayElementCount, PointerTarget,
                        RootClassType, RootClassName, TypeSelector };

// Column layout constants (shared with format.cpp span computation)
inline constexpr int kFoldCol     = 3;   // 3-char fold indicator prefix per line
inline constexpr int kColType     = 14;  // Max type column width (fits "uint64_t[999]")
inline constexpr int kColName     = 22;
inline constexpr int kColValue    = 96;
inline constexpr int kColComment  = 28;  // "// Enter=Save Esc=Cancel" fits
inline constexpr int kColBaseAddr = 12;  // "0x" + up to 10 hex digits (40-bit address)
inline constexpr int kSepWidth    = 1;
inline constexpr int kMinTypeW    = 8;   // Minimum type column width (fits "uint64_t")
inline constexpr int kMaxTypeW    = 128; // Maximum type column width
inline constexpr int kMinNameW    = 8;   // Minimum name column width (matches ASCII preview)
inline constexpr int kMaxNameW    = 128; // Maximum name column width

inline ColumnSpan typeSpanFor(const LineMeta& lm, int typeW = kColType) {
    if (lm.lineKind != LineKind::Field || lm.isContinuation) return {};
    int ind = kFoldCol + lm.depth * 3;
    return {ind, ind + typeW, true};
}

inline ColumnSpan nameSpanFor(const LineMeta& lm, int typeW = kColType, int nameW = kColName) {
    if (lm.isContinuation || lm.lineKind != LineKind::Field) return {};

    int ind = kFoldCol + lm.depth * 3;
    int start = ind + typeW + kSepWidth;

    // Hex/Padding: ASCII preview occupies the name column (padded to nameW)
    if (isHexPreview(lm.nodeKind))
        return {start, start + nameW, true};

    return {start, start + nameW, true};
}

inline ColumnSpan valueSpanFor(const LineMeta& lm, int /*lineLength*/, int typeW = kColType, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer ||
        lm.lineKind == LineKind::ArrayElementSeparator) return {};
    int ind = kFoldCol + lm.depth * 3;

    // Hex/Padding uses nameW for ASCII column (same as regular name column)
    bool isHexPad = isHexPreview(lm.nodeKind);
    int valWidth = isHexPad ? 23 : kColValue;

    int prefixW = typeW + nameW + 2 * kSepWidth;

    if (lm.isContinuation) {
        int start = ind + prefixW;
        return {start, start + valWidth, true};
    }
    if (lm.lineKind != LineKind::Field) return {};

    int start = ind + prefixW;
    return {start, start + valWidth, true};
}

inline ColumnSpan commentSpanFor(const LineMeta& lm, int lineLength, int typeW = kColType, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer) return {};
    int ind = kFoldCol + lm.depth * 3;

    bool isHexPad = isHexPreview(lm.nodeKind);
    int valWidth = isHexPad ? 23 : kColValue;

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
    int idx = lineText.indexOf(QStringLiteral(" \u00B7"));
    if (idx < 0) return {};
    int start = 0;
    while (start < idx && !lineText[start].isLetterOrNumber()
           && lineText[start] != '<' && lineText[start] != '\'') start++;
    if (start >= idx) return {};
    // Exclude trailing ▾ from the editable span
    int end = idx;
    while (end > start && lineText[end - 1] == QChar(0x25BE)) end--;
    if (end <= start) return {};
    return {start, end, true};
}

inline ColumnSpan commandRowAddrSpan(const QString& lineText) {
    int tag = lineText.indexOf(QStringLiteral(" \u00B7"));
    if (tag < 0) return {};
    int start = tag + 3;  // after " · "
    int end = start;
    while (end < lineText.size() && !lineText[end].isSpace()) end++;
    if (end <= start) return {};
    return {start, end, true};
}

// ── CommandRow root-class spans ──
// Combined CommandRow format ends with: "  struct▾ ClassName {"

inline int commandRowRootStart(const QString& lineText) {
    int best = -1;
    int i;
    i = lineText.lastIndexOf(QStringLiteral("struct\u25BE"));
    if (i > best) best = i;
    i = lineText.lastIndexOf(QStringLiteral("class\u25BE"));
    if (i > best) best = i;
    i = lineText.lastIndexOf(QStringLiteral("enum\u25BE"));
    if (i > best) best = i;
    return best;
}

inline ColumnSpan commandRowRootTypeSpan(const QString& lineText) {
    int start = commandRowRootStart(lineText);
    if (start < 0) return {};
    int end = start;
    while (end < lineText.size() && lineText[end] != QChar(' ')
           && lineText[end] != QChar(0x25BE)) end++;
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
    int ind = kFoldCol + lm.depth * 3;
    // Find '[' in the type portion
    int bracket = lineText.indexOf('[', ind);
    if (bracket <= ind) return {};
    return {ind, bracket, true};
}

inline ColumnSpan arrayElemCountSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * 3;
    int openBracket = lineText.indexOf('[', ind);
    int closeBracket = lineText.indexOf(']', openBracket);
    if (openBracket < 0 || closeBracket < 0 || closeBracket <= openBracket + 1) return {};
    return {openBracket + 1, closeBracket, true};
}

// Click-area version: includes brackets [N] for hit testing
inline ColumnSpan arrayElemCountClickSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * 3;
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
    int ind = kFoldCol + lm.depth * 3;
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
                        const QString& typeOverride = {});
    QString fmtOffsetMargin(uint64_t absoluteOffset, bool isContinuation, int hexDigits = 8);
    QString fmtStructHeader(const Node& node, int depth, bool collapsed, int colType = kColType, int colName = kColName);
    QString fmtStructFooter(const Node& node, int depth, int totalSize = -1);
    QString fmtArrayHeader(const Node& node, int depth, int viewIdx, bool collapsed, int colType = kColType, int colName = kColName, const QString& elemStructName = {});
    QString structTypeName(const Node& node);  // Full type string for struct headers
    QString arrayTypeName(NodeKind elemKind, int count, const QString& structName = {});
    QString pointerTypeName(NodeKind kind, const QString& targetName);
    QString fmtPointerHeader(const Node& node, int depth, bool collapsed,
                             const Provider& prov, uint64_t addr,
                             const QString& ptrTypeName, int colType = kColType, int colName = kColName);
    QString validateBaseAddress(const QString& text);
    QString indent(int depth);
    QString readValue(const Node& node, const Provider& prov,
                      uint64_t addr, int subLine);
    QString editableValue(const Node& node, const Provider& prov,
                          uint64_t addr, int subLine);
    QByteArray parseValue(NodeKind kind, const QString& text, bool* ok);
    QByteArray parseAsciiValue(const QString& text, int expectedSize, bool* ok);
    QString validateValue(NodeKind kind, const QString& text);
} // namespace fmt

// ── Compose function forward declaration ──

ComposeResult compose(const NodeTree& tree, const Provider& prov, uint64_t viewRootId = 0);

} // namespace rcx
