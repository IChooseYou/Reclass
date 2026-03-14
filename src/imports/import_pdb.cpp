#include "import_pdb.h"

#ifdef _WIN32

#include <windows.h>
#include <QFile>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QDebug>

// ── RawPDB headers ──
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_TPIStream.h"
#include "PDB_TPITypes.h"
#include "PDB_DBIStream.h"
#include "PDB_InfoStream.h"
#include "PDB_CoalescedMSFStream.h"
#include "Foundation/PDB_Memory.h"

namespace rcx {

// ── Memory-mapped file (mirrors ExampleMemoryMappedFile) ──

struct MappedFile {
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = nullptr;
    const void* base = nullptr;
    size_t size = 0;

    bool open(const QString& path) {
        hFile = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
                            GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!hMapping) { close(); return false; }

        base = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        if (!base) { close(); return false; }

        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(hFile, &info)) { close(); return false; }
        size = (static_cast<size_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        return true;
    }

    void close() {
        if (base) { UnmapViewOfFile(base); base = nullptr; }
        if (hMapping) { CloseHandle(hMapping); hMapping = nullptr; }
        if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; }
        size = 0;
    }

    ~MappedFile() { close(); }
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
};

// ── TypeTable (mirrors ExampleTypeTable) ──
// Builds an O(1) lookup table from type index → record pointer.

class TypeTable {
public:
    explicit TypeTable(const PDB::TPIStream& tpiStream) {
        m_firstIndex = tpiStream.GetFirstTypeIndex();
        m_lastIndex = tpiStream.GetLastTypeIndex();
        m_count = tpiStream.GetTypeRecordCount();

        const PDB::DirectMSFStream& ds = tpiStream.GetDirectMSFStream();
        m_stream = PDB::CoalescedMSFStream(ds, ds.GetSize(), 0u);

        m_records = PDB_NEW_ARRAY(const PDB::CodeView::TPI::Record*, m_count);
        uint32_t idx = 0;
        tpiStream.ForEachTypeRecordHeaderAndOffset(
            [this, &idx](const PDB::CodeView::TPI::RecordHeader&, size_t offset) {
                m_records[idx++] = m_stream.GetDataAtOffset<const PDB::CodeView::TPI::Record>(offset);
            });
    }

    ~TypeTable() { PDB_DELETE_ARRAY(m_records); }

    uint32_t firstIndex() const { return m_firstIndex; }
    uint32_t lastIndex()  const { return m_lastIndex; }
    size_t   count()      const { return m_count; }

    const PDB::CodeView::TPI::Record* get(uint32_t typeIndex) const {
        if (typeIndex < m_firstIndex || typeIndex >= m_lastIndex) return nullptr;
        return m_records[typeIndex - m_firstIndex];
    }

private:
    uint32_t m_firstIndex = 0;
    uint32_t m_lastIndex = 0;
    size_t   m_count = 0;
    const PDB::CodeView::TPI::Record** m_records = nullptr;
    PDB::CoalescedMSFStream m_stream;

    TypeTable(const TypeTable&) = delete;
    TypeTable& operator=(const TypeTable&) = delete;
};

// ── Leaf numeric helpers (variable-length integer encoding) ──

using TRK = PDB::CodeView::TPI::TypeRecordKind;

static uint8_t leafSize(TRK kind) {
    if (kind < TRK::LF_NUMERIC) return sizeof(TRK); // value is the kind itself
    switch (kind) {
    case TRK::LF_CHAR:   return sizeof(TRK) + sizeof(uint8_t);
    case TRK::LF_SHORT:
    case TRK::LF_USHORT: return sizeof(TRK) + sizeof(uint16_t);
    case TRK::LF_LONG:
    case TRK::LF_ULONG:  return sizeof(TRK) + sizeof(uint32_t);
    case TRK::LF_QUADWORD:
    case TRK::LF_UQUADWORD: return sizeof(TRK) + sizeof(uint64_t);
    default: return sizeof(TRK);
    }
}

static const char* leafName(const char* data, TRK kind) {
    return data + leafSize(kind);
}

static uint64_t leafValue(const char* data, TRK kind) {
    if (kind < TRK::LF_NUMERIC) {
        return static_cast<uint16_t>(kind);
    }
    const char* p = data + sizeof(TRK);
    switch (kind) {
    case TRK::LF_CHAR:      return *reinterpret_cast<const uint8_t*>(p);
    case TRK::LF_SHORT:     return *reinterpret_cast<const int16_t*>(p);
    case TRK::LF_USHORT:    return *reinterpret_cast<const uint16_t*>(p);
    case TRK::LF_LONG:      return *reinterpret_cast<const int32_t*>(p);
    case TRK::LF_ULONG:     return *reinterpret_cast<const uint32_t*>(p);
    case TRK::LF_QUADWORD:  return *reinterpret_cast<const int64_t*>(p);
    case TRK::LF_UQUADWORD: return *reinterpret_cast<const uint64_t*>(p);
    default: return 0;
    }
}

// ── Primitive type index mapping (< 0x1000) ──

