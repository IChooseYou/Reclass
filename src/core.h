#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QSet>
#include <cstdint>
#include <memory>
#include <variant>

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
    {NodeKind::Hex8,      "Hex8",      "Hex8",        1,  1,  1, KF_HexPreview},
    {NodeKind::Hex16,     "Hex16",     "Hex16",       2,  1,  2, KF_HexPreview},
    {NodeKind::Hex32,     "Hex32",     "Hex32",       4,  1,  4, KF_HexPreview},
    {NodeKind::Hex64,     "Hex64",     "Hex64",       8,  1,  8, KF_HexPreview},
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
    {NodeKind::Vec2,      "Vec2",      "Vec2",        8,  2,  4, KF_Vector},
    {NodeKind::Vec3,      "Vec3",      "Vec3",       12,  3,  4, KF_Vector},
    {NodeKind::Vec4,      "Vec4",      "Vec4",       16,  4,  4, KF_Vector},
    {NodeKind::Mat4x4,    "Mat4x4",    "Mat4x4",     64,  4,  4, KF_None},
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
};

// ── Provider interface ──

class Provider {
public:
    virtual ~Provider() = default;
    virtual uint8_t  readU8 (uint64_t addr) const = 0;
    virtual uint16_t readU16(uint64_t addr) const = 0;
    virtual uint32_t readU32(uint64_t addr) const = 0;
    virtual uint64_t readU64(uint64_t addr) const = 0;
    virtual float    readF32(uint64_t addr) const = 0;
    virtual double   readF64(uint64_t addr) const = 0;
    virtual QByteArray readBytes(uint64_t addr, int len) const = 0;
    virtual bool     isValid() const = 0;
    virtual bool     isReadable(uint64_t addr, int len) const = 0;
    virtual int      size() const = 0;
    virtual bool     isWritable() const { return false; }
    virtual bool     writeBytes(uint64_t addr, const QByteArray& data) {
        Q_UNUSED(addr); Q_UNUSED(data); return false;
    }
};

class FileProvider : public Provider {
    QByteArray m_data;

    template<class T>
    T readT(uint64_t a) const {
        if (a + sizeof(T) > (uint64_t)m_data.size()) return T{};
        T v; memcpy(&v, m_data.data() + a, sizeof(T)); return v;
    }

public:
    explicit FileProvider(const QByteArray& data) : m_data(data) {}
    static FileProvider fromFile(const QString& path) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) return FileProvider(f.readAll());
        return FileProvider({});
    }

    bool isValid() const override { return !m_data.isEmpty(); }
    bool isReadable(uint64_t addr, int len) const override {
        if (len <= 0) return len == 0;
        if (addr > (uint64_t)m_data.size()) return false;
        return (uint64_t)len <= (uint64_t)m_data.size() - addr;
    }
    int  size()    const override { return m_data.size(); }

    uint8_t  readU8 (uint64_t a) const override { return readT<uint8_t>(a); }
    uint16_t readU16(uint64_t a) const override { return readT<uint16_t>(a); }
    uint32_t readU32(uint64_t a) const override { return readT<uint32_t>(a); }
    uint64_t readU64(uint64_t a) const override { return readT<uint64_t>(a); }
    float    readF32(uint64_t a) const override { return readT<float>(a); }
    double   readF64(uint64_t a) const override { return readT<double>(a); }

    QByteArray readBytes(uint64_t a, int len) const override {
        if (a >= (uint64_t)m_data.size()) return {};
        int avail = qMin(len, (int)((uint64_t)m_data.size() - a));
        return m_data.mid((int)a, avail);
    }

    bool isWritable() const override { return true; }
    bool writeBytes(uint64_t addr, const QByteArray& data) override {
        if (addr + data.size() > (uint64_t)m_data.size()) return false;
        memcpy(m_data.data() + addr, data.data(), data.size());
        return true;
    }
};

class NullProvider : public Provider {
public:
    uint8_t  readU8 (uint64_t) const override { return 0; }
    uint16_t readU16(uint64_t) const override { return 0; }
    uint32_t readU32(uint64_t) const override { return 0; }
    uint64_t readU64(uint64_t) const override { return 0; }
    float    readF32(uint64_t) const override { return 0.0f; }
    double   readF64(uint64_t) const override { return 0.0; }
    QByteArray readBytes(uint64_t, int) const override { return {}; }
    bool     isValid() const override { return false; }
    bool     isReadable(uint64_t, int) const override { return false; }
    int      size()    const override { return 0; }
};

// ── Node ──

