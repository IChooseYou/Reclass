#include "core.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace rcx::fmt {

// ── Column layout ──
// COL_TYPE and COL_NAME use shared constants from core.h (kColType, kColName)
static constexpr int COL_TYPE    = kColType;
static constexpr int COL_NAME    = kColName;
static constexpr int COL_VALUE   = kColValue;
static constexpr int COL_COMMENT = 28;  // "// Enter=Save Esc=Cancel" fits
static const QString SEP = QStringLiteral(" ");

static QString fit(QString s, int w) {
    if (w <= 0) return {};
    if (s.size() > w) {
        if (w >= 2) s = s.left(w - 1) + QChar(0x2026); // ellipsis
        else s = s.left(w);
    }
    return s.leftJustified(w, ' ');
}

// ── Type name ──

// Override seam: injectable type-name provider
static TypeNameFn g_typeNameFn = nullptr;

void setTypeNameProvider(TypeNameFn fn) { g_typeNameFn = fn; }

// Unpadded type name for width calculation
QString typeNameRaw(NodeKind kind) {
    if (g_typeNameFn) return g_typeNameFn(kind);
    auto* m = kindMeta(kind);
    return m ? QString::fromLatin1(m->typeName) : QStringLiteral("???");
}

QString typeName(NodeKind kind, int colType) {
    if (g_typeNameFn) return fit(g_typeNameFn(kind), colType);
    auto* m = kindMeta(kind);
    return fit(m ? QString::fromLatin1(m->typeName) : QStringLiteral("???"), colType);
}

// Array type string: "uint32_t[16]" or "Material[2]"
QString arrayTypeName(NodeKind elemKind, int count, const QString& structName) {
    QString elem;
    if (elemKind == NodeKind::Struct && !structName.isEmpty())
        elem = structName;
    else {
        auto* m = kindMeta(elemKind);
        elem = m ? QString::fromLatin1(m->typeName) : QStringLiteral("???");
    }
    return elem + QStringLiteral("[") + QString::number(count) + QStringLiteral("]");
}

// Pointer type string: "void*" or "StructName*"
QString pointerTypeName(NodeKind kind, const QString& targetName) {
    Q_UNUSED(kind);
    QString target = targetName.isEmpty() ? QStringLiteral("void") : targetName;
    return target + QStringLiteral("*");
}

// ── Value formatting ──

static QString hexVal(uint64_t v) {
    return QStringLiteral("0x") + QString::number(v, 16);
}

static QString rawHex(uint64_t v, int digits) {
    return QString::number(v, 16).rightJustified(digits, '0');
}

QString fmtInt8(int8_t v)     { return hexVal((uint8_t)v); }
QString fmtInt16(int16_t v)   { return hexVal((uint16_t)v); }
QString fmtInt32(int32_t v)   { return hexVal((uint32_t)v); }
QString fmtInt64(int64_t v)   { return hexVal((uint64_t)v); }
QString fmtUInt8(uint8_t v)   { return hexVal(v); }
QString fmtUInt16(uint16_t v) { return hexVal(v); }
QString fmtUInt32(uint32_t v) { return hexVal(v); }
QString fmtUInt64(uint64_t v) { return hexVal(v); }

QString fmtFloat(float v) {
    if (std::isnan(v)) return QStringLiteral("NaN");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inff") : QStringLiteral("-inff");

    // 6 significant digits — covers full single-precision range
    QString s = QString::number(v, 'g', 6);

    // If 'g' chose scientific notation, reformat as plain decimal
    if (s.contains('e') || s.contains('E')) {
        s = QString::number(v, 'f', 8);
        if (s.contains('.')) {
            int i = s.size() - 1;
            while (i > 0 && s[i] == '0') i--;
            if (s[i] == '.') i++;  // keep at least one decimal digit
            s.truncate(i + 1);
        }
    }

    if (!s.contains('.'))
        s += QStringLiteral(".f");
    else
        s += QLatin1Char('f');
    return s;
}
QString fmtDouble(double v) {
    QString s = QString::number(v, 'g', 6);
    if (!s.contains('.') && !s.contains('e') && !s.contains('E'))
        s += QStringLiteral(".0");
    return s;
}
QString fmtBool(uint8_t v)    { return v ? QStringLiteral("true") : QStringLiteral("false"); }

