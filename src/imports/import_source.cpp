#include "import_source.h"
#include <QHash>
#include <QSet>
#include <QVector>
#include <QRegularExpression>
#include <QDebug>

namespace rcx {

// ── Built-in type alias table ──

struct TypeInfo {
    NodeKind kind;
    int      size; // bytes (0 = dynamic/pointer)
};

static QHash<QString, TypeInfo> buildTypeTable(int ptrSize = 8) {
    QHash<QString, TypeInfo> t;
    // Pointer/size_t kinds depend on target architecture
    NodeKind ptrKind  = (ptrSize >= 8) ? NodeKind::Pointer64 : NodeKind::Pointer32;
    NodeKind uintpKind = (ptrSize >= 8) ? NodeKind::UInt64   : NodeKind::UInt32;
    NodeKind intpKind  = (ptrSize >= 8) ? NodeKind::Int64    : NodeKind::Int32;

    // stdint.h
    t[QStringLiteral("uint8_t")]  = {NodeKind::UInt8,  1};
    t[QStringLiteral("int8_t")]   = {NodeKind::Int8,   1};
    t[QStringLiteral("uint16_t")] = {NodeKind::UInt16, 2};
    t[QStringLiteral("int16_t")]  = {NodeKind::Int16,  2};
    t[QStringLiteral("uint32_t")] = {NodeKind::UInt32, 4};
    t[QStringLiteral("int32_t")]  = {NodeKind::Int32,  4};
    t[QStringLiteral("uint64_t")] = {NodeKind::UInt64, 8};
    t[QStringLiteral("int64_t")]  = {NodeKind::Int64,  8};

    // Standard C
    t[QStringLiteral("char")]     = {NodeKind::Int8,   1};
    t[QStringLiteral("short")]    = {NodeKind::Int16,  2};
    t[QStringLiteral("int")]      = {NodeKind::Int32,  4};
    t[QStringLiteral("long")]     = {NodeKind::Int32,  4};
    t[QStringLiteral("float")]    = {NodeKind::Float,  4};
    t[QStringLiteral("double")]   = {NodeKind::Double, 8};
    t[QStringLiteral("bool")]     = {NodeKind::Bool,   1};
    t[QStringLiteral("_Bool")]    = {NodeKind::Bool,   1};
    t[QStringLiteral("void")]     = {NodeKind::Hex8,   1};
    t[QStringLiteral("wchar_t")]  = {NodeKind::UInt16, 2};

    // Multi-word C types (pre-merged by parser)
    t[QStringLiteral("unsigned char")]      = {NodeKind::UInt8,  1};
    t[QStringLiteral("signed char")]        = {NodeKind::Int8,   1};
    t[QStringLiteral("unsigned short")]     = {NodeKind::UInt16, 2};
    t[QStringLiteral("signed short")]       = {NodeKind::Int16,  2};
    t[QStringLiteral("unsigned int")]       = {NodeKind::UInt32, 4};
    t[QStringLiteral("signed int")]         = {NodeKind::Int32,  4};
    t[QStringLiteral("unsigned")]           = {NodeKind::UInt32, 4};
    t[QStringLiteral("long long")]          = {NodeKind::Int64,  8};
    t[QStringLiteral("unsigned long")]      = {NodeKind::UInt32, 4};
    t[QStringLiteral("signed long")]        = {NodeKind::Int32,  4};
    t[QStringLiteral("unsigned long long")] = {NodeKind::UInt64, 8};
    t[QStringLiteral("signed long long")]   = {NodeKind::Int64,  8};
    t[QStringLiteral("long int")]           = {NodeKind::Int32,  4};
    t[QStringLiteral("long long int")]      = {NodeKind::Int64,  8};
    t[QStringLiteral("unsigned long int")]  = {NodeKind::UInt32, 4};
    t[QStringLiteral("unsigned long long int")] = {NodeKind::UInt64, 8};
    t[QStringLiteral("short int")]          = {NodeKind::Int16,  2};
    t[QStringLiteral("unsigned short int")] = {NodeKind::UInt16, 2};

    // Windows types
    t[QStringLiteral("BYTE")]      = {NodeKind::UInt8,  1};
    t[QStringLiteral("UCHAR")]     = {NodeKind::UInt8,  1};
    t[QStringLiteral("BOOLEAN")]   = {NodeKind::UInt8,  1};
    t[QStringLiteral("CHAR")]      = {NodeKind::Int8,   1};
    t[QStringLiteral("WORD")]      = {NodeKind::UInt16, 2};
    t[QStringLiteral("USHORT")]    = {NodeKind::UInt16, 2};
    t[QStringLiteral("SHORT")]     = {NodeKind::Int16,  2};
    t[QStringLiteral("WCHAR")]     = {NodeKind::UInt16, 2};
    t[QStringLiteral("TCHAR")]     = {NodeKind::UInt16, 2};
    t[QStringLiteral("DWORD")]     = {NodeKind::UInt32, 4};
    t[QStringLiteral("ULONG")]     = {NodeKind::UInt32, 4};
    t[QStringLiteral("UINT")]      = {NodeKind::UInt32, 4};
    t[QStringLiteral("LONG")]      = {NodeKind::Int32,  4};
    t[QStringLiteral("LONG32")]    = {NodeKind::Int32,  4};
    t[QStringLiteral("INT")]       = {NodeKind::Int32,  4};
    t[QStringLiteral("BOOL")]      = {NodeKind::Int32,  4};
    t[QStringLiteral("FLOAT")]     = {NodeKind::Float,  4};
    t[QStringLiteral("QWORD")]     = {NodeKind::UInt64, 8};
    t[QStringLiteral("ULONGLONG")] = {NodeKind::UInt64, 8};
    t[QStringLiteral("DWORD64")]   = {NodeKind::UInt64, 8};
    t[QStringLiteral("ULONG64")]   = {NodeKind::UInt64, 8};
    t[QStringLiteral("UINT64")]    = {NodeKind::UInt64, 8};
    t[QStringLiteral("LONGLONG")]  = {NodeKind::Int64,  8};
    t[QStringLiteral("LONG64")]    = {NodeKind::Int64,  8};
    t[QStringLiteral("INT64")]     = {NodeKind::Int64,  8};

    // Platform pointer-size types (depend on target architecture)
    t[QStringLiteral("PVOID")]      = {ptrKind,   ptrSize};
    t[QStringLiteral("LPVOID")]     = {ptrKind,   ptrSize};
    t[QStringLiteral("HANDLE")]     = {ptrKind,   ptrSize};
    t[QStringLiteral("HMODULE")]    = {ptrKind,   ptrSize};
    t[QStringLiteral("HWND")]       = {ptrKind,   ptrSize};
    t[QStringLiteral("HINSTANCE")]  = {ptrKind,   ptrSize};
    t[QStringLiteral("SIZE_T")]     = {uintpKind, ptrSize};
    t[QStringLiteral("ULONG_PTR")] = {uintpKind, ptrSize};
    t[QStringLiteral("UINT_PTR")]  = {uintpKind, ptrSize};
    t[QStringLiteral("DWORD_PTR")] = {uintpKind, ptrSize};
    t[QStringLiteral("LONG_PTR")]  = {intpKind,  ptrSize};
    t[QStringLiteral("INT_PTR")]   = {intpKind,  ptrSize};
    t[QStringLiteral("SSIZE_T")]   = {intpKind,  ptrSize};
    t[QStringLiteral("uintptr_t")] = {uintpKind, ptrSize};
    t[QStringLiteral("intptr_t")]  = {intpKind,  ptrSize};
    t[QStringLiteral("size_t")]    = {uintpKind, ptrSize};
    t[QStringLiteral("ptrdiff_t")] = {intpKind,  ptrSize};
    t[QStringLiteral("ssize_t")]   = {intpKind,  ptrSize};

    // Pointer type aliases
    t[QStringLiteral("PCHAR")]  = {ptrKind, ptrSize};
    t[QStringLiteral("LPSTR")]  = {ptrKind, ptrSize};
    t[QStringLiteral("LPCSTR")] = {ptrKind, ptrSize};
    t[QStringLiteral("PCSTR")]  = {ptrKind, ptrSize};
    t[QStringLiteral("PWSTR")]  = {ptrKind, ptrSize};
    t[QStringLiteral("LPWSTR")] = {ptrKind, ptrSize};
    t[QStringLiteral("LPCWSTR")]= {ptrKind, ptrSize};
    t[QStringLiteral("PCWSTR")] = {ptrKind, ptrSize};

    return t;
}

// ── Tokenizer ──

enum class TokKind {
    Ident, Number, Star, Semi, LBrace, RBrace,
    LBracket, RBracket, LParen, RParen, Comma, Colon,
    Equals, Hash, Eof, Other
};

struct Token {
    TokKind kind = TokKind::Eof;
    QString text;
    int     line = 0;
};

// Parsed offset comment associated with a line
struct LineOffset {
    int line;
    int offset; // hex offset value
};

struct Tokenizer {
    const QString& src;
    int pos = 0;
    int line = 1;
    QVector<Token> tokens;
    QVector<LineOffset> offsets; // captured // 0xNN comments

