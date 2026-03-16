#include <QtTest/QTest>
#include <QApplication>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <Qsci/qscilexercpp.h>
#include <QColor>
#include <QFont>

#include "core.h"
#include "generator.h"

// Raw Scintilla message IDs not exposed by QsciScintillaBase wrapper
static constexpr int SCI_GETSELBACK  = 2477;
static constexpr int SCI_GETSELFORE  = 2476;

// ── Helper: extract BGR long from QColor (Scintilla stores colors as 0x00BBGGRR) ──

static long toBGR(const QColor& c) {
    return (long)c.red() | ((long)c.green() << 8) | ((long)c.blue() << 16);
}

// ── Replicates MainWindow::setupRenderedSci so the test stays in sync ──

static void setupRenderedSci(QsciScintilla* sci) {
    QFont f("Consolas", 12);
    f.setFixedPitch(true);

    sci->setFont(f);
    sci->setReadOnly(false);
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setTabWidth(4);
    sci->setIndentationsUseTabs(false);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT, (long)2);
    sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT, (long)2);

    // Line number margin
    sci->setMarginType(0, QsciScintilla::NumberMargin);
    sci->setMarginWidth(0, "00000");
    sci->setMarginsBackgroundColor(QColor("#252526"));
    sci->setMarginsForegroundColor(QColor("#858585"));
    sci->setMarginsFont(f);

    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);

    // Lexer FIRST — setLexer() resets caret/selection/paper colors
    auto* lexer = new QsciLexerCPP(sci);
    lexer->setFont(f);
    lexer->setColor(QColor("#569cd6"), QsciLexerCPP::Keyword);
    lexer->setColor(QColor("#569cd6"), QsciLexerCPP::KeywordSet2);
    lexer->setColor(QColor("#b5cea8"), QsciLexerCPP::Number);
    lexer->setColor(QColor("#ce9178"), QsciLexerCPP::DoubleQuotedString);
    lexer->setColor(QColor("#ce9178"), QsciLexerCPP::SingleQuotedString);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::Comment);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::CommentLine);
    lexer->setColor(QColor("#6a9955"), QsciLexerCPP::CommentDoc);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Default);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Identifier);
    lexer->setColor(QColor("#c586c0"), QsciLexerCPP::PreProcessor);
    lexer->setColor(QColor("#d4d4d4"), QsciLexerCPP::Operator);
    for (int i = 0; i <= 127; i++) {
        lexer->setPaper(QColor("#1e1e1e"), i);
        lexer->setFont(f, i);
    }
    sci->setLexer(lexer);
    sci->setBraceMatching(QsciScintilla::NoBraceMatch);

    // Colors AFTER setLexer() — the lexer resets these on attach
    sci->setPaper(QColor("#1e1e1e"));
    sci->setColor(QColor("#d4d4d4"));
    sci->setCaretForegroundColor(QColor("#d4d4d4"));
    sci->setCaretLineVisible(true);
    sci->setCaretLineBackgroundColor(QColor(43, 43, 43));
    sci->setSelectionBackgroundColor(QColor("#264f78"));
    sci->setSelectionForegroundColor(QColor("#d4d4d4"));
}

// ── Test tree helper ──

static rcx::NodeTree makeTestTree() {
    rcx::NodeTree tree;
    rcx::Node root;
    root.kind = rcx::NodeKind::Struct;
    root.name = "TestStruct";
    root.structTypeName = "TestStruct";
    root.parentId = 0;
    root.offset = 0;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    rcx::Node f1;
    f1.kind = rcx::NodeKind::Int32;
    f1.name = "health";
    f1.parentId = rootId;
    f1.offset = 0;
    tree.addNode(f1);

    rcx::Node f2;
    f2.kind = rcx::NodeKind::Float;
    f2.name = "speed";
    f2.parentId = rootId;
    f2.offset = 4;
    tree.addNode(f2);

    return tree;
}

// ── Test class ──