QString fmtPointer32(uint32_t v) {
    if (v == 0) return QStringLiteral("-> NULL");
    return QStringLiteral("-> ") + hexVal(v);
}

QString fmtPointer64(uint64_t v) {
    if (v == 0) return QStringLiteral("-> NULL");
    return QStringLiteral("-> ") + hexVal(v);
}

// ── Indentation ──

QString indent(int depth) {
    return QString(depth * 3, ' ');
}

// ── Offset margin ──

QString fmtOffsetMargin(uint64_t absoluteOffset, bool isContinuation, int hexDigits) {
    if (isContinuation) return QStringLiteral("  \u00B7 ");
    return QString::number(absoluteOffset, 16).toUpper()
               .rightJustified(hexDigits, '0') + QChar(' ');
}

// ── Struct type name (for width calculation) ──

QString structTypeName(const Node& node) {
    // Full type string: "struct TypeName" or just "struct" if no typename
    QString base = typeName(node.kind).trimmed();  // "struct"
    if (!node.structTypeName.isEmpty())
        return base + QStringLiteral(" ") + node.structTypeName;
    return base;
}

// ── Struct header / footer ──

QString fmtStructHeader(const Node& node, int depth, bool collapsed, int colType, int colName) {
    // Columnar format: <type> <name> { (or no brace when collapsed)
    QString ind = indent(depth);
    QString type = fit(structTypeName(node), colType);
    QString suffix = collapsed ? QString() : QStringLiteral("{");
    return ind + type + SEP + node.name + SEP + suffix;
}

QString fmtStructFooter(const Node& /*node*/, int depth, int /*totalSize*/) {
    return indent(depth) + QStringLiteral("};");
}

// ── Array header ──
// Columnar format: <type[count]> <name> { (or no brace when collapsed)
QString fmtArrayHeader(const Node& node, int depth, int /*viewIdx*/, bool collapsed, int colType, int colName, const QString& elemStructName) {
    QString ind = indent(depth);
    QString type = fit(arrayTypeName(node.elementKind, node.arrayLen, elemStructName), colType);
    QString suffix = collapsed ? QString() : QStringLiteral("{");
    return ind + type + SEP + node.name + SEP + suffix;
}

// ── Pointer header (merged pointer + struct header) ──

QString fmtPointerHeader(const Node& node, int depth, bool collapsed,
                         const Provider& prov, uint64_t addr,
                         const QString& ptrTypeName, int colType, int colName) {
    QString ind = indent(depth);
    QString type = fit(ptrTypeName, colType);
    if (collapsed) {
        // Collapsed: show pointer value instead of brace (name padded for value alignment)
        QString name = fit(node.name, colName);
        QString val = fit(readValue(node, prov, addr, 0), COL_VALUE);
        return ind + type + SEP + name + SEP + val;
    }
    return ind + type + SEP + node.name + SEP + QStringLiteral("{");
}

// ── Hex / ASCII preview ──

static inline bool isAsciiPrintable(uint8_t c) { return c >= 0x20 && c <= 0x7E; }

// Escape control characters for display
static QString sanitizeString(const QString& s) {
    QString out;
    out.reserve(s.size() + 8);
    for (QChar c : s) {
        if (c == '\n')      out += QStringLiteral("\\n");
        else if (c == '\r') out += QStringLiteral("\\r");
        else if (c == '\t') out += QStringLiteral("\\t");
        else if (c == '\\') out += QStringLiteral("\\\\");
        else if (c < QChar(0x20)) out += QStringLiteral("\\x") + QString::number(c.unicode(), 16);
        else out += c;
    }
    return out;
}

static QString bytesToAscii(const QByteArray& b, int slot) {
    QString out;
    out.reserve(slot);
    for (int i = 0; i < slot; ++i) {
        uint8_t c = (i < b.size()) ? (uint8_t)b[i] : 0;
        out += isAsciiPrintable(c) ? QChar(c) : QChar('.');
    }
    return out;
}

