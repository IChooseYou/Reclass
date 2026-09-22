// Tests for the SourceChooserPopup.
//
// Interaction: the per-row × delete button added so users can remove a
// single bound source instead of only "Clear All" — the click/key actually
// *fires* removeRequested and, critically, clicking elsewhere on a saved row
// does NOT delete it (guards against the paint rect and the hit-test rect
// drifting apart, since both come from the shared deleteBtnRect()).
//
// Chrome: the popup family rule (the comment above
// SourceChooserPopup::paintEvent) pinned in pixels, under both theme
// polarities and at dpr 1.0 and 1.25 — the popup rendered through
// QWidget::render onto a scaled QImage, the way test_breadcrumb's
// testKeyboardFocusRingIsOneDeviceRowAt125Percent does. The frame is
// exactly one device row / column on every side and nothing else is painted
// on those rows (build/issue.png had the active row's accent bar on the left
// border and the scrollbar over the right one); the body is one surface
// (the filter was a backgroundAlt strip); the accent is spent on the one
// word "active"; the scrollbar lives inside the frame and appears only when
// the list truly cannot fit below the anchor; the section hairlines and the
// Clear All divider run under the scroll track to the right frame; a
// disabled Clear All never lights t.hover; setSources() while up re-fits the
// height in place — keeping the bottom edge when the popup is flipped above
// the bar; a flipped popup ends above the bar, not on its bottom edge; and
// every seam reaches the frame at every width (the popup paints them under
// its children, whose 1-logical inset is not device-exact).
#include <QtTest/QTest>
#include <QSignalSpy>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QAbstractItemModel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QtMath>
#include "sourcechooserpopup.h"
#include "paintutil.h"
#include "themes/theme.h"
#include "themes/thememanager.h"

using namespace rcx;

namespace {

// A shipped theme by file name, so the chrome rule is checked under both
// polarities regardless of what ThemeManager holds in this bare target.
Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool sameColour(QRgb px, const QColor& c, int tol = 2) {
    return qAbs(qRed(px) - c.red()) <= tol && qAbs(qGreen(px) - c.green()) <= tol
        && qAbs(qBlue(px) - c.blue()) <= tol;
}

// Number of pixels in `r` (device) within `tol` of `c`.
int countColour(const QImage& img, const QRect& r, const QColor& c, int tol = 2) {
    int n = 0;
    for (int y = qMax(0, r.top()); y <= r.bottom() && y < img.height(); ++y)
        for (int x = qMax(0, r.left()); x <= r.right() && x < img.width(); ++x)
            if (sameColour(img.pixel(x, y), c, tol)) ++n;
    return n;
}

// The popup rendered onto an image at `dpr` — the painter's device
// transform then carries the scale, so paintutil's edge fills pick device
// rows exactly as they do on a 125 % screen.
QImage renderAt(QWidget& w, qreal dpr) {
    QImage img(qRound(w.width() * dpr), qRound(w.height() * dpr), QImage::Format_ARGB32);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::black);
    QPainter p(&img);
    w.render(&p);
    return img;
}

// The device rows / columns the four edge fills pick for a logical rect at
// `dpr`: qFloor(edge ± 0.5), paintutil's rule.
struct Edges { int top, bottom, left, right; };
Edges edgesOf(const QRect& logical, qreal dpr) {
    const QRectF dev(logical.left() * dpr, logical.top() * dpr,
                     logical.width() * dpr, logical.height() * dpr);
    return { qFloor(dev.top() + 0.5), qFloor(dev.bottom() - 0.5),
             qFloor(dev.left() + 0.5), qFloor(dev.right() - 0.5) };
}
QRect rowAcross(const Edges& e, int y) { return QRect(e.left, y, e.right - e.left + 1, 1); }
QRect colAlong(const Edges& e, int x)  { return QRect(x, e.top, 1, e.bottom - e.top + 1); }

}  // namespace

class TestSourceChooser : public QObject {
    Q_OBJECT
private:
    SourceChooserPopup* m_popup = nullptr;
    QListView* m_list = nullptr;

