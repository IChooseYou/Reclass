// Regression pins for the shared dock chrome (src/widgets/dock_header.h,
// panel_search_field.h, section_header.h).
//
// The single most important assertion here is the height one: the Project
// dock's header sits beside the editor's document tab bar and their bottom
// separator lines must land on the same device row. That alignment has drifted
// before (see the project-header/doc-tab alignment memory), and it used to be
// enforced only by a comment, because the four docks each hand-rolled their own
// title bar at 31 / 24 / 24 / "whatever Fusion draws". With one DockHeader the
// invariant is testable, and main.cpp static_asserts kDocTabBarHeight against
// the same constant.
//
// No MainWindow is instantiated: nothing links src/main.cpp, which is also why
// the collapsed-rail rule lives in a free function (railVisibleFor) instead of
// inside the visibilityChanged lambda.

#include <QtTest/QTest>
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QToolButton>
#include <cstdio>

#include "widgets/dock_header.h"
#include "widgets/panel_search_field.h"
#include "widgets/section_header.h"

#define LOG(...) do { std::fprintf(stdout, __VA_ARGS__); std::fflush(stdout); } while (0)

class TestDockHeader : public QObject {
    Q_OBJECT

private slots:
    // ── The alignment invariant ──
    void testHeaderHeightIsTheTabBarHeight() {
        // The content band matches the document tab bar exactly...
        QCOMPARE(rcx::kDockHeaderContentH, 31);
        // ...and the seam costs one more logical row, so its device row lands
        // ON the doc-tab seam instead of one row above it (measured at DPR
        // 1.25: a 31-px header put the line at y=189, the tabs at y=190).
        QCOMPARE(rcx::kDockHeaderSeam, 1);
        QCOMPARE(rcx::kDockHeaderHeight, 32);
        rcx::DockHeader h(QStringLiteral("Project"));
        QCOMPARE(h.height(), rcx::kDockHeaderHeight);
        QCOMPARE(h.sizeHint().height(), rcx::kDockHeaderHeight);
        QCOMPARE(h.minimumSizeHint().height(), rcx::kDockHeaderHeight);
        // Elastic width: a long title must NOT propagate a width floor up to
        // the dock (the bug that made the separator "fight back" at ~300 px).
        QCOMPARE(h.minimumSizeHint().width(), 0);
        LOG("header height=%d\n", h.height());
    }

    // All four docks must agree, whatever their titles.
    void testAllFourDocksShareOneHeader() {
        rcx::DockHeader project(QStringLiteral("Project"));
        rcx::DockHeader scanner(QStringLiteral("Scanner"));
        rcx::DockHeader symbols(QStringLiteral("Symbols"));
        rcx::DockHeader bookmarks(QStringLiteral("Bookmarks"));
        const rcx::DockHeader* all[] = {&project, &scanner, &symbols, &bookmarks};
        for (const auto* h : all) {
            QCOMPARE(h->height(), rcx::kDockHeaderHeight);
            QVERIFY(h->titleLabel() != nullptr);
            QVERIFY(h->closeButton() != nullptr);
            // Same font on every dock title: a tabified Project/Bookmarks pair
            // must not change text metrics on a tab switch.
            QCOMPARE(h->titleLabel()->font().family(),
                     project.titleLabel()->font().family());
            QCOMPARE(h->titleLabel()->font().pointSize(),
                     project.titleLabel()->font().pointSize());
        }
        QCOMPARE(scanner.titleLabel()->text(), QStringLiteral("Scanner"));
    }

    void testCloseButtonIsAnSvgNotATextGlyph() {
        rcx::DockHeader h(QStringLiteral("Symbols"));
        auto* c = h.closeButton();
        QCOMPARE(c->size(), QSize(rcx::kDockHeaderBtn, rcx::kDockHeaderBtn));
        QVERIFY(!c->icon().isNull());          // was a U+2715 text glyph
        QVERIFY(c->text().isEmpty());
        QVERIFY(!c->toolTip().isEmpty());
    }

    void testCloseButtonClearsTheRightBorder() {
        rcx::DockHeader h(QStringLiteral("Project"));
        h.setBorderRight(Qt::red);
        h.resize(260, rcx::kDockHeaderHeight);
        h.show();
        QApplication::processEvents();
        auto* close = h.closeButton();
        const QRect button = close->geometry();
        QVERIFY(h.width() - button.right() - 1 >= rcx::kGutter);
        QTest::mouseMove(close, close->rect().center());
        QApplication::processEvents();
        QCOMPARE(close->geometry(), button);
        const QImage img = h.grab().toImage();
        QCOMPARE(img.pixelColor(img.width() - 1, img.height() / 2), QColor(Qt::red));
    }

    // Right-slot widgets go BEFORE the close button, so x stays last.
    void testRightSlotSitsLeftOfClose() {
        rcx::DockHeader h(QStringLiteral("Symbols"));
        auto* dl = rcx::makeHeaderToolButton(QStringLiteral(":/vsicons/cloud-download.svg"),
                                             QStringLiteral("Download all symbols"), &h);
        h.addRightWidget(dl);
        h.resize(300, rcx::kDockHeaderHeight);
        h.layout()->activate();
        QVERIFY(dl->x() < h.closeButton()->x());
        QCOMPARE(dl->size(), QSize(rcx::kDockHeaderBtn, rcx::kDockHeaderBtn));
        // Icon-only: the label lives in the tooltip.
        QVERIFY(dl->text().isEmpty());
        QVERIFY(dl->toolTip().contains(QStringLiteral("Download")));
    }