static QString bytesToHex(const QByteArray& b, int slot) {
    QString out;
    out.reserve(slot * 3);
    for (int i = 0; i < slot; ++i) {
        uint8_t c = (i < b.size()) ? (uint8_t)b[i] : 0;
        out += QString::asprintf("%02X", (unsigned)c);
        if (i + 1 < slot) out += ' ';
    }
    return out;
}

static QString fmtAsciiAndBytes(const Provider& prov, uint64_t addr,
                                int sizeBytes, int slotBytes = 8) {
    const int slot = qMax(slotBytes, sizeBytes);
    QByteArray b = prov.isReadable(addr, slot)
                       ? prov.readBytes(addr, slot)
                       : QByteArray(slot, '\0');
    return bytesToAscii(b, slot) + QStringLiteral("  ") + bytesToHex(b, slot);
}

// ── Single value from provider (unified) ──

enum class ValueMode { Display, Editable };

static QString readValueImpl(const Node& node, const Provider& prov,
                             uint64_t addr, int subLine, ValueMode mode) {
    const bool display = (mode == ValueMode::Display);
    switch (node.kind) {
    case NodeKind::Hex8:      return display ? hexVal(prov.readU8(addr))  : rawHex(prov.readU8(addr), 2);
    case NodeKind::Hex16:     return display ? hexVal(prov.readU16(addr)) : rawHex(prov.readU16(addr), 4);
    case NodeKind::Hex32:     return display ? hexVal(prov.readU32(addr)) : rawHex(prov.readU32(addr), 8);
    case NodeKind::Hex64:     return display ? hexVal(prov.readU64(addr)) : rawHex(prov.readU64(addr), 16);
    case NodeKind::Int8:      return fmtInt8((int8_t)prov.readU8(addr));
    case NodeKind::Int16:     return fmtInt16((int16_t)prov.readU16(addr));
    case NodeKind::Int32:     return fmtInt32((int32_t)prov.readU32(addr));
    case NodeKind::Int64:     return fmtInt64((int64_t)prov.readU64(addr));
    case NodeKind::UInt8:     return fmtUInt8(prov.readU8(addr));
    case NodeKind::UInt16:    return fmtUInt16(prov.readU16(addr));
    case NodeKind::UInt32:    return fmtUInt32(prov.readU32(addr));
    case NodeKind::UInt64:    return fmtUInt64(prov.readU64(addr));
    case NodeKind::Float:     { auto s = fmtFloat(prov.readF32(addr));   return display ? s : s.trimmed(); }
    case NodeKind::Double:    { auto s = fmtDouble(prov.readF64(addr));  return display ? s : s.trimmed(); }
    case NodeKind::Bool:      return fmtBool(prov.readU8(addr));
    case NodeKind::Pointer32: {
        uint32_t val = prov.readU32(addr);
        if (!display) return rawHex(val, 8);
        QString s = fmtPointer32(val);
        QString sym = prov.getSymbol((uint64_t)val);
        if (!sym.isEmpty()) s += QStringLiteral("  ") + sym;
        return s;
    }
    case NodeKind::Pointer64: {
        uint64_t val = prov.readU64(addr);
        if (!display) return rawHex(val, 16);
        QString s = fmtPointer64(val);
        QString sym = prov.getSymbol(val);
        if (!sym.isEmpty()) s += QStringLiteral("  ") + sym;
        return s;
    }
    case NodeKind::Vec2:
    case NodeKind::Vec3:
    case NodeKind::Vec4: {
        int count = sizeForKind(node.kind) / 4;
        QStringList parts;
        for (int i = 0; i < count; i++)
            parts << fmtFloat(prov.readF32(addr + i * 4)).trimmed();
        return parts.join(QStringLiteral(", "));
    }
    case NodeKind::Mat4x4: {
        if (!display) return {};  // not editable as single value
        if (subLine < 0 || subLine >= 4) return QStringLiteral("?");
        QString line = QStringLiteral("row%1 [").arg(subLine);
        for (int c = 0; c < 4; c++) {
            if (c > 0) line += QStringLiteral(", ");
            line += fmtFloat(prov.readF32(addr + (subLine * 4 + c) * 4)).trimmed();
        }
        line += QStringLiteral("]");
        return line;
    }
    case NodeKind::Padding:   return display ? hexVal(prov.readU8(addr)) : rawHex(prov.readU8(addr), 2);
    case NodeKind::UTF8: {
        QByteArray bytes = prov.readBytes(addr, node.strLen);
        int end = bytes.indexOf('\0');
        if (end >= 0) bytes.truncate(end);
        QString s = QString::fromUtf8(bytes);
        if (display) s = sanitizeString(s);
        return display ? (QStringLiteral("\"") + s + QStringLiteral("\"")) : s;
    }
    case NodeKind::UTF16: {
        QByteArray bytes = prov.readBytes(addr, node.strLen * 2);
        QString s = QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.data()),
                                       bytes.size() / 2);
        int end = s.indexOf(QChar(0));
        if (end >= 0) s.truncate(end);
        if (display) s = sanitizeString(s);
        return display ? (QStringLiteral("L\"") + s + QStringLiteral("\"")) : s;
    }
    default:
        return {};
    }
}