class TestRenderedView : public QObject {
    Q_OBJECT

private slots:

    // ── Verify caret line background is NOT yellow after setup ──

    void testCaretLineBackgroundNotYellow() {
        QsciScintilla sci;
        setupRenderedSci(&sci);
        sci.show();
        sci.setText("struct Foo {\n    int x;\n};\n");
        QTest::qWait(50);

        long bgr = sci.SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACK);
        long expected = toBGR(QColor(43, 43, 43));

        // Yellow would be 0x00FFFF or similar high-value — ours should be dark
        long yellow = toBGR(QColor(255, 255, 0));
        QVERIFY2(bgr != yellow,
                 qPrintable(QString("Caret line is yellow (0x%1), expected dark (0x%2)")
                            .arg(bgr, 6, 16, QChar('0'))
                            .arg(expected, 6, 16, QChar('0'))));
        QCOMPARE(bgr, expected);
    }

    // ── Verify caret line is enabled ──

    void testCaretLineEnabled() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        long visible = sci.SendScintilla(QsciScintillaBase::SCI_GETCARETLINEVISIBLE);
        QCOMPARE(visible, (long)1);
    }

    // ── Verify editor background (paper) is dark ──

    void testPaperColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        // Query default style background via Scintilla
        long bgr = sci.SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK,
                                     (unsigned long)0 /*STYLE_DEFAULT*/);
        long expected = toBGR(QColor("#1e1e1e"));
        QCOMPARE(bgr, expected);
    }

    // ── Verify caret (cursor) foreground color ──

    void testCaretForegroundColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        long bgr = sci.SendScintilla(QsciScintillaBase::SCI_GETCARETFORE);
        long expected = toBGR(QColor("#d4d4d4"));
        QCOMPARE(bgr, expected);
    }

    // ── Verify selection colors are set (no direct Scintilla getter, but we can
    //    verify they survive a round-trip through the SCI_SETSEL* messages by
    //    checking the element colour API introduced in Scintilla 5.x) ──

    void testSelectionColorsApplied() {
        QsciScintilla sci;
        setupRenderedSci(&sci);
        sci.show();
        sci.setText("int x = 42;\n");
        QTest::qWait(50);

        // Select text and verify rendering doesn't crash
        sci.SendScintilla(QsciScintillaBase::SCI_SETSEL, (unsigned long)0, (long)3);
        QTest::qWait(50);

        // SCI_GETELEMENTCOLOUR (element 10 = SC_ELEMENT_SELECTION_BACK) returns
        // the selection back colour on Scintilla >= 5.2.  If not available, fall
        // back to verifying the calls didn't throw and caret line is still correct.
        constexpr int SCI_GETELEMENTCOLOUR = 2753;
        constexpr int SC_ELEMENT_SELECTION_BACK = 10;

        long selBack = sci.SendScintilla(SCI_GETELEMENTCOLOUR,
                                         (unsigned long)SC_ELEMENT_SELECTION_BACK);
        if (selBack != 0) {
            // Scintilla 5.x: colour stored as 0xAABBGGRR (with alpha in high byte)
            long bgrMask = selBack & 0x00FFFFFF;
            long expected = toBGR(QColor("#264f78"));
            QCOMPARE(bgrMask, expected);
        } else {
            // Older Scintilla: just verify caret line is still correct as a proxy
            long caretBg = sci.SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACK);
            long expected = toBGR(QColor(43, 43, 43));
            QCOMPARE(caretBg, expected);
        }
    }

    // ── Verify lexer keyword color is VS Code blue, not default ──

    void testKeywordColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QColor kw = lexer->color(QsciLexerCPP::Keyword);
        QCOMPARE(kw, QColor("#569cd6"));
    }

    // ── Verify comment color is VS Code green ──

    void testCommentColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QCOMPARE(lexer->color(QsciLexerCPP::Comment),    QColor("#6a9955"));
        QCOMPARE(lexer->color(QsciLexerCPP::CommentLine), QColor("#6a9955"));
    }

    // ── Verify number color is VS Code light green ──

    void testNumberColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QCOMPARE(lexer->color(QsciLexerCPP::Number), QColor("#b5cea8"));
    }

    // ── Verify string color is VS Code orange ──

    void testStringColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QCOMPARE(lexer->color(QsciLexerCPP::DoubleQuotedString), QColor("#ce9178"));
        QCOMPARE(lexer->color(QsciLexerCPP::SingleQuotedString), QColor("#ce9178"));
    }

    // ── Verify preprocessor color is VS Code purple ──

    void testPreprocessorColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QCOMPARE(lexer->color(QsciLexerCPP::PreProcessor), QColor("#c586c0"));
    }

    // ── Verify default/identifier text color ──

    void testDefaultTextColor() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QCOMPARE(lexer->color(QsciLexerCPP::Default),    QColor("#d4d4d4"));
        QCOMPARE(lexer->color(QsciLexerCPP::Identifier), QColor("#d4d4d4"));
        QCOMPARE(lexer->color(QsciLexerCPP::Operator),   QColor("#d4d4d4"));
    }

    // ── Verify all 128 lexer styles have dark paper ──

    void testAllStylesHaveDarkPaper() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        auto* lexer = qobject_cast<QsciLexerCPP*>(sci.lexer());
        QVERIFY(lexer != nullptr);

        QColor expected("#1e1e1e");
        for (int i = 0; i <= 127; i++) {
            QColor paper = lexer->paper(i);
            QVERIFY2(paper == expected,
                     qPrintable(QString("Style %1 paper is %2, expected %3")
                                .arg(i).arg(paper.name()).arg(expected.name())));
        }
    }

    // ── Verify margin colors match dark theme ──

    void testMarginColors() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        // Query margin background via Scintilla (style 33 = STYLE_LINENUMBER)
        long marginBg = sci.SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK,
                                          (unsigned long)33);
        long expectedBg = toBGR(QColor("#252526"));
        QCOMPARE(marginBg, expectedBg);

        long marginFg = sci.SendScintilla(QsciScintillaBase::SCI_STYLEGETFORE,
                                          (unsigned long)33);
        long expectedFg = toBGR(QColor("#858585"));
        QCOMPARE(marginFg, expectedFg);
    }

    // ── End-to-end: generate C++ and load into rendered view ──

    void testGeneratedCodeInRenderedView() {
        auto tree = makeTestTree();
        uint64_t rootId = tree.nodes[0].id;
        QString code = rcx::renderCpp(tree, rootId);

        // Verify generated code has no pragma pack / cstdint
        QVERIFY(!code.contains("#pragma pack"));
        QVERIFY(!code.contains("#include <cstdint>"));
        QVERIFY(code.contains("#pragma once"));
        QVERIFY(code.contains("struct TestStruct"));

        // Load into rendered sci and verify colors survive
        QsciScintilla sci;
        setupRenderedSci(&sci);
        sci.show();
        sci.setText(code);
        QTest::qWait(100);

        // Caret line must still be dark after text load
        long caretBg = sci.SendScintilla(QsciScintillaBase::SCI_GETCARETLINEBACK);
        long expected = toBGR(QColor(43, 43, 43));
        QCOMPARE(caretBg, expected);

        // Paper must still be dark
        long paperBg = sci.SendScintilla(QsciScintillaBase::SCI_STYLEGETBACK,
                                         (unsigned long)0);
        QCOMPARE(paperBg, toBGR(QColor("#1e1e1e")));
    }

    // ── Verify brace matching is disabled ──

    void testBraceMatchDisabled() {
        QsciScintilla sci;
        setupRenderedSci(&sci);

        QCOMPARE(sci.braceMatching(), QsciScintilla::NoBraceMatch);
    }
};

QTEST_MAIN(TestRenderedView)
#include "test_rendered_view.moc"