    // ── Collapsed-Project rail ──
    // The rail is the handle back to a dock the user CLOSED. QDockWidget also
    // reports visibilityChanged(false) for a merely DESELECTED tab, and the
    // rail must stay hidden for that case.
    void testRailVisibility() {
        QVERIFY(!rcx::railVisibleFor(/*dockVisible=*/true,  /*userClosed=*/false));
        QVERIFY(!rcx::railVisibleFor(/*dockVisible=*/true,  /*userClosed=*/true));
        // Tabbed behind Bookmarks and deselected: hidden dock, NOT closed.
        QVERIFY(!rcx::railVisibleFor(/*dockVisible=*/false, /*userClosed=*/false));
        // Genuinely closed (header x, View toggle, layout preset).
        QVERIFY(rcx::railVisibleFor(/*dockVisible=*/false, /*userClosed=*/true));
    }

    // ── Shared panel search field ──
    void testPanelSearchField() {
        rcx::PanelSearchField f(QStringLiteral(":/vsicons/search.svg"),
                                QStringLiteral("Search names"));
        QCOMPARE(f.height(), rcx::PanelSearchField::kFieldHeight);
        QCOMPARE(f.height(), 26);
        const QString qss = f.styleSheet();
        QVERIFY(qss.contains(QStringLiteral("border-radius: 0px")));   // square
        QVERIFY(qss.contains(QStringLiteral("border: none")));         // no box at rest
        // The clear action appears only with text in the field.
        const auto acts = f.actions();
        QCOMPARE(acts.size(), 2);
        QVERIFY(!acts.at(1)->isVisible());
        f.setText(QStringLiteral("x"));
        QVERIFY(acts.at(1)->isVisible());
        f.clear();
        QVERIFY(!acts.at(1)->isVisible());
    }

    // The field is chrome, so it wears the chrome face — by construction, not
    // because each of its three call sites remembered to set it. It did not,
    // and the Project filter box silently dropped out of JetBrains Mono into
    // the system UI font when it moved onto this shared widget.
    void testPanelSearchFieldWearsTheChromeFont() {
        rcx::PanelSearchField f(QStringLiteral(":/vsicons/search.svg"),
                                QStringLiteral("Search names"));
        QCOMPARE(f.font().family(), rcx::chromeFont().family());
        QCOMPARE(f.font().pointSize(), rcx::chromeFont().pointSize());
        QVERIFY(f.font().fixedPitch());
        // ...and it survives a re-theme (the path a font change takes).
        f.setFont(QFont(QStringLiteral("Arial"), 14));
        f.applyTheme(rcx::ThemeManager::instance().current());
        QCOMPARE(f.font().family(), rcx::chromeFont().family());
    }

    // The dock title and the panel field must resolve to ONE face; the header
    // reads it from chromeFont() too, so this pins them to each other.
    void testHeaderTitleAndSearchFieldShareOneFace() {
        rcx::DockHeader h(QStringLiteral("Project"));
        rcx::PanelSearchField f(QStringLiteral(":/vsicons/filter.svg"),
                                QStringLiteral("Filter types"));
        QCOMPARE(h.titleLabel()->font().family(), f.font().family());
    }

    // A widget section header takes its band height from the SAME spec the
    // delegate-painted one does, so the scanner's dividers and the Project
    // tree's are one height instead of ~20 vs ~24 px.
    void testSectionHeaderWidgetTakesTheSpecHeight() {
        QFont base(QStringLiteral("JetBrains Mono"), 10);
        rcx::SectionHeaderLabel lbl(QStringLiteral("RESULTS"));
        lbl.setFont(rcx::sectionHeaderFont(base));
        const int expect =
            rcx::sectionHeaderHeight(QFontMetrics(rcx::sectionHeaderFont(base)));
        QCOMPARE(lbl.minimumHeight(), expect);
        QCOMPARE(lbl.maximumHeight(), expect);
        rcx::SectionHeaderButton btn(QStringLiteral("SCAN FOR"));
        btn.setFont(rcx::sectionHeaderFont(base));
        QCOMPARE(btn.minimumHeight(), expect);
        QCOMPARE(btn.maximumHeight(), expect);
    }

    // ── One SectionHeader spec ──
    void testSectionHeaderSpec() {
        QFont base(QStringLiteral("JetBrains Mono"), 10);
        const QFont f = rcx::sectionHeaderFont(base);
        QCOMPARE(f.pointSize(), 8);
        QCOMPARE(f.weight(), QFont::Normal);              // NOT DemiBold
        QCOMPARE(f.capitalization(), QFont::AllUppercase);
        QCOMPARE(f.letterSpacingType(), QFont::AbsoluteSpacing);
        QCOMPARE(f.letterSpacing(), 1.5);
        const QFontMetrics fm(f);
        QCOMPARE(rcx::sectionHeaderHeight(fm), fm.height() + 8);
    }

    // A pixel-sized base font returns pointSize() == -1; deriving from it
    // directly collapsed every section header to the 8 pt floor (or worse).
    // sectionHeaderFont routes through rcx::resolvedPointSize instead.
    void testSectionHeaderFontSurvivesAPixelSizedBase() {
        QFont px(QStringLiteral("JetBrains Mono"));
        px.setPixelSize(20);
        QCOMPARE(px.pointSize(), -1);
        const QFont f = rcx::sectionHeaderFont(px);
        QVERIFY(f.pointSize() >= 8);
        // 20 px is well above 10 pt, so the derived size must be too.
        QVERIFY2(f.pointSize() > 8,
                 qPrintable(QStringLiteral("derived %1 pt from a 20 px base")
                            .arg(f.pointSize())));
    }
};

QTEST_MAIN(TestDockHeader)
#include "test_dock_header.moc"