QString readValue(const Node& node, const Provider& prov,
                  uint64_t addr, int subLine) {
    return readValueImpl(node, prov, addr, subLine, ValueMode::Display);
}

// ── Full node line ──

QString fmtNodeLine(const Node& node, const Provider& prov,
                    uint64_t addr, int depth, int subLine,
                    const QString& comment, int colType, int colName,
                    const QString& typeOverride) {
    QString ind = indent(depth);
    QString type = typeOverride.isEmpty() ? typeName(node.kind, colType) : fit(typeOverride, colType);
    QString name = fit(node.name, colName);
    // Blank prefix for continuation lines (same width as type+sep+name+sep)
    const int prefixW = colType + colName + 2 * kSepWidth;

    // Comment suffix (only present when a comment is provided; no trailing padding)
    QString cmtSuffix = comment.isEmpty() ? QString()
                                          : fit(comment, COL_COMMENT);

    // Mat4x4: subLine 0..3 = rows
    if (node.kind == NodeKind::Mat4x4) {
        QString val = fit(readValue(node, prov, addr, subLine), COL_VALUE);
        if (subLine == 0) return ind + type + SEP + name + SEP + val + cmtSuffix;
        return ind + QString(prefixW, ' ') + val + cmtSuffix;
    }

    // Hex nodes and Padding: hex byte preview
    if (isHexPreview(node.kind)) {
        if (node.kind == NodeKind::Padding) {
            const int totalSz = qMax(1, node.arrayLen);
            const int lineOff = subLine * 8;
            const int lineBytes = qMin(8, totalSz - lineOff);
            QByteArray b = prov.isReadable(addr + lineOff, lineBytes)
                ? prov.readBytes(addr + lineOff, lineBytes) : QByteArray(lineBytes, '\0');
            QString ascii = bytesToAscii(b, lineBytes);
            QString hex = bytesToHex(b, lineBytes).leftJustified(23, ' '); // 8*3-1
            if (subLine == 0)
                return ind + type + SEP + ascii + SEP + hex + cmtSuffix;
            return ind + QString(colType + (int)SEP.size(), ' ') + ascii + SEP + hex + cmtSuffix;
        }
        // Hex8..Hex64: single line, ASCII padded to 8 chars so hex column aligns
        const int sz = sizeForKind(node.kind);
        QByteArray b = prov.isReadable(addr, sz)
            ? prov.readBytes(addr, sz) : QByteArray(sz, '\0');
        QString ascii = bytesToAscii(b, sz).leftJustified(8, ' ');
        QString hex = bytesToHex(b, sz).leftJustified(23, ' ');
        return ind + type + SEP + ascii + SEP + hex + cmtSuffix;
    }

    QString val = fit(readValue(node, prov, addr, subLine), COL_VALUE);
    return ind + type + SEP + name + SEP + val + cmtSuffix;
}

// ── Editable value (parse-friendly form for edit dialog) ──

