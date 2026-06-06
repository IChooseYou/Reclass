// Unit tests for ClipboardCodec::parseLenientHex.
//
// The function turns clipboard text into raw bytes for the byte-range
// paste flow. Contract (from clipboard.h):
//   - tokenize on any non-hex character
//   - strip a leading 0x/0X prefix per token (not mid-token)
//   - left-pad each token to even nibble count
//   - concatenate left-to-right
//
// Notable edge cases this test locks down:
//   * "0x100" produces TWO bytes (01 00), not one — the previous
//     implementation chopped the leading 0 along with the x, then
//     never padded the odd-length tail, yielding "10" → 1 byte.
//   * "1 2 3" produces [01 02 03], not [12 30].
//   * A mid-token x ("DEx") is malformed; old code silently truncated.

#include <QtTest/QTest>
#include <QByteArray>
#include "clipboard.h"

using namespace rcx;

class TestLenientHex : public QObject {
    Q_OBJECT

    static QByteArray hex(std::initializer_list<uint8_t> bytes) {
        QByteArray b;
        for (auto v : bytes) b.append((char)v);
        return b;
    }

private slots:
    void testHexDumpForm() {
        QCOMPARE(ClipboardCodec::parseLenientHex("DE AD BE EF"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    void testSingleTokenNoSeparators() {
        QCOMPARE(ClipboardCodec::parseLenientHex("DEADBEEF"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    void testCStyleArrayLiteral() {
        QCOMPARE(ClipboardCodec::parseLenientHex("{0xDE, 0xAD, 0xBE, 0xEF}"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    void testCommaSeparated() {
        QCOMPARE(ClipboardCodec::parseLenientHex("DE,AD,BE,EF"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    void testMixedCase() {
        QCOMPARE(ClipboardCodec::parseLenientHex("de Ad bE Ef"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    void testZeroXPrefixOnly() {
        QCOMPARE(ClipboardCodec::parseLenientHex("0xDEADBEEF"),
                 hex({0xDE, 0xAD, 0xBE, 0xEF}));
    }

    // ── The bug this work was triggered by ─────────────────────────
    // "0x100" should produce TWO bytes 0x01 0x00.
    // The previous parser produced one byte 0x10.
    void testSingleSmallLiteralLeftPads() {
        QCOMPARE(ClipboardCodec::parseLenientHex("0x100"),
                 hex({0x01, 0x00}));
    }

    void testSingleOneDigitLiteral() {
        // "0xA" → token "A" → left-pad → "0A" → one byte 0x0A
        QCOMPARE(ClipboardCodec::parseLenientHex("0xA"),
                 hex({0x0A}));
    }

    // Per-token left-pad: bare "1 2 3" → three bytes 01 02 03,
    // not one byte 0x12 with a stray trailing nibble.
    void testPerTokenLeftPad() {
        QCOMPARE(ClipboardCodec::parseLenientHex("1 2 3"),
                 hex({0x01, 0x02, 0x03}));
    }

    void testMultipleSmallLiterals() {
        QCOMPARE(ClipboardCodec::parseLenientHex("0x1 0x2 0x3"),
                 hex({0x01, 0x02, 0x03}));
    }

    void testEmptyInput() {
        QString err;
        QByteArray out = ClipboardCodec::parseLenientHex("", &err);
        QVERIFY(out.isEmpty());
        QCOMPARE(err, QStringLiteral("No hex data"));
    }

    void testWhitespaceOnly() {
        QString err;
        QByteArray out = ClipboardCodec::parseLenientHex("   \n\t  ", &err);
        QVERIFY(out.isEmpty());
        QCOMPARE(err, QStringLiteral("No hex data"));
    }

    // Garbage tokens should bail out cleanly with a readable error,
    // not silently parse partial data.
    void testInvalidCharacterFails() {
        QString err;
        QByteArray out = ClipboardCodec::parseLenientHex("DE GG", &err);
        QVERIFY(out.isEmpty());
        QVERIFY2(err.contains("Invalid"), qPrintable(err));
    }

    // Mid-token 'x' is not allowed (only the leading 0x/0X prefix).
    void testMidTokenXIsInvalid() {
        QString err;
        QByteArray out = ClipboardCodec::parseLenientHex("DEx0", &err);
        QVERIFY(out.isEmpty());
    }

    // Backwards compatibility with the hex-dump preview format
    // (uppercase, space-separated). Reclass copies bytes back in this
    // exact form so round-trip copy→paste must be lossless.
    void testHexDumpRoundtrip() {
        QByteArray src = hex({0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57});
        QString preview;
        for (int i = 0; i < src.size(); ++i) {
            if (i > 0) preview += QLatin1Char(' ');
            preview += QStringLiteral("%1").arg(
                (uint8_t)src[i], 2, 16, QLatin1Char('0')).toUpper();
        }
        QCOMPARE(ClipboardCodec::parseLenientHex(preview), src);
    }

    // Bonus: WinDbg-style backtick-grouped 64-bit values.
    // `7ff6`6cce0000` → tokens "7ff6", "6cce0000" → [7F F6 6C CE 00 00]
    void testBacktickGrouped() {
        QCOMPARE(ClipboardCodec::parseLenientHex("7ff6`6cce0000"),
                 hex({0x7F, 0xF6, 0x6C, 0xCE, 0x00, 0x00}));
    }
};

QTEST_APPLESS_MAIN(TestLenientHex)
#include "test_lenient_hex.moc"