    static SourceEntry header(const QString& name) {
        SourceEntry h;
        h.entryKind = SourceEntry::SectionHeader;
        h.displayName = name;
        h.enabled = false;
        return h;
    }
    static SourceEntry saved(const QString& name, int savedIndex, bool active) {
        SourceEntry e;
        e.entryKind = SourceEntry::SavedSource;
        e.displayName = name;
        e.providerIdentifier = QStringLiteral("processmemory");
        e.kindLabel = QStringLiteral("Process");
        e.iconPath = iconForProvider(e.providerIdentifier);
        e.pid = QString::number(1000 + savedIndex);
        e.arch = QStringLiteral("x64");
        e.baseAddress = QStringLiteral("0x140000000");
        e.savedIndex = savedIndex;
        e.isActive = active;
        return e;
    }
    static SourceEntry provider(const QString& name, const QString& id, const QString& dll) {
        SourceEntry p;
        p.entryKind = SourceEntry::ProviderAction;
        p.displayName = name;
        p.providerIdentifier = id;
        p.kindLabel = kindLabelFor(id);
        p.dllFileName = dll;
        p.iconPath = iconForProvider(id);
        return p;
    }
    static SourceEntry clearAll(bool enabled) {
        SourceEntry c;
        c.entryKind = SourceEntry::ClearAction;
        c.displayName = QStringLiteral("Clear All");
        c.iconPath = QStringLiteral(":/vsicons/clear-all.svg");
        c.enabled = enabled;
        return c;
    }

    // The list the controller builds for a live session: one attached
    // process, the six providers the app ships, Clear All. `savedCount` > 1
    // adds more processes (the scroll case).
    static QVector<SourceEntry> liveEntries(int savedCount = 1) {
        QVector<SourceEntry> entries;
        entries.append(header(QStringLiteral("Connected")));
        for (int i = 0; i < savedCount; ++i)
            entries.append(saved(QStringLiteral("process_%1.exe").arg(i), i, i == 0));
        entries.append(header(QStringLiteral("Add Source")));
        entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File"), QStringLiteral("built-in")));
        entries.append(provider(QStringLiteral("Kernel Memory"), QStringLiteral("kernelmemory"), QStringLiteral("libKernelMemoryPlugin.dll")));
        entries.append(provider(QStringLiteral("Process Memory"), QStringLiteral("processmemory"), QStringLiteral("libProcessMemoryPlugin.dll")));
        entries.append(provider(QStringLiteral("ReClass.NET Compat Layer"), QStringLiteral("RC.netcompatlayer"), QStringLiteral("libRcNetCompatPlugin.dll")));
        entries.append(provider(QStringLiteral("Remote Process Memory"), QStringLiteral("remoteprocessmemory"), QStringLiteral("libRemoteProcessMemoryPlugin.dll")));
        entries.append(provider(QStringLiteral("WinDbg Memory"), QStringLiteral("windbgmemory"), QStringLiteral("libWinDbgMemoryPlugin.dll")));
        entries.append(clearAll(true));
        return entries;
    }

    // popup() sizes the frame and shows it; then qWait + processEvents,
    // never qWaitForWindowExposed: the hidden test desktop
    // (tools/run_tests_hidden.py) never composites, so an exposure wait
    // times out there — and its platform closes a Qt::Popup at the first
    // event pump, so from here on the popup may be hidden again. Every test
    // drives the list synchronously (sendEvent on its viewport, the key on
    // the list, currentIndex set by hand) or renders the laid-out widget
    // through QWidget::render — none of which needs a mapped window. The
    // same rule test_breadcrumb's menu tests follow.
    static void open(SourceChooserPopup& popup, const QPoint& at) {
        popup.popup(at);
        QTest::qWait(30);
        QApplication::processEvents();
    }

    // The popup's entry order: [Connected hdr, saved0, saved1, Add Source hdr,
    // provider, Clear All]. Saved rows live at model rows 1 and 2.
    int savedRow(int savedIndex) const { return 1 + savedIndex; }