QString editableValue(const Node& node, const Provider& prov,
                      uint64_t addr, int subLine) {
    return readValueImpl(node, prov, addr, subLine, ValueMode::Editable);
}

// ── Value parsing (text → bytes) ──

template<class T>
static QByteArray toBytes(T v) {
    QByteArray b(sizeof(T), Qt::Uninitialized);
    memcpy(b.data(), &v, sizeof(T));
    return b;
}

static QString stripHex(const QString& s) {
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        return s.mid(2);
    return s;
}

// Parse ASCII text into raw byte array (each char becomes a byte)
QByteArray parseAsciiValue(const QString& text, int expectedSize, bool* ok) {
    *ok = false;
    if (text.size() != expectedSize) return {};
    QByteArray result(expectedSize, Qt::Uninitialized);
    for (int i = 0; i < expectedSize; i++) {
        uint c = text[i].unicode();
        if (c > 255) return {};  // Non-Latin1 character
        result[i] = (char)c;
    }
    *ok = true;
    return result;
}

// Parse space-separated hex byte string into raw byte array (no endian conversion)
static QByteArray parseHexBytes(const QString& s, int expectedSize, bool* ok) {
    QString clean = s;
    clean.remove(' ');
    if (clean.size() != expectedSize * 2) { *ok = false; return {}; }
    QByteArray result(expectedSize, Qt::Uninitialized);
    for (int i = 0; i < expectedSize; i++) {
        bool byteOk;
        uint byte = clean.mid(i * 2, 2).toUInt(&byteOk, 16);
        if (!byteOk) { *ok = false; return {}; }
        result[i] = (char)byte;
    }
    *ok = true;
    return result;
}

// Range-checked narrowing: sets *ok = false if parsed value doesn't fit in T
template<class T, class ParseT>
static QByteArray parseIntChecked(ParseT val, bool* ok) {
    if (*ok) {
        using L = std::numeric_limits<T>;
        if constexpr (std::is_signed_v<T>) {
            if (val < (ParseT)L::min() || val > (ParseT)L::max()) *ok = false;
        } else {
            if (val > (ParseT)L::max()) *ok = false;
        }
    }
    return *ok ? toBytes<T>(static_cast<T>(val)) : QByteArray{};
}