static NodeKind mapPrimitiveType(uint32_t typeIndex) {
    uint32_t base = typeIndex & 0xFF;
    switch (base) {
    // void
    case 0x03: return NodeKind::Hex8;
    // signed char
    case 0x10: return NodeKind::Int8;
    // unsigned char
    case 0x20: return NodeKind::UInt8;
    // real char
    case 0x70: return NodeKind::Int8;
    // wchar
    case 0x71: return NodeKind::UInt16;
    // char8
    case 0x7c: return NodeKind::UInt8;
    // char16
    case 0x7a: return NodeKind::UInt16;
    // char32
    case 0x7b: return NodeKind::UInt32;
    // short
    case 0x11: return NodeKind::Int16;
    // ushort
    case 0x21: return NodeKind::UInt16;
    // long
    case 0x12: return NodeKind::Int32;
    // ulong
    case 0x22: return NodeKind::UInt32;
    // int8
    case 0x68: return NodeKind::Int8;
    // uint8
    case 0x69: return NodeKind::UInt8;
    // int16
    case 0x72: return NodeKind::Int16;
    // uint16
    case 0x73: return NodeKind::UInt16;
    // int32
    case 0x74: return NodeKind::Int32;
    // uint32
    case 0x75: return NodeKind::UInt32;
    // quad (int64)
    case 0x13: return NodeKind::Int64;
    // uquad (uint64)
    case 0x23: return NodeKind::UInt64;
    // int64
    case 0x76: return NodeKind::Int64;
    // uint64
    case 0x77: return NodeKind::UInt64;
    // float
    case 0x40: return NodeKind::Float;
    // double
    case 0x41: return NodeKind::Double;
    // bool
    case 0x30: return NodeKind::Bool;
    case 0x31: return NodeKind::UInt16; // bool16
    case 0x32: return NodeKind::UInt32; // bool32
    case 0x33: return NodeKind::UInt64; // bool64
    // HRESULT
    case 0x08: return NodeKind::UInt32;
    // bit
    case 0x60: return NodeKind::UInt8;
    // int128 / uint128 approximation
    case 0x78: return NodeKind::Hex64; // int128 → Hex64 (best we can do)
    case 0x79: return NodeKind::Hex64; // uint128
    default:   return NodeKind::Hex32;
    }
}

static NodeKind hexForSize(uint64_t len) {
    switch (len) {
    case 1: return NodeKind::Hex8;
    case 2: return NodeKind::Hex16;
    case 4: return NodeKind::Hex32;
    case 8: return NodeKind::Hex64;
    default: return NodeKind::Hex32;
    }
}

// ── Helper: read the leaf kind from the start of LF_UNION.data ──
// (LF_UNION lacks the lfEasy member that LF_CLASS has)
static TRK unionLeafKind(const char* data) {
    return *reinterpret_cast<const TRK*>(data);
}

// ── Import context ──

struct PdbCtx {
    NodeTree tree;
    const TypeTable* tt = nullptr;
    QHash<uint32_t, uint64_t> typeCache; // typeIndex → nodeId
    QHash<QString, uint32_t> structDefByName; // struct/class definition name → typeIndex
    QHash<QString, uint32_t> unionDefByName;  // union definition name → typeIndex
    bool udtDefIndexBuilt = false;

    uint64_t importUDT(uint32_t typeIndex);
    uint64_t importEnum(uint32_t typeIndex);
    void importFieldList(uint32_t fieldListIndex, uint64_t parentId);
    void importMemberType(uint32_t typeIndex, int offset, const QString& name, uint64_t parentId);
    void buildUdtDefinitionIndex();
    uint32_t findUdtDefinitionIndex(TRK kind, const char* typeName);

    // Resolve LF_MODIFIER chain to underlying type index
    uint32_t unwrapModifier(uint32_t typeIndex) const {
        if (typeIndex < tt->firstIndex()) return typeIndex;
        const auto* rec = tt->get(typeIndex);
        if (!rec) return typeIndex;
        if (rec->header.kind == TRK::LF_MODIFIER)
            return rec->data.LF_MODIFIER.type;
        return typeIndex;
    }
};

void PdbCtx::buildUdtDefinitionIndex() {
    if (udtDefIndexBuilt || !tt) return;
    udtDefIndexBuilt = true;

    for (uint32_t ti = tt->firstIndex(); ti < tt->lastIndex(); ti++) {
        const auto* rec = tt->get(ti);
        if (!rec) continue;

        bool isUnion = false;
        bool isFwd = false;
        const char* candidateName = nullptr;

        if (rec->header.kind == TRK::LF_UNION) {
            isUnion = true;
            isFwd = rec->data.LF_UNION.property.fwdref;
            candidateName = leafName(rec->data.LF_UNION.data, unionLeafKind(rec->data.LF_UNION.data));
        } else if (rec->header.kind == TRK::LF_STRUCTURE || rec->header.kind == TRK::LF_CLASS) {
            isFwd = rec->data.LF_CLASS.property.fwdref;
            candidateName = leafName(rec->data.LF_CLASS.data, rec->data.LF_CLASS.lfEasy.kind);
        } else {
            continue;
        }

        if (isFwd || !candidateName || candidateName[0] == '\0') continue;

        QString qname = QString::fromUtf8(candidateName);
        QHash<QString, uint32_t>& lookup = isUnion ? unionDefByName : structDefByName;
        if (!lookup.contains(qname)) lookup.insert(qname, ti);
    }
}

uint32_t PdbCtx::findUdtDefinitionIndex(TRK kind, const char* typeName) {
    if (!typeName || typeName[0] == '\0') return 0;

    buildUdtDefinitionIndex();

    const QString qname = QString::fromUtf8(typeName);
    if (kind == TRK::LF_UNION) {
        auto it = unionDefByName.constFind(qname);
        return (it != unionDefByName.cend()) ? it.value() : 0;
    }

    if (kind == TRK::LF_STRUCTURE || kind == TRK::LF_CLASS) {
        auto it = structDefByName.constFind(qname);
        return (it != structDefByName.cend()) ? it.value() : 0;
    }

    return 0;
}

uint64_t PdbCtx::importUDT(uint32_t typeIndex) {
    if (typeIndex < tt->firstIndex()) return 0;

    auto it = typeCache.find(typeIndex);
    if (it != typeCache.end()) return it.value();

    const auto* rec = tt->get(typeIndex);
    if (!rec) return 0;

    const char* name = nullptr;
    uint32_t fieldListIndex = 0;
    uint16_t fieldCount = 0;
    bool isUnion = false;
    const char* sizeData = nullptr;

    if (rec->header.kind == TRK::LF_STRUCTURE || rec->header.kind == TRK::LF_CLASS) {
        // Skip forward references — find the definition
        if (rec->data.LF_CLASS.property.fwdref) return 0;
        fieldCount = rec->data.LF_CLASS.count;
        fieldListIndex = rec->data.LF_CLASS.field;
        sizeData = rec->data.LF_CLASS.data;
        name = leafName(sizeData, rec->data.LF_CLASS.lfEasy.kind);
    } else if (rec->header.kind == TRK::LF_UNION) {
        if (rec->data.LF_UNION.property.fwdref) return 0;
        isUnion = true;
        fieldCount = rec->data.LF_UNION.count;
        fieldListIndex = rec->data.LF_UNION.field;
        sizeData = rec->data.LF_UNION.data;
        name = leafName(sizeData, unionLeafKind(sizeData));
    } else {
        return 0;
    }
    (void)fieldCount;

    QString qname = name ? QString::fromUtf8(name) : QStringLiteral("<anon>");

    Node s;
    s.kind = NodeKind::Struct;
    s.name = qname;
    s.structTypeName = qname;
    s.classKeyword = isUnion ? QStringLiteral("union") : QStringLiteral("struct");
    s.parentId = 0;
    s.collapsed = true;
    int idx = tree.addNode(s);
    uint64_t nodeId = tree.nodes[idx].id;

    typeCache[typeIndex] = nodeId;

    importFieldList(fieldListIndex, nodeId);
    return nodeId;
}

