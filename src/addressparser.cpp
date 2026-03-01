#include "addressparser.h"

namespace rcx {

// ── Address Expression Parser ──────────────────────────────────────────
//
// Parses expressions like:
//   "7FF66CCE0000"                    → plain hex address
//   "0x100 + 0x200"                   → arithmetic on hex values
//   "<Program.exe> + 0xDE"            → module base + offset
//   "[<Program.exe> + 0xDE] - AB"     → dereference pointer, then subtract
//   "7ff6`6cce0000"                   → WinDbg-style backtick separator (stripped before parsing)
//   "base + e_lfanew"                 → C/C++ style identifier resolution
//   "0xFF & 0x0F"                     → bitwise AND
//   "1 << 4"                          → shift left
//
// Grammar (C operator precedence):
//
//   bitwiseOr  = bitwiseXor ('|' bitwiseXor)*
//   bitwiseXor = bitwiseAnd ('^' bitwiseAnd)*
//   bitwiseAnd = shift ('&' shift)*
//   shift      = expr (('<<' | '>>') expr)*
//   expr       = term (('+' | '-') term)*
//   term       = unary (('*' | '/') unary)*
//   unary      = '-' unary | '~' unary | atom
//   atom       = '[' bitwiseOr ']'    -- read pointer at address (dereference)
//              | '<' moduleName '>'   -- resolve module base address
//              | '(' bitwiseOr ')'    -- grouping
//              | identifier           -- C/C++ name resolved via callback
//              | hexLiteral           -- hex number, optional 0x prefix
//
// All numeric literals are hexadecimal (base 16).
// Identifiers: [a-zA-Z_][a-zA-Z0-9_]* containing at least one non-hex char.
// Pure hex-digit words (e.g. "DEAD") are treated as hex literals.

class ExpressionParser {
public:
    ExpressionParser(const QString& input, const AddressParserCallbacks* callbacks)
        : m_input(input), m_callbacks(callbacks) {}

    AddressParseResult parse() {
        skipSpaces();
        if (atEnd())
            return error("empty expression");

        uint64_t value = 0;
        if (!parseBitwiseOr(value))
            return error(m_error);

        skipSpaces();
        if (!atEnd())
            return error(QStringLiteral("unexpected '%1'").arg(m_input[m_pos]));

        return {true, value, {}, -1};
    }

private:
    const QString& m_input;
    const AddressParserCallbacks* m_callbacks;
    int m_pos = 0;
    QString m_error;
    int m_errorPos = 0;

    // ── Helpers ──

    bool atEnd() const { return m_pos >= m_input.size(); }

    QChar peek() const { return atEnd() ? QChar('\0') : m_input[m_pos]; }

    void advance() { m_pos++; }

    void skipSpaces() {
        while (!atEnd() && m_input[m_pos].isSpace())
            m_pos++;
    }

    AddressParseResult error(const QString& msg) const {
        return {false, 0, msg, m_errorPos};
    }

    bool fail(const QString& msg) {
        m_error = msg;
        m_errorPos = m_pos;
        return false;
    }

    bool expect(QChar ch) {
        skipSpaces();
        if (peek() != ch)
            return fail(QStringLiteral("expected '%1'").arg(ch));
        advance();
        return true;
    }

    static bool isHexDigit(QChar ch) {
        return (ch >= '0' && ch <= '9')
            || (ch >= 'a' && ch <= 'f')
            || (ch >= 'A' && ch <= 'F');
    }

    static bool isIdentStart(QChar ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    }

    static bool isIdentChar(QChar ch) {
        return isIdentStart(ch) || (ch >= '0' && ch <= '9');
    }

    // ── Recursive descent parsing ──

    // bitwiseOr = bitwiseXor ('|' bitwiseXor)*
    bool parseBitwiseOr(uint64_t& result) {
        if (!parseBitwiseXor(result))
            return false;
        for (;;) {
            skipSpaces();
            if (peek() != '|')
                break;
            advance();
            uint64_t rhs = 0;
            if (!parseBitwiseXor(rhs))
                return false;
            result |= rhs;
        }
        return true;
    }