struct Node {
    uint64_t id         = 0;
    NodeKind kind       = NodeKind::Hex8;
    QString  name;
    uint64_t parentId   = 0;   // 0 = root (no parent)
    int      offset     = 0;
    int      arrayLen   = 0;
    int      strLen     = 64;
    bool     collapsed  = false;
    uint64_t refId      = 0;       // Pointer32/64: id of Struct to expand at *ptr

    int byteSize() const {
        switch (kind) {
        case NodeKind::UTF8:    return strLen;
        case NodeKind::UTF16:   return strLen * 2;
        case NodeKind::Padding: return qMax(1, arrayLen);
        default: return sizeForKind(kind);
        }
    }

    QJsonObject toJson() const {
        QJsonObject o;
        o["id"]        = QString::number(id);
        o["kind"]      = kindToString(kind);
        o["name"]      = name;
        o["parentId"]  = QString::number(parentId);
        o["offset"]    = offset;
        o["arrayLen"]  = arrayLen;
        o["strLen"]    = strLen;
        o["collapsed"] = collapsed;
        o["refId"]     = QString::number(refId);
        return o;
    }
    static Node fromJson(const QJsonObject& o) {
        Node n;
        n.id        = o["id"].toString("0").toULongLong();
        n.kind      = kindFromString(o["kind"].toString());
        n.name      = o["name"].toString();
        n.parentId  = o["parentId"].toString("0").toULongLong();
        n.offset    = o["offset"].toInt(0);
        n.arrayLen  = o["arrayLen"].toInt(0);
        n.strLen    = o["strLen"].toInt(64);
        n.collapsed = o["collapsed"].toBool(false);
        n.refId     = o["refId"].toString("0").toULongLong();
        return n;
    }
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
        nodes.append(copy);
        m_idCache.clear();
        return nodes.size() - 1;
    }

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
        QSet<int> visited;
        int cur = idx;
        while (cur >= 0 && cur < nodes.size() && nodes[cur].parentId != 0) {
            if (visited.contains(cur)) break;
            visited.insert(cur);
            cur = indexOfId(nodes[cur].parentId);
            if (cur < 0) break;
            d++;
        }
        return d;
    }

    int64_t computeOffset(int idx) const {
        int64_t total = 0;
        QSet<int> visited;
        int cur = idx;
        while (cur >= 0 && cur < nodes.size()) {
            if (visited.contains(cur)) break;
            visited.insert(cur);
            total += nodes[cur].offset;
            if (nodes[cur].parentId == 0) break;
            cur = indexOfId(nodes[cur].parentId);
        }
        return total;
    }

    int structSpan(uint64_t structId,
                   const QHash<uint64_t, QVector<int>>* childMap = nullptr) const {
        int maxEnd = 0;
        QVector<int> kids = childMap ? childMap->value(structId) : childrenOf(structId);
        for (int ci : kids) {
            const Node& c = nodes[ci];
            int sz = (c.kind == NodeKind::Struct || c.kind == NodeKind::Array)
                ? structSpan(c.id, childMap) : c.byteSize();
            int end = c.offset + sz;
            if (end > maxEnd) maxEnd = end;
        }
        return maxEnd;
    }

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
    Header, Field, Continuation, Footer
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
    LineKind lineKind       = LineKind::Field;
    NodeKind nodeKind       = NodeKind::Int32;
    QString  offsetText;
    uint32_t markerMask     = 0;
};

// ── Layout Info ──

struct LayoutInfo {
    int nameW = 22;  // Effective name column width (default = kColName)
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
    struct Insert      { Node node; };
    struct Remove      { uint64_t nodeId; QVector<Node> subtree;
                         QVector<OffsetAdj> offAdjs; };
    struct ChangeBase  { uint64_t oldBase, newBase; };
    struct WriteBytes  { uint64_t addr; QByteArray oldBytes, newBytes; };
}

using Command = std::variant<
    cmd::ChangeKind, cmd::Rename, cmd::Collapse,
    cmd::Insert, cmd::Remove, cmd::ChangeBase, cmd::WriteBytes
>;

// ── Column spans (for inline editing) ──

struct ColumnSpan {
    int  start = 0;   // inclusive column index
    int  end   = 0;   // exclusive column index
    bool valid = false;
};

enum class EditTarget { Name, Type, Value, BaseAddress };

// Column layout constants (shared with format.cpp span computation)
inline constexpr int kFoldCol     = 3;   // 3-char fold indicator prefix per line
inline constexpr int kColType     = 10;
inline constexpr int kColName     = 22;
inline constexpr int kColValue    = 32;
inline constexpr int kColComment  = 28;  // "// Enter=Save Esc=Cancel" fits
inline constexpr int kColBaseAddr = 12;  // "0x" + up to 10 hex digits (40-bit address)
inline constexpr int kSepWidth    = 2;
inline constexpr int kMinNameW    = 8;   // Minimum name column width (matches ASCII preview)
inline constexpr int kMaxNameW    = 22;  // Maximum name column width (= kColName)