QByteArray parseValue(NodeKind kind, const QString& text, bool* ok) {
    *ok = false;
    QString s = text.trimmed();

    // Allow empty for string types (will produce zero-length content, caller pads)
    if (s.isEmpty()) {
        if (kind == NodeKind::UTF8 || kind == NodeKind::UTF16) {
            *ok = true;
            return {};
        }
        return {};
    }

    switch (kind) {
    case NodeKind::Hex8:    return parseHexBytes(stripHex(s), 1, ok);
    case NodeKind::Hex16: {
        QString cleaned = stripHex(s);
        // Space-separated bytes → raw byte order (display order preserved)
        if (cleaned.contains(' '))
            return parseHexBytes(cleaned, 2, ok);
        // Single value → native-endian
        uint val = cleaned.toUInt(ok, 16);
        if (*ok && val > 0xFFFF) *ok = false;
        return *ok ? toBytes<uint16_t>(static_cast<uint16_t>(val)) : QByteArray{};
    }
    case NodeKind::Hex32: {
        QString cleaned = stripHex(s);
        // Space-separated bytes → raw byte order (display order preserved)
        if (cleaned.contains(' '))
            return parseHexBytes(cleaned, 4, ok);
        // Single value → native-endian
        uint val = cleaned.toUInt(ok, 16);
        return *ok ? toBytes<uint32_t>(val) : QByteArray{};
    }
    case NodeKind::Hex64: {
        QString cleaned = stripHex(s);
        // Space-separated bytes → raw byte order (display order preserved)
        if (cleaned.contains(' '))
            return parseHexBytes(cleaned, 8, ok);
        // Single value → native-endian
        qulonglong val = cleaned.toULongLong(ok, 16);
        return *ok ? toBytes<uint64_t>(val) : QByteArray{};
    }
    case NodeKind::Int8: {
        bool isHex = s.startsWith("0x", Qt::CaseInsensitive);
        if (isHex) {
            uint val = stripHex(s).toUInt(ok, 16);
            if (*ok && val > 0xFF) *ok = false;
            return *ok ? toBytes<int8_t>(static_cast<int8_t>(val)) : QByteArray{};
        } else {
            int val = s.toInt(ok, 10);
            return parseIntChecked<int8_t>(val, ok);
        }
    }
    case NodeKind::Int16: {
        bool isHex = s.startsWith("0x", Qt::CaseInsensitive);
        if (isHex) {
            uint val = stripHex(s).toUInt(ok, 16);
            if (*ok && val > 0xFFFF) *ok = false;
            return *ok ? toBytes<int16_t>(static_cast<int16_t>(val)) : QByteArray{};
        } else {
            int val = s.toInt(ok, 10);
            return parseIntChecked<int16_t>(val, ok);
        }
    }
    case NodeKind::Int32: {
        bool isHex = s.startsWith("0x", Qt::CaseInsensitive);
        if (isHex) {
            qulonglong val = stripHex(s).toULongLong(ok, 16);
            if (*ok && val > 0xFFFFFFFFULL) *ok = false;
            return *ok ? toBytes<int32_t>(static_cast<int32_t>(val)) : QByteArray{};
        } else {
            int val = s.toInt(ok, 10);
            return *ok ? toBytes<int32_t>(val) : QByteArray{};
        }
    }
    case NodeKind::Int64: {
        bool isHex = s.startsWith("0x", Qt::CaseInsensitive);
        if (isHex) {
            qulonglong val = stripHex(s).toULongLong(ok, 16);
            return *ok ? toBytes<int64_t>(static_cast<int64_t>(val)) : QByteArray{};
        } else {
            qlonglong val = s.toLongLong(ok, 10);
            return *ok ? toBytes<int64_t>(val) : QByteArray{};
        }
    }
    case NodeKind::UInt8:   { int b = s.startsWith("0x",Qt::CaseInsensitive)?16:10; uint val = stripHex(s).toUInt(ok,b);              return parseIntChecked<uint8_t>(val, ok); }
    case NodeKind::UInt16:  { int b = s.startsWith("0x",Qt::CaseInsensitive)?16:10; uint val = stripHex(s).toUInt(ok,b);              return parseIntChecked<uint16_t>(val, ok); }
    case NodeKind::UInt32:  { int b = s.startsWith("0x",Qt::CaseInsensitive)?16:10; qulonglong val = stripHex(s).toULongLong(ok,b);  return parseIntChecked<uint32_t>(val, ok); }
    case NodeKind::UInt64:  { int b = s.startsWith("0x",Qt::CaseInsensitive)?16:10; qulonglong val = stripHex(s).toULongLong(ok,b);   return *ok ? toBytes<uint64_t>(val) : QByteArray{}; }
    case NodeKind::Float: {
        QString n = s.trimmed();
        if (n.endsWith('f', Qt::CaseInsensitive)) n.chop(1);
        n.replace(',', '.');
        float val = n.toFloat(ok);
        return *ok ? toBytes<float>(val) : QByteArray{};
    }
    case NodeKind::Double: {
        QString n = s.trimmed();
        n.replace(',', '.');
        double val = n.toDouble(ok);
        return *ok ? toBytes<double>(val) : QByteArray{};
    }
    case NodeKind::Bool: {
        if (s == QStringLiteral("true") || s == QStringLiteral("1")) {
            *ok = true; return toBytes<uint8_t>(1);
        }
        if (s == QStringLiteral("false") || s == QStringLiteral("0")) {
            *ok = true; return toBytes<uint8_t>(0);
        }
        return {};  // unknown token → ok stays false
    }
    case NodeKind::Pointer32: {
        uint val = stripHex(s).toUInt(ok, 16);
        return *ok ? toBytes<uint32_t>(val) : QByteArray{};
    }
    case NodeKind::Pointer64: {
        qulonglong val = stripHex(s).toULongLong(ok, 16);
        return *ok ? toBytes<uint64_t>(val) : QByteArray{};
    }
    case NodeKind::UTF8: {
        *ok = true;
        if (s.startsWith('"') && s.endsWith('"'))
            s = s.mid(1, s.size() - 2);
        return s.toUtf8();
    }
    case NodeKind::UTF16: {
        *ok = true;
        if (s.startsWith(QStringLiteral("L\""))) s = s.mid(2);
        else if (s.startsWith('"')) s = s.mid(1);
        if (s.endsWith('"')) s.chop(1);
        QByteArray b(s.size() * 2, Qt::Uninitialized);
        memcpy(b.data(), s.utf16(), s.size() * 2);
        return b;
    }
    default:
        return {};
    }
}