    // bitwiseXor = bitwiseAnd ('^' bitwiseAnd)*
    bool parseBitwiseXor(uint64_t& result) {
        if (!parseBitwiseAnd(result))
            return false;
        for (;;) {
            skipSpaces();
            if (peek() != '^')
                break;
            advance();
            uint64_t rhs = 0;
            if (!parseBitwiseAnd(rhs))
                return false;
            result ^= rhs;
        }
        return true;
    }

    // bitwiseAnd = shift ('&' shift)*
    bool parseBitwiseAnd(uint64_t& result) {
        if (!parseShift(result))
            return false;
        for (;;) {
            skipSpaces();
            if (peek() != '&')
                break;
            advance();
            uint64_t rhs = 0;
            if (!parseShift(rhs))
                return false;
            result &= rhs;
        }
        return true;
    }

    // shift = expr (('<<' | '>>') expr)*
    bool parseShift(uint64_t& result) {
        if (!parseExpression(result))
            return false;
        for (;;) {
            skipSpaces();
            QChar c = peek();
            if (c != '<' && c != '>')
                break;
            // Must be << or >> (not < or > alone)
            if (m_pos + 1 >= m_input.size() || m_input[m_pos + 1] != c)
                break;
            bool isLeft = (c == '<');
            advance(); advance(); // skip << or >>
            uint64_t rhs = 0;
            if (!parseExpression(rhs))
                return false;
            result = isLeft ? (result << rhs) : (result >> rhs);
        }
        return true;
    }

    // expr = term (('+' | '-') term)*
    bool parseExpression(uint64_t& result) {
        if (!parseTerm(result))
            return false;

        for (;;) {
            skipSpaces();
            QChar op = peek();
            if (op != '+' && op != '-')
                break;
            advance();

            uint64_t rhs = 0;
            if (!parseTerm(rhs))
                return false;

            result = (op == '+') ? result + rhs : result - rhs;
        }
        return true;
    }

    // term = unary (('*' | '/') unary)*
    bool parseTerm(uint64_t& result) {
        if (!parseUnary(result))
            return false;

        for (;;) {
            skipSpaces();
            QChar op = peek();
            if (op != '*' && op != '/')
                break;
            advance();

            uint64_t rhs = 0;
            if (!parseUnary(rhs))
                return false;

            if (op == '*') {
                result *= rhs;
            } else {
                if (rhs == 0)
                    return fail("division by zero");
                result /= rhs;
            }
        }
        return true;
    }

    // unary = '-' unary | '~' unary | atom
    bool parseUnary(uint64_t& result) {
        skipSpaces();
        if (peek() == '-') {
            advance();
            uint64_t inner = 0;
            if (!parseUnary(inner))
                return false;
            result = static_cast<uint64_t>(-static_cast<int64_t>(inner));
            return true;
        }
        if (peek() == '~') {
            advance();
            uint64_t inner = 0;
            if (!parseUnary(inner))
                return false;
            result = ~inner;
            return true;
        }
        return parseAtom(result);
    }

    // atom = '[' bitwiseOr ']' | '<' name '>' | '(' bitwiseOr ')' | identifier | hexLiteral
    bool parseAtom(uint64_t& result) {
        skipSpaces();
        if (atEnd())
            return fail("unexpected end of expression");

        QChar ch = peek();

        if (ch == '[') return parseDereference(result);
        if (ch == '<') return parseModuleName(result);
        if (ch == '(') return parseGrouping(result);

        // Try identifier before hex — identifiers start with [a-zA-Z_]
        if (isIdentStart(ch))
            return parseIdentifierOrHex(result);

        return parseHexNumber(result);
    }

    // Identifier or hex literal disambiguation.
    // Scan [a-zA-Z_][a-zA-Z0-9_]*. If it contains any non-hex char → identifier.
    // Otherwise → backtrack and parse as hex number.
    bool parseIdentifierOrHex(uint64_t& result) {
        int start = m_pos;
        bool hasNonHex = false;

        // Scan full token
        while (!atEnd() && isIdentChar(peek())) {
            if (!isHexDigit(peek()))
                hasNonHex = true;
            advance();
        }

        QString token = m_input.mid(start, m_pos - start);

        if (!hasNonHex) {
            // Pure hex digits (e.g. "DEAD") — backtrack, parse as hex
            m_pos = start;
            return parseHexNumber(result);
        }

        // It's an identifier — resolve via callback
        if (!m_callbacks || !m_callbacks->resolveIdentifier) {
            result = 0;
            return true;
        }

        bool ok = false;
        result = m_callbacks->resolveIdentifier(token, &ok);
        if (!ok)
            return fail(QStringLiteral("unknown identifier '%1'").arg(token));
        return true;
    }