    static void chromeData() {
        QTest::addColumn<QString>("themeName");
        QTest::addColumn<double>("dpr");
        QTest::newRow("tw@1.0")  << QStringLiteral("tw") << 1.0;
        QTest::newRow("tw@1.25") << QStringLiteral("tw") << 1.25;
        QTest::newRow("vs@1.0")  << QStringLiteral("vs") << 1.0;
        QTest::newRow("vs@1.25") << QStringLiteral("vs") << 1.25;
    }

private slots:
    void init() {
        m_popup = new SourceChooserPopup();
        m_popup->applyTheme(ThemeManager::instance().current());
        m_popup->setFont(QFont(QStringLiteral("Consolas"), 11));

        QVector<SourceEntry> entries;
        entries.append(header(QStringLiteral("Connected")));
        entries.append(saved(QStringLiteral("notepad.exe"), 0, true));
        entries.append(saved(QStringLiteral("game.exe"),    1, false));
        entries.append(header(QStringLiteral("Add Source")));
        entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File"), QStringLiteral("built-in")));
        entries.append(clearAll(true));
        m_popup->setSources(entries);
        open(*m_popup, QPoint(100, 100));

        m_list = m_popup->findChild<QListView*>();
        QVERIFY(m_list != nullptr);
        QVERIFY(m_list->model() != nullptr);
    }

    void cleanup() {
        delete m_popup;  m_popup = nullptr;  m_list = nullptr;
    }

    // ── Interaction ──