    explicit Tokenizer(const QString& s) : src(s) {}

    void tokenize() {
        while (pos < src.size()) {
            skipWhitespace();
            if (pos >= src.size()) break;

            QChar c = src[pos];

            // Line comments
            if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                parseLineComment();
                continue;
            }
            // Block comments
            if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
                parseBlockComment();
                continue;
            }
            // Preprocessor lines - skip entirely
            if (c == '#') {
                skipToEndOfLine();
                continue;
            }
            // Identifiers / keywords
            if (c.isLetter() || c == '_') {
                parseIdent();
                continue;
            }
            // Numbers
            if (c.isDigit()) {
                parseNumber();
                continue;
            }
            // Single-character tokens
            TokKind tk = TokKind::Other;
            switch (c.toLatin1()) {
            case '*': tk = TokKind::Star;     break;
            case ';': tk = TokKind::Semi;     break;
            case '{': tk = TokKind::LBrace;   break;
            case '}': tk = TokKind::RBrace;   break;
            case '[': tk = TokKind::LBracket; break;
            case ']': tk = TokKind::RBracket; break;
            case '(': tk = TokKind::LParen;   break;
            case ')': tk = TokKind::RParen;   break;
            case ',': tk = TokKind::Comma;    break;
            case ':': tk = TokKind::Colon;    break;
            case '=': tk = TokKind::Equals;   break;
            default:  tk = TokKind::Other;    break;
            }
            tokens.append({tk, QString(c), line});
            pos++;
        }
        tokens.append({TokKind::Eof, {}, line});
    }

private:
    void skipWhitespace() {
        while (pos < src.size()) {
            if (src[pos] == '\n') { line++; pos++; }
            else if (src[pos].isSpace()) { pos++; }
            else break;
        }
    }

    void skipToEndOfLine() {
        while (pos < src.size() && src[pos] != '\n') pos++;
    }

    void parseLineComment() {
        int commentLine = line;
        pos += 2; // skip //
        int start = pos;
        while (pos < src.size() && src[pos] != '\n') pos++;
        QString comment = src.mid(start, pos - start).trimmed();

        // Capture offset comments like "0x10" or "// 0x10"
        static QRegularExpression offsetRe(QStringLiteral("^(?:->\\s*\\S+\\s+)?0x([0-9A-Fa-f]+)$"));
        // Also handle "-> TypeName 0x1A" style
        static QRegularExpression offsetRe2(QStringLiteral("0x([0-9A-Fa-f]+)"));
        auto m = offsetRe.match(comment);
        if (!m.hasMatch()) {
            // Try simpler: just look for "0xHEX" at end of comment
            // Handles "// 0x10", "// -> Material* 0x10", etc.
            static QRegularExpression endHexRe(QStringLiteral("\\b0x([0-9A-Fa-f]+)\\s*$"));
            m = endHexRe.match(comment);
        }
        if (m.hasMatch()) {
            bool ok;
            int val = m.captured(1).toInt(&ok, 16);
            if (ok) {
                offsets.append({commentLine, val});
            }
        }
    }

    void parseBlockComment() {
        pos += 2; // skip /*
        while (pos + 1 < src.size()) {
            if (src[pos] == '\n') line++;
            if (src[pos] == '*' && src[pos + 1] == '/') { pos += 2; return; }
            pos++;
        }
        pos = src.size(); // unterminated
    }

    void parseIdent() {
        int start = pos;
        while (pos < src.size() && (src[pos].isLetterOrNumber() || src[pos] == '_')) pos++;
        tokens.append({TokKind::Ident, src.mid(start, pos - start), line});
    }

    void parseNumber() {
        int start = pos;
        if (src[pos] == '0' && pos + 1 < src.size() &&
            (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
            pos += 2;
            while (pos < src.size() && (src[pos].isDigit() ||
                   (src[pos] >= 'a' && src[pos] <= 'f') ||
                   (src[pos] >= 'A' && src[pos] <= 'F'))) pos++;
        } else {
            while (pos < src.size() && src[pos].isDigit()) pos++;
        }
        // Skip integer suffixes (U, L, LL, ULL, etc.)
        while (pos < src.size() && (src[pos] == 'u' || src[pos] == 'U' ||
                                     src[pos] == 'l' || src[pos] == 'L')) pos++;
        tokens.append({TokKind::Number, src.mid(start, pos - start), line});
    }
};

// ── Parser ──

struct ParsedField {
    QString typeName;      // base type name (resolved through multi-word merge)
    QString name;
    bool    isPointer = false;
    int     pointerDepth = 0;  // number of * levels
    QVector<int> arraySizes;   // [4], [4][4] etc.
    int     commentOffset = -1; // from // 0xNN (-1 = none)
    int     bitfieldWidth = -1; // -1 = not a bitfield
    QString pointerTarget;     // for Type* -> the type name
    bool    isUnion = false;               // union container
    QVector<ParsedField> unionMembers;     // children of union
};

struct ParsedStruct {
    QString name;
    QString keyword; // "struct", "class", or "enum"
    QVector<ParsedField> fields;
    int declaredSize = -1; // from static_assert
    QVector<QPair<QString, int64_t>> enumValues; // for keyword="enum"
};