    // '[' bitwiseOr ']' — read the pointer value at the computed address
    bool parseDereference(uint64_t& result) {
        advance(); // skip '['

        uint64_t address = 0;
        if (!parseBitwiseOr(address))
            return false;
        if (!expect(']'))
            return false;

        // Without a callback, just return 0 (syntax-check mode)
        if (!m_callbacks || !m_callbacks->readPointer) {
            result = 0;
            return true;
        }

        bool ok = false;
        result = m_callbacks->readPointer(address, &ok);
        if (!ok)
            return fail(QStringLiteral("failed to read memory at 0x%1").arg(address, 0, 16));
        return true;
    }

    // '<' moduleName '>' — resolve a module's base address (e.g. <Program.exe>)
    bool parseModuleName(uint64_t& result) {
        advance(); // skip '<'

        int nameStart = m_pos;
        while (!atEnd() && peek() != '>')
            advance();
        if (atEnd())
            return fail("expected '>'");

        QString name = m_input.mid(nameStart, m_pos - nameStart).trimmed();
        advance(); // skip '>'

        if (name.isEmpty())
            return fail("empty module name");

        // Without a callback, just return 0 (syntax-check mode)
        if (!m_callbacks || !m_callbacks->resolveModule) {
            result = 0;
            return true;
        }

        bool ok = false;
        result = m_callbacks->resolveModule(name, &ok);
        if (!ok)
            return fail(QStringLiteral("module '%1' not found").arg(name));
        return true;
    }

    // '(' bitwiseOr ')' — parenthesized sub-expression for grouping
    bool parseGrouping(uint64_t& result) {
        advance(); // skip '('
        if (!parseBitwiseOr(result))
            return false;
        return expect(')');
    }

    // Hex number with optional "0x" prefix. All literals are base-16.
    bool parseHexNumber(uint64_t& result) {
        skipSpaces();
        if (atEnd())
            return fail("unexpected end of expression");

        int start = m_pos;

        // Skip optional 0x/0X prefix
        if (m_pos + 1 < m_input.size()
            && m_input[m_pos] == '0'
            && (m_input[m_pos + 1] == 'x' || m_input[m_pos + 1] == 'X'))
            m_pos += 2;

        // Consume hex digits
        int digitsStart = m_pos;
        while (!atEnd() && isHexDigit(peek()))
            advance();

        if (m_pos == digitsStart) {
            m_errorPos = start;
            return fail("expected hex number");
        }

        QString digits = m_input.mid(digitsStart, m_pos - digitsStart);
        bool ok = false;
        result = digits.toULongLong(&ok, 16);
        if (!ok) {
            m_errorPos = start;
            return fail("invalid hex number");
        }
        return true;
    }
};

// ── Public API ─────────────────────────────────────────────────────────

AddressParseResult AddressParser::evaluate(const QString& formula, int ptrSize,
                                           const AddressParserCallbacks* cb)
{
    // ptrSize is used by the caller to configure the readPointer callback;
    // the parser itself doesn't need it directly.
    Q_UNUSED(ptrSize);

    // WinDbg displays 64-bit addresses with backtick separators for readability,
    // e.g. "00007ff6`1a2b3c4d". Strip them so users can paste directly.
    // Also remove ' in case user uses it
    QString cleaned = formula;
    cleaned.remove('`');
    cleaned.remove('\'');

    ExpressionParser parser(cleaned, cb);
    return parser.parse();
}

QString AddressParser::validate(const QString& formula)
{
    QString cleaned = formula;
    cleaned.remove('`');
    cleaned.remove('\'');
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty())
        return QStringLiteral("empty");

    // Parse with no callbacks — modules, dereferences, identifiers succeed but return 0.
    // This checks syntax only.
    ExpressionParser parser(cleaned, nullptr);
    auto result = parser.parse();
    return result.ok ? QString() : result.error;
}

} // namespace rcx
