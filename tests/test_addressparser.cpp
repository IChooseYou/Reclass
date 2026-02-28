#include "addressparser.h"
#include <QTest>

using rcx::AddressParser;
using rcx::AddressParserCallbacks;
using rcx::AddressParseResult;

class TestAddressParser : public QObject {
    Q_OBJECT

private slots:
    // -- Hex literals --

    void bareHex()      { auto r = AddressParser::evaluate("AB");          QVERIFY(r.ok); QCOMPARE(r.value, 0xABULL); }
    void prefixedHex()  { auto r = AddressParser::evaluate("0x1F4");       QVERIFY(r.ok); QCOMPARE(r.value, 0x1F4ULL); }
    void zeroLiteral()  { auto r = AddressParser::evaluate("0");           QVERIFY(r.ok); QCOMPARE(r.value, 0ULL); }
    void large64bit()   { auto r = AddressParser::evaluate("7FF66CCE0000");QVERIFY(r.ok); QCOMPARE(r.value, 0x7FF66CCE0000ULL); }

    // -- Arithmetic --

    void addition() {
        auto r = AddressParser::evaluate("0x100 + 0x200");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x300ULL);
    }
    void subtraction() {
        auto r = AddressParser::evaluate("0x300 - 0x100");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x200ULL);
    }
    void multiplication() {
        auto r = AddressParser::evaluate("0x10 * 4");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x40ULL);
    }
    void division() {
        auto r = AddressParser::evaluate("0x100 / 2");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x80ULL);
    }
    void precedence() {
        // 0x10 + 2*3 = 0x10 + 6 = 0x16
        auto r = AddressParser::evaluate("0x10 + 2 * 3");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x16ULL);
    }
    void parentheses() {
        // (0x10 + 2) * 3 = 0x12 * 3 = 0x36
        auto r = AddressParser::evaluate("(0x10 + 2) * 3");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x36ULL);
    }

    // -- Unary minus --

    void unaryMinus() {
        auto r = AddressParser::evaluate("-0x10 + 0x20");
        QVERIFY(r.ok); QCOMPARE(r.value, 0x10ULL);
    }

    // -- Module resolution --

    void moduleResolve() {
        AddressParserCallbacks cbs;
        cbs.resolveModule = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "Program.exe");
            return *ok ? 0x140000000ULL : 0;
        };
        auto r = AddressParser::evaluate("<Program.exe> + 0x123", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x140000123ULL);
    }

    void moduleNotFound() {
        AddressParserCallbacks cbs;
        cbs.resolveModule = [](const QString&, bool* ok) -> uint64_t {
            *ok = false;
            return 0;
        };
        auto r = AddressParser::evaluate("<NoSuch.dll>", 8, &cbs);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("not found"));
    }

    // -- Dereference --

    void derefSimple() {
        AddressParserCallbacks cbs;
        cbs.readPointer = [](uint64_t addr, bool* ok) -> uint64_t {
            *ok = (addr == 0x1000);
            return *ok ? 0xDEADBEEFULL : 0;
        };
        auto r = AddressParser::evaluate("[0x1000]", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xDEADBEEFULL);
    }

    void derefNested() {
        AddressParserCallbacks cbs;
        cbs.resolveModule = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "mod");
            return *ok ? 0x400000ULL : 0;
        };
        cbs.readPointer = [](uint64_t addr, bool* ok) -> uint64_t {
            *ok = true;
            if (addr == 0x400100) return 0x500000;
            if (addr == 0x900000) return 0xABCDEF;
            return 0;
        };
        // [<mod> + [<mod> + 0x100]] = [0x400000 + [0x400000+0x100]]
        //   inner deref: [0x400100] = 0x500000
        //   outer: [0x400000 + 0x500000] = [0x900000] = 0xABCDEF
        auto r = AddressParser::evaluate("[<mod> + [<mod> + 0x100]]", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xABCDEFULL);
    }

    void derefReadFailure() {
        AddressParserCallbacks cbs;
        cbs.readPointer = [](uint64_t, bool* ok) -> uint64_t {
            *ok = false;
            return 0;
        };
        auto r = AddressParser::evaluate("[0x1000]", 8, &cbs);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("failed to read"));
    }

    // -- Complex expression from plan --

    void complexExpr() {
        AddressParserCallbacks cbs;
        cbs.resolveModule = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "Program.exe");
            return *ok ? 0x140000000ULL : 0;
        };
        cbs.readPointer = [](uint64_t addr, bool* ok) -> uint64_t {
            *ok = true;
            if (addr == 0x1400000DEULL) return 0x500000;
            return 0;
        };
        // [<Program.exe> + 0xDE] - AB = [0x1400000DE] - 0xAB = 0x500000 - 0xAB = 0x4FFF55
        auto r = AddressParser::evaluate("[<Program.exe> + 0xDE] - AB", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x4FFF55ULL);
    }

    // -- Errors --

    void emptyInput() {
        auto r = AddressParser::evaluate("");
        QVERIFY(!r.ok);
    }
    void unmatchedBracket() {
        auto r = AddressParser::evaluate("[0x1000");
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("']'"));
    }
    void unmatchedAngle() {
        auto r = AddressParser::evaluate("<Program.exe");
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("'>'"));
    }
    void divisionByZero() {
        auto r = AddressParser::evaluate("0x100 / 0");
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("division by zero"));
    }
    void trailingGarbage() {
        auto r = AddressParser::evaluate("0x100 xyz");
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("unexpected"));
    }
    void trailingOperator() {
        auto r = AddressParser::evaluate("0x100 +");
        QVERIFY(!r.ok);
    }

    // -- Validation --

    void validateValid() {
        QCOMPARE(AddressParser::validate("0x100 + 0x200"), QString());
        QCOMPARE(AddressParser::validate("<Prog.exe> + [0x100]"), QString());
    }
    void validateInvalid() {
        QVERIFY(!AddressParser::validate("").isEmpty());
        QVERIFY(!AddressParser::validate("[0x100").isEmpty());
        QVERIFY(!AddressParser::validate("0x100 xyz").isEmpty());
    }

    // -- Backtick stripping --

    void backtickStripping() {
        auto r = AddressParser::evaluate("7ff6`6cce0000");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x7FF66CCE0000ULL);
    }

    // -- Whitespace tolerance --

    void whitespace() {
        auto r = AddressParser::evaluate("  0x100  +  0x200  ");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x300ULL);
    }

    // -- Legacy compat: simple hex --

    void simpleHexAddress() {
        auto r = AddressParser::evaluate("140000000");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x140000000ULL);
    }

    // -- Multiple additions --

    void multipleAdditions() {
        auto r = AddressParser::evaluate("0x100 + 0x200 + 0x300");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x600ULL);
    }

    // -- Identifier resolution --

    void identBase() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "base");
            return *ok ? 0x140000000ULL : 0;
        };
        auto r = AddressParser::evaluate("base", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x140000000ULL);
    }

    void identFieldName() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base")     { *ok = true; return 0x140000000ULL; }
            if (name == "e_lfanew") { *ok = true; return 0xE8ULL; }
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("base + e_lfanew", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x1400000E8ULL);
    }

    void identUnknown() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString&, bool* ok) -> uint64_t {
            *ok = false; return 0;
        };
        auto r = AddressParser::evaluate("unknown_var", 8, &cbs);
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("unknown identifier"));
    }

    // -- Hex vs identifier disambiguation --

    void hexDisambigDEAD() {
        // "DEAD" is all hex digits → should parse as hex number 0xDEAD
        auto r = AddressParser::evaluate("DEAD");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xDEADULL);
    }

    void hexDisambigBase() {
        // "base" has 's' (non-hex) → identifier
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "base"); return *ok ? 42ULL : 0;
        };
        auto r = AddressParser::evaluate("base", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 42ULL);
    }

    void hexDisambigABCwithUnderscore() {
        // "ABC_field" has '_' → identifier, not hex
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            *ok = (name == "ABC_field"); return *ok ? 99ULL : 0;
        };
        auto r = AddressParser::evaluate("ABC_field", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 99ULL);
    }

    // -- Bitwise operators --

    void bitwiseAnd() {
        auto r = AddressParser::evaluate("0xFF & 0x0F");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x0FULL);
    }

    void bitwiseOr() {
        auto r = AddressParser::evaluate("0xA0 | 0x0B");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xABULL);
    }

    void bitwiseXor() {
        auto r = AddressParser::evaluate("0xA ^ 0x5");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xFULL);
    }

    void shiftLeft() {
        auto r = AddressParser::evaluate("1 << 4");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x10ULL);
    }

    void shiftRight() {
        auto r = AddressParser::evaluate("0xFF00 >> 8");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xFFULL);
    }

    // -- Unary bitwise NOT --

    void unaryNot() {
        auto r = AddressParser::evaluate("~0");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xFFFFFFFFFFFFFFFFULL);
    }

    void unaryNotMask() {
        // ~0xFFF = 0xFFFFFFFFFFFFF000
        auto r = AddressParser::evaluate("~0xFFF");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xFFFFFFFFFFFFF000ULL);
    }

    // -- Operator precedence --

    void shiftPrecedence() {
        // C precedence: shift binds looser than addition
        // 1 + 2 << 3 = (1 + 2) << 3 = 3 << 3 = 24 = 0x18
        auto r = AddressParser::evaluate("1 + 2 << 3");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x18ULL);
    }

    void andOrPrecedence() {
        // & binds tighter than |
        // 0xFF | 0x100 & 0xF00 = 0xFF | (0x100 & 0xF00) = 0xFF | 0x100 = 0x1FF
        auto r = AddressParser::evaluate("0xFF | 0x100 & 0xF00");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x1FFULL);
    }

    void xorPrecedence() {
        // ^ between & and |: a | b ^ c & d = a | (b ^ (c & d))
        // 0xF0 | 0x0F ^ 0xFF & 0x0F = 0xF0 | (0x0F ^ (0xFF & 0x0F))
        //   = 0xF0 | (0x0F ^ 0x0F) = 0xF0 | 0x00 = 0xF0
        auto r = AddressParser::evaluate("0xF0 | 0x0F ^ 0xFF & 0x0F");
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0xF0ULL);
    }

    // -- E_lfanew end-to-end --

    void elfanewScenario() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base")     { *ok = true; return 0x140000000ULL; }
            if (name == "e_lfanew") { *ok = true; return 0xE8ULL; }
            *ok = false; return 0;
        };
        // base + e_lfanew = 0x140000000 + 0xE8 = 0x1400000E8
        auto r = AddressParser::evaluate("base + e_lfanew", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x1400000E8ULL);
    }

    void pageAlignedExpr() {
        AddressParserCallbacks cbs;
        cbs.resolveIdentifier = [](const QString& name, bool* ok) -> uint64_t {
            if (name == "base")     { *ok = true; return 0x140000000ULL; }
            if (name == "e_lfanew") { *ok = true; return 0xE8ULL; }
            *ok = false; return 0;
        };
        // (base + e_lfanew) & ~0xFFF = 0x1400000E8 & ~0xFFF = 0x140000000
        auto r = AddressParser::evaluate("(base + e_lfanew) & ~0xFFF", 8, &cbs);
        QVERIFY(r.ok);
        QCOMPARE(r.value, 0x140000000ULL);
    }

    // -- Validate with new syntax --

    void validateIdentifier() {
        QCOMPARE(AddressParser::validate("base + e_lfanew"), QString());
    }

    void validateBitwiseOps() {
        QCOMPARE(AddressParser::validate("0xFF & 0x0F"), QString());
        QCOMPARE(AddressParser::validate("1 << 4"), QString());
        QCOMPARE(AddressParser::validate("~0xFFF"), QString());
    }
};

QTEST_GUILESS_MAIN(TestAddressParser)
#include "test_addressparser.moc"