// ── Value validation (returns error message or empty string if valid) ──

QString validateValue(NodeKind kind, const QString& text) {
    QString s = text.trimmed();
    if (s.isEmpty()) return {};

    // For integer/hex types, validate character set first
    bool isHexKind = (kind >= NodeKind::Hex8 && kind <= NodeKind::Hex64)
                  || kind == NodeKind::Pointer32 || kind == NodeKind::Pointer64;
    bool isIntKind = (kind >= NodeKind::Int8 && kind <= NodeKind::UInt64);

    if (isHexKind || isIntKind) {
        bool hasHexPrefix = s.startsWith("0x", Qt::CaseInsensitive);
        QString digits = hasHexPrefix ? s.mid(2) : s;

        if (hasHexPrefix || isHexKind) {
            // Hex mode: only 0-9, a-f, A-F
            for (QChar c : digits) {
                if (!c.isDigit() && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
                    return QStringLiteral("invalid hex '%1'").arg(c);
            }
        } else {
            // Decimal mode: only digits (and leading minus for signed)
            int start = 0;
            bool isSigned = (kind >= NodeKind::Int8 && kind <= NodeKind::Int64);
            if (isSigned && !digits.isEmpty() && digits[0] == '-') start = 1;
            for (int i = start; i < digits.size(); i++) {
                if (!digits[i].isDigit())
                    return QStringLiteral("invalid '%1'").arg(digits[i]);
            }
        }
    }

    // Then do the actual parse for range checking
    bool ok;
    parseValue(kind, text, &ok);
    if (ok) return {};

    // Type-appropriate error messages
    bool isFloatKind = (kind == NodeKind::Float || kind == NodeKind::Double);
    if (isFloatKind)
        return QStringLiteral("invalid number");

    // Return byte-capacity max based on type size
    const auto* m = kindMeta(kind);
    if (m && m->size > 0 && m->size <= 8) {
        uint64_t maxVal = (m->size == 8) ? ~0ULL : ((1ULL << (m->size * 8)) - 1);
        return QStringLiteral("too large! max=0x%1").arg(maxVal, m->size * 2, 16, QChar('0'));
    }
    return QStringLiteral("invalid");
}

// ── Base address validation (supports simple +/- equations) ──

QString validateBaseAddress(const QString& text) {
    QString s = text.trimmed();
    if (s.isEmpty()) return QStringLiteral("empty");

    int pos = 0;
    bool firstTerm = true;

    while (pos < s.size()) {
        // Skip whitespace
        while (pos < s.size() && s[pos].isSpace()) pos++;
        if (pos >= s.size()) break;

        // Check for +/- operator (except first term)
        if (!firstTerm) {
            if (s[pos] == '+' || s[pos] == '-') pos++;
            else return QStringLiteral("invalid '%1'").arg(s[pos]);
            while (pos < s.size() && s[pos].isSpace()) pos++;
        }

        // Skip 0x prefix if present
        if (pos + 1 < s.size() && s[pos] == '0' && (s[pos+1] == 'x' || s[pos+1] == 'X'))
            pos += 2;

        // Must have at least one hex digit
        int numStart = pos;
        while (pos < s.size() && (s[pos].isDigit() ||
               (s[pos] >= 'a' && s[pos] <= 'f') ||
               (s[pos] >= 'A' && s[pos] <= 'F'))) pos++;

        if (pos == numStart) return QStringLiteral("invalid");

        firstTerm = false;
    }

    return {};
}

} // namespace rcx::fmt