struct PendingRef {
    uint64_t nodeId;
    QString  className;
};

// Multi-word type prefix keywords
static bool isTypeModifier(const QString& s) {
    return s == QStringLiteral("unsigned") ||
           s == QStringLiteral("signed") ||
           s == QStringLiteral("long") ||
           s == QStringLiteral("short");
}

static bool isQualifier(const QString& s) {
    return s == QStringLiteral("const") ||
           s == QStringLiteral("volatile") ||
           s == QStringLiteral("mutable") ||
           s == QStringLiteral("struct") ||
           s == QStringLiteral("class") ||
           s == QStringLiteral("enum");
}

struct Parser {
    const QVector<Token>& tokens;
    const QVector<LineOffset>& lineOffsets;
    int cur = 0;

    QVector<ParsedStruct> structs;
    QSet<QString> forwardDecls;
    QHash<QString, QString> typedefs; // alias -> real type
    QSet<QString> pointerTypedefs;    // aliases that are pointer-to-struct
    QHash<QString, QVector<int>> arrayTypedefs; // aliases that are array types (alias -> dimensions)
    QHash<QString, int> sizeAsserts;  // struct name -> declared size
    QHash<QString, int> structAlignments; // struct name -> ALIGN(N) value

    explicit Parser(const QVector<Token>& t, const QVector<LineOffset>& lo)
        : tokens(t), lineOffsets(lo) {}

    const Token& peek(int ahead = 0) const {
        int i = cur + ahead;
        return (i < tokens.size()) ? tokens[i] : tokens.back();
    }

    Token advance() {
        if (cur < tokens.size() - 1) return tokens[cur++];
        return tokens.back();
    }

    bool check(TokKind k) const { return peek().kind == k; }
    bool checkIdent(const QString& s) const { return peek().kind == TokKind::Ident && peek().text == s; }

    bool match(TokKind k) {
        if (check(k)) { advance(); return true; }
        return false;
    }

    bool matchIdent(const QString& s) {
        if (checkIdent(s)) { advance(); return true; }
        return false;
    }

    void skipToSemiOrBrace() {
        int depth = 0;
        while (peek().kind != TokKind::Eof) {
            if (peek().kind == TokKind::LBrace) depth++;
            else if (peek().kind == TokKind::RBrace) {
                if (depth == 0) break;
                depth--;
            }
            else if (peek().kind == TokKind::Semi && depth == 0) {
                advance(); return;
            }
            advance();
        }
    }

    // Skip ALIGN( N ) macro if present (Vergilius-style headers)
    // Returns the alignment value, or 0 if no ALIGN macro.
    int skipAlignMacro() {
        if (checkIdent("ALIGN") || checkIdent("__declspec")) {
            advance();
            int alignVal = 0;
            if (match(TokKind::LParen)) {
                // Try to read the alignment number
                if (peek().kind == TokKind::Number) {
                    alignVal = peek().text.toInt();
                }
                int depth = 1;
                while (depth > 0 && peek().kind != TokKind::Eof) {
                    if (peek().kind == TokKind::LParen) depth++;
                    else if (peek().kind == TokKind::RParen) depth--;
                    advance();
                }
            }
            return alignVal;
        }
        return 0;
    }

    // Check if next tokens after keyword are ALIGN(...) then Ident/LBrace
    bool peekPastAlign(int offset, TokKind expected) const {
        int i = cur + offset;
        if (i < tokens.size() && tokens[i].kind == TokKind::Ident &&
            (tokens[i].text == QStringLiteral("ALIGN") ||
             tokens[i].text == QStringLiteral("__declspec"))) {
            i++; // skip ALIGN
            if (i < tokens.size() && tokens[i].kind == TokKind::LParen) {
                int depth = 1; i++;
                while (i < tokens.size() && depth > 0) {
                    if (tokens[i].kind == TokKind::LParen) depth++;
                    else if (tokens[i].kind == TokKind::RParen) depth--;
                    i++;
                }
            }
            return i < tokens.size() && tokens[i].kind == expected;
        }
        return false;
    }

    // ── Top-level parse ──

    void parse() {
        while (peek().kind != TokKind::Eof) {
            if (checkIdent("struct") || checkIdent("class")) {
                parseStructOrForward();
            } else if (checkIdent("union")) {
                parseTopLevelUnion();
            } else if (checkIdent("static_assert")) {
                parseStaticAssert();
            } else if (checkIdent("typedef")) {
                parseTypedef();
            } else if (checkIdent("enum")) {
                parseEnumDef();
            } else if (peek().kind == TokKind::Hash) {
                // preprocessor (shouldn't reach here if tokenizer skipped them)
                advance();
                while (peek().kind != TokKind::Eof && peek().kind != TokKind::Semi) advance();
            } else {
                advance(); // skip unknown
            }
        }
    }

    void parseStructOrForward() {
        QString keyword = advance().text; // "struct" or "class"

        // Skip ALIGN( N ) between keyword and name
        int alignVal = skipAlignMacro();

        // Anonymous struct: struct { ... }
        if (check(TokKind::LBrace)) {
            // Skip anonymous struct at top level
            skipToSemiOrBrace();
            if (check(TokKind::RBrace)) { advance(); match(TokKind::Semi); }
            return;
        }

        if (!check(TokKind::Ident)) { skipToSemiOrBrace(); return; }
        QString name = advance().text;

        if (alignVal > 0)
            structAlignments[name] = alignVal;

        // Check for inheritance: struct Foo : public Bar {
        // Just skip the inheritance clause
        if (check(TokKind::Colon)) {
            advance(); // ':'
            while (peek().kind != TokKind::LBrace && peek().kind != TokKind::Semi &&
                   peek().kind != TokKind::Eof) {
                advance();
            }
        }

        // Forward declaration: struct Foo;
        if (check(TokKind::Semi)) {
            advance();
            forwardDecls.insert(name);
            return;
        }

        if (!match(TokKind::LBrace)) { skipToSemiOrBrace(); return; }

        ParsedStruct ps;
        ps.name = name;
        ps.keyword = keyword;

        parseStructBody(ps);

        if (!match(TokKind::RBrace)) { skipToSemiOrBrace(); return; }
        match(TokKind::Semi);

        structs.append(ps);
    }