inline ColumnSpan typeSpanFor(const LineMeta& lm) {
    if (lm.lineKind != LineKind::Field || lm.isContinuation) return {};
    int ind = kFoldCol + lm.depth * 3;
    return {ind, ind + kColType, true};
}

inline ColumnSpan nameSpanFor(const LineMeta& lm, int nameW = kColName) {
    if (lm.isContinuation || lm.lineKind != LineKind::Field) return {};

    int ind = kFoldCol + lm.depth * 3;
    int start = ind + kColType + kSepWidth;

    // Hex/Padding: ASCII preview takes the name column position (8 chars)
    if (isHexPreview(lm.nodeKind))
        return {start, start + 8, true};

    return {start, start + nameW, true};
}

inline ColumnSpan valueSpanFor(const LineMeta& lm, int /*lineLength*/, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer) return {};
    int ind = kFoldCol + lm.depth * 3;

    // Hex/Padding layout: [Type][sep][ASCII(8)][sep][hex bytes(23)]
    bool isHexPad = isHexPreview(lm.nodeKind);
    int valWidth = isHexPad ? 23 : kColValue;  // hex bytes or value column

    if (lm.isContinuation) {
        int prefixW = isHexPad
            ? (kColType + kSepWidth + 8 + kSepWidth)
            : (kColType + nameW + 4);
        int start = ind + prefixW;
        return {start, start + valWidth, true};
    }
    if (lm.lineKind != LineKind::Field) return {};

    int start = isHexPad
        ? (ind + kColType + kSepWidth + 8 + kSepWidth)
        : (ind + kColType + kSepWidth + nameW + kSepWidth);
    return {start, start + valWidth, true};
}

inline ColumnSpan commentSpanFor(const LineMeta& lm, int lineLength, int nameW = kColName) {
    if (lm.lineKind == LineKind::Header || lm.lineKind == LineKind::Footer) return {};
    int ind = kFoldCol + lm.depth * 3;

    bool isHexPad = isHexPreview(lm.nodeKind);
    int valWidth = isHexPad ? 23 : kColValue;

    int start;
    if (lm.isContinuation) {
        int prefixW = isHexPad
            ? (kColType + kSepWidth + 8 + kSepWidth)
            : (kColType + nameW + 4);
        start = ind + prefixW + valWidth;
    } else {
        start = isHexPad
            ? (ind + kColType + kSepWidth + 8 + kSepWidth + valWidth)
            : (ind + kColType + kSepWidth + nameW + kSepWidth + valWidth);
    }
    return {start, lineLength, start < lineLength};
}

// Base address span (only valid for root struct headers)
// Line format: " - struct Name { base: 0x00400000"
inline ColumnSpan baseAddressSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isRootHeader) return {};
    // Find "base: " after the opening brace
    int baseIdx = lineText.indexOf(QStringLiteral("base: "));
    if (baseIdx < 0) return {};
    int startPos = baseIdx + 6;  // after "base: "
    // Value goes to end of line
    int endPos = lineText.size();
    while (endPos > startPos && lineText[endPos-1].isSpace())
        endPos--;
    if (endPos <= startPos) return {};
    return {startPos, endPos, true};
}

// Full "base: 0x..." span for coloring (includes "base: " prefix)
inline ColumnSpan baseAddressFullSpanFor(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isRootHeader) return {};
    int baseIdx = lineText.indexOf(QStringLiteral("base: "));
    if (baseIdx < 0) return {};
    int endPos = lineText.size();
    while (endPos > baseIdx && lineText[endPos-1].isSpace())
        endPos--;
    return {baseIdx, endPos, true};
}

// ── ViewState ──

struct ViewState {
    int scrollLine = 0;
    int cursorLine = 0;
    int cursorCol  = 0;
};

// ── Format function forward declarations ──

namespace fmt {
    using TypeNameFn = QString (*)(NodeKind);
    void setTypeNameProvider(TypeNameFn fn);
    QString typeName(NodeKind kind);
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
                        const QString& comment = {}, int colName = kColName);
    QString fmtOffsetMargin(int64_t relativeOffset, bool isContinuation);
    QString fmtStructHeader(const Node& node, int depth);
    QString fmtStructHeaderWithBase(const Node& node, int depth, uint64_t baseAddress);
    QString fmtStructFooter(const Node& node, int depth, int totalSize = -1);
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

ComposeResult compose(const NodeTree& tree, const Provider& prov);

} // namespace rcx