uint64_t PdbCtx::importEnum(uint32_t typeIndex) {
    if (typeIndex < tt->firstIndex()) return 0;

    auto it = typeCache.find(typeIndex);
    if (it != typeCache.end()) return it.value();

    const auto* rec = tt->get(typeIndex);
    if (!rec || rec->header.kind != TRK::LF_ENUM) return 0;
    if (rec->data.LF_ENUM.property.fwdref) return 0;

    QString qname = rec->data.LF_ENUM.name
        ? QString::fromUtf8(rec->data.LF_ENUM.name)
        : QStringLiteral("<anon>");

    Node s;
    s.kind = NodeKind::Struct;
    s.name = qname;
    s.structTypeName = qname;
    s.classKeyword = QStringLiteral("enum");
    s.parentId = 0;
    s.collapsed = true;

    // Extract enum members from field list
    uint32_t fieldListIndex = rec->data.LF_ENUM.field;
    const auto* flRec = tt->get(fieldListIndex);
    if (flRec && flRec->header.kind == TRK::LF_FIELDLIST) {
        auto maxSize = flRec->header.size - sizeof(uint16_t);
        for (size_t i = 0; i < maxSize; ) {
            auto* field = reinterpret_cast<const PDB::CodeView::TPI::FieldList*>(
                reinterpret_cast<const uint8_t*>(&flRec->data.LF_FIELD.list) + i);
            if (field->kind != TRK::LF_ENUMERATE) break;

            int64_t val = static_cast<int64_t>(leafValue(
                field->data.LF_ENUMERATE.value,
                field->data.LF_ENUMERATE.lfEasy.kind));
            const char* eName = leafName(
                field->data.LF_ENUMERATE.value,
                field->data.LF_ENUMERATE.lfEasy.kind);
            if (eName)
                s.enumMembers.emplaceBack(QString::fromUtf8(eName), val);

            i += static_cast<size_t>(eName - reinterpret_cast<const char*>(field));
            i += strnlen(eName, maxSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
    }

    int idx = tree.addNode(s);
    uint64_t nodeId = tree.nodes[idx].id;
    typeCache[typeIndex] = nodeId;
    return nodeId;
}

void PdbCtx::importFieldList(uint32_t fieldListIndex, uint64_t parentId) {
    const auto* rec = tt->get(fieldListIndex);
    if (!rec || rec->header.kind != TRK::LF_FIELDLIST) return;

    auto maximumSize = rec->header.size - sizeof(uint16_t);
    QSet<QPair<int,int>> bitfieldSlots;
    QHash<QPair<int,int>, uint64_t> bitfieldNodeIds;

    for (size_t i = 0; i < maximumSize; ) {
        auto* field = reinterpret_cast<const PDB::CodeView::TPI::FieldList*>(
            reinterpret_cast<const uint8_t*>(&rec->data.LF_FIELD.list) + i);

        if (field->kind == TRK::LF_MEMBER) {
            // Extract offset from variable-length leaf
            uint16_t offset = 0;
            if (field->data.LF_MEMBER.lfEasy.kind < TRK::LF_NUMERIC)
                offset = *reinterpret_cast<const uint16_t*>(field->data.LF_MEMBER.offset);
            else
                offset = static_cast<uint16_t>(leafValue(field->data.LF_MEMBER.offset,
                    field->data.LF_MEMBER.lfEasy.kind));

            const char* memberName = leafName(field->data.LF_MEMBER.offset,
                                               field->data.LF_MEMBER.lfEasy.kind);
            uint32_t memberType = field->data.LF_MEMBER.index;
            QString qname = memberName ? QString::fromUtf8(memberName) : QString();

            // Check for bitfield type
            uint32_t resolvedType = unwrapModifier(memberType);
            const auto* typeRec = tt->get(resolvedType);
            if (typeRec && typeRec->header.kind == TRK::LF_BITFIELD) {
                uint32_t underlying = typeRec->data.LF_BITFIELD.type;
                uint8_t bitLen = typeRec->data.LF_BITFIELD.length;
                uint8_t bitPos = typeRec->data.LF_BITFIELD.position;

                // Determine slot size from underlying type
                uint64_t slotSize = 4;
                if (underlying < tt->firstIndex()) {
                    NodeKind k = mapPrimitiveType(underlying);
                    slotSize = sizeForKind(k);
                }

                auto key = qMakePair((int)offset, (int)slotSize);
                if (!bitfieldSlots.contains(key)) {
                    bitfieldSlots.insert(key);
                    // Create bitfield container node
                    Node n;
                    n.kind = NodeKind::Struct;
                    n.classKeyword = QStringLiteral("bitfield");
                    n.elementKind = hexForSize(slotSize);
                    n.parentId = parentId;
                    n.offset = offset;
                    n.collapsed = false;
                    int idx = tree.addNode(n);
                    bitfieldNodeIds[key] = tree.nodes[idx].id;
                }
                // Add this member to the bitfield container
                uint64_t bfNodeId = bitfieldNodeIds[key];
                int bfIdx = tree.indexOfId(bfNodeId);
                if (bfIdx >= 0) {
                    BitfieldMember bm;
                    bm.name = qname;
                    bm.bitOffset = bitPos;
                    bm.bitWidth = bitLen;
                    tree.nodes[bfIdx].bitfieldMembers.append(bm);
                }
            } else {
                importMemberType(memberType, offset, qname, parentId);
            }

            // Advance past this LF_MEMBER
            i += static_cast<size_t>(memberName - reinterpret_cast<const char*>(field));
            i += strnlen(memberName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3); // align to 4
        }
        else if (field->kind == TRK::LF_BCLASS) {
            const char* leafEnd = leafName(field->data.LF_BCLASS.offset,
                                           field->data.LF_BCLASS.lfEasy.kind);
            i += static_cast<size_t>(leafEnd - reinterpret_cast<const char*>(field));
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_VBCLASS || field->kind == TRK::LF_IVBCLASS) {
            TRK vbpKind = *reinterpret_cast<const TRK*>(field->data.LF_IVBCLASS.vbpOffset);
            uint8_t vbpSize1 = leafSize(vbpKind);
            TRK vbtKind = *reinterpret_cast<const TRK*>(field->data.LF_IVBCLASS.vbpOffset + vbpSize1);
            uint8_t vbpSize2 = leafSize(vbtKind);
            i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_VBCLASS) + vbpSize1 + vbpSize2;
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_INDEX) {
            // Continuation of field list in another record
            importFieldList(field->data.LF_INDEX.type, parentId);
            i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_INDEX);
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_VFUNCTAB) {
            i += sizeof(PDB::CodeView::TPI::FieldList::Data::LF_VFUNCTAB);
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_NESTTYPE) {
            const char* nestName = field->data.LF_NESTTYPE.name;
            i += static_cast<size_t>(nestName - reinterpret_cast<const char*>(field));
            i += strnlen(nestName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_STMEMBER) {
            const char* smName = field->data.LF_STMEMBER.name;
            i += static_cast<size_t>(smName - reinterpret_cast<const char*>(field));
            i += strnlen(smName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_METHOD) {
            const char* mName = field->data.LF_METHOD.name;
            i += static_cast<size_t>(mName - reinterpret_cast<const char*>(field));
            i += strnlen(mName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_ONEMETHOD) {
            // Determine if it has a vbaseoff field
            auto prop = static_cast<PDB::CodeView::TPI::MethodProperty>(
                field->data.LF_ONEMETHOD.attributes.mprop);
            const char* mName;
            if (prop == PDB::CodeView::TPI::MethodProperty::Intro ||
                prop == PDB::CodeView::TPI::MethodProperty::PureIntro)
                mName = reinterpret_cast<const char*>(field->data.LF_ONEMETHOD.vbaseoff) + sizeof(uint32_t);
            else
                mName = reinterpret_cast<const char*>(field->data.LF_ONEMETHOD.vbaseoff);

            i += static_cast<size_t>(mName - reinterpret_cast<const char*>(field));
            i += strnlen(mName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
        else if (field->kind == TRK::LF_ENUMERATE) {
            const char* eName = leafName(field->data.LF_ENUMERATE.value,
                                          field->data.LF_ENUMERATE.lfEasy.kind);
            i += static_cast<size_t>(eName - reinterpret_cast<const char*>(field));
            i += strnlen(eName, maximumSize - i - 1) + 1;
            i = (i + 3) & ~size_t(3);
        }
        else {
            break; // unknown field kind, stop
        }
    }
}

void PdbCtx::importMemberType(uint32_t typeIndex, int offset, const QString& name, uint64_t parentId) {
    // Handle primitive type indices (< 0x1000)
    if (typeIndex < tt->firstIndex()) {
        uint32_t ptrMode = (typeIndex >> 8) & 0xF;
        if (ptrMode == 0x04 || ptrMode == 0x05) {
            // 32-bit pointer to a base type
            Node n;
            n.kind = NodeKind::Pointer32;
            n.name = name;
            n.parentId = parentId;
            n.offset = offset;
            n.collapsed = true;
            tree.addNode(n);
            return;
        }
        if (ptrMode == 0x06) {
            // 64-bit pointer to a base type
            Node n;
            n.kind = NodeKind::Pointer64;
            n.name = name;
            n.parentId = parentId;
            n.offset = offset;
            n.collapsed = true;
            tree.addNode(n);
            return;
        }
        if (ptrMode != 0x00) {
            // Some other pointer mode (near, far, huge) — treat as 32-bit
            Node n;
            n.kind = NodeKind::Pointer32;
            n.name = name;
            n.parentId = parentId;
            n.offset = offset;
            n.collapsed = true;
            tree.addNode(n);
            return;
        }
        // Direct base type
        Node n;
        n.kind = mapPrimitiveType(typeIndex);
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        tree.addNode(n);
        return;
    }

    const auto* rec = tt->get(typeIndex);
    if (!rec) {
        Node n;
        n.kind = NodeKind::Hex32;
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        tree.addNode(n);
        return;
    }

    switch (rec->header.kind) {
    case TRK::LF_MODIFIER:
        importMemberType(rec->data.LF_MODIFIER.type, offset, name, parentId);
        break;

    case TRK::LF_POINTER: {
        uint32_t ptrSize = rec->data.LF_POINTER.attr.size;
        uint32_t pointee = rec->data.LF_POINTER.utype;

        // Unwrap modifier on pointee
        uint32_t realPointee = unwrapModifier(pointee);

        Node n;
        n.kind = (ptrSize <= 4) ? NodeKind::Pointer32 : NodeKind::Pointer64;
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        n.collapsed = true;

        // Check if pointee is a UDT
        if (realPointee >= tt->firstIndex()) {
            const auto* pointeeRec = tt->get(realPointee);
            if (pointeeRec) {
                if (pointeeRec->header.kind == TRK::LF_STRUCTURE ||
                    pointeeRec->header.kind == TRK::LF_CLASS ||
                    pointeeRec->header.kind == TRK::LF_UNION) {
                    // If this is a forward ref, search for the definition
                    uint32_t defIndex = realPointee;
                    bool isFwd = false;
                    if (pointeeRec->header.kind == TRK::LF_UNION)
                        isFwd = pointeeRec->data.LF_UNION.property.fwdref;
                    else
                        isFwd = pointeeRec->data.LF_CLASS.property.fwdref;

                    if (isFwd) {
                        const char* typeName = nullptr;
                        if (pointeeRec->header.kind == TRK::LF_UNION)
                            typeName = leafName(pointeeRec->data.LF_UNION.data, unionLeafKind(pointeeRec->data.LF_UNION.data));
                        else
                            typeName = leafName(pointeeRec->data.LF_CLASS.data,
                                               pointeeRec->data.LF_CLASS.lfEasy.kind);

                        uint32_t resolved = findUdtDefinitionIndex(pointeeRec->header.kind, typeName);
                        if (resolved != 0) defIndex = resolved;
                    }
                    // Skip anonymous pointer targets — they'd create root orphans
                    const char* ptName = nullptr;
                    const auto* defRec2 = tt->get(defIndex);
                    if (defRec2) {
                        if (defRec2->header.kind == TRK::LF_UNION)
                            ptName = leafName(defRec2->data.LF_UNION.data,
                                              unionLeafKind(defRec2->data.LF_UNION.data));
                        else if (defRec2->header.kind == TRK::LF_STRUCTURE ||
                                 defRec2->header.kind == TRK::LF_CLASS)
                            ptName = leafName(defRec2->data.LF_CLASS.data,
                                              defRec2->data.LF_CLASS.lfEasy.kind);
                    }
                    bool isAnonTarget = !ptName || ptName[0] == '<' || ptName[0] == '\0';
                    if (!isAnonTarget)
                        n.refId = importUDT(defIndex);
                } else if (pointeeRec->header.kind == TRK::LF_PROCEDURE ||
                           pointeeRec->header.kind == TRK::LF_MFUNCTION) {
                    n.kind = (ptrSize <= 4) ? NodeKind::FuncPtr32 : NodeKind::FuncPtr64;
                }
            }
        }
        tree.addNode(n);
        break;
    }

    case TRK::LF_STRUCTURE:
    case TRK::LF_CLASS:
    case TRK::LF_UNION: {
        // Embedded struct/union
        uint32_t defIndex = typeIndex;

        // Handle forward reference
        bool isFwd = false;
        if (rec->header.kind == TRK::LF_UNION)
            isFwd = rec->data.LF_UNION.property.fwdref;
        else
            isFwd = rec->data.LF_CLASS.property.fwdref;

        if (isFwd) {
            const char* typeName = nullptr;
            if (rec->header.kind == TRK::LF_UNION)
                typeName = leafName(rec->data.LF_UNION.data, unionLeafKind(rec->data.LF_UNION.data));
            else
                typeName = leafName(rec->data.LF_CLASS.data, rec->data.LF_CLASS.lfEasy.kind);

            uint32_t resolved = findUdtDefinitionIndex(rec->header.kind, typeName);
            if (resolved != 0) defIndex = resolved;
        }

        const char* typeName = nullptr;
        bool isUnion = (rec->header.kind == TRK::LF_UNION);
        if (isUnion)
            typeName = leafName(rec->data.LF_UNION.data, unionLeafKind(rec->data.LF_UNION.data));
        else
            typeName = leafName(rec->data.LF_CLASS.data, rec->data.LF_CLASS.lfEasy.kind);

        // Anonymous types: inline fields directly instead of creating root orphan
        bool isAnonymous = !typeName || typeName[0] == '<' || typeName[0] == '\0';
        if (isAnonymous) {
            // Resolve to definition if needed
            const auto* defRec = tt->get(defIndex);
            uint32_t fieldListIdx = 0;
            if (defRec) {
                if (defRec->header.kind == TRK::LF_UNION)
                    fieldListIdx = defRec->data.LF_UNION.field;
                else if (defRec->header.kind == TRK::LF_STRUCTURE ||
                         defRec->header.kind == TRK::LF_CLASS)
                    fieldListIdx = defRec->data.LF_CLASS.field;
            }
            if (fieldListIdx != 0) {
                // Create inline container (no refId, no root orphan)
                Node n;
                n.kind = NodeKind::Struct;
                n.name = name;
                n.classKeyword = isUnion ? QStringLiteral("union") : QStringLiteral("struct");
                n.parentId = parentId;
                n.offset = offset;
                n.collapsed = true;
                int idx = tree.addNode(n);
                uint64_t inlineId = tree.nodes[idx].id;
                importFieldList(fieldListIdx, inlineId);
                break;
            }
            // Fallthrough if no field list
        }

        uint64_t refId = importUDT(defIndex);

        Node n;
        n.kind = NodeKind::Struct;
        n.name = name;
        n.structTypeName = typeName ? QString::fromUtf8(typeName) : QString();
        n.classKeyword = isUnion ? QStringLiteral("union") : QStringLiteral("struct");
        n.parentId = parentId;
        n.offset = offset;
        n.refId = refId;
        n.collapsed = true;
        tree.addNode(n);
        break;
    }

    case TRK::LF_ARRAY: {
        uint32_t elemType = rec->data.LF_ARRAY.elemtype;
        uint64_t totalSize = leafValue(rec->data.LF_ARRAY.data,
            *reinterpret_cast<const TRK*>(rec->data.LF_ARRAY.data));

        // Get element size
        uint64_t elemSize = 0;
        uint32_t realElemType = unwrapModifier(elemType);
        if (realElemType < tt->firstIndex()) {
            NodeKind ek = mapPrimitiveType(realElemType);
            elemSize = sizeForKind(ek);
        } else {
            const auto* elemRec = tt->get(realElemType);
            if (elemRec) {
                if (elemRec->header.kind == TRK::LF_STRUCTURE || elemRec->header.kind == TRK::LF_CLASS) {
                    const char* sizeData = elemRec->data.LF_CLASS.data;
                    elemSize = leafValue(sizeData, elemRec->data.LF_CLASS.lfEasy.kind);
                } else if (elemRec->header.kind == TRK::LF_UNION) {
                    const char* sizeData = elemRec->data.LF_UNION.data;
                    elemSize = leafValue(sizeData, *reinterpret_cast<const TRK*>(sizeData));
                } else if (elemRec->header.kind == TRK::LF_POINTER) {
                    elemSize = elemRec->data.LF_POINTER.attr.size;
                } else if (elemRec->header.kind == TRK::LF_ENUM) {
                    // Size of enum's underlying type
                    uint32_t ut = elemRec->data.LF_ENUM.utype;
                    if (ut < tt->firstIndex()) {
                        NodeKind ek = mapPrimitiveType(ut);
                        elemSize = sizeForKind(ek);
                    } else {
                        elemSize = 4;
                    }
                } else if (elemRec->header.kind == TRK::LF_ARRAY) {
                    // Nested array — get total size
                    elemSize = leafValue(elemRec->data.LF_ARRAY.data,
                        *reinterpret_cast<const TRK*>(elemRec->data.LF_ARRAY.data));
                }
            }
        }

        int count = (elemSize > 0) ? static_cast<int>(totalSize / elemSize) : 1;

        Node n;
        n.kind = NodeKind::Array;
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        n.arrayLen = count;

        // Determine element kind
        if (realElemType < tt->firstIndex()) {
            n.elementKind = mapPrimitiveType(realElemType);
        } else {
            const auto* elemRec = tt->get(realElemType);
            if (elemRec) {
                if (elemRec->header.kind == TRK::LF_STRUCTURE ||
                    elemRec->header.kind == TRK::LF_CLASS ||
                    elemRec->header.kind == TRK::LF_UNION) {
                    n.elementKind = NodeKind::Struct;
                    n.refId = importUDT(realElemType);
                    const char* tn = nullptr;
                    if (elemRec->header.kind == TRK::LF_UNION)
                        tn = leafName(elemRec->data.LF_UNION.data, unionLeafKind(elemRec->data.LF_UNION.data));
                    else
                        tn = leafName(elemRec->data.LF_CLASS.data, elemRec->data.LF_CLASS.lfEasy.kind);
                    if (tn) n.structTypeName = QString::fromUtf8(tn);
                } else if (elemRec->header.kind == TRK::LF_POINTER) {
                    uint32_t sz = elemRec->data.LF_POINTER.attr.size;
                    n.elementKind = (sz <= 4) ? NodeKind::Pointer32 : NodeKind::Pointer64;
                } else {
                    n.elementKind = hexForSize(elemSize);
                }
            }
        }
        tree.addNode(n);
        break;
    }

    case TRK::LF_ENUM: {
        // Map enum to its underlying integer type, link to enum definition
        uint32_t utype = rec->data.LF_ENUM.utype;
        uint64_t enumNodeId = importEnum(typeIndex);
        Node n;
        if (utype < tt->firstIndex()) {
            n.kind = mapPrimitiveType(utype);
        } else {
            n.kind = NodeKind::UInt32; // fallback
        }
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        n.refId = enumNodeId;
        tree.addNode(n);
        break;
    }

    case TRK::LF_PROCEDURE:
    case TRK::LF_MFUNCTION: {
        Node n;
        n.kind = NodeKind::Hex64;
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        tree.addNode(n);
        break;
    }

    case TRK::LF_BITFIELD: {
        uint32_t underlying = rec->data.LF_BITFIELD.type;
        uint8_t bitLen = rec->data.LF_BITFIELD.length;
        uint8_t bitPos = rec->data.LF_BITFIELD.position;
        uint64_t slotSize = 4;
        if (underlying < tt->firstIndex()) {
            NodeKind k = mapPrimitiveType(underlying);
            slotSize = sizeForKind(k);
        }
        Node n;
        n.kind = NodeKind::Struct;
        n.classKeyword = QStringLiteral("bitfield");
        n.elementKind = hexForSize(slotSize);
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        n.bitfieldMembers.push_back(BitfieldMember{name, bitPos, bitLen});
        tree.addNode(n);
        break;
    }

    default: {
        // Unknown complex type — emit as Hex32
        Node n;
        n.kind = NodeKind::Hex32;
        n.name = name;
        n.parentId = parentId;
        n.offset = offset;
        tree.addNode(n);
        break;
    }
    }
}

// ── Helper: open PDB and build type table ──

struct PdbFile {
    MappedFile mapped;
    PDB::RawFile* rawFile = nullptr;
    PDB::TPIStream* tpiStream = nullptr;
    TypeTable* typeTable = nullptr;

    ~PdbFile() {
        delete typeTable;
        delete tpiStream;
        delete rawFile;
    }

    bool open(const QString& pdbPath, QString* errorMsg) {
        auto setErr = [&](const QString& msg) { if (errorMsg) *errorMsg = msg; };

        if (!QFile::exists(pdbPath)) {
            setErr(QStringLiteral("PDB file not found: ") + pdbPath);
            return false;
        }

        if (!mapped.open(pdbPath)) {
            setErr(QStringLiteral("Failed to memory-map PDB file: ") + pdbPath);
            return false;
        }

        if (PDB::ValidateFile(mapped.base, mapped.size) != PDB::ErrorCode::Success) {
            setErr(QStringLiteral("Invalid PDB file: ") + pdbPath);
            return false;
        }

        rawFile = new PDB::RawFile(PDB::CreateRawFile(mapped.base));

        if (PDB::HasValidTPIStream(*rawFile) != PDB::ErrorCode::Success) {
            setErr(QStringLiteral("PDB has no valid TPI stream: ") + pdbPath);
            return false;
        }

        tpiStream = new PDB::TPIStream(PDB::CreateTPIStream(*rawFile));
        typeTable = new TypeTable(*tpiStream);
        return true;
    }
};

// ── Public API: extractPdbSymbols ──

PdbSymbolResult extractPdbSymbols(const QString& pdbPath, QString* errorMsg) {
    auto setErr = [&](const QString& msg) { if (errorMsg) *errorMsg = msg; };

    MappedFile mapped;
    if (!QFile::exists(pdbPath)) {
        setErr(QStringLiteral("PDB file not found: ") + pdbPath);
        return {};
    }
    if (!mapped.open(pdbPath)) {
        setErr(QStringLiteral("Failed to memory-map PDB file: ") + pdbPath);
        return {};
    }
    if (PDB::ValidateFile(mapped.base, mapped.size) != PDB::ErrorCode::Success) {
        setErr(QStringLiteral("Invalid PDB file: ") + pdbPath);
        return {};
    }

    PDB::RawFile rawFile = PDB::CreateRawFile(mapped.base);
    if (PDB::HasValidDBIStream(rawFile) != PDB::ErrorCode::Success) {
        setErr(QStringLiteral("PDB has no valid DBI stream: ") + pdbPath);
        return {};
    }

    const PDB::DBIStream dbiStream = PDB::CreateDBIStream(rawFile);

    // Validate required sub-streams
    if (dbiStream.HasValidSymbolRecordStream(rawFile) != PDB::ErrorCode::Success ||
        dbiStream.HasValidPublicSymbolStream(rawFile) != PDB::ErrorCode::Success ||
        dbiStream.HasValidImageSectionStream(rawFile) != PDB::ErrorCode::Success) {
        setErr(QStringLiteral("PDB DBI stream missing required sub-streams"));
        return {};
    }

    const PDB::ImageSectionStream imageSectionStream = dbiStream.CreateImageSectionStream(rawFile);
    const PDB::CoalescedMSFStream symbolRecordStream = dbiStream.CreateSymbolRecordStream(rawFile);

    PdbSymbolResult result;

    // Derive module name from PDB filename (e.g. "ntoskrnl.pdb" → "ntoskrnl")
    QFileInfo fi(pdbPath);
    result.moduleName = fi.completeBaseName();

    // Read public symbols (S_PUB32)
    const PDB::PublicSymbolStream publicSymbolStream = dbiStream.CreatePublicSymbolStream(rawFile);
    {
        const PDB::ArrayView<PDB::HashRecord> hashRecords = publicSymbolStream.GetRecords();
        const size_t count = hashRecords.GetLength();
        result.symbols.reserve(static_cast<int>(count));

        for (const PDB::HashRecord& hashRecord : hashRecords) {
            const PDB::CodeView::DBI::Record* record =
                publicSymbolStream.GetRecord(symbolRecordStream, hashRecord);
            if (record->header.kind != PDB::CodeView::DBI::SymbolRecordKind::S_PUB32)
                continue;

            const uint32_t rva = imageSectionStream.ConvertSectionOffsetToRVA(
                record->data.S_PUB32.section, record->data.S_PUB32.offset);
            if (rva == 0u)
                continue;

            result.symbols.push_back(PdbSymbol{QString::fromUtf8(record->data.S_PUB32.name), rva});
        }
    }

    // Read global symbols (S_GDATA32, S_GTHREAD32, S_LDATA32, S_LTHREAD32, S_GPROC32, S_LPROC32)
    if (dbiStream.HasValidGlobalSymbolStream(rawFile) == PDB::ErrorCode::Success) {
        const PDB::GlobalSymbolStream globalSymbolStream = dbiStream.CreateGlobalSymbolStream(rawFile);
        const PDB::ArrayView<PDB::HashRecord> hashRecords = globalSymbolStream.GetRecords();

        result.symbols.reserve(result.symbols.size() + static_cast<int>(hashRecords.GetLength()));

        for (const PDB::HashRecord& hashRecord : hashRecords) {
            const PDB::CodeView::DBI::Record* record =
                globalSymbolStream.GetRecord(symbolRecordStream, hashRecord);

            const char* name = nullptr;
            uint32_t rva = 0u;

            if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_GDATA32) {
                name = record->data.S_GDATA32.name;
                rva = imageSectionStream.ConvertSectionOffsetToRVA(
                    record->data.S_GDATA32.section, record->data.S_GDATA32.offset);
            } else if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_GTHREAD32) {
                name = record->data.S_GTHREAD32.name;
                rva = imageSectionStream.ConvertSectionOffsetToRVA(
                    record->data.S_GTHREAD32.section, record->data.S_GTHREAD32.offset);
            } else if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_LDATA32) {
                name = record->data.S_LDATA32.name;
                rva = imageSectionStream.ConvertSectionOffsetToRVA(
                    record->data.S_LDATA32.section, record->data.S_LDATA32.offset);
            } else if (record->header.kind == PDB::CodeView::DBI::SymbolRecordKind::S_LTHREAD32) {
                name = record->data.S_LTHREAD32.name;
                rva = imageSectionStream.ConvertSectionOffsetToRVA(
                    record->data.S_LTHREAD32.section, record->data.S_LTHREAD32.offset);
            }

            if (rva == 0u)
                continue;
            if (!name || name[0] == '\0')
                continue;

            result.symbols.push_back(PdbSymbol{QString::fromUtf8(name), rva});
        }
    }

    qDebug() << "[PDB] extractPdbSymbols:" << result.symbols.size() << "symbols from"
             << result.moduleName;
    return result;
}

// ── Public API: enumeratePdbTypes ──

QVector<PdbTypeInfo> enumeratePdbTypes(const QString& pdbPath, QString* errorMsg) {
    PdbFile pdb;
    if (!pdb.open(pdbPath, errorMsg)) return {};

    const TypeTable& tt = *pdb.typeTable;
    QVector<PdbTypeInfo> result;

    for (uint32_t ti = tt.firstIndex(); ti < tt.lastIndex(); ti++) {
        const auto* rec = tt.get(ti);
        if (!rec) continue;

        bool isUDT = (rec->header.kind == TRK::LF_STRUCTURE ||
                      rec->header.kind == TRK::LF_CLASS ||
                      rec->header.kind == TRK::LF_UNION);
        bool isEnum = (rec->header.kind == TRK::LF_ENUM);
        if (!isUDT && !isEnum) continue;

        const char* name = nullptr;
        uint16_t fieldCount = 0;
        bool isUnion = false;
        uint64_t size = 0;

        if (isEnum) {
            if (rec->data.LF_ENUM.property.fwdref) continue;
            fieldCount = rec->data.LF_ENUM.count;
            name = rec->data.LF_ENUM.name;
            // Size from underlying type
            uint32_t ut = rec->data.LF_ENUM.utype;
            if (ut < tt.firstIndex()) {
                NodeKind ek = mapPrimitiveType(ut);
                size = sizeForKind(ek);
            } else {
                size = 4;
            }
        } else if (rec->header.kind == TRK::LF_UNION) {
            if (rec->data.LF_UNION.property.fwdref) continue;
            isUnion = true;
            fieldCount = rec->data.LF_UNION.count;
            const char* sizeData = rec->data.LF_UNION.data;
            TRK sizeKind = *reinterpret_cast<const TRK*>(sizeData);
            size = leafValue(sizeData, sizeKind);
            name = leafName(sizeData, sizeKind);
        } else {
            if (rec->data.LF_CLASS.property.fwdref) continue;
            fieldCount = rec->data.LF_CLASS.count;
            const char* sizeData = rec->data.LF_CLASS.data;
            size = leafValue(sizeData, rec->data.LF_CLASS.lfEasy.kind);
            name = leafName(sizeData, rec->data.LF_CLASS.lfEasy.kind);
        }

        if (!name || name[0] == '\0') continue;
        // Skip anonymous types with compiler-generated names
        if (name[0] == '<') continue;

        PdbTypeInfo info;
        info.typeIndex = ti;
        info.name = QString::fromUtf8(name);
        info.size = size;
        info.childCount = fieldCount;
        info.isUnion = isUnion;
        info.isEnum = isEnum;
        result.append(info);
    }

    int enumCount = 0;
    for (const auto& r : result)
        if (r.isEnum) enumCount++;
    qDebug() << "[PDB] enumeratePdbTypes:" << result.size() << "types,"
             << enumCount << "enums";

    return result;
}

// ── Public API: importPdbSelected ──

NodeTree importPdbSelected(const QString& pdbPath,
                           const QVector<uint32_t>& typeIndices,
                           QString* errorMsg,
                           ProgressCb progressCb) {
    PdbFile pdb;
    if (!pdb.open(pdbPath, errorMsg)) return {};

    PdbCtx ctx;
    ctx.tt = pdb.typeTable;

    int total = typeIndices.size();
    int enumDispatched = 0, enumCreated = 0;
    for (int i = 0; i < total; i++) {
        uint32_t ti = typeIndices[i];
        const auto* rec = pdb.typeTable->get(ti);
        if (rec && rec->header.kind == TRK::LF_ENUM) {
            enumDispatched++;
            uint64_t id = ctx.importEnum(ti);
            if (id != 0) enumCreated++;
            else qDebug() << "[PDB] importEnum FAILED for typeIndex" << ti;
        } else {
            ctx.importUDT(ti);
        }
        if (progressCb && !progressCb(i + 1, total)) {
            if (errorMsg) *errorMsg = QStringLiteral("Import cancelled");
            return ctx.tree; // return partial result
        }
    }

    // Count enum nodes in tree
    int enumNodes = 0;
    for (const auto& n : ctx.tree.nodes)
        if (n.classKeyword == QLatin1String("enum")) enumNodes++;
    qDebug() << "[PDB] importPdbSelected:" << total << "types,"
             << enumDispatched << "enum dispatches,"
             << enumCreated << "enum created,"
             << enumNodes << "enum nodes in tree,"
             << ctx.tree.nodes.size() << "total nodes";

    if (ctx.tree.nodes.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("No types imported");
    }
    return ctx.tree;
}

// ── Public API: importPdb (legacy) ──

NodeTree importPdb(const QString& pdbPath, const QString& structFilter, QString* errorMsg) {
    PdbFile pdb;
    if (!pdb.open(pdbPath, errorMsg)) return {};

    const TypeTable& tt = *pdb.typeTable;
    PdbCtx ctx;
    ctx.tt = &tt;

    for (uint32_t ti = tt.firstIndex(); ti < tt.lastIndex(); ti++) {
        const auto* rec = tt.get(ti);
        if (!rec) continue;

        bool isUDT = (rec->header.kind == TRK::LF_STRUCTURE ||
                      rec->header.kind == TRK::LF_CLASS ||
                      rec->header.kind == TRK::LF_UNION);
        if (!isUDT) continue;

        bool fwdref = false;
        const char* name = nullptr;

        if (rec->header.kind == TRK::LF_UNION) {
            fwdref = rec->data.LF_UNION.property.fwdref;
            name = leafName(rec->data.LF_UNION.data, unionLeafKind(rec->data.LF_UNION.data));
        } else {
            fwdref = rec->data.LF_CLASS.property.fwdref;
            name = leafName(rec->data.LF_CLASS.data, rec->data.LF_CLASS.lfEasy.kind);
        }

        if (fwdref) continue;
        if (!name) continue;

        if (!structFilter.isEmpty()) {
            if (QString::fromUtf8(name) != structFilter) continue;
        }

        ctx.importUDT(ti);

        // If filtering to a single struct, stop after finding it
        if (!structFilter.isEmpty()) break;
    }

    if (ctx.tree.nodes.isEmpty()) {
        if (!structFilter.isEmpty()) {
            if (errorMsg) *errorMsg = QStringLiteral("Type '") + structFilter +
                QStringLiteral("' not found in PDB");
        } else {
            if (errorMsg) *errorMsg = QStringLiteral("No types found in PDB");
        }
    }

    return ctx.tree;
}

} // namespace rcx

#else // !_WIN32

namespace rcx {

PdbSymbolResult extractPdbSymbols(const QString&, QString* errorMsg) {
    if (errorMsg) *errorMsg = QStringLiteral("PDB import requires Windows");
    return {};
}

QVector<PdbTypeInfo> enumeratePdbTypes(const QString&, QString* errorMsg) {
    if (errorMsg) *errorMsg = QStringLiteral("PDB import requires Windows");
    return {};
}

NodeTree importPdbSelected(const QString&, const QVector<uint32_t>&,
                           QString* errorMsg, ProgressCb) {
    if (errorMsg) *errorMsg = QStringLiteral("PDB import requires Windows");
    return {};
}

NodeTree importPdb(const QString&, const QString&, QString* errorMsg) {
    if (errorMsg) *errorMsg = QStringLiteral("PDB import requires Windows");
    return {};
}

} // namespace rcx

#endif