    // Clicking the × on a saved row removes exactly that source.
    void testDeleteButtonClickEmitsRemove() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);
        QSignalSpy selectSpy(m_popup, &SourceChooserPopup::sourceSelected);

        QRect vr = m_list->visualRect(m_list->model()->index(savedRow(1), 0));
        QVERIFY(vr.isValid() && vr.width() > 30);
        // deleteBtnRect = 16px wide, right edge at itemRect.right()-6, centred.
        QPoint xCentre(vr.right() - 14, vr.center().y());

        QMouseEvent rel(QEvent::MouseButtonRelease, xCentre,
                        m_list->viewport()->mapToGlobal(xCentre),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_list->viewport(), &rel);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(removeSpy.takeFirst().at(0).toInt(), 1);   // game.exe = savedIndex 1
        QCOMPARE(selectSpy.count(), 0);                     // not a row activation
    }

    // Clicking the row body (over the name, far from the ×) must NOT delete —
    // this is the regression guard for the two rects drifting apart.
    void testBodyClickDoesNotDelete() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        QRect vr = m_list->visualRect(m_list->model()->index(savedRow(0), 0));
        QPoint body(vr.left() + 30, vr.center().y());

        QMouseEvent rel(QEvent::MouseButtonRelease, body,
                        m_list->viewport()->mapToGlobal(body),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_list->viewport(), &rel);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 0);
    }

    // Delete key on a focused saved row removes that source.
    void testDeleteKeyEmitsRemove() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        m_list->setFocus();
        m_list->setCurrentIndex(m_list->model()->index(savedRow(0), 0));
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(m_list, &del);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(removeSpy.takeFirst().at(0).toInt(), 0);   // notepad.exe = savedIndex 0
    }

    // Delete key on a non-saved row (the Clear All action) is a no-op.
    void testDeleteKeyOnActionNoOp() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        m_list->setFocus();
        // Last row is "Clear All" (ClearAction, not a saved source).
        int lastRow = m_list->model()->rowCount() - 1;
        m_list->setCurrentIndex(m_list->model()->index(lastRow, 0));
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(m_list, &del);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 0);
    }

    // ── Chrome ──

    void testFrameIsOneDeviceRowOnEverySide_data() { chromeData(); }
    void testFrameIsOneDeviceRowOnEverySide() {
        // The frame is theme.border on exactly the outermost device row /
        // column of each side, unbroken end to end — the active row's
        // accent bar used to sit on the left column and the scrollbar on
        // the right one — and the row / column just inside carries none of
        // it (a 1-logical-px strip lands on two device rows at 125 %).
        QFETCH(QString, themeName);
        QFETCH(double, dpr);
        const Theme t = loadTheme(themeName);
        QVERIFY(t.border.isValid());
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        open(popup, QPoint(100, 100));
        const QImage img = renderAt(popup, dpr);
        const Edges e = edgesOf(popup.rect(), dpr);
        QCOMPARE(e.top, 0);
        QCOMPARE(e.left, 0);
        QCOMPARE(e.bottom, img.height() - 1);
        QCOMPARE(e.right, img.width() - 1);
        const int w = e.right - e.left + 1, h = e.bottom - e.top + 1;
        QCOMPARE(countColour(img, rowAcross(e, e.top), t.border), w);
        QCOMPARE(countColour(img, rowAcross(e, e.bottom), t.border), w);
        QCOMPARE(countColour(img, colAlong(e, e.left), t.border), h);
        QCOMPARE(countColour(img, colAlong(e, e.right), t.border), h);
        // Exactly one row / column thick: just inside the frame there is none.
        QCOMPARE(countColour(img, QRect(e.left + 1, e.top + 1, w - 2, 1), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.left + 1, e.bottom - 1, w - 2, 1), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.left + 1, e.top + 1, 1, h - 2), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.right - 1, e.top + 1, 1, h - 2), t.border), 0);
    }

    void testBodyIsOneSurface_data() { chromeData(); }
    void testBodyIsOneSurface() {
        // One surface: theme.background is the ground everywhere and
        // backgroundAlt appears nowhere (the filter was a cream strip on a
        // grey body). The filter field is that surface with a single
        // device-exact seam under it — containerBorderColor at rest,
        // borderFocused while it has focus — and nothing on the row above.
        QFETCH(QString, themeName);
        QFETCH(double, dpr);
        const Theme t = loadTheme(themeName);
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        open(popup, QPoint(100, 100));
        const QImage img = renderAt(popup, dpr);
        const QRect all(0, 0, img.width(), img.height());
        QVERIFY2(countColour(img, all, t.background) > all.width() * all.height() / 2,
                 "theme.background is not the dominant surface");
        // The four device columns inside each side border carry no ink
        // (gutter + padding), so they show the ground of every band: the
        // field, the list and the footer must all be theme.background there,
        // with only the seam rows (containerBorderColor) in between — no
        // backgroundAlt strip, no stale tint, no accent bar. (A whole-image
        // backgroundAlt count is the wrong probe: on vs it is two units from
        // the greys anti-aliased text passes through.)
        const Edges pe = edgesOf(popup.rect(), dpr);
        for (const QRect strip : { QRect(pe.left + 1, pe.top + 1, 4, pe.bottom - pe.top - 1),
                                   QRect(pe.right - 4, pe.top + 1, 4, pe.bottom - pe.top - 1) }) {
            const int ground = countColour(img, strip, t.background);
            const int seams  = countColour(img, strip, containerBorderColor(t));
            QVERIFY2(ground + seams == strip.width() * strip.height(),
                     qPrintable(QStringLiteral("edge strip at x=%1: %2 ground + %3 seam of %4")
                                    .arg(strip.left()).arg(ground).arg(seams)
                                    .arg(strip.width() * strip.height())));
            QCOMPARE(countColour(img, strip, t.backgroundAlt, 0), 0);
        }
        auto* field = popup.findChild<QLineEdit*>();
        QVERIFY(field);
        const QRect fieldRect(field->mapTo(&popup, QPoint(0, 0)), field->size());
        const Edges f = edgesOf(fieldRect, dpr);
        const int fw = f.right - f.left + 1;
        const int seamRest    = countColour(img, rowAcross(f, f.bottom), containerBorderColor(t));
        const int seamFocused = countColour(img, rowAcross(f, f.bottom), t.borderFocused);
        QVERIFY2(seamRest == fw || seamFocused == fw,
                 qPrintable(QStringLiteral("field seam: %1 rest / %2 focused of %3")
                                .arg(seamRest).arg(seamFocused).arg(fw)));
        QCOMPARE(countColour(img, rowAcross(f, f.bottom - 1), containerBorderColor(t)), 0);
        QCOMPARE(countColour(img, rowAcross(f, f.bottom - 1), t.borderFocused), 0);
        // The band above the seam is the surface (placeholder ink aside).
        QVERIFY(countColour(img, QRect(f.left, f.top, fw, f.bottom - f.top), t.background)
                > fw * (f.bottom - f.top) / 2);
    }

    void testAccentIsSpentOnTheActiveWordOnly_data() { chromeData(); }
    void testAccentIsSpentOnTheActiveWordOnly() {
        // The accent (indHoverSpan) is budgeted: the one place it appears is
        // the word "active" on the active source's second line — inside
        // that row, right of its icon, clear of the × gutter. No side bar,
        // no arch pill, nothing on any other row or on the frame.
        QFETCH(QString, themeName);
        QFETCH(double, dpr);
        const Theme t = loadTheme(themeName);
        SourceChooserPopup popup;
        popup.applyTheme(t);
        const QFont font(QStringLiteral("Consolas"), 11);
        popup.setFont(font);
        popup.setSources(liveEntries(2));
        open(popup, QPoint(100, 100));
        const QImage img = renderAt(popup, dpr);
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        const QPoint vp = list->viewport()->mapTo(&popup, QPoint(0, 0));
        const QRect active = list->visualRect(list->model()->index(1, 0)).translated(vp);
        QVERIFY(active.isValid());
        const Edges a = edgesOf(active, dpr);
        // Every near-accent pixel, anywhere.
        int minX = img.width(), maxX = -1, minY = img.height(), maxY = -1, n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (sameColour(img.pixel(x, y), t.indHoverSpan, 40)) {
                    ++n;
                    minX = qMin(minX, x); maxX = qMax(maxX, x);
                    minY = qMin(minY, y); maxY = qMax(maxY, y);
                }
        QVERIFY2(n > 0, "the word \"active\" should be in the accent");
        // The two-line card's icon is fm.height() + 4 wide after the gutter;
        // the text starts a gutter after it.
        const int textX = active.left() + kGutter + QFontMetrics(font).height() + 4 + kGutter;
        QVERIFY2(minX >= qFloor(textX * dpr), qPrintable(QStringLiteral("accent at x=%1 < text x=%2").arg(minX).arg(qFloor(textX * dpr))));
        QVERIFY2(maxX < qFloor((active.right() - 28) * dpr), "accent under the x gutter");
        QVERIFY2(minY > a.top && maxY < a.bottom,
                 qPrintable(QStringLiteral("accent rows %1..%2 outside the active row %3..%4").arg(minY).arg(maxY).arg(a.top).arg(a.bottom)));
        // In particular: none on the frame, none in the row's first columns.
        const Edges e = edgesOf(popup.rect(), dpr);
        QCOMPARE(countColour(img, colAlong(e, e.left), t.indHoverSpan, 40), 0);
        QCOMPARE(countColour(img, QRect(a.left, a.top, 4, a.bottom - a.top + 1), t.indHoverSpan, 40), 0);
    }

    void testSizesToTheListWhenItFits() {
        // Height comes from the real row hints, not a 520-px cap: with room
        // below the anchor the default list gets exactly its height and no
        // scrollbar (build/issue.png had a 90 % handle for a 3-px overflow).
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        SourceChooserPopup popup;
        popup.applyTheme(loadTheme(QStringLiteral("tw")));
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        const QPoint anchor(screen.left() + 40, screen.top() + 20);
        open(popup, anchor);
        QCOMPARE(popup.pos(), anchor);
        QCOMPARE(popup.height(), popup.layout()->sizeHint().height());
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        QCOMPARE(list->verticalScrollBar()->maximum(), 0);
        const int last = list->model()->rowCount() - 1;
        const QRect lastRow = list->visualRect(list->model()->index(last, 0));
        QVERIFY2(lastRow.bottom() <= list->viewport()->height(), "the last row is cut off");
    }

    void testScrollbarStaysInsideTheFrame() {
        // The scroll case: more rows than the screen has room for below (or
        // above) the anchor. The popup caps at the screen, the list scrolls,
        // and the bar sits INSIDE the right frame column — the frame is
        // still unbroken, and the handle is the popup's own blend, not the
        // app-wide solid textFaint rod.
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        const Theme t = loadTheme(QStringLiteral("tw"));
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries(24));
        open(popup, screen.center());
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        QVERIFY2(popup.height() < popup.layout()->sizeHint().height(), "the screen should cap the popup");
        QVERIFY2(list->verticalScrollBar()->maximum() > 0, "the list should scroll");
        QVERIFY(popup.y() + popup.height() <= screen.bottom() + 1);
        QVERIFY(popup.y() >= screen.top());
        const qreal dpr = 1.25;
        const QImage img = renderAt(popup, dpr);
        const Edges e = edgesOf(popup.rect(), dpr);
        const int h = e.bottom - e.top + 1;
        QCOMPARE(countColour(img, colAlong(e, e.right), t.border), h);
        QCOMPARE(countColour(img, colAlong(e, e.left), t.border), h);
        QCOMPARE(countColour(img, colAlong(e, e.right), t.textFaint), 0);
        // The handle is drawn, inside the frame, and it is not solid textFaint.
        const QRect barCols(e.right - 10, e.top + 1, 10, h - 2);
        QCOMPARE(countColour(img, barCols, t.textFaint), 0);
        QVERIFY2(countColour(img, barCols, t.background) < barCols.width() * barCols.height(),
                 "no scrollbar handle rendered beside the list");
    }

    void testSeamsCrossTheScrollTrack() {
        // The section hairlines and the Clear All divider run from the left
        // frame column to the right one — under the 6-px scroll track too.
        // The delegate's rows end at the viewport, so while the list
        // scrolled those seams stopped 8 device px short of the right frame
        // and the track sat on them as a bare strip; the scrollbar now
        // continues them (SeamScrollBar, widgets/popup_chrome.h).
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        const Theme t = loadTheme(QStringLiteral("tw"));
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries(24));
        open(popup, screen.center());
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        QVERIFY2(list->verticalScrollBar()->maximum() > 0, "the list should scroll");
        const QColor seam = containerBorderColor(t);
        const QPoint vp = list->viewport()->mapTo(&popup, QPoint(0, 0));
        for (const qreal dpr : { 1.0, 1.25 }) {
            // At the top: the Connected header (row 0) and its hairline.
            list->scrollToTop();
            QApplication::processEvents();
            QImage img = renderAt(popup, dpr);
            const Edges e = edgesOf(popup.rect(), dpr);
            const int w = e.right - e.left - 1;   // the interior columns
            const QRect hdr = list->visualRect(list->model()->index(0, 0)).translated(vp);
            QVERIFY(hdr.isValid());
            const Edges h = edgesOf(hdr, dpr);
            QCOMPARE(countColour(img, QRect(e.left + 1, h.bottom, w, 1), seam), w);
            // At the bottom: the Clear All divider, the top row of the 4-px
            // gap above its card.
            list->scrollToBottom();
            QApplication::processEvents();
            img = renderAt(popup, dpr);
            const int last = list->model()->rowCount() - 1;
            const QRect clr = list->visualRect(list->model()->index(last, 0))
                                  .translated(vp).adjusted(0, 4, 0, 0);
            QVERIFY(clr.isValid());
            const Edges c = edgesOf(clr, dpr);
            QCOMPARE(countColour(img, QRect(e.left + 1, c.top, w, 1), seam), w);
        }
    }

    void testDisabledClearAllDoesNotLightHover() {
        // A disabled Clear All (nothing to clear) under the mouse stays on
        // the surface: the hover ground is gated on enabled, like the bar's
        // disabled cells. An enabled row under the same mouse move DOES
        // light t.hover, so the probe is not vacuous.
        const Theme t = loadTheme(QStringLiteral("tw"));
        QVERIFY(t.hover != t.background);
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        QVector<SourceEntry> entries;
        entries.append(header(QStringLiteral("Add Source")));
        entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File"), QStringLiteral("built-in")));
        entries.append(clearAll(false));
        popup.setSources(entries);
        open(popup, QPoint(100, 100));
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        const QPoint vp = list->viewport()->mapTo(&popup, QPoint(0, 0));
        auto rowRect = [&](int row) {
            return list->visualRect(list->model()->index(row, 0)).translated(vp);
        };
        auto hoverRow = [&](int row) {
            const QPoint c = list->visualRect(list->model()->index(row, 0)).center();
            QMouseEvent mv(QEvent::MouseMove, c, list->viewport()->mapToGlobal(c),
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(list->viewport(), &mv);
            QApplication::processEvents();
        };
        hoverRow(1);
        QImage img = renderAt(popup, 1.0);
        const QRect live = rowRect(1);
        QVERIFY2(countColour(img, live, t.hover) > live.width() * live.height() / 2,
                 "an enabled row under the mouse should be t.hover");
        hoverRow(2);
        img = renderAt(popup, 1.0);
        // Below its divider gap, the disabled card is the surface only.
        const QRect body = rowRect(2).adjusted(0, 6, 0, 0);
        QCOMPARE(countColour(img, body, t.hover), 0);
        QVERIFY(countColour(img, body, t.background) > body.width() * body.height() / 2);
    }

    void testSetSourcesWhileUpRefitsInPlace() {
        // A per-row x delete rebuilds the list while the popup is up. The
        // popup used to keep the height the old list needed, and the list's
        // stretch turned the difference into bare ground between the last
        // row and the footer; now it re-fits to the new rows with its top
        // edge where it was. No event pump between popup() and the probes:
        // the hidden desktop's platform would close the popup at the first
        // one, and the re-fit is gated on it being up.
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        SourceChooserPopup popup;
        popup.applyTheme(loadTheme(QStringLiteral("tw")));
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries(4));
        const QPoint anchor(screen.left() + 40, screen.top() + 20);
        popup.popup(anchor);
        QVERIFY(popup.isVisible());
        const int tall = popup.height();
        QCOMPARE(tall, popup.layout()->sizeHint().height());
        popup.setSources(liveEntries(1));
        QCOMPARE(popup.pos(), anchor);
        QVERIFY2(popup.height() < tall,
                 qPrintable(QStringLiteral("three rows fewer should shrink the popup: %1 (was %2), "
                                           "layout hint %3, list hint %4, visible %5")
                                .arg(popup.height()).arg(tall)
                                .arg(popup.layout()->sizeHint().height())
                                .arg(popup.findChild<QListView*>()->sizeHint().height())
                                .arg(popup.isVisible())));
        QCOMPARE(popup.height(), popup.layout()->sizeHint().height());
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        const int last = list->model()->rowCount() - 1;
        const QRect lastRow = list->visualRect(list->model()->index(last, 0));
        QCOMPARE(lastRow.bottom() + 1, list->viewport()->height());   // no bare ground under it
        popup.hide();
    }

    void testSetSourcesWhileFlippedKeepsTheBottomEdge() {
        // Flipped above the bar, the popup's BOTTOM edge is the one on the
        // bar. The in-place re-fit after a per-row x delete always kept the
        // top edge, so a flipped popup shrank upward and left a gap between
        // its bottom frame and the bar it hung from, the removed rows'
        // height tall. Now the bottom edge stays. No event pump between
        // popup() and the probes (the hidden desktop would close the popup;
        // the re-fit is gated on it being up).
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        SourceChooserPopup popup;
        popup.applyTheme(loadTheme(QStringLiteral("tw")));
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries(4));
        constexpr int kBarH = 28;
        const QPoint anchor(screen.left() + 40, screen.bottom() - 40);
        const int barTop = anchor.y() - kBarH;
        popup.popup(anchor, barTop);
        QVERIFY(popup.isVisible());
        QVERIFY2(popup.y() < anchor.y(), "the popup should flip above the anchor");
        QCOMPARE(popup.y() + popup.height(), barTop);
        const int tall = popup.height();
        popup.setSources(liveEntries(1));
        QVERIFY2(popup.height() < tall, "three rows fewer should shrink the popup");
        QCOMPARE(popup.height(), popup.layout()->sizeHint().height());
        QCOMPARE(popup.y() + popup.height(), barTop);
        popup.hide();
    }

    void testSeamsReachTheFrameAtEveryWidth() {
        // At 125 % the layout's 1-logical inset is 1.25 device px, so at the
        // widths where W * dpr has a .5 or .75 fraction the children ended
        // one device column short of the right frame — the field's seam,
        // the section hairlines, the Clear All divider and the footer's
        // seam all stopped with a notch of ground before it (the default
        // 432 just happened to snap clean). The popup paints every seam
        // itself across its whole width now, the frame last. Four
        // consecutive widths cover every phase; the list scrolls, so the
        // track is under the probe too.
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        const qreal dpr = 1.25;
        for (const QString& name : {QStringLiteral("tw"), QStringLiteral("vs")}) {
            const Theme t = loadTheme(name);
            QVERIFY(t.background.isValid());
            const QColor seam = containerBorderColor(t);
            for (int w = 430; w <= 433; ++w) {
                SourceChooserPopup popup;
                popup.applyTheme(t);
                popup.setFont(QFont(QStringLiteral("Consolas"), 11));
                popup.setSources(liveEntries(24));
                open(popup, screen.center());
                popup.setFixedSize(w, popup.height());
                QApplication::processEvents();
                popup.layout()->activate();
                auto* list = popup.findChild<QListView*>();
                QVERIFY(list);
                QVERIFY2(list->verticalScrollBar()->maximum() > 0, "the list should scroll");
                const QString tag = QStringLiteral("%1 w=%2: ").arg(name).arg(w);
                // The interior of device row `y` is the seam (or `alt`, the
                // field's focused colour) from frame to frame.
                auto seamRow = [&](const QImage& img, const Edges& e, int y, const QColor& alt,
                                   const char* what, QString* why) {
                    const int inner = e.right - e.left - 1;
                    const QRect row(e.left + 1, y, inner, 1);
                    const int n = countColour(img, row, seam);
                    const int m = alt.isValid() ? countColour(img, row, alt) : 0;
                    if (n == inner || m == inner) return true;
                    *why = tag + QStringLiteral("%1: %2 of %3 seam px on device row %4")
                                     .arg(QLatin1String(what)).arg(qMax(n, m)).arg(inner).arg(y);
                    return false;
                };
                QString why;
                list->scrollToTop();
                QApplication::processEvents();
                QImage img = renderAt(popup, dpr);
                const Edges e = edgesOf(popup.rect(), dpr);
                QCOMPARE(e.right, img.width() - 1);
                QCOMPARE(countColour(img, colAlong(e, e.right), t.border), e.bottom - e.top + 1);
                // The field's seam (in whichever colour the field is in).
                auto* field = popup.findChild<QLineEdit*>();
                QVERIFY(field);
                const Edges f = edgesOf(QRect(field->mapTo(&popup, QPoint(0, 0)), field->size()), dpr);
                QVERIFY2(seamRow(img, e, f.bottom, t.borderFocused, "field seam", &why), qPrintable(why));
                // The Connected header's hairline.
                const QPoint vp = list->viewport()->mapTo(&popup, QPoint(0, 0));
                const Edges h = edgesOf(list->visualRect(list->model()->index(0, 0)).translated(vp), dpr);
                QVERIFY2(seamRow(img, e, h.bottom, QColor(), "section hairline", &why), qPrintable(why));
                // The footer's seam.
                QLabel* footer = nullptr;
                for (QLabel* l : popup.findChildren<QLabel*>())
                    if (l->text().contains(QStringLiteral("navigate"))) footer = l;
                QVERIFY(footer);
                const Edges fo = edgesOf(QRect(footer->mapTo(&popup, QPoint(0, 0)), footer->size()), dpr);
                QVERIFY2(seamRow(img, e, fo.top, QColor(), "footer seam", &why), qPrintable(why));
                // The Clear All divider, at the bottom.
                list->scrollToBottom();
                QApplication::processEvents();
                img = renderAt(popup, dpr);
                const int last = list->model()->rowCount() - 1;
                const Edges c = edgesOf(list->visualRect(list->model()->index(last, 0))
                                            .translated(vp).adjusted(0, 4, 0, 0), dpr);
                QVERIFY2(seamRow(img, e, c.top, QColor(), "Clear All divider", &why), qPrintable(why));
                popup.hide();
            }
        }
    }

    void testFlipsAboveTheBarNotOverIt() {
        // Near the bottom of the screen the popup flips above the anchor.
        // The anchor is the bar's BOTTOM edge, so a popup that ended there
        // covered the bar and the chip that opened it; given the bar's top
        // edge (anchorTop) the flipped popup ends one device row above the
        // bar. Without it (no bar) it ends on the anchor, as before.
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        SourceChooserPopup popup;
        popup.applyTheme(loadTheme(QStringLiteral("tw")));
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        constexpr int kBarH = 28;
        const QPoint anchor(screen.left() + 40, screen.bottom() - 40);
        const int barTop = anchor.y() - kBarH;
        popup.popup(anchor, barTop);
        QTest::qWait(30);
        QApplication::processEvents();
        QVERIFY2(popup.y() < anchor.y(), "the popup should flip above the anchor");
        QCOMPARE(popup.y() + popup.height(), barTop);
        QVERIFY(popup.y() >= screen.top());
        popup.popup(anchor);
        QTest::qWait(30);
        QApplication::processEvents();
        QCOMPARE(popup.y() + popup.height(), anchor.y());
    }
};

QTEST_MAIN(TestSourceChooser)
#include "test_source_chooser.moc"