    void parseStructBody(ParsedStruct& ps) {
        while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
            // Nested struct definition
            if (checkIdent("struct") || checkIdent("class")) {
                // Check: struct [ALIGN(N)] Name {
                if ((peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::LBrace) ||
                    peekPastAlign(1, TokKind::Ident)) {
                    // Nested named struct: parse as a top-level struct, then treat as embedded field
                    parseStructOrForward();
                    continue;
                }
                // Check: struct [ALIGN(N)] {
                if (peek(1).kind == TokKind::LBrace || peekPastAlign(1, TokKind::LBrace)) {
                    // Anonymous nested struct { ... } fieldName;
                    advance(); // skip "struct"
                    skipAlignMacro();
                    advance(); // skip "{"
                    // Skip body
                    int depth = 1;
                    while (peek().kind != TokKind::Eof && depth > 0) {
                        if (peek().kind == TokKind::LBrace) depth++;
                        else if (peek().kind == TokKind::RBrace) depth--;
                        if (depth > 0) advance();
                    }
                    if (check(TokKind::RBrace)) advance();
                    // field name
                    if (check(TokKind::Ident)) advance();
                    match(TokKind::Semi);
                    continue;
                }
                // Might be "struct TypeName fieldName;" - fall through to field parsing
            }

            // Union: create container with all members
            if (checkIdent("union")) {
                parseUnion(ps);
                continue;
            }

            // Enum definition inside struct
            if (checkIdent("enum")) {
                parseEnumDef();
                continue;
            }

            // Static assert inside struct
            if (checkIdent("static_assert")) {
                parseStaticAssert();
                continue;
            }

            // Try to parse as a field
            ParsedField field;
            if (parseField(field)) {
                ps.fields.append(field);
            } else {
                advance(); // skip unrecognized token
            }
        }
    }

    // Top-level named union definition: union [ALIGN(N)] Name { ... };
    // Parsed as a struct with classKeyword "union" and all members as fields
    void parseTopLevelUnion() {
        advance(); // skip "union"
        int alignVal = skipAlignMacro();

        // Forward declaration: union Name;
        if (check(TokKind::Ident) && peek(1).kind == TokKind::Semi) {
            QString name = advance().text;
            advance(); // skip ;
            forwardDecls.insert(name);
            return;
        }

        // Anonymous union at top level (skip)
        if (check(TokKind::LBrace)) {
            skipToSemiOrBrace();
            if (check(TokKind::RBrace)) { advance(); match(TokKind::Semi); }
            return;
        }

        if (!check(TokKind::Ident)) { skipToSemiOrBrace(); return; }
        QString name = advance().text;

        if (alignVal > 0)
            structAlignments[name] = alignVal;

        if (!match(TokKind::LBrace)) { skipToSemiOrBrace(); return; }

        ParsedStruct ps;
        ps.name = name;
        ps.keyword = QStringLiteral("union");

        // Parse body — same as struct body but members overlap at offset 0
        parseStructBody(ps);

        if (!match(TokKind::RBrace)) { skipToSemiOrBrace(); return; }
        match(TokKind::Semi);

        structs.append(ps);
    }

    void parseUnion(ParsedStruct& ps) {
        advance(); // skip "union"

        // Skip ALIGN( N ) between union keyword and name/brace
        skipAlignMacro();

        // Optional union tag name (before {)
        if (check(TokKind::Ident) && peek(1).kind == TokKind::LBrace) {
            advance(); // skip union tag name
        }

        if (!match(TokKind::LBrace)) { skipToSemiOrBrace(); return; }

        // Parse ALL members of the union
        ParsedField unionField;
        unionField.isUnion = true;

        while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
            // Handle nested unions inside this union
            if (checkIdent("union")) {
                // Recurse: create a sub-union ParsedStruct temporarily,
                // then steal its fields as a nested union member
                ParsedStruct tmp;
                parseUnion(tmp);
                for (auto& f : tmp.fields)
                    unionField.unionMembers.append(f);
                continue;
            }

            // Handle anonymous struct inside union: struct [ALIGN(N)] { ... };
            if ((checkIdent("struct") || checkIdent("class")) &&
                (peek(1).kind == TokKind::LBrace || peekPastAlign(1, TokKind::LBrace))) {
                advance(); // skip "struct"
                skipAlignMacro();
                advance(); // skip "{"
                int depth = 1;
                while (peek().kind != TokKind::Eof && depth > 0) {
                    if (peek().kind == TokKind::LBrace) depth++;
                    else if (peek().kind == TokKind::RBrace) depth--;
                    if (depth > 0) advance();
                }
                if (check(TokKind::RBrace)) advance();
                if (check(TokKind::Ident)) advance(); // optional field name
                match(TokKind::Semi);
                continue;
            }

            // Handle nested named struct definition inside union: struct [ALIGN(N)] Name {
            if ((checkIdent("struct") || checkIdent("class")) &&
                ((peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::LBrace) ||
                 peekPastAlign(1, TokKind::Ident))) {
                parseStructOrForward();
                continue;
            }

            ParsedField field;
            if (parseField(field)) {
                unionField.unionMembers.append(field);
            } else {
                advance();
            }
        }
        match(TokKind::RBrace);

        // Optional field name after union close: union { ... } u3;
        if (check(TokKind::Ident)) {
            unionField.name = advance().text;
        }
        match(TokKind::Semi);

        // Determine offset from first member with a known offset
        for (const auto& m : unionField.unionMembers) {
            if (m.commentOffset >= 0) {
                unionField.commentOffset = m.commentOffset;
                break;
            }
        }

        ps.fields.append(unionField);
    }

    bool parseField(ParsedField& field) {
        int startPos = cur;

        // Skip qualifiers
        while (isQualifier(peek().text)) advance();

        // Parse type
        QString typeName = parseTypeName();
        if (typeName.isEmpty()) { cur = startPos; return false; }

        // Resolve typedef — track pointer and array typedefs in the chain
        bool typedefPointer = false;
        QVector<int> typedefArrayDims;
        {
            QString resolved = typeName;
            QSet<QString> seen;
            while (typedefs.contains(resolved) && !seen.contains(resolved)) {
                if (pointerTypedefs.contains(resolved))
                    typedefPointer = true;
                if (typedefArrayDims.isEmpty() && arrayTypedefs.contains(resolved))
                    typedefArrayDims = arrayTypedefs[resolved];
                seen.insert(resolved);
                resolved = typedefs[resolved];
            }
            typeName = resolved;
        }

        // Pointer stars
        bool isPointer = typedefPointer;
        int ptrDepth = typedefPointer ? 1 : 0;
        while (match(TokKind::Star)) {
            isPointer = true;
            ptrDepth++;
        }

        // Skip const after pointer
        while (checkIdent("const") || checkIdent("volatile")) advance();

        // More pointer stars (const Type * const * name)
        while (match(TokKind::Star)) {
            isPointer = true;
            ptrDepth++;
        }

        // Field name
        if (!check(TokKind::Ident)) { cur = startPos; return false; }
        field.name = advance().text;

        // Array sizes: [N], [N][M], etc.
        while (check(TokKind::LBracket)) {
            advance(); // [
            if (check(TokKind::Number)) {
                bool ok;
                QString numText = peek().text;
                int val;
                if (numText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
                    val = numText.mid(2).toInt(&ok, 16);
                else
                    val = numText.toInt(&ok);
                if (ok) field.arraySizes.append(val);
                advance();
            } else if (check(TokKind::RBracket)) {
                field.arraySizes.append(0); // unsized array
            }
            match(TokKind::RBracket);
        }

        // Apply array dimensions from typedef (e.g. typedef ULONG GDI_HANDLE_BUFFER[60])
        if (!typedefArrayDims.isEmpty()) {
            if (field.arraySizes.isEmpty())
                field.arraySizes = typedefArrayDims;
            else {
                // Combine: typedef dims come first, field dims appended
                QVector<int> combined = typedefArrayDims;
                combined.append(field.arraySizes);
                field.arraySizes = combined;
            }
        }

        // Bitfield: Type name : width
        if (check(TokKind::Colon)) {
            advance();
            if (check(TokKind::Number)) {
                bool ok;
                field.bitfieldWidth = peek().text.toInt(&ok);
                advance();
            }
        }

        // Expect semicolon
        if (!match(TokKind::Semi)) { cur = startPos; return false; }

        // Check if next token line has an offset comment
        // We associate offset comments with the field's line
        int fieldLine = tokens[startPos].line;
        for (const auto& lo : lineOffsets) {
            if (lo.line == fieldLine) {
                field.commentOffset = lo.offset;
                break;
            }
        }

        field.typeName = typeName;
        field.isPointer = isPointer;
        field.pointerDepth = ptrDepth;
        if (isPointer) field.pointerTarget = typeName;

        return true;
    }

    QString parseTypeName() {
        if (peek().kind != TokKind::Ident) return {};

        QString first = peek().text;

        // Handle "struct/class TypeName" as a type reference
        if (first == QStringLiteral("struct") || first == QStringLiteral("class") ||
            first == QStringLiteral("enum")) {
            advance(); // skip struct/class/enum
            if (check(TokKind::Ident))
                return advance().text;
            return {};
        }

        // Multi-word type building: unsigned, signed, long, short
        if (isTypeModifier(first)) {
            advance();
            QStringList parts;
            parts << first;

            // Collect further modifiers and the base type
            while (check(TokKind::Ident) && (isTypeModifier(peek().text) || peek().text == QStringLiteral("int") ||
                   peek().text == QStringLiteral("char") || peek().text == QStringLiteral("long"))) {
                parts << advance().text;
            }
            return parts.join(' ');
        }

        // Simple identifier type
        advance();
        return first;
    }

    void parseStaticAssert() {
        advance(); // "static_assert"
        if (!match(TokKind::LParen)) { skipToSemiOrBrace(); return; }

        // Parse: sizeof(X) == 0xNN
        // Skip to find sizeof
        int depth = 1;
        QString structName;
        int sizeVal = -1;

        // Simple state machine to extract sizeof(StructName) and size value
        while (depth > 0 && peek().kind != TokKind::Eof) {
            if (checkIdent("sizeof")) {
                advance();
                if (match(TokKind::LParen)) {
                    if (check(TokKind::Ident))
                        structName = advance().text;
                    match(TokKind::RParen);
                }
            } else if (peek().kind == TokKind::Number && sizeVal < 0) {
                bool ok;
                QString numText = peek().text;
                if (numText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
                    sizeVal = numText.mid(2).toInt(&ok, 16);
                else
                    sizeVal = numText.toInt(&ok);
                if (!ok) sizeVal = -1;
                advance();
            } else if (peek().kind == TokKind::LParen) {
                depth++;
                advance();
            } else if (peek().kind == TokKind::RParen) {
                depth--;
                if (depth > 0) advance();
            } else {
                advance();
            }
        }
        if (depth == 0) advance(); // consume closing ')'
        match(TokKind::Semi);

        if (!structName.isEmpty() && sizeVal > 0) {
            sizeAsserts[structName] = sizeVal;
        }
    }

    void parseTypedef() {
        advance(); // "typedef"

        // typedef struct { ... } Name;
        if (checkIdent("struct") || checkIdent("class")) {
            QString keyword = peek().text;
            if (peek(1).kind == TokKind::LBrace ||
                (peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::LBrace)) {
                // Full struct typedef - parse as struct, then register alias
                parseStructOrForward();
                return;
            }
            // typedef struct ExistingName * AliasName;
            advance(); // skip struct/class
            if (check(TokKind::Ident)) {
                QString existingName = advance().text;
                // Pointer stars
                bool hasPtr = false;
                while (match(TokKind::Star)) { hasPtr = true; }
                // Skip const/volatile after pointer
                while (checkIdent("const") || checkIdent("volatile")) advance();
                if (check(TokKind::Ident)) {
                    QString aliasName = advance().text;
                    if (aliasName != existingName) { // skip self-referencing typedefs
                        typedefs[aliasName] = existingName;
                        if (hasPtr) pointerTypedefs.insert(aliasName);
                    }
                }
            }
            match(TokKind::Semi);
            return;
        }

        // typedef BaseType [*] AliasName [N];
        // Skip leading const/volatile qualifiers: typedef const Type* Alias;
        while (checkIdent("const") || checkIdent("volatile")) advance();
        QString baseType = parseTypeName();
        if (baseType.isEmpty()) { skipToSemiOrBrace(); return; }
        bool hasPtr = false;
        while (match(TokKind::Star)) { hasPtr = true; }
        // Skip const/volatile after pointer
        while (checkIdent("const") || checkIdent("volatile")) advance();
        while (match(TokKind::Star)) { hasPtr = true; }

        // Function pointer typedef: typedef RetType ( *Name )( args... );
        if (check(TokKind::LParen)) {
            int save = cur;
            advance(); // skip (
            bool isFnPtr = false;
            QString fnName;
            if (match(TokKind::Star) && check(TokKind::Ident)) {
                fnName = advance().text;
                if (match(TokKind::RParen) && check(TokKind::LParen)) {
                    isFnPtr = true;
                }
            }
            if (isFnPtr) {
                // Skip the argument list and register as pointer type
                skipToSemiOrBrace();
                pointerTypedefs.insert(fnName);
                typedefs[fnName] = QStringLiteral("void");
            } else {
                cur = save;
                skipToSemiOrBrace();
            }
            return;
        }

        if (check(TokKind::Ident)) {
            QString alias = advance().text;
            // Array dimensions: typedef Type Name[N][M];
            QVector<int> dims;
            while (check(TokKind::LBracket)) {
                advance();
                if (check(TokKind::Number)) {
                    bool ok;
                    QString numText = peek().text;
                    int val = numText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                        ? numText.mid(2).toInt(&ok, 16) : numText.toInt(&ok);
                    if (ok) dims.append(val);
                    advance();
                }
                match(TokKind::RBracket);
            }
            if (alias != baseType) { // skip self-referencing typedefs
                typedefs[alias] = baseType;
                if (hasPtr) pointerTypedefs.insert(alias);
                if (!dims.isEmpty()) arrayTypedefs[alias] = dims;
            }
        }
        match(TokKind::Semi);
    }

    void parseEnumDef() {
        advance(); // skip "enum"

        // Optional "class" or "struct" (enum class)
        if (checkIdent("class") || checkIdent("struct"))
            advance();

        // Optional name
        QString name;
        if (check(TokKind::Ident) && peek(1).kind != TokKind::Semi) {
            // Could be: enum Name { ... }; or enum Name : Type { ... };
            // But NOT: enum Name; (forward decl) or enum Name field; (field usage)
            if (peek(1).kind == TokKind::LBrace || peek(1).kind == TokKind::Colon) {
                name = advance().text;
            } else {
                // Not an enum definition — revert. This might be a field like "enum Foo bar;"
                return;
            }
        }

        // Optional underlying type: enum Name : uint8_t { ... }
        if (check(TokKind::Colon)) {
            advance();
            parseTypeName(); // skip underlying type
        }

        // Forward declaration: enum Name;
        if (check(TokKind::Semi)) {
            advance();
            return;
        }

        if (!match(TokKind::LBrace)) { skipToSemiOrBrace(); return; }

        ParsedStruct ps;
        ps.name = name;
        ps.keyword = QStringLiteral("enum");

        // Parse enum members: Name [= Value], ...
        int64_t nextValue = 0;
        while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
            if (!check(TokKind::Ident)) { advance(); continue; }
            QString memberName = advance().text;
            int64_t memberValue = nextValue;

            if (check(TokKind::Equals)) {
                advance();
                // Parse value: could be number, negative number, or expression
                bool negative = false;
                if (peek().kind == TokKind::Other && peek().text == QStringLiteral("-")) {
                    negative = true;
                    advance();
                }
                if (check(TokKind::Number)) {
                    bool ok;
                    QString numText = peek().text;
                    if (numText.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
                        memberValue = numText.mid(2).toLongLong(&ok, 16);
                    else
                        memberValue = numText.toLongLong(&ok);
                    if (negative) memberValue = -memberValue;
                    advance();
                } else {
                    // Complex expression — skip to comma or brace
                    while (peek().kind != TokKind::Comma &&
                           peek().kind != TokKind::RBrace &&
                           peek().kind != TokKind::Eof)
                        advance();
                }
            }

            ps.enumValues.append({memberName, memberValue});
            nextValue = memberValue + 1;

            // Skip comma between members
            match(TokKind::Comma);
        }
        match(TokKind::RBrace);
        match(TokKind::Semi);

        if (!ps.name.isEmpty())
            structs.append(ps);
    }
};

// ── Padding field detection ──

static bool isPaddingName(const QString& name) {
    return name.startsWith(QStringLiteral("_pad"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("pad_"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("__pad"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("padding"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("_padding"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("__padding"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("_reserved"), Qt::CaseInsensitive) ||
           name.startsWith(QStringLiteral("reserved"), Qt::CaseInsensitive);
}

// Expand padding into best-fit hex nodes (same approach as import_reclass_xml.cpp)
static void emitHexPadding(NodeTree& tree, uint64_t parentId, int offset, int size) {
    if (size <= 0) return;
    NodeKind hexKind;
    int hexSize;
    if (size >= 8 && size % 8 == 0) {
        hexKind = NodeKind::Hex64; hexSize = 8;
    } else if (size >= 4 && size % 4 == 0) {
        hexKind = NodeKind::Hex32; hexSize = 4;
    } else if (size >= 2 && size % 2 == 0) {
        hexKind = NodeKind::Hex16; hexSize = 2;
    } else {
        hexKind = NodeKind::Hex8; hexSize = 1;
    }
    int count = size / hexSize;
    for (int i = 0; i < count; i++) {
        Node n;
        n.kind = hexKind;
        n.parentId = parentId;
        n.offset = offset + i * hexSize;
        tree.addNode(n);
    }
}

// ── Bitfield grouping: emit a bitfield container with named members ──

static void emitBitfieldGroup(NodeTree& tree, uint64_t parentId, int offset,
                               const QVector<ParsedField>& fields,
                               int startIdx, int endIdx) {
    int totalBits = 0;
    for (int i = startIdx; i < endIdx; i++)
        totalBits += fields[i].bitfieldWidth;
    int bytes = (totalBits + 7) / 8;
    NodeKind containerKind;
    if (bytes <= 1)      containerKind = NodeKind::Hex8;
    else if (bytes <= 2) containerKind = NodeKind::Hex16;
    else if (bytes <= 4) containerKind = NodeKind::Hex32;
    else                 containerKind = NodeKind::Hex64;

    Node n;
    n.kind = NodeKind::Struct;
    n.classKeyword = QStringLiteral("bitfield");
    n.elementKind = containerKind;
    n.parentId = parentId;
    n.offset = offset;
    n.collapsed = false;

    // Populate bitfield members with computed bit offsets
    uint8_t bitOffset = 0;
    for (int i = startIdx; i < endIdx; i++) {
        BitfieldMember bm;
        bm.name = fields[i].name;
        bm.bitOffset = bitOffset;
        bm.bitWidth = (uint8_t)fields[i].bitfieldWidth;
        n.bitfieldMembers.append(bm);
        bitOffset += bm.bitWidth;
    }

    tree.addNode(n);
}

// ── NodeTree builder: recursive field emitter ──

struct BuildContext {
    NodeTree& tree;
    const QHash<QString, TypeInfo>& typeTable;
    QHash<QString, uint64_t>& classIds;
    QVector<PendingRef>& pendingRefs;
    bool useCommentOffsets;
    QSet<QString> enumNames;  // enum type names (emit as UInt32 + refId)
    int ptrSize = 8;          // target pointer size (4 or 8)
    const QHash<QString, int>& sizeAsserts; // declared struct sizes from static_assert
    const QHash<QString, int>& structAlignments; // struct name -> ALIGN(N) value
};

// Forward declaration
static int fieldNaturalAlignment(const ParsedField& field, const BuildContext& ctx);

// Compute natural alignment for a union from its members (max member alignment)
static int unionNaturalAlignment(const ParsedField& field, const BuildContext& ctx) {
    int maxAlign = 1;
    for (const auto& member : field.unionMembers) {
        int a = fieldNaturalAlignment(member, ctx);
        if (a > maxAlign) maxAlign = a;
    }
    return maxAlign;
}

// Return natural alignment for a parsed field (used when computing offsets without comments)
static int fieldNaturalAlignment(const ParsedField& field, const BuildContext& ctx) {
    if (field.isPointer) return ctx.ptrSize;
    if (field.isUnion) return unionNaturalAlignment(field, ctx);
    if (field.bitfieldWidth >= 0) {
        // Bitfield alignment is determined by its storage type
        auto it = ctx.typeTable.find(field.typeName);
        if (it != ctx.typeTable.end()) return alignmentFor(it->kind);
        return 4; // default bitfield alignment
    }
    auto it = ctx.typeTable.find(field.typeName);
    if (it != ctx.typeTable.end()) return alignmentFor(it->kind);
    // Unknown type (struct reference) — align to pointer size
    return ctx.ptrSize;
}

static inline int alignUp(int offset, int align) {
    return (offset + align - 1) & ~(align - 1);
}

// Look up the byte size of a struct type (from already-built tree or static_assert declarations)
static int structTypeSize(const QString& typeName, const BuildContext& ctx) {
    auto classIt = ctx.classIds.find(typeName);
    if (classIt != ctx.classIds.end()) {
        int span = ctx.tree.structSpan(classIt.value());
        if (span > 0) {
            // Pad to struct's declared alignment (ALIGN(N))
            auto alignIt = ctx.structAlignments.find(typeName);
            if (alignIt != ctx.structAlignments.end() && *alignIt > 1)
                span = alignUp(span, *alignIt);
            return span;
        }
    }
    auto sizeIt = ctx.sizeAsserts.find(typeName);
    if (sizeIt != ctx.sizeAsserts.end())
        return sizeIt.value();
    return 0;
}

static void buildFields(BuildContext& ctx, uint64_t parentId, int baseOffset,
                        const QVector<ParsedField>& fields) {
    int computedOffset = 0;

    for (int fi = 0; fi < fields.size(); fi++) {
        const auto& field = fields[fi];

        // Bitfield group: consume consecutive bitfields, emit bitfield container
        if (field.bitfieldWidth >= 0) {
            int groupOffset;
            if (ctx.useCommentOffsets && field.commentOffset >= 0)
                groupOffset = field.commentOffset - baseOffset;
            else {
                int bfAlign = fieldNaturalAlignment(field, ctx);
                computedOffset = alignUp(computedOffset, bfAlign);
                groupOffset = computedOffset;
            }
            int startIdx = fi;
            int totalBits = 0;
            while (fi < fields.size() && fields[fi].bitfieldWidth >= 0) {
                totalBits += fields[fi].bitfieldWidth;
                fi++;
            }
            fi--; // compensate for outer loop increment
            if (totalBits > 0)
                emitBitfieldGroup(ctx.tree, parentId, groupOffset,
                                   fields, startIdx, fi + 1);
            int bytes = (totalBits + 7) / 8;
            int nodeSize = (bytes <= 1) ? 1 : (bytes <= 2) ? 2 : (bytes <= 4) ? 4 : 8;
            computedOffset = groupOffset + nodeSize;
            continue;
        }

        // Union container field
        if (field.isUnion) {
            int unionOffset;
            if (ctx.useCommentOffsets && field.commentOffset >= 0)
                unionOffset = field.commentOffset - baseOffset;
            else {
                int uAlign = fieldNaturalAlignment(field, ctx);
                computedOffset = alignUp(computedOffset, uAlign);
                unionOffset = computedOffset;
            }

            Node unionNode;
            unionNode.kind = NodeKind::Struct;
            unionNode.classKeyword = QStringLiteral("union");
            unionNode.name = field.name;
            unionNode.parentId = parentId;
            unionNode.offset = unionOffset;
            unionNode.collapsed = true;

            int unionIdx = ctx.tree.addNode(unionNode);
            uint64_t unionId = ctx.tree.nodes[unionIdx].id;

            // Build each union member independently so each starts at offset 0
            int absUnionOffset = baseOffset + unionOffset;
            for (const auto& member : field.unionMembers) {
                QVector<ParsedField> single;
                single.append(member);
                buildFields(ctx, unionId, absUnionOffset, single);
            }

            // Advance computed offset past the union (max member size)
            int unionSpan = ctx.tree.structSpan(unionId);
            computedOffset = unionOffset + (unionSpan > 0 ? unionSpan : 0);
            continue;
        }

        int fieldOffset;
        if (ctx.useCommentOffsets && field.commentOffset >= 0)
            fieldOffset = field.commentOffset - baseOffset;
        else {
            int fAlign = fieldNaturalAlignment(field, ctx);
            computedOffset = alignUp(computedOffset, fAlign);
            fieldOffset = computedOffset;
        }

        // Resolve type
        auto typeIt = ctx.typeTable.find(field.typeName);
        bool knownType = typeIt != ctx.typeTable.end();

        // Pointer field
        if (field.isPointer) {
            NodeKind ptrKind = (ctx.ptrSize >= 8) ? NodeKind::Pointer64 : NodeKind::Pointer32;

            // Array of pointers: PVOID arr[N]
            if (!field.arraySizes.isEmpty()) {
                int totalElements = 1;
                for (int dim : field.arraySizes) totalElements *= (dim > 0 ? dim : 1);

                Node n;
                n.kind = NodeKind::Array;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                n.arrayLen = totalElements;
                n.elementKind = ptrKind;
                ctx.tree.addNode(n);
                computedOffset = fieldOffset + totalElements * ctx.ptrSize;
                continue;
            }

            Node n;
            n.kind = ptrKind;
            n.name = field.name;
            n.parentId = parentId;
            n.offset = fieldOffset;
            n.collapsed = true;

            int nodeIdx = ctx.tree.addNode(n);
            uint64_t nodeId = ctx.tree.nodes[nodeIdx].id;

            if (!field.pointerTarget.isEmpty() &&
                field.pointerTarget != QStringLiteral("void")) {
                ctx.pendingRefs.append({nodeId, field.pointerTarget});
            }

            computedOffset = fieldOffset + ctx.ptrSize;
            continue;
        }

        // Enum-typed field: emit as UInt32 with refId to enum definition
        if (!knownType && ctx.enumNames.contains(field.typeName)) {
            int elemSize = 4;
            NodeKind elemKind = NodeKind::UInt32;
            if (!field.arraySizes.isEmpty()) {
                int totalElements = 1;
                for (int dim : field.arraySizes) totalElements *= (dim > 0 ? dim : 1);
                Node n;
                n.kind = NodeKind::Array;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                n.arrayLen = totalElements;
                n.elementKind = elemKind;
                ctx.tree.addNode(n);
                computedOffset = fieldOffset + totalElements * elemSize;
            } else {
                Node n;
                n.kind = elemKind;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                int nodeIdx = ctx.tree.addNode(n);
                uint64_t nodeId = ctx.tree.nodes[nodeIdx].id;
                ctx.pendingRefs.append({nodeId, field.typeName});
                computedOffset = fieldOffset + elemSize;
            }
            continue;
        }

        // Determine base type info
        NodeKind baseKind = NodeKind::Hex8;
        int baseSize = 1;
        bool isStructType = false;

        if (knownType) {
            baseKind = typeIt->kind;
            baseSize = typeIt->size;
        } else {
            isStructType = true;
        }

        // Padding fields
        if (isPaddingName(field.name) && !field.arraySizes.isEmpty()) {
            int totalSize = baseSize;
            for (int dim : field.arraySizes) totalSize *= (dim > 0 ? dim : 1);
            emitHexPadding(ctx.tree, parentId, fieldOffset, totalSize);
            computedOffset = fieldOffset + totalSize;
            continue;
        }

        // Array fields
        if (!field.arraySizes.isEmpty() && !isStructType) {
            int firstDim = field.arraySizes.value(0, 1);
            if (firstDim <= 0) firstDim = 1;

            if (baseKind == NodeKind::Int8 && field.arraySizes.size() == 1 &&
                (field.typeName == QStringLiteral("char") ||
                 field.typeName == QStringLiteral("CHAR"))) {
                Node n;
                n.kind = NodeKind::UTF8;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                n.strLen = firstDim;
                ctx.tree.addNode(n);
                computedOffset = fieldOffset + firstDim;
                continue;
            }

            if (baseKind == NodeKind::UInt16 && field.arraySizes.size() == 1 &&
                (field.typeName == QStringLiteral("wchar_t") ||
                 field.typeName == QStringLiteral("WCHAR") ||
                 field.typeName == QStringLiteral("TCHAR"))) {
                Node n;
                n.kind = NodeKind::UTF16;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                n.strLen = firstDim;
                ctx.tree.addNode(n);
                computedOffset = fieldOffset + firstDim * 2;
                continue;
            }

            if (baseKind == NodeKind::Float && field.arraySizes.size() == 1) {
                if (firstDim == 2) {
                    Node n; n.kind = NodeKind::Vec2; n.name = field.name;
                    n.parentId = parentId; n.offset = fieldOffset;
                    ctx.tree.addNode(n); computedOffset = fieldOffset + 8; continue;
                }
                if (firstDim == 3) {
                    Node n; n.kind = NodeKind::Vec3; n.name = field.name;
                    n.parentId = parentId; n.offset = fieldOffset;
                    ctx.tree.addNode(n); computedOffset = fieldOffset + 12; continue;
                }
                if (firstDim == 4) {
                    Node n; n.kind = NodeKind::Vec4; n.name = field.name;
                    n.parentId = parentId; n.offset = fieldOffset;
                    ctx.tree.addNode(n); computedOffset = fieldOffset + 16; continue;
                }
            }

            if (baseKind == NodeKind::Float && field.arraySizes.size() == 2 &&
                field.arraySizes[0] == 4 && field.arraySizes[1] == 4) {
                Node n; n.kind = NodeKind::Mat4x4; n.name = field.name;
                n.parentId = parentId; n.offset = fieldOffset;
                ctx.tree.addNode(n); computedOffset = fieldOffset + 64; continue;
            }

            int totalElements = 1;
            for (int dim : field.arraySizes) totalElements *= (dim > 0 ? dim : 1);

            Node n;
            n.kind = NodeKind::Array;
            n.name = field.name;
            n.parentId = parentId;
            n.offset = fieldOffset;
            n.arrayLen = totalElements;
            n.elementKind = baseKind;
            ctx.tree.addNode(n);
            computedOffset = fieldOffset + totalElements * baseSize;
            continue;
        }

        // Struct-type field
        if (isStructType) {
            int elemSize = structTypeSize(field.typeName, ctx);

            if (!field.arraySizes.isEmpty()) {
                int totalElements = 1;
                for (int dim : field.arraySizes) totalElements *= (dim > 0 ? dim : 1);

                Node n;
                n.kind = NodeKind::Array;
                n.name = field.name;
                n.parentId = parentId;
                n.offset = fieldOffset;
                n.arrayLen = totalElements;
                n.elementKind = NodeKind::Struct;
                n.structTypeName = field.typeName;
                n.collapsed = true;

                int nodeIdx = ctx.tree.addNode(n);
                uint64_t nodeId = ctx.tree.nodes[nodeIdx].id;
                ctx.pendingRefs.append({nodeId, field.typeName});
                if (elemSize > 0)
                    computedOffset = fieldOffset + totalElements * elemSize;
                continue;
            }

            Node n;
            n.kind = NodeKind::Struct;
            n.name = field.name;
            n.parentId = parentId;
            n.offset = fieldOffset;
            n.structTypeName = field.typeName;
            n.collapsed = true;

            int nodeIdx = ctx.tree.addNode(n);
            uint64_t nodeId = ctx.tree.nodes[nodeIdx].id;
            ctx.pendingRefs.append({nodeId, field.typeName});
            if (elemSize > 0)
                computedOffset = fieldOffset + elemSize;
            continue;
        }

        // Simple primitive field
        Node n;
        n.kind = baseKind;
        n.name = field.name;
        n.parentId = parentId;
        n.offset = fieldOffset;
        ctx.tree.addNode(n);
        computedOffset = fieldOffset + baseSize;
    }
}

// ── Check if any field (or union member) has a comment offset ──

static bool hasAnyCommentOffset(const QVector<ParsedField>& fields) {
    for (const auto& f : fields) {
        if (f.commentOffset >= 0) return true;
        if (f.isUnion && hasAnyCommentOffset(f.unionMembers)) return true;
    }
    return false;
}

// ── NodeTree builder ──

NodeTree importFromSource(const QString& sourceCode, QString* errorMsg, int pointerSize) {
    if (sourceCode.trimmed().isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("Empty source code");
        return {};
    }

    // Tokenize
    Tokenizer tokenizer(sourceCode);
    tokenizer.tokenize();

    // Parse
    Parser parser(tokenizer.tokens, tokenizer.offsets);
    parser.parse();

    if (parser.structs.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("No struct or enum definitions found");
        return {};
    }

    // Build type table (pointer-size types depend on target architecture)
    QHash<QString, TypeInfo> typeTable = buildTypeTable(pointerSize);

    // Register typedefs into type table
    for (auto it = parser.typedefs.begin(); it != parser.typedefs.end(); ++it) {
        if (typeTable.contains(it.value())) {
            typeTable[it.key()] = typeTable[it.value()];
        }
    }

    NodeTree tree;
    tree.baseAddress = 0x00400000;
    tree.pointerSize = pointerSize;

    QHash<QString, uint64_t> classIds;
    QVector<PendingRef> pendingRefs;

    // Determine offset mode: if ANY field in ANY struct has a comment offset, use comment mode
    bool useCommentOffsets = false;
    for (const auto& ps : parser.structs) {
        if (hasAnyCommentOffset(ps.fields)) { useCommentOffsets = true; break; }
    }

    // Collect enum type names for field-type detection
    QSet<QString> enumNames;
    for (const auto& ps : parser.structs) {
        if (ps.keyword == QStringLiteral("enum") && !ps.name.isEmpty())
            enumNames.insert(ps.name);
    }

    BuildContext ctx{tree, typeTable, classIds, pendingRefs, useCommentOffsets, enumNames, pointerSize, parser.sizeAsserts, parser.structAlignments};

    // Build nodes for each struct/enum
    for (const auto& ps : parser.structs) {
        Node structNode;
        structNode.kind = NodeKind::Struct;
        structNode.name = ps.name;
        structNode.structTypeName = ps.name;
        structNode.classKeyword = ps.keyword;
        structNode.parentId = 0;
        structNode.offset = 0;
        structNode.collapsed = true;

        // Enum: store members directly on the node, no child fields
        if (ps.keyword == QStringLiteral("enum")) {
            structNode.enumMembers = ps.enumValues;
            int idx = tree.addNode(structNode);
            uint64_t nodeId = tree.nodes[idx].id;
            if (!ps.name.isEmpty())
                classIds[ps.name] = nodeId;
            continue;
        }

        int structIdx = tree.addNode(structNode);
        uint64_t structId = tree.nodes[structIdx].id;
        classIds[ps.name] = structId;

        buildFields(ctx, structId, 0, ps.fields);

        // Union: all direct children overlap at offset 0
        if (ps.keyword == QStringLiteral("union")) {
            QVector<int> children = tree.childrenOf(structId);
            for (int ci : children)
                tree.nodes[ci].offset = 0;
        }

        // Apply static_assert size: add tail padding if needed
        auto sizeIt = parser.sizeAsserts.find(ps.name);
        if (sizeIt != parser.sizeAsserts.end()) {
            int declaredSize = sizeIt.value();
            int currentSpan = tree.structSpan(structId);
            if (declaredSize > currentSpan) {
                emitHexPadding(tree, structId, currentSpan, declaredSize - currentSpan);
            }
        }
    }

    if (tree.nodes.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("No nodes generated from source");
        return {};
    }

    // Resolve deferred pointer/struct references
    for (const auto& ref : pendingRefs) {
        int nodeIdx = tree.indexOfId(ref.nodeId);
        if (nodeIdx < 0) continue;

        auto it = classIds.find(ref.className);
        if (it != classIds.end()) {
            tree.nodes[nodeIdx].refId = it.value();
        }
    }

    return tree;
}

} // namespace rcx
