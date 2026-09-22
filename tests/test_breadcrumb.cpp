// Drill-down trail (v3 — CLICK-driven focus path) and the AddressBar that
// renders it. The trail reflects where the selection sits in the inline-
// expanded tree: selecting a typed pointer (or any row inside its expansion)
// adds it; there is no follow-arrow affordance. Crumb click = collapse below
// + scroll. Exercises focusChainToNode / handleNodeClick / collapseToFocus /
// addressBarState on the controller side, and — RibbonBar-style — the
// AddressBar widget: geometry through itemRect(id), hover through a
// synthesized MouseMove, pixels through grab(). Never waits for window
// exposure (show() + qWait + processEvents is all that's needed), so the
// whole target runs on the hidden desktop.
//
// One hidden-desktop fact the P3 tests lean on: a Qt::Popup (the places
// QMenu) is closed by the platform at the first event pump after show(),
// so a menu is inspected and driven synchronously, before any pump. Focus
// is asserted through the focus chain (window()->focusWidget()), which is
// kept whether or not the window is active; the focus-out test hands the
// overlay the event itself if the platform delivered none.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QPainter>
#include <QScreen>
#include <QTemporaryDir>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtEndian>
#include <Qsci/qsciscintilla.h>

#include "address_callbacks.h"
#include "controller.h"
#include "core.h"
#include "gotoaddressdialog.h"
#include "paintutil.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"   // a source nothing can be read from
#include "sourcechooserpopup.h"
#include "typeselectorpopup.h"      // TypeEntry / TypePopupMode: the line-0 root pick
#include "widgets/address_bar.h"

using namespace rcx;

namespace {

bool sameColour(QRgb px, const QColor& c, int tol = 2) {
    return qAbs(qRed(px) - c.red()) <= tol && qAbs(qGreen(px) - c.green()) <= tol
        && qAbs(qBlue(px) - c.blue()) <= tol;
}

// Device-pixel rect of a logical rect in a grab() image.
QRect devRect(const QImage& img, const QRect& logical) {
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    return QRect(qRound(logical.left() * dpr), qRound(logical.top() * dpr),
                 qRound(logical.width() * dpr), qRound(logical.height() * dpr));
}

// Number of pixels in `r` (device) that are exactly `c`.
int countColour(const QImage& img, const QRect& r, const QColor& c) {
    int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y)
        for (int x = r.left(); x <= r.right() && x < img.width(); ++x)
            if (y >= 0 && x >= 0 && sameColour(img.pixel(x, y), c)) ++n;
    return n;
}

// Mean distance of the rect's pixels from the ground — a proxy for "how
// much ink is there" (the disabled-cell / dimmed-icon opacity probe).
// `exclude` (logical) masks a sub-rect out of the average — the liveness
// dot sits on the chip icon and is painted at full opacity either way.
double inkAmount(const QImage& img, const QRect& logical, const QColor& bg,
                 const QRect& exclude = QRect()) {
    const QRect r = devRect(img, logical);
    const QRect ex = exclude.isNull() ? QRect() : devRect(img, exclude);
    double sum = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y)
        for (int x = r.left(); x <= r.right() && x < img.width(); ++x) {
            if (!ex.isNull() && ex.contains(x, y)) continue;
            const QRgb px = img.pixel(x, y);
            sum += qAbs(qRed(px) - bg.red()) + qAbs(qGreen(px) - bg.green()) + qAbs(qBlue(px) - bg.blue());
            ++n;
        }
    return n ? sum / n : 0.0;
}

// Plain hover move (no buttons) delivered straight to the widget.
void hoverAt(QWidget& w, const QPoint& pos) {
    QMouseEvent mv(QEvent::MouseMove, pos, w.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &mv);
}

QImage grabOf(QWidget& w) {
    QApplication::processEvents();
    return w.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

void showBar(AddressBar& bar, int width) {
    bar.resize(width, AddressBar::kAddressBarHeight);
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
}

// A shipped theme by file name (test_ribbon_layout's helper), so a colour
// rule can be checked under both polarities regardless of what
// ThemeManager holds in this bare target.
Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

// The first device column (left → right) holding a pixel that is not the
// ground, scanning every row but the seam (the bottom device row is the
// hairline end to end). -1 when nothing is inked.
int firstInkColumn(const QImage& img, const QColor& bg) {
    for (int x = 0; x < img.width(); ++x)
        for (int y = 0; y < img.height() - 1; ++y) {
            const QRgb px = img.pixel(x, y);
            if (qAbs(qRed(px) - bg.red()) + qAbs(qGreen(px) - bg.green())
                + qAbs(qBlue(px) - bg.blue()) >= 12)
                return x;
        }
    return -1;
}

// The bar's bottom device row under a logical x-range: where the seam and,
// while editing, the focus ring live.
QRect seamRowUnder(const QImage& img, const QRect& logical) {
    const QRect d = devRect(img, logical);
    return QRect(d.left(), img.height() - 1, d.width(), 1);
}

QStringList actionTexts(const QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << a->text();
    return out;
}
QAction* actionWithText(const QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions())
        if (a->text() == text) return a;
    return nullptr;
}
// The bar frees a menu with deleteLater on hide, which a processEvents()
// at the test's loop level does not run — a hidden predecessor can still
// be a child. The one that is up is the one that is visible.
// The width an overlay fitted to `text` must have: the cell's left padding,
// the text, and the one-space gap the caret lives in — which is also the gap
// a resting cell leaves before its "= 0x…" suffix, so the field ends exactly
// where that suffix begins. `f` is the face the cell was painted in.
int fittedEditW(const QString& text, const QFont& f) {
    return AddressBar::kBasePad + QFontMetrics(f).horizontalAdvance(text)
         + QFontMetrics(f).horizontalAdvance(QLatin1Char(' '));
}

QMenu* visibleMenu(const QWidget* w, const QString& name) {
    for (QMenu* m : w->findChildren<QMenu*>(name))
        if (m->isVisible()) return m;
    return nullptr;
}

}  // namespace

// RcxEditor.vptr → QWidgetPrivate.parent → QWidget. dptr is a non-drillable
// pointer (no refId); leaf is a plain top-level field.
struct Ids { uint64_t editor, priv, widget, vptr, dptr, parent, leaf; };

static Ids buildChain(NodeTree& tree) {
    tree.baseAddress = 0;
    Ids id{};
    auto addRoot = [&](const char* type) -> uint64_t {
        Node n; n.kind = NodeKind::Struct; n.structTypeName = type;
        n.parentId = 0; n.offset = 0; n.collapsed = false;
        return tree.nodes[tree.addNode(n)].id;
    };
    auto addPtr = [&](uint64_t parent, const char* name, uint64_t refId, int off) -> uint64_t {
        Node n; n.kind = NodeKind::Pointer64; n.name = name;
        n.parentId = parent; n.offset = off; n.refId = refId; n.collapsed = true;
        return tree.nodes[tree.addNode(n)].id;
    };
    id.editor = addRoot("RcxEditor");
    id.priv   = addRoot("QWidgetPrivate");
    id.widget = addRoot("QWidget");
    id.vptr   = addPtr(id.editor, "vptr",   id.priv,   0);
    id.dptr   = addPtr(id.editor, "dptr",   0,         8);   // no refId → not drillable
    id.parent = addPtr(id.priv,   "parent", id.widget, 0);
    Node lf; lf.kind = NodeKind::UInt32; lf.name = "leaf"; lf.parentId = id.editor; lf.offset = 16;
    id.leaf = tree.nodes[tree.addNode(lf)].id;
    return id;
}

// Crumbs in the production dotted shape (addressBarState): "Class.field"
// ancestors, a bare "Class" deepest, rootId = crumb index. classId is any
// non-zero id — the widget only reads label, index and address.
static Crumb crumb(const QString& label, uint64_t index, uint64_t classId = 1) {
    Crumb c; c.label = label; c.rootId = index; c.classId = classId;
    return c;
}
static QVector<Crumb> twoLevel() {
    return { crumb(QStringLiteral("RcxEditor.vptr"), 0, 1),
             crumb(QStringLiteral("QWidgetPrivate"), 1, 2) };
}
// A live process source over the given trail — what a real push looks like.
static AddressBarState stateWith(const QVector<Crumb>& crumbs) {
    AddressBarState s;
    s.sourceName   = QStringLiteral("RCTarget.exe");
    s.sourceKindId = QStringLiteral("processmemory");
    s.liveness     = liveness::Live;
    s.baseAddress  = 0x7FF600000000ULL;
    s.resolvedBase = s.baseAddress;
    s.crumbs       = crumbs;
    return s;
}

class TestBreadcrumb : public QObject {
    Q_OBJECT
private:
    RcxDocument*   m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter*     m_splitter = nullptr;
    RcxEditor*     m_editor = nullptr;
    Ids            m_id{};
    // rebaseTo pushes into the Goto dialog's persisted recent list (the
    // user's real QSettings) — snapshot it per test and put it back.
    QStringList    m_savedRecent;
    bool           m_hadRecent = false;

    int idx(uint64_t nodeId) const { return m_doc->tree.indexOfId(nodeId); }
    bool collapsed(uint64_t nodeId) const {
        int i = idx(nodeId);
        return i >= 0 && m_doc->tree.nodes[i].collapsed;
    }
    void expand(uint64_t id) { m_doc->tree.nodes[idx(id)].collapsed = false; m_ctrl->refresh(); }
    int lineOf(uint64_t id) const {
        const auto& m = m_ctrl->lastResult().meta;
        for (int i = 0; i < m.size(); ++i)
            if (m[i].nodeId == id && m[i].lineKind != LineKind::Footer) return i;
        return -1;
    }
    QVector<Crumb> crumbs() const { return m_editor->addressBar()->state().crumbs; }
    QStringList segments() const { return m_editor->addressBar()->segments(); }
    // Bytes holding real little-endian pointers for buildChain at base 0,
    // pointerSize 8: RcxEditor@0 .vptr = 0x20 → QWidgetPrivate@0x20 .parent
    // = 0x30 → QWidget@0x30. 64 bytes, so every target is readable.
    static QByteArray chainBytes() {
        QByteArray b(64, '\0');
        qToLittleEndian<quint64>(0x20, b.data() + 0x00);
        qToLittleEndian<quint64>(0x30, b.data() + 0x20);
        return b;
    }
    void drillTwoLevels() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
    }
    void drillOneLevel() {
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
    }
    // A second drillable field in RcxEditor (ptr2 → QWidget at +0x18), so a
    // chevron has somewhere sideways to go. Added per test rather than to
    // buildChain: every earlier slot was written against the shared chain.
    uint64_t addSibling() {
        Node n; n.kind = NodeKind::Pointer64; n.name = QStringLiteral("ptr2");
        n.parentId = m_id.editor; n.offset = 24; n.refId = m_id.widget; n.collapsed = true;
        const uint64_t id = m_doc->tree.nodes[m_doc->tree.addNode(n)].id;
        m_ctrl->refresh();
        return id;
    }
    // The menu a press on `cellId` opens, inspected before any event pump
    // (the hidden desktop closes a popup at the first one).
    QMenu* openMenuOn(AddressBar* bar, const QString& cellId, const QString& menuName) {
        const QRect r = bar->itemRect(cellId);
        if (r.isNull()) return nullptr;
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, r.center());
        return visibleMenu(bar, menuName);
    }
    void closeMenuOn(AddressBar* bar, const QString& cellId, QMenu* menu) {
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(cellId).center());
        QTest::qWait(10);                                        // runs the menu's deleteLater
    }
    static QStringList actionData(const QMenu* menu) {
        QStringList out;
        for (QAction* a : menu->actions()) out << a->data().toString();
        return out;
    }

private slots:
    void init() {
        {
            QSettings s("RC", "RC");
            m_hadRecent = s.contains(GotoAddressDialog::kSettingsKey);
            m_savedRecent = s.value(GotoAddressDialog::kSettingsKey).toStringList();
        }
        m_doc = new RcxDocument();
        m_id = buildChain(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);
        m_splitter->resize(800, 600);
        // show() + qWait, never qWaitForWindowExposed: an unrendered desktop
        // never composites, so an exposure wait fails on the hidden desktop.
        m_splitter->show();
        QTest::qWait(30);
        QApplication::processEvents();
        m_ctrl->setViewRootId(m_id.editor);
    }

    // Line 0 of the editor's Scintilla, newline stripped.
    QString lineZeroText() const {
        QsciScintilla* sci = m_editor->scintilla();
        const int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        if (len <= 0) return {};
        QByteArray buf(len + 1, '\0');
        sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)0, (void*)buf.data());
        QString line = QString::fromUtf8(buf.constData(), len);
        while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
        return line;
    }

    void cleanup() {
        delete m_ctrl; m_ctrl = nullptr;
        m_editor = nullptr;
        delete m_splitter; m_splitter = nullptr;
        delete m_doc; m_doc = nullptr;
        QSettings s("RC", "RC");
        if (m_hadRecent) s.setValue(GotoAddressDialog::kSettingsKey, m_savedRecent);
        else             s.remove(GotoAddressDialog::kSettingsKey);
    }

    // ── focusChainToNode: the click-driven focus logic ──

    void testTopLevelSelectionHasEmptyFocus() {
        // A collapsed top-level pointer (nothing "inside") and a plain field
        // both yield the bare root crumb.
        QVERIFY(m_ctrl->focusChainToNode(m_id.vptr).isEmpty());
        QVERIFY(m_ctrl->focusChainToNode(m_id.leaf).isEmpty());
    }

    void testSelectExpandedPointerAddsIt() {
        expand(m_id.vptr);
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.vptr);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0], m_id.vptr);
    }

    void testSelectInsideExpansionAddsContainingPointer() {
        expand(m_id.vptr);   // QWidgetPrivate renders inline; `parent` sits in it
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.parent);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0], m_id.vptr);   // selecting inside QWidgetPrivate shows vptr
    }

    void testNestedSelectionBuildsFullChain() {
        expand(m_id.vptr);
        expand(m_id.parent);  // QWidget renders inside QWidgetPrivate
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.parent);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain[0], m_id.vptr);
        QCOMPARE(chain[1], m_id.parent);
    }

    void testHandleNodeClickSetsFocusPath() {
        expand(m_id.vptr);
        const int ln = lineOf(m_id.vptr);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], m_id.vptr);
        // Clicking a top-level row clears focus back to the root crumb.
        const int leafLn = lineOf(m_id.leaf);
        QVERIFY(leafLn >= 0);
        m_ctrl->handleNodeClick(m_editor, leafLn, m_id.leaf, Qt::NoModifier);
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    void testDottedCrumbLabels() {
        // Selecting an expanded pointer renders "<containingClass>.<field>" for
        // the source crumb and the bare class for the current one.
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QApplication::processEvents();
        const QStringList seg = segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.vptr")));   // class.field source
        QVERIFY(seg.contains(QStringLiteral("QWidgetPrivate")));   // current class
        QVERIFY(!seg.contains(QStringLiteral("vptr")));            // no bare field crumb
    }

    void testClearSelectionResetsFocus() {
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->clearSelection();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    void testCollapseToCrumbTruncatesAndCollapses() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 2);

        m_ctrl->collapseToFocus(1);   // collapse below QWidgetPrivate (parent)
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], m_id.vptr);
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!collapsed(m_id.vptr));

        m_ctrl->collapseToFocus(0);   // collapse vptr; back to bare root
        QVERIFY(m_ctrl->focusPath().isEmpty());
        QVERIFY(collapsed(m_id.vptr));
    }

    void testMultiLevelDottedLabels() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QApplication::processEvents();
        const QStringList seg = segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.vptr")));        // depth-0 class.field
        QVERIFY(seg.contains(QStringLiteral("QWidgetPrivate.parent"))); // depth-1 class.field
        QVERIFY(seg.contains(QStringLiteral("QWidget")));               // current class
    }

    void testCollapseToFocusOutOfRangeSafe() {
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->collapseToFocus(-1);   // negative → no-op
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->collapseToFocus(99);   // past the end → no crash, no collapse
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QVERIFY(!collapsed(m_id.vptr));
    }

    void testCyclicRefIdSeenGuard() {
        // A.pa → B, B.pb → A (a refId cycle), viewed from an UNRELATED root C
        // so no chain reaches the view root. focusChainTo's seen-guard must
        // break the cycle and return empty rather than spin.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        auto addCls = [&](const char* t) -> uint64_t {
            Node n; n.kind = NodeKind::Struct; n.structTypeName = t; n.parentId = 0;
            n.collapsed = false; return m_doc->tree.nodes[m_doc->tree.addNode(n)].id;
        };
        auto addPtr = [&](uint64_t parent, const char* nm, uint64_t ref) -> uint64_t {
            Node n; n.kind = NodeKind::Pointer64; n.name = nm; n.parentId = parent;
            n.refId = ref; n.collapsed = false;
            return m_doc->tree.nodes[m_doc->tree.addNode(n)].id;
        };
        uint64_t a = addCls("A"), b = addCls("B"), c = addCls("C");
        uint64_t pa = addPtr(a, "pa", b);
        uint64_t pb = addPtr(b, "pb", a);
        m_ctrl->setViewRootId(c);          // unreachable from the A↔B cycle
        QVector<uint64_t> chain = m_ctrl->focusChainToNode(pb);
        QVERIFY(chain.isEmpty());          // seen-guard fired, no hang
        // Sanity: from the proper root A, the chain is bounded and correct.
        m_ctrl->setViewRootId(a);
        QVector<uint64_t> ok = m_ctrl->focusChainToNode(pb);
        QCOMPARE(ok.size(), 2);
        QCOMPARE(ok[0], pa);
        QCOMPARE(ok[1], pb);
    }

    void testReconcileTrimsFoldCollapsedFocus() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 2);
        // Fold-collapse vptr behind the trail's back; refresh reconciles.
        m_doc->tree.nodes[idx(m_id.vptr)].collapsed = true;
        m_ctrl->refresh();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    // ── Embedded structs are drill frames of their own ──

    void testEmbeddedStructCrumbLabel() {
        // RcxEditor.stats is an EMBEDDED (non-pointer) struct expanded in
        // place. It is its own drill hop (drillTargetId(n) == n.id), so the
        // deepest crumb must name the embedded class — not fall through
        // refId == 0 to the first root class ("RcxEditor" twice).
        Node st; st.kind = NodeKind::Struct; st.name = "stats";
        st.structTypeName = "Stats"; st.parentId = m_id.editor; st.offset = 24;
        st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsId; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        m_ctrl->refresh();

        const int ln = lineOf(hpId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], statsId);
        QApplication::processEvents();
        const QStringList seg = segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.stats")));
        QVERIFY(seg.contains(QStringLiteral("Stats")));
        QVERIFY(!seg.contains(QStringLiteral("RcxEditor")));   // not the root twice

        // The path survives reconcileFocusPath on the next refresh (the
        // container check and the label must agree on what a frame is).
        m_ctrl->refresh();
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], statsId);
        // Fold-collapsing the embedded struct trims it like a pointer.
        m_doc->tree.nodes[idx(statsId)].collapsed = true;
        m_ctrl->refresh();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    // ── rebaseTo: the one base-address mutation ──

    void testNavigateToFormulaIsUndoable() {
        // Seed a formula base so undo has both fields to restore.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QCOMPARE(m_doc->undoStack.count(), 1);
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));

        QVERIFY(m_ctrl->navigateToFormula(QStringLiteral("0x40")));
        QCOMPARE(m_doc->undoStack.count(), 2);   // exactly one command
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());   // bare literal

        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));
        m_doc->undoStack.redo();
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());
    }

    void testRebaseToInvalidIsRefused() {
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const uint64_t before = m_doc->tree.baseAddress;
        QString err;
        QVERIFY(!m_ctrl->rebaseTo(QStringLiteral("[0x100"), &err));   // unclosed deref
        QVERIFY(!err.isEmpty());
        QCOMPARE(m_doc->undoStack.count(), 0);
        QCOMPARE(m_doc->tree.baseAddress, before);
        QVERIFY(!hint.isEmpty());
        QVERIFY(hint.last().at(0).toString().startsWith(QStringLiteral("Base: ")));
        // Empty / whitespace is refused the same way, not treated as 0.
        QVERIFY(!m_ctrl->rebaseTo(QStringLiteral("   "), &err));
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRebaseToLiteralClearsFormulaExpressionKeepsIt() {
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x10*2")));
        QCOMPARE(m_doc->tree.baseAddress, 0x20ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x10*2"));

        QVERIFY(m_ctrl->rebaseTo(QStringLiteral(" 0x30 ")));
        QCOMPARE(m_doc->tree.baseAddress, 0x30ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());

        // WinDbg backticks are stripped before the parser sees the text.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x7ff6`0000")));
        QCOMPARE(m_doc->tree.baseAddress, 0x7ff60000ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());

        // Same base + same formula → nothing pushed (no no-op undo entries).
        const int n = m_doc->undoStack.count();
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x7FF60000")));
        QCOMPARE(m_doc->undoStack.count(), n);
    }

    void testRebaseToKeepsFocusPath() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        const QVector<uint64_t> before = m_ctrl->focusPath();
        QCOMPARE(before.size(), 2);
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x10")));
        QCOMPARE(m_ctrl->focusPath(), before);   // the expanded chain is still on screen
        m_doc->undoStack.undo();
        QCOMPARE(m_ctrl->focusPath(), before);
    }

    // ── makeAddressCallbacks: one callback set for every caller ──

    void testCallbackFactoryHasKernelPagingWhenProviderDoes() {
        // No source: nothing wired; the parser reports module / symbol /
        // pointer terms with its own "unavailable" error.
        const AddressParserCallbacks none = makeAddressCallbacks(nullptr, 8);
        QVERIFY(!none.resolveModule && !none.readPointer && !none.resolveIdentifier);
        QVERIFY(!none.vtop && !none.cr3 && !none.physRead);

        // A plain provider: the memory / symbol trio, no paging.
        BufferProvider plain(QByteArray(16, '\0'));
        const AddressParserCallbacks p = makeAddressCallbacks(&plain, 8);
        QVERIFY(p.resolveModule && p.readPointer && p.resolveIdentifier);
        QVERIFY(!p.vtop && !p.cr3 && !p.physRead);

        // readPointer reads at the document's pointer size through Provider::read.
        BufferProvider bytes(QByteArray::fromHex("efbeadde00000000"));
        const AddressParserCallbacks q = makeAddressCallbacks(&bytes, 4);
        bool ok = false;
        QCOMPARE(q.readPointer(0, &ok), 0xDEADBEEFULL);
        QVERIFY(ok);

        // A provider that reports kernel paging gets vtop / cr3 / physRead
        // too — the branch the live evaluator and navigateToFormula lacked.
        struct PagingProvider : BufferProvider {
            using BufferProvider::BufferProvider;
            bool hasKernelPaging() const override { return true; }
            uint64_t getCr3() const override { return 0x1000; }
        } paging(QByteArray(16, '\0'));
        const AddressParserCallbacks k = makeAddressCallbacks(&paging, 8);
        QVERIFY(k.vtop && k.cr3 && k.physRead);
        QCOMPARE(k.cr3(0, &ok), 0x1000ULL);
        QVERIFY(ok);
    }

    // ── Crumb payload: class id, entry hop, address, keyword ──

    void testCrumbAddressesFollowPointerValues() {
        // Real pointer values in the buffer: the address of the class at
        // crumb i+1 is what compose dereferenced for hop i (LineMeta::ptrBase
        // on the first row rendered inside it). QWidget gets one field so its
        // expansion renders a row to carry the base.
        m_doc->provider = std::make_unique<BufferProvider>(chainBytes());
        Node fl; fl.kind = NodeKind::UInt32; fl.name = "flags"; fl.parentId = m_id.widget; fl.offset = 0;
        m_doc->tree.addNode(fl);
        drillTwoLevels();
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        QCOMPARE(c[0].address, 0x00ULL);
        QCOMPARE(c[1].address, 0x20ULL);
        QCOMPARE(c[2].address, 0x30ULL);
        // The bar shows exactly that address on the crumb's tooltip.
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        hoverAt(*bar, bar->itemRect(QStringLiteral("crumb:1")).center());
        QVERIFY2(bar->toolTip().contains(QStringLiteral("@ 0x20")), qPrintable(bar->toolTip()));
        hoverAt(*bar, bar->itemRect(QStringLiteral("crumb:2")).center());
        QVERIFY2(bar->toolTip().contains(QStringLiteral("@ 0x30")), qPrintable(bar->toolTip()));

        // Null / unreadable pointers: the frame is unknown, not "0x0 + offset".
        m_doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        m_ctrl->refresh();
        c = crumbs();
        QCOMPARE(c.size(), 3);                         // the trail itself survives
        QCOMPARE(c[0].address, 0x00ULL);
        QCOMPARE(c[1].address, 0x00ULL);
        QCOMPARE(c[2].address, 0x00ULL);
        QVERIFY2(bar->toolTip().contains(QStringLiteral("(unreadable)")), qPrintable(bar->toolTip()));
    }

    void testCrumbAddressOfEmbeddedStructIsItsRow() {
        // An embedded struct is its own hop and sits at base + its offset —
        // compose's ptrBase does not change for it (it is not a pointer), so
        // the crumb takes the row's own absolute address instead.
        Node st; st.kind = NodeKind::Struct; st.name = "stats"; st.structTypeName = "Stats";
        st.parentId = m_id.editor; st.offset = 24; st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsId; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        m_ctrl->refresh();
        m_ctrl->handleNodeClick(m_editor, lineOf(hpId), hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ statsId }));
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].address, 0ULL);
        QCOMPARE(c[1].address, 24ULL);
        QCOMPARE(c[1].classId, statsId);
        QCOMPARE(c[1].pointerId, statsId);              // its own hop
        QCOMPARE(c[1].label, QStringLiteral("Stats"));
        // A rebase moves every crumb with it (the focus path is kept).
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x100")));
        c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].address, 0x100ULL);
        QCOMPARE(c[1].address, 0x118ULL);
    }

    void testCrumbAddressOfRefIdEmbedIsItsRow() {
        // `Stats stats` embedded BY REFERENCE: a Struct field carrying a refId
        // and no children of its own (the type chooser's embed-class shape;
        // compose renders the referenced class's fields at this row). It
        // drills through refId like a pointer but dereferences nothing, so
        // its frame is its own row — not the enclosing frame's base, which
        // the pointer rule handed back (ptrBase never moves for it).
        Node cls; cls.kind = NodeKind::Struct; cls.structTypeName = "Stats";
        cls.parentId = 0; cls.collapsed = false;
        const uint64_t statsCls = m_doc->tree.nodes[m_doc->tree.addNode(cls)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsCls; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        Node st; st.kind = NodeKind::Struct; st.name = "stats"; st.refId = statsCls;
        st.parentId = m_id.editor; st.offset = 24; st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        m_ctrl->refresh();

        const int ln = lineOf(hpId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ statsId }));
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].label, QStringLiteral("RcxEditor.stats"));
        QCOMPARE(c[1].label, QStringLiteral("Stats"));
        QCOMPARE(c[1].classId, statsCls);
        QCOMPARE(c[1].pointerId, statsId);
        QCOMPARE(c[0].address, 0ULL);
        QCOMPARE(c[1].address, 24ULL);                  // base + stats.offset
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x100")));
        c = crumbs();
        QCOMPARE(c[1].address, 0x118ULL);
    }

    void testCrumbAddressOfArrayHopIsTheElement() {
        // An Array of struct with a refId, nested under an EXPANDED pointer
        // (so the enclosing frame has a non-zero ptrBase to leak). The array
        // drills through refId but dereferences nothing: its frame is the
        // element compose rendered — the [0] separator's address — not the
        // pointer's target the old pointer rule returned.
        m_doc->provider = std::make_unique<BufferProvider>(chainBytes());
        Node cls; cls.kind = NodeKind::Struct; cls.structTypeName = "Item";
        cls.parentId = 0; cls.collapsed = false;
        const uint64_t itemCls = m_doc->tree.nodes[m_doc->tree.addNode(cls)].id;
        Node v; v.kind = NodeKind::UInt32; v.name = "v"; v.parentId = itemCls; v.offset = 0;
        const uint64_t vId = m_doc->tree.nodes[m_doc->tree.addNode(v)].id;
        Node arr; arr.kind = NodeKind::Array; arr.name = "items"; arr.refId = itemCls;
        arr.elementKind = NodeKind::Struct; arr.arrayLen = 2;
        arr.parentId = m_id.priv; arr.offset = 8; arr.collapsed = false;
        const uint64_t itemsId = m_doc->tree.nodes[m_doc->tree.addNode(arr)].id;
        expand(m_id.vptr);   // QWidgetPrivate at 0x20 renders inline, items at 0x28

        const int ln = lineOf(vId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, vId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, itemsId }));
        const QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        QCOMPARE(c[1].label, QStringLiteral("QWidgetPrivate.items"));
        QCOMPARE(c[2].label, QStringLiteral("Item"));
        QCOMPARE(c[2].classId, itemCls);
        QCOMPARE(c[1].address, 0x20ULL);                // the pointer's target
        QCOMPARE(c[2].address, 0x28ULL);                // element 0, not 0x20
    }

    void testCrumbPointerIdsAndClassIds() {
        drillTwoLevels();
        const QVector<uint64_t>& fp = m_ctrl->focusPath();
        const QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        // pointerId = the hop crumb i was entered through = focusPath[i-1].
        QCOMPARE(c[0].pointerId, 0ULL);
        QCOMPARE(c[1].pointerId, fp[0]);
        QCOMPARE(c[2].pointerId, fp[1]);
        // classId = the container at each depth.
        QCOMPARE(c[0].classId, m_id.editor);
        QCOMPARE(c[1].classId, m_id.priv);
        QCOMPARE(c[2].classId, m_id.widget);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(c[i].rootId, uint64_t(i));
            QCOMPARE(c[i].keyword, QStringLiteral("struct"));   // resolved default
        }
        // The keyword follows the class node's resolved keyword.
        m_doc->tree.nodes[idx(m_id.widget)].classKeyword = QStringLiteral("class");
        m_doc->tree.nodes[idx(m_id.priv)].classKeyword = QStringLiteral("union");
        m_ctrl->refresh();
        QCOMPARE(crumbs()[2].keyword, QStringLiteral("class"));
        QCOMPARE(crumbs()[1].keyword, QStringLiteral("union"));
        QCOMPARE(crumbs()[0].keyword, QStringLiteral("struct"));
    }

    void testProductionCrumbsHaveNoConnector() {
        drillTwoLevels();
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        for (int i = 0; i < c.size(); ++i) {
            QVERIFY(!c[i].label.isEmpty());
            QVERIFY(c[i].classId != 0);
            // Ancestors are dotted ("Class.field"); the deepest is bare.
            QCOMPARE(c[i].label.contains(QLatin1Char('.')), i < c.size() - 1);
        }
        // The bar echoes exactly the labels — no italic field connector, no
        // separator token (the chevron cells are geometry, not text).
        QApplication::processEvents();
        QCOMPARE(segments(),
                 (QStringList{ QStringLiteral("RcxEditor.vptr"),
                               QStringLiteral("QWidgetPrivate.parent"),
                               QStringLiteral("QWidget") }));
        // Lone root: one undotted crumb naming the view root.
        m_ctrl->clearSelection();
        c = crumbs();
        QCOMPARE(c.size(), 1);
        QVERIFY(!c[0].label.contains(QLatin1Char('.')));
        QCOMPARE(c[0].classId, m_id.editor);
        QCOMPARE(c[0].pointerId, 0ULL);
    }

    // ── The state the controller pushes ──

    void testAddressBarStateCarriesSourceAndBase() {
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QApplication::processEvents();
        const AddressBarState& s = m_editor->addressBar()->state();
        QCOMPARE(s.baseAddress, 0x24ULL);
        QCOMPARE(s.resolvedBase, 0x24ULL);
        QCOMPARE(s.baseFormula, QStringLiteral("0x20+0x4"));
        // A BufferProvider is never live: None before the first status tick,
        // Static after it (the tick is timer-driven, so either is fine here;
        // P2b wires sourceStatusChanged → setLiveness for the in-between).
        QVERIFY(s.liveness == liveness::None || s.liveness == liveness::Static);
        QVERIFY(s.canBack && !s.canForward);    // the rebase was a gesture: the place left is behind
        QVERIFY(!s.canUp);                      // nothing drilled
        drillTwoLevels();
        QVERIFY(m_editor->addressBar()->state().canUp);
        // A second split pane is seeded with the current state at once — the
        // per-refresh push is change-guarded and would otherwise leave it
        // empty until the trail next moved.
        RcxEditor* pane2 = m_ctrl->addSplitEditor(m_splitter);
        QCOMPARE(pane2->addressBar()->state(), m_editor->addressBar()->state());
        QCOMPARE(pane2->addressBar()->state().crumbs.size(), 3);
    }

    // ── AddressBar widget ──
    // Driven RibbonBar-style: a bare bar, show() + qWait, geometry through
    // itemRect(id), hover through a synthesized MouseMove, pixels via grab().

    void testBarGeometry() {
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QCOMPARE(bar.height(), 26);
        QCOMPARE(bar.height(), PanelSearchField::kFieldHeight);
        // kGutter spent once, on the Back glyph's INK (pinned by
        // testFirstInkLandsOnTheGutter): the cell starts earlier by the
        // glyph's centring pad and the SVG's inset, like a doc tab's rect.
        QVERIFY(bar.itemRect(QStringLiteral("back")).left() <= kGutter);
        QVERIFY(bar.itemRect(QStringLiteral("back")).left() >= 0);
        // Every cell of the anatomy is laid out, left → right, without
        // overlaps, on one 22-px row.
        const QStringList order = {
            QStringLiteral("back"), QStringLiteral("fwd"), QStringLiteral("hist"), QStringLiteral("up"),
            QStringLiteral("src"), QStringLiteral("src.chev"),
            QStringLiteral("base"), QStringLiteral("crumb:0"), QStringLiteral("chev:0"),
            QStringLiteral("crumb:1"), QStringLiteral("chev:1"), QStringLiteral("space"),
            QStringLiteral("recent") };
        int lastRight = -1;
        for (const QString& id : order) {
            const QRect r = bar.itemRect(id);
            QVERIFY2(!r.isNull(), qPrintable(id + QStringLiteral(" not laid out")));
            QVERIFY2(r.left() > lastRight, qPrintable(id + QStringLiteral(" overlaps its left neighbour")));
            QCOMPARE(r.top(), AddressBar::kCellTop);
            QCOMPARE(r.height(), AddressBar::kCellH);
            lastRight = r.right();
        }
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());   // nothing folded
        // The field divider sits between `up` and `src`; `recent` hugs the
        // right edge.
        QVERIFY(bar.itemRect(QStringLiteral("src")).left() - bar.itemRect(QStringLiteral("up")).right()
                >= 2 * AddressBar::kDividerPad);
        QCOMPARE(bar.itemRect(QStringLiteral("recent")).right() + 1 + AddressBar::kRightMargin, bar.width());
        // Hit-testing round-trips; the gutter belongs to nobody.
        QCOMPARE(bar.itemIdAt(bar.itemRect(QStringLiteral("crumb:1")).center()), QStringLiteral("crumb:1"));
        QCOMPARE(bar.itemIdAt(bar.itemRect(QStringLiteral("base")).center()), QStringLiteral("base"));
        QVERIFY(bar.itemIdAt(QPoint(0, 13)).isEmpty());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        QVERIFY(!bar.isEditing());
    }

    // The timeline's buttons close the bar after a divider, pinned right, each
    // named for what a click does: Record (a red dot) — Stop (a red square,
    // red words) while recording — and, only while looking back at a
    // recording, Back to live (a green play triangle). There is no Pause:
    // values are live unless you look back. No time is written on either.
    void testTimelineButtonsSayWhatTheyDo() {
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        AddressBarState s = stateWith(twoLevel());
        bar.setState(s);
        showBar(bar, 1080);
        // Timeline switched off: nothing there.
        QVERIFY(bar.itemRect(QStringLiteral("tl.live")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("tl.rec")).isNull());

        s.timeline = true;
        s.canRecord = true;
        bar.setState(s);
        QApplication::processEvents();
        const QRect crumb = bar.itemRect(QStringLiteral("crumb:1"));
        QRect rec = bar.itemRect(QStringLiteral("tl.rec"));
        QVERIFY(!rec.isNull());
        QVERIFY2(bar.itemRect(QStringLiteral("tl.live")).isNull(), "Back to live offered while live");
        QCOMPARE(bar.cellText(QStringLiteral("tl.rec")), QStringLiteral("Record"));
        QCOMPARE(rec.right() + 1 + AddressBar::kRightMargin, bar.width());
        QVERIFY(bar.itemRect(QStringLiteral("recent")).right() + 2 * AddressBar::kDividerPad < rec.left());
        QVERIFY(bar.traversalIds().contains(QStringLiteral("tl.rec")));
        QImage img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, rec), bar.theme().markerPtr) > 0, "Record has no red dot");

        int lives = 0, records = 0;
        AddressBar::Callbacks cb;
        cb.onTimelineBackToLive = [&lives]() { ++lives; };
        cb.onTimelineRecord = [&records]() { ++records; };
        bar.setCallbacks(cb);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, rec.center());
        QApplication::processEvents();
        QCOMPARE(records, 1);

        // Recording: "Stop", still pinned right, and nothing counts.
        s.recording = true;
        bar.setState(s);
        QApplication::processEvents();
        QCOMPARE(bar.cellText(QStringLiteral("tl.rec")), QStringLiteral("Stop"));
        rec = bar.itemRect(QStringLiteral("tl.rec"));
        QCOMPARE(rec.right() + 1 + AddressBar::kRightMargin, bar.width());
        img = grabOf(bar);
        QVERIFY(countColour(img, devRect(img, rec), bar.theme().markerPtr) > 0);
        QVERIFY(bar.itemRect(QStringLiteral("tl.live")).isNull());

        // Looking back at a recording: "Back to live" appears, green, beside
        // Record; the crumbs stay where they were.
        s.past = true;
        bar.setState(s);
        QApplication::processEvents();
        const QRect live = bar.itemRect(QStringLiteral("tl.live"));
        QVERIFY(!live.isNull());
        QCOMPARE(bar.cellText(QStringLiteral("tl.live")), QStringLiteral("Back to live"));
        QVERIFY(live.right() < bar.itemRect(QStringLiteral("tl.rec")).left());
        QCOMPARE(bar.itemRect(QStringLiteral("crumb:1")), crumb);
        img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, live), bar.theme().indHintGreen) > 0, "Back to live is not green");
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, live.center());
        QApplication::processEvents();
        QCOMPARE(lives, 1);

        // Live again: it goes.
        s.past = false;
        bar.setState(s);
        QApplication::processEvents();
        QVERIFY(bar.itemRect(QStringLiteral("tl.live")).isNull());

        // A narrow pane never lays a button past its right edge: the words go
        // first (the glyphs stay), then one glyph is left — Back to live while
        // looking back, Record while live — then none.
        bool sawGlyphOnly = false;
        for (int w : {520, 420, 320, 290, 260, 240}) {
            bar.resize(w, bar.height());
            bool shownLookingBack = false;
            for (bool lookingBack : {true, false}) {
                s.past = lookingBack;
                bar.setState(s);
                QApplication::processEvents();
                for (const char* id : {"tl.live", "tl.rec", "recent"}) {
                    const QRect r = bar.itemRect(QString::fromLatin1(id));
                    QVERIFY2(r.isNull() || r.right() < bar.width(),
                             qPrintable(QStringLiteral("%1 past the edge at %2 px").arg(QLatin1String(id)).arg(w)));
                }
                const bool live = !bar.itemRect(QStringLiteral("tl.live")).isNull();
                const bool rec = !bar.itemRect(QStringLiteral("tl.rec")).isNull();
                if (lookingBack) {
                    if (rec) QVERIFY(live);                        // Back to live outlives Record
                    shownLookingBack = live || rec;
                    if (live && bar.cellText(QStringLiteral("tl.live")).isEmpty()) sawGlyphOnly = true;
                } else {
                    QVERIFY(!live);
                    // Live, the slot Back to live would take holds Record.
                    QVERIFY2(rec == shownLookingBack,
                             qPrintable(QStringLiteral("Record %1 live at %2 px").arg(rec ? "shown" : "dropped").arg(w)));
                }
            }
        }
        QVERIFY2(sawGlyphOnly, "the buttons never went glyph-only before dropping");
    }

    // Record → Stop, and Back to live coming and going, never move the rest
    // of the bar: at any width no crumb moves, folds or comes back.
    void testTimelineButtonStatesNeverReflowTheBar() {
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        AddressBarState s = stateWith(twoLevel());
        s.timeline = true;
        s.canRecord = true;
        bar.setState(s);
        showBar(bar, 1080);
        for (int w : {300, 360, 420, 480, 560, 720}) {
            bar.resize(w, bar.height());
            QHash<QString, QRect> first;
            for (int state = 0; state < 4; ++state) {
                AddressBarState t = s;
                t.past = state == 1 || state == 3;
                t.recording = state >= 2;
                bar.setState(t);
                QApplication::processEvents();
                for (const char* id : {"back", "fwd", "hist", "up", "src", "base", "overflow",
                                       "crumb:0", "crumb:1", "chev:0", "chev:1"}) {
                    const QString key = QString::fromLatin1(id);
                    const QRect r = bar.itemRect(key);
                    if (state == 0) first.insert(key, r);
                    else QVERIFY2(r == first.value(key),
                                  qPrintable(QStringLiteral("%1 moved at %2 px in state %3").arg(key).arg(w).arg(state)));
                }
            }
        }
    }

    void testFirstInkLandsOnTheGutter() {
        // The bar's leftmost ink — the Back arrow, enabled so it paints in
        // full — sits on the device column kGutter maps to, the column the
        // doc-tab title's first ink sits on. The cell pad and the SVG's own
        // inset used to push it ~5.6 px right of every other strip.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        AddressBarState s = stateWith(twoLevel());
        s.canBack = true;
        bar.setState(s);
        showBar(bar, 800);
        const QImage img = grabOf(bar);
        const int ink = firstInkColumn(img, editorPaperColor(bar.theme()));
        const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
        QVERIFY2(qAbs(ink - qRound(kGutter * dpr)) <= 1,
                 qPrintable(QStringLiteral("first ink at device column %1, gutter is %2 (dpr %3)")
                                .arg(ink).arg(qRound(kGutter * dpr)).arg(dpr)));
        // Still no ink in the gutter itself: nothing hangs off the left edge.
        QVERIFY(ink >= 1);
    }

    void testCrumbClickFiresIndexNotDeepest() {
        drillTwoLevels();   // RcxEditor.vptr › QWidgetPrivate.parent › QWidget
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy spy(m_editor, &RcxEditor::crumbClicked);
        // The deepest crumb is "you are here", and its label is that class's
        // NAME: a click opens a rename over it — no crumbClicked, no collapse,
        // no undo entry until something is committed, the trail as it was. It
        // reads as a field the way the base segment does: no hover fill, an
        // I-beam.
        const QRect deep = bar->itemRect(QStringLiteral("crumb:2"));
        QVERIFY(!deep.isNull());
        const int undoBefore = m_doc->undoStack.count();
        const bool vptrWas = collapsed(m_id.vptr), parentWas = collapsed(m_id.parent);
        hoverAt(*bar, deep.center());
        QImage img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, deep), t.hover) < 16, "deepest crumb took a hover fill");
        QCOMPARE(bar->cursor().shape(), Qt::IBeamCursor);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, deep.center());
        QApplication::processEvents();
        QVERIFY2(bar->isClassNameEditing(), "the deepest crumb did not open its rename");
        QCOMPARE(bar->editText(), QStringLiteral("QWidget"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QCOMPARE(spy.count(), 0);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(collapsed(m_id.vptr), vptrWas);
        QCOMPARE(collapsed(m_id.parent), parentWas);
        QCOMPARE(segments().size(), 3);
        spy.clear();
        // Press inside, release outside → no click.
        const QRect r1 = bar->itemRect(QStringLiteral("crumb:1"));
        QVERIFY(!r1.isNull());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, r1.center());
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, QPoint(r1.center().x(), r1.bottom() + 40));
        QApplication::processEvents();
        QCOMPARE(spy.count(), 0);
        // An ancestor click → crumbClicked(index) → collapseToFocus(index).
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, r1.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(collapsed(m_id.parent));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate") }));
    }

    void testBarNeverTakesFocus() {
        // NoFocus contract: a click on the bar must not steal focus from the
        // document (here: a sibling QLineEdit standing in for Scintilla).
        QWidget win;
        auto* lay = new QVBoxLayout(&win);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* bar = new AddressBar(&win);
        int clicked = -1;
        AddressBar::Callbacks cb;
        cb.onCrumb = [&](int i) { clicked = i; };
        bar->setCallbacks(std::move(cb));
        bar->setState(stateWith(twoLevel()));
        auto* edit = new QLineEdit(&win);
        lay->addWidget(bar);
        lay->addWidget(edit);
        win.resize(800, 80);
        win.show();
        edit->setFocus();
        QTest::qWait(50);
        QApplication::processEvents();
        QCOMPARE(win.focusWidget(), edit);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);

        const QRect r = bar->itemRect(QStringLiteral("crumb:0"));
        QVERIFY(!r.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, r.center());
        QApplication::processEvents();
        QCOMPARE(clicked, 0);
        QCOMPARE(win.focusWidget(), edit);
        QVERIFY(!bar->hasFocus());
    }

    // Pixel contract: nothing filled at rest, hover is the `hover` fill on
    // ancestors only, the tone ladder is textDim (ancestor) / text (deepest)
    // / textDim (lone), the band is paper, the seam is one device row of
    // containerBorderColor, and no accent pixel exists anywhere on the bar.
    void testPixelsRestHoverAndTones() {
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QImage img = grabOf(bar);
        const QRect all(0, 0, img.width(), img.height());
        // Text anti-aliasing can hit a fill colour by coincidence, so a
        // handful of stray pixels is tolerated — a fill would be hundreds.
        QVERIFY2(countColour(img, all, t.hover) < 16, "hover fill at rest");
        QVERIFY2(countColour(img, all, pressedFill(t)) < 16, "pressed fill at rest");
        QCOMPARE(countColour(img, all, t.indHoverSpan), 0);
        // The band is paper — sample the stretch.
        const QRect space = devRect(img, bar.itemRect(QStringLiteral("space")));
        QVERIFY(space.width() > 4);
        QVERIFY(sameColour(img.pixel(space.center()), editorPaperColor(t)));
        // The seam: the bottom device row is containerBorderColor end to end.
        const QRect bottom(0, img.height() - 1, img.width(), 1);
        QVERIFY(countColour(img, bottom, containerBorderColor(t)) > img.width() * 9 / 10);
        QVERIFY2(countColour(img, QRect(0, img.height() - 2, img.width(), 1), containerBorderColor(t))
                     < img.width() / 10, "seam is more than one device row");
        // Tones: the ancestor is textDim with no `text` ink; the deepest
        // carries `text` (DemiBold).
        const QRect c0 = devRect(img, bar.itemRect(QStringLiteral("crumb:0")));
        const QRect c1 = devRect(img, bar.itemRect(QStringLiteral("crumb:1")));
        QVERIFY(countColour(img, c0, t.textDim) > 0);
        QCOMPARE(countColour(img, c0, t.text), 0);
        QVERIFY(countColour(img, c1, t.text) > 0);

        // Hover on an ancestor: the fill, and the tone steps up to text.
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:0")).center());
        img = grabOf(bar);
        QVERIFY(countColour(img, c0, t.hover) > 0);
        QVERIFY(countColour(img, c0, t.text) > 0);
        QCOMPARE(countColour(img, all, t.indHoverSpan), 0);
        // Hover on the deepest: inert, no fill.
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:1")).center());
        img = grabOf(bar);
        QVERIFY2(countColour(img, c1, t.hover) < 16, "deepest crumb took a hover fill");
        // Hover on the base: a text field, no fill either.
        const QRect base = devRect(img, bar.itemRect(QStringLiteral("base")));
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        img = grabOf(bar);
        QVERIFY2(countColour(img, base, t.hover) < 16, "base took a hover fill");
        // Leaving clears the fill.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&bar, &leave);
        img = grabOf(bar);
        QVERIFY2(countColour(img, all, t.hover) < 16, "hover fill survived leave");

        // A lone crumb only repeats what the doc tab already names: textDim,
        // regular — no `text` ink.
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor"), 0) }));
        img = grabOf(bar);
        const QRect lone = devRect(img, bar.itemRect(QStringLiteral("crumb:0")));
        QVERIFY(countColour(img, lone, t.textDim) > 0);
        QCOMPARE(countColour(img, lone, t.text), 0);
    }

    void testOverflowFoldsFromTheRootNeverTheDeepest() {
        AddressBar bar;
        QVector<Crumb> trail;
        QStringList labels;
        for (int i = 0; i < 6; ++i) {
            const QString l = i < 5 ? QStringLiteral("C%1.f%1").arg(i) : QStringLiteral("C5");
            trail.push_back(crumb(l, uint64_t(i), uint64_t(i + 1)));
            labels << l;
        }
        bar.setState(stateWith(trail));
        showBar(bar, 300);
        QVERIFY(!bar.itemRect(QStringLiteral("overflow")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:0")).isNull());
        QVERIFY(!bar.itemRect(QStringLiteral("crumb:5")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:5")).left() > bar.itemRect(QStringLiteral("overflow")).right());
        const QStringList hidden = bar.overflowMenuLabels();
        QVERIFY(hidden.size() >= 1 && hidden.size() <= 5);
        QCOMPARE(hidden.first(), QStringLiteral("C0.f0"));     // folded from the root
        QVERIFY(!hidden.contains(QStringLiteral("C5")));       // never the deepest
        QCOMPARE(bar.segments().first(), QStringLiteral("«"));
        // Hidden and shown partition the trail, in order.
        QCOMPARE(hidden + bar.segments().mid(1), labels);

        // Wide: nothing folds, everything fits inside the strip.
        bar.resize(1200, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());
        for (int i = 0; i < 6; ++i)
            QVERIFY2(!bar.itemRect(QStringLiteral("crumb:%1").arg(i)).isNull(), "crumb dropped at 1200 px");
        QVERIFY(bar.overflowMenuLabels().isEmpty());
        QCOMPARE(bar.segments(), labels);
        QVERIFY(bar.itemRect(QStringLiteral("chev:5")).right() < bar.itemRect(QStringLiteral("recent")).left());
        QCOMPARE(bar.itemRect(QStringLiteral("recent")).right() + 1 + AddressBar::kRightMargin, 1200);
        // Narrow again: folds again (the layout follows the width).
        bar.resize(300, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(!bar.itemRect(QStringLiteral("overflow")).isNull());
    }

    // The narrow-pane steps (7a–7e). Six fit steps bottom out near 395 px
    // with a process source and a formula base; below that the harness used
    // to show the deepest crumb and `recent` laid out PAST the right edge —
    // the widget's own rule says the deepest crumb is never dropped, and a
    // crumb off the strip is dropped in every way that matters. Now the
    // chip goes icon-only, the base shows its bare literal,
    // Forward goes and, last, Back and the divider go; the deepest crumb
    // and `recent` end inside the strip down to kNarrowFloorW.
    void testNarrowPanesKeepTheDeepestCrumbAndRecent() {
        AddressBar bar;
        const QString formula = QStringLiteral("<RCTarget.exe>+0x1234");
        AddressBarState s = stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0, 1),
                                        crumb(QStringLiteral("QWidgetPrivate.parent"), 1, 2),
                                        crumb(QStringLiteral("QWidget"), 2, 3) });
        s.baseFormula  = formula;
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        s.canBack = true;
        bar.setState(s);
        const QFontMetrics fm(bar.font());
        auto rect   = [&](const char* id) { return bar.itemRect(QLatin1String(id)); };
        auto inside = [&](const char* id) {
            const QRect r = rect(id);
            return !r.isNull() && r.left() >= 0 && r.right() < bar.width();
        };
        // The invariant, as one verdict (a QVERIFY inside a lambda would
        // leave the lambda, not the test): empty = holds, else what broke.
        auto namesThePlace = [&]() -> QString {
            if (!inside("crumb:2")) return QStringLiteral("the deepest crumb ran past the right edge");
            if (!inside("recent"))  return QStringLiteral("`recent` ran past the right edge");
            if (rect("crumb:2").right() >= rect("recent").left()) return QStringLiteral("deepest crumb under recent");
            if (!rect("space").isNull() && rect("space").right() >= rect("recent").left())
                return QStringLiteral("space under recent");
            if (bar.itemIdAt(rect("crumb:2").center()) != QLatin1String("crumb:2")) return QStringLiteral("crumb:2 not hit-testable");
            if (bar.itemIdAt(rect("recent").center()) != QLatin1String("recent")) return QStringLiteral("recent not hit-testable");
            if (bar.segments().last() != QLatin1String("QWidget")) return QStringLiteral("deepest label lost");
            return QString();
        };
        auto holdsAt = [&](int w) {
            const QString why = namesThePlace();
            return why.isEmpty() ? QByteArray() : QStringLiteral("%1 at %2 px").arg(why).arg(w).toUtf8();
        };

        // 480 px: the six fit steps are enough — none of step 7 turns.
        showBar(bar, 480);
        QVERIFY2(holdsAt(480).isEmpty(), holdsAt(480).constData());
        // The chip still names its source — possibly middle-elided already
        // ("RCTa….exe": with three crumbs and a formula base step 2 turns
        // at 480), but 7a has not: the name is on the chip, not in the tip.
        QVERIFY2(bar.sourceDisplayText().startsWith(QStringLiteral("RCTa")), qPrintable(bar.sourceDisplayText()));
        QVERIFY(!rect("back").isNull());
        QVERIFY(!rect("fwd").isNull());
        QVERIFY2(bar.baseDisplayText() != QStringLiteral("0x7FF6DEAD1234"), qPrintable(bar.baseDisplayText()));

        // 300 px: under the ~395-px floor. The chip is icon-only (7a) —
        // the icon and its dot are still there, the name is the tooltip's.
        bar.resize(300, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY2(holdsAt(300).isEmpty(), holdsAt(300).constData());
        QVERIFY2(bar.sourceDisplayText().isEmpty(), qPrintable(bar.sourceDisplayText()));
        QVERIFY(!bar.sourceIconRect().isNull());
        QVERIFY(rect("src").contains(bar.sourceIconRect()));
        QVERIFY(!bar.livenessDotRect().isNull());
        QVERIFY(!rect("src.chev").isNull());
        QCOMPARE(rect("src").width(), AddressBar::kChipPad + AddressBar::kChipIconPx + AddressBar::kChipChevGap);
        hoverAt(bar, rect("src").center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("RCTarget.exe")), qPrintable(bar.toolTip()));
        // Back / Forward survive 300 px (7d / 7e are the last resorts, not this).
        QVERIFY(!rect("back").isNull());
        QVERIFY(!rect("fwd").isNull());
        QVERIFY(rect("up").isNull());       // step 5 went first
        QVERIFY(rect("chev:2").isNull());   // and step 6
        QVERIFY(!rect("overflow").isNull());
        QCOMPARE(bar.state().baseFormula, formula);   // display only: the state keeps the formula

        // 240 px: Forward is gone (7d), the base is its bare literal at
        // ≤ 60 px (7c), and the deepest crumb still fits. Whether Back
        // outlives 240 is the font's call — this trail lands within a few
        // px of it after 7d — so 7e is pinned at the floor below, not here.
        static_assert(AddressBar::kNarrowFloorW <= 240, "240 px is meant to sit on or above the floor");
        bar.resize(240, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY2(holdsAt(240).isEmpty(), holdsAt(240).constData());
        for (const char* id : {"fwd", "hist", "up", "chev:2"})
            QVERIFY2(rect(id).isNull(), qPrintable(QStringLiteral("%1 laid out at 240 px").arg(QString::fromLatin1(id))));
        if (rect("back").isNull()) {
            QCOMPARE(bar.sourceIconRect().left(), kGutter);               // 7e: the icon takes the gutter
        } else {
            QVERIFY(rect("back").right() < bar.sourceIconRect().left());  // 7d: Back, the divider, then the icon
        }
        QVERIFY(bar.sourceDisplayText().isEmpty());
        QVERIFY2(bar.baseDisplayText().startsWith(QStringLiteral("0x")), qPrintable(bar.baseDisplayText()));
        QVERIFY(fm.horizontalAdvance(bar.baseDisplayText()) <= AddressBar::kBaseBareMinW);
        QCOMPARE(bar.state().baseFormula, formula);
        hoverAt(bar, rect("base").center());
        QVERIFY2(bar.toolTip().contains(formula), qPrintable(bar.toolTip()));   // the tooltip keeps it
        // The deepest crumb sits at its 60-px floor (pads either side).
        QVERIFY(rect("crumb:2").width() <= 2 * AddressBar::kCrumbPad + AddressBar::kDeepestMinW);
        QVERIFY(!rect("overflow").isNull());
        QCOMPARE(bar.overflowMenuLabels(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                                         QStringLiteral("QWidgetPrivate.parent") }));
        // Keyboard mode walks what is laid out: no Forward stop, the
        // deepest crumb first.
        QVERIFY(!bar.traversalIds().contains(QStringLiteral("fwd")));
        QVERIFY(bar.traversalIds().contains(QStringLiteral("src")));
        bar.enterKeyboardMode();
        QCOMPARE(bar.focusId(), QStringLiteral("crumb:2"));
        bar.leaveKeyboardMode(false);

        // The floor: 7e is certain here (after 7d this trail is still
        // wider than kNarrowFloorW) — Back and the divider are gone, the
        // chip's icon is the first ink on the gutter, and the two still
        // sit inside.
        bar.resize(AddressBar::kNarrowFloorW, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY2(holdsAt(AddressBar::kNarrowFloorW).isEmpty(), holdsAt(AddressBar::kNarrowFloorW).constData());
        for (const char* id : {"back", "fwd", "hist", "up", "chev:2"})
            QVERIFY2(rect(id).isNull(), qPrintable(QStringLiteral("%1 laid out at the floor").arg(QString::fromLatin1(id))));
        QCOMPARE(bar.sourceIconRect().left(), kGutter);
        QCOMPARE(bar.traversalIds().first(), QStringLiteral("src"));

        // 7d is its own step, in order: walking the floor..300 band a px at
        // a time, Forward is never laid out without Back, Back stands alone
        // somewhere in it (the 24 px Forward frees are a real band), and
        // once Back goes the icon takes the gutter. Every width keeps the
        // two inside (the invariant relayout asserts in debug).
        bool backAlone = false;
        for (int w = 300; w >= AddressBar::kNarrowFloorW; --w) {
            bar.resize(w, AddressBar::kAddressBarHeight);
            QApplication::processEvents();
            QVERIFY2(holdsAt(w).isEmpty(), holdsAt(w).constData());
            const bool back = !rect("back").isNull();
            const bool fwd  = !rect("fwd").isNull();
            QVERIFY2(back || !fwd, qPrintable(QStringLiteral("Forward without Back at %1 px").arg(w)));
            if (back && !fwd) backAlone = true;
            if (!back) { QCOMPARE(bar.sourceIconRect().left(), kGutter); }
        }
        QVERIFY(backAlone);
        for (int w = 300; w <= 480; w += 7) {
            bar.resize(w, AddressBar::kAddressBarHeight);
            QApplication::processEvents();
            QVERIFY2(holdsAt(w).isEmpty(), holdsAt(w).constData());
        }

        // Wide again: everything comes back.
        bar.resize(800, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QCOMPARE(bar.sourceDisplayText(), QStringLiteral("RCTarget.exe"));
        QVERIFY(!rect("back").isNull());
        QVERIFY(!rect("up").isNull());
        QVERIFY(rect("overflow").isNull());
        QVERIFY2(bar.baseDisplayText().startsWith(QStringLiteral("<RCT")), qPrintable(bar.baseDisplayText()));
    }

    void testEqualStatePushAppliesOnce() {
        // The controller pushes every refresh tick; an equal state is free.
        AddressBar bar;
        const AddressBarState s = stateWith(twoLevel());
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), 1);
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), 1);
        AddressBarState s2 = s;
        s2.liveness = liveness::Stale;
        bar.setState(s2);
        QCOMPARE(bar.stateApplyCount(), 2);
        // Through the editor too: the controller's push of what the bar
        // already shows must not count. Settle with one refresh first — the
        // status tick can move m_lastStatus between two pushes.
        m_ctrl->refresh();
        const int n = m_editor->addressBar()->stateApplyCount();
        m_editor->setAddressBarState(m_editor->addressBar()->state());
        m_ctrl->refresh();
        QCOMPARE(m_editor->addressBar()->stateApplyCount(), n);
    }

    void testTooltipsFollowHover() {
        // The widget toolTip mirrors the hovered cell (the bridge shows it);
        // QEvent::ToolTip is never handled here.
        AddressBar bar;
        QVector<Crumb> trail = twoLevel();
        trail[0].address = 0x1000;
        trail[1].address = 0x7FF6DEAD0030ULL;
        bar.setState(stateWith(trail));
        showBar(bar, 800);
        QVERIFY(bar.toolTip().isEmpty());
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Base address")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("0x7FF600000000")));
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:1")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("@ 0x7FF6DEAD0030")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().startsWith(QStringLiteral("QWidgetPrivate")));
        // An unreadable frame says so; the text is republished in place on
        // the state push, without a dismiss.
        trail[1].address = 0;
        bar.setState(stateWith(trail));
        QVERIFY2(bar.toolTip().contains(QStringLiteral("(unreadable)")), qPrintable(bar.toolTip()));
        hoverAt(bar, bar.itemRect(QStringLiteral("back")).center());
        QCOMPARE(bar.toolTip(), QStringLiteral("Back  Alt+Left"));
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Process RCTarget.exe")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("Live")));
        hoverAt(bar, bar.itemRect(QStringLiteral("chev:0")).center());
        QCOMPARE(bar.toolTip(), QStringLiteral("Fields of RcxEditor"));
        // Leaving clears it.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&bar, &leave);
        QVERIFY(bar.toolTip().isEmpty());
    }

    void testBackCellDisabledWhenNoHistory() {
        // No history yet (P5): the Back cell is painted at 40 % and ignores
        // clicks; with history it paints in full and fires.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        int backs = 0;
        AddressBar::Callbacks cb;
        cb.onBack = [&] { ++backs; };
        bar.setCallbacks(std::move(cb));
        AddressBarState s = stateWith(twoLevel());   // canBack = false
        bar.setState(s);
        showBar(bar, 800);
        const QRect back = bar.itemRect(QStringLiteral("back"));
        QVERIFY(!back.isNull());
        const double inkOff = inkAmount(grabOf(bar), back, editorPaperColor(t));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QApplication::processEvents();
        QCOMPARE(backs, 0);

        s.canBack = true;
        bar.setState(s);
        const double inkOn = inkAmount(grabOf(bar), back, editorPaperColor(t));
        QVERIFY2(inkOn > 3.0, qPrintable(QStringLiteral("enabled Back paints no ink (%1)").arg(inkOn)));
        QVERIFY2(inkOff < inkOn * 0.6,
                 qPrintable(QStringLiteral("disabled Back didn't dim: on=%1 off=%2").arg(inkOn).arg(inkOff)));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QApplication::processEvents();
        QCOMPARE(backs, 1);
    }

    // ── Source chip + base segment ──

    void testSourceChipTooltipNamesKindAndStatus() {
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));   // Process RCTarget.exe, Live
        showBar(bar, 800);
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        const QString tip = bar.toolTip();
        QVERIFY2(tip.contains(kindLabelFor(QStringLiteral("processmemory"))), qPrintable(tip));
        QVERIFY(tip.contains(QStringLiteral("RCTarget.exe")));
        QVERIFY(tip.contains(QStringLiteral("Live — reading")));   // the status chip's words
        QVERIFY(tip.contains(QStringLiteral("click to change source")));
        QCOMPARE(bar.sourceDisplayText(), QStringLiteral("RCTarget.exe"));
        // The chevron is part of the chip: same tip, same hover group.
        hoverAt(bar, bar.itemRect(QStringLiteral("src.chev")).center());
        QCOMPARE(bar.toolTip(), tip);
        // Status words follow the liveness, republished in place.
        AddressBarState s = stateWith(twoLevel());
        s.liveness = liveness::Disconnected;
        bar.setState(s);
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Disconnected")), qPrintable(bar.toolTip()));
        s.liveness = liveness::Static;
        s.sourceKindId = QStringLiteral("File");
        bar.setState(s);
        QVERIFY(bar.toolTip().startsWith(kindLabelFor(QStringLiteral("File"))));
        QVERIFY(bar.toolTip().contains(QStringLiteral("Static file")));
        // No provider: the chip invites one, in the cell and in the tip.
        s.sourceName.clear(); s.sourceKindId.clear(); s.liveness = liveness::None;
        bar.setState(s);
        QCOMPARE(bar.sourceDisplayText(), QStringLiteral("Select source"));
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Select source")), qPrintable(bar.toolTip()));
        QVERIFY(bar.livenessDotRect().isNull());
    }

    void testSourceChipLivenessDot() {
        // The status chip's mapping on the bar: green reading, focusGlow
        // stale, markerError disconnected (with the icon at 40 %), nothing
        // for a static file. The dot rides the icon's bottom-right corner.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        const QColor paper = editorPaperColor(t);
        AddressBarState s = stateWith(twoLevel());   // Live
        bar.setState(s);
        showBar(bar, 800);
        const QRect icon = bar.sourceIconRect();
        const QRect dot  = bar.livenessDotRect();
        QVERIFY(!icon.isNull());
        QVERIFY(!dot.isNull());
        QCOMPARE(dot.size(), QSize(AddressBar::kDotPx, AddressBar::kDotPx));
        QVERIFY(icon.contains(dot.topLeft()));            // on the icon...
        QVERIFY(dot.right() > icon.right() && dot.bottom() > icon.bottom());   // ...over its corner
        QVERIFY(dot.bottom() < bar.itemRect(QStringLiteral("src")).bottom()); // inside the cell
        // The ring around the dot is excluded from the icon-ink probe.
        const QRect ring = dot.adjusted(-AddressBar::kDotRing, -AddressBar::kDotRing,
                                        AddressBar::kDotRing, AddressBar::kDotRing);
        QImage img = grabOf(bar);
        QRect dd = devRect(img, dot);
        QVERIFY(countColour(img, dd, t.indHintGreen) > 0);
        const double inkLive = inkAmount(img, icon, paper, ring);
        QVERIFY2(inkLive > 3.0, qPrintable(QStringLiteral("live icon paints no ink (%1)").arg(inkLive)));

        s.liveness = liveness::Stale;
        bar.setState(s);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.focusGlow) > 0);
        QCOMPARE(countColour(img, dd, t.indHintGreen), 0);

        s.liveness = liveness::Disconnected;
        bar.setState(s);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.markerError) > 0);
        const double inkGone = inkAmount(img, icon, paper, ring);
        QVERIFY2(inkGone < inkLive * 0.6,
                 qPrintable(QStringLiteral("disconnected icon didn't dim: live=%1 gone=%2").arg(inkLive).arg(inkGone)));

        // A static file has nothing to be alive: no dot at all, icon at full ink.
        s.liveness = liveness::Static;
        bar.setState(s);
        QVERIFY(bar.livenessDotRect().isNull());
        img = grabOf(bar);
        QCOMPARE(countColour(img, dd, t.indHintGreen), 0);
        QCOMPARE(countColour(img, dd, t.focusGlow), 0);
        QCOMPARE(countColour(img, dd, t.markerError), 0);
        QVERIFY(inkAmount(img, icon, paper, ring) > inkGone);

        // setLiveness alone — the sourceStatusChanged path — moves the dot
        // without counting as a state push.
        const int n = bar.stateApplyCount();
        bar.setLiveness(liveness::Live);
        QCOMPARE(bar.stateApplyCount(), n);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.indHintGreen) > 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
    }

    void testBaseFormulaElidedForDisplayOnly() {
        // The regression this pins: the command row elided a long formula
        // for display and then edited the elided text, feeding the ellipsis
        // to the parser. Here the display is elided, the state keeps every
        // character, and the resolved address follows in textMuted.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        AddressBarState s = stateWith(twoLevel());
        bar.setState(s);
        showBar(bar, 1200);
        // A literal: shown as-is, no suffix (it would repeat the number).
        QCOMPARE(bar.baseDisplayText(), QStringLiteral("0x7FF600000000"));
        QImage img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) < 16,
                 "resolved suffix drawn beside a literal");

        const QString formula = QStringLiteral("<RCTarget.exe>+0x1234+[0x10]*2");
        QCOMPARE(formula.size(), 30);
        s.baseFormula  = formula;
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        QCOMPARE(bar.state().baseFormula, formula);            // the state: every character
        const QString shown = bar.baseDisplayText();           // the display: elided
        QVERIFY2(shown.size() < formula.size(), qPrintable(shown));
        QVERIFY(shown.contains(QChar(0x2026)));
        QVERIFY2(shown.startsWith(QStringLiteral("<RCT")) && shown.endsWith(QStringLiteral("*2")),
                 qPrintable(shown));                            // middle elision keeps both ends
        QVERIFY(QFontMetrics(bar.font()).horizontalAdvance(shown) <= AddressBar::kBaseMaxW);
        img = grabOf(bar);
        const QRect base = devRect(img, bar.itemRect(QStringLiteral("base")));
        QVERIFY(countColour(img, base, t.textMuted) > 8);      // the " = 0x…" suffix
        QCOMPARE(countColour(img, base, t.indHoverSpan), 0);
        // The tooltip carries what the segment could not.
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY2(bar.toolTip().contains(formula), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("0x7FF6DEAD1234")));
        QVERIFY(bar.toolTip().startsWith(QStringLiteral("Base address")));

        // 30 px short of the natural width: overflow step 1 takes only what
        // is over — the formula loses a few characters and keeps its suffix.
        // (`recent` hugs the right edge, so the natural width is the trail's
        // right end plus the recent cell.)
        const int naturalW = bar.itemRect(QStringLiteral("chev:1")).right() + 1
                           + AddressBar::kRecentW + AddressBar::kRightMargin;
        QVERIFY(naturalW < 1200);
        bar.resize(naturalW - 30, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());
        const QString slightly = bar.baseDisplayText();
        QVERIFY2(slightly.size() < formula.size() && slightly.contains(QChar(0x2026)), qPrintable(slightly));
        QVERIFY2(QFontMetrics(bar.font()).horizontalAdvance(slightly) > AddressBar::kBaseMinW,
                 "a 30-px squeeze fell straight to the 90-px floor");
        img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) > 8,
                 "resolved suffix dropped for a 30-px squeeze");
        // A real squeeze: the base at its 90-px floor, suffix gone, the
        // state untouched.
        bar.resize(300, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(QFontMetrics(bar.font()).horizontalAdvance(bar.baseDisplayText()) <= AddressBar::kBaseMinW);
        img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) < 16,
                 "resolved suffix survived the narrow fold");
        QCOMPARE(bar.state().baseFormula, formula);
    }

    void testSourceChipClickOpensPopupUnderTheBar() {
        // Click the chip → sourcePopupRequested with the anchor on the bar's
        // bottom edge → the controller's SourceChooserPopup opens there and
        // the chip stays t.hover until the popup hides, by any route.
        //
        // A Qt::Popup cannot keep OS focus on the hidden test desktop: the
        // first event pump after show() takes it away again and the platform
        // closes the popup (the artifact behind test_source_chooser's
        // exposure failures). So the menu-open probe grabs BEFORE the pump
        // and the released probe after it — the platform's close and an
        // explicit hide() both leave through hideEvent → dismissed().
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy spy(m_editor, &RcxEditor::sourcePopupRequested);
        const QRect src = bar->itemRect(QStringLiteral("src"));
        QVERIFY(!src.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, src.center());
        QCOMPARE(spy.count(), 1);
        const QPoint anchor = spy.at(0).at(0).toPoint();
        QCOMPARE(anchor.y(), bar->mapToGlobal(QPoint(0, bar->height())).y());
        QCOMPARE(anchor.x(), bar->mapToGlobal(QPoint(src.left(), 0)).x());
        auto* popup = m_editor->findChild<SourceChooserPopup*>();
        QVERIFY(popup);
        QVERIFY(popup->isVisible());
        QImage img = bar->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        // A cell with its dropdown up is t.hover, not pressedFill: on tw.json
        // pressedFill is t.selected, which boxed the chip in pale blue for
        // the life of the popup (build/issue.png).
        QVERIFY2(countColour(img, devRect(img, src), t.hover) > 0, "chip not hover-filled while the popup is up");
        if (pressedFill(t) != t.hover)
            QCOMPARE(countColour(img, devRect(img, src), pressedFill(t)), 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
        QApplication::processEvents();
        popup->hide();
        QApplication::processEvents();
        QVERIFY(!popup->isVisible());
        img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, src), t.hover) < 16, "chip stayed filled after the popup hid");
    }

    void testSourceStatusSignalMovesTheDot() {
        // The controller's status flip reaches the chip without a refresh
        // push: emit the signal and read the bar's liveness back.
        AddressBar* bar = m_editor->addressBar();
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Stale);
        QCOMPARE(bar->state().liveness, liveness::Stale);
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Disconnected);
        QCOMPARE(bar->state().liveness, liveness::Disconnected);
        // A second pane gets its own connection.
        RcxEditor* pane2 = m_ctrl->addSplitEditor(m_splitter);
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Live);
        QCOMPARE(pane2->addressBar()->state().liveness, liveness::Live);
        QCOMPARE(bar->state().liveness, liveness::Live);
    }

    void testCrumbPathTextMatchesTheModel() {
        // "Copy path" rebuilds the dotted path from the labels alone and must
        // agree with trailPathText() over the tree for the same trail.
        AddressBar bar;
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                 crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                 crumb(QStringLiteral("QWidget"), 2) }));
        QCOMPARE(bar.crumbPathText(0), QStringLiteral("RcxEditor"));
        QCOMPARE(bar.crumbPathText(1), QStringLiteral("RcxEditor.vptr"));
        QCOMPARE(bar.crumbPathText(2), QStringLiteral("RcxEditor.vptr.parent"));
        drillTwoLevels();
        QApplication::processEvents();
        QCOMPARE(m_editor->addressBar()->crumbPathText(2),
                 trailPathText(m_doc->tree, m_ctrl->viewRootId(), m_ctrl->focusPath()));
    }

    // ── P3: the base edit ──

    void testBaseClickOpensOverlayOnFullFormula() {
        // The regression the overlay exists for: the display elides a long
        // formula, the EDIT opens on every character of it — never on the
        // ellipsis the command row used to hand the parser.
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        const QString formula = QStringLiteral("<RCTarget.exe>+0x1234+[0x10]*2");
        QCOMPARE(formula.size(), 30);
        s.baseFormula  = formula;
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        showBar(bar, 800);
        const QString shown = bar.baseDisplayText();
        QVERIFY2(shown.size() < formula.size() && shown.contains(QChar(0x2026)), qPrintable(shown));
        QVERIFY(!bar.isEditing());
        const QRect base = bar.itemRect(QStringLiteral("base"));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, base.center());
        QApplication::processEvents();
        QVERIFY(bar.isEditing());
        QCOMPARE(bar.editText(), formula);
        QCOMPARE(bar.editWidget()->selectedText(), formula);      // selectAll: typing replaces
        QVERIFY(bar.editWidget()->isVisible());
        QCOMPARE(bar.editWidget()->font(), bar.font());           // the chrome face
        // Geometry: starts at the base cell, at least kEditMinW wide, inside
        // the strip, on the cell row.
        const QRect r = bar.editRect();
        QCOMPARE(r, bar.editWidget()->geometry());
        QCOMPARE(r.left(), base.left());
        QVERIFY(r.width() >= AddressBar::kEditMinW);
        QVERIFY(bar.rect().contains(r));
        QCOMPARE(r.top(), AddressBar::kCellTop);
        QCOMPARE(r.height(), AddressBar::kCellH);
        // The bar is focusable only while the overlay is up.
        QCOMPARE(bar.focusPolicy(), Qt::StrongFocus);
        QCOMPARE(bar.window()->focusWidget(), bar.editWidget());
        // A second click while open just re-selects; still one overlay.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, base.center());
        QVERIFY(bar.isEditing());
        QCOMPARE(bar.findChildren<QLineEdit*>().size(), 1);
        // The interior is PanelSearchField's rule set, verbatim.
        // PanelSearchField's interior, then OUR padding: the overlay's insets
        // are the cell's (kBasePad in, kEditRightInset out) so the first glyph
        // lands on the column the cell painted it on.
        QVERIFY2(bar.editWidget()->styleSheet().startsWith(panelFieldInteriorQss(bar.theme())),
                 qPrintable(bar.editWidget()->styleSheet()));
        QVERIFY(bar.editWidget()->styleSheet().endsWith(QStringLiteral("QLineEdit { padding: 0px; }")));
        QVERIFY(bar.editWidget()->styleSheet().contains(QStringLiteral("border-radius: 0px")));
        QVERIFY(bar.editWidget()->styleSheet().contains(QStringLiteral("border: none")));
    }

    void testBaseCommitRebasesUndoably() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        const int undoBefore = m_doc->undoStack.count();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(bar->editText(), QStringLiteral("0x0"));
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x1000"));
        QCOMPARE(bar->editText(), QStringLiteral("0x1000"));
        // A literal parses: the live preview is the controller's evaluator.
        QVERIFY(bar->editTextValid());
        // A literal evaluates to itself, so there is nothing to preview:
        // `0x1000 = 0x1000` is the same number twice, appearing out of
        // nowhere the moment you click into a plain address.
        QVERIFY2(bar->editPreviewText().isEmpty(), qPrintable(bar->editPreviewText()));
        // A FORMULA does resolve to something else, and says so.
        bar->editWidget()->setText(QStringLiteral("0x1000+0x24"));
        QApplication::processEvents();
        QCOMPARE(bar->editPreviewText(), QStringLiteral("= 0x1024"));
        bar->editWidget()->setText(QStringLiteral("0x1000"));
        QApplication::processEvents();
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(commit.count(), 1);
        QCOMPARE(commit.at(0).at(0).toString(), QStringLiteral("0x1000"));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(m_doc->tree.baseAddress, 0x1000ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());       // bare literal
        QVERIFY(!bar->isEditing());                              // accepted → closed
        QVERIFY(!bar->editWidget()->isVisible());
        QCOMPARE(bar->state().baseAddress, 0x1000ULL);
        QCOMPARE(bar->baseDisplayText(), QStringLiteral("0x1000"));
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
        // Undo puts the old base back on the segment through the push.
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(bar->state().baseAddress, 0ULL);
    }

    void testBaseCommitRefusedKeepsOverlay() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const int undoBefore = m_doc->undoStack.count();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("garbage"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), undoBefore);          // nothing pushed
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(!hint.isEmpty());
        QVERIFY2(hint.last().at(0).toString().startsWith(QStringLiteral("Base: ")),
                 qPrintable(hint.last().at(0).toString()));
        QVERIFY(bar->isEditing());                               // refused → stays open
        QCOMPARE(bar->editText(), QStringLiteral("garbage"));
        QVERIFY(!bar->editTextValid());
        QVERIFY(!bar->editPreviewText().isEmpty());              // the parser's words
        // The seam under the field is markerError, one device row.
        const QImage img = grabOf(*bar);
        const QRect seam = seamRowUnder(img, bar->editRect());
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
        // Typing again answers the refusal: the seam follows the new text.
        QTest::keyClicks(bar->editWidget(), QStringLiteral("\b\b\b\b\b\b\b0x10"));
        QVERIFY(bar->editTextValid());
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    void testEditSeamFollowsValidity() {
        // While typing, the parser's verdict picks the seam colour under
        // the field, and a parsing formula shows what it resolves to.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        AddressBar::Callbacks cb;
        // A real evaluation: "0x10+8" is not "0x18", so there is something to
        // preview. (A literal previews nothing — see testBaseSeparator….)
        cb.evaluate = [](const QString& s) { return s == QStringLiteral("0x10+8") ? QStringLiteral("0x18") : QString(); };
        bar.setCallbacks(std::move(cb));
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        // At rest: the plain seam, no ring.
        QImage img = grabOf(bar);
        QRect seam = seamRowUnder(img, bar.itemRect(QStringLiteral("base")));
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        QCOMPARE(countColour(img, seam, t.markerError), 0);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        QTest::keyClicks(bar.editWidget(), QStringLiteral("0x10+8"));
        QVERIFY(bar.editTextValid());
        QCOMPARE(bar.editPreviewText(), QStringLiteral("= 0x18"));
        img = grabOf(bar);
        seam = seamRowUnder(img, bar.editRect());
        QVERIFY(countColour(img, seam, t.borderFocused) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.markerError), 0);
        // The preview picks up where the TEXT ends, in textMuted — the column
        // the resting " = 0x…" suffix occupies, not wherever the box happens
        // to stop.
        const int after0 = bar.editRect().right() + 1;
        const QRect after(after0, AddressBar::kCellTop,
                          bar.itemRect(QStringLiteral("recent")).left() - after0,
                          AddressBar::kCellH);
        QVERIFY(countColour(img, devRect(img, after), t.textMuted) > 0);
        // An unclosed deref does not parse: the ring turns markerError and
        // the parser's words replace the preview.
        QTest::keyClicks(bar.editWidget(), QStringLiteral("\b\b\b\b\b\b[0x100"));
        QCOMPARE(bar.editText(), QStringLiteral("[0x100"));
        QVERIFY(!bar.editTextValid());
        QVERIFY(!bar.editPreviewText().isEmpty());
        QVERIFY(!bar.editPreviewText().startsWith(AddressBar::baseSepText().trimmed()));
        img = grabOf(bar);
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
    }

    void testEscapeRestoresAndReturnsFocus() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const QString shownBefore = bar->baseDisplayText();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(bar->window()->focusWidget(), bar->editWidget());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x5555"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QApplication::processEvents();
        QVERIFY(!bar->isEditing());
        QCOMPARE(commit.count(), 0);
        QCOMPARE(bar->baseDisplayText(), shownBefore);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
        QVERIFY(!bar->editWidget()->isVisible());
    }

    void testFocusOutReverts() {
        // Click-away is a cancel, never a commit (Explorer / Goto-dialog
        // semantics): focus moving to a sibling field drops the typed text.
        QWidget win;
        auto* lay = new QVBoxLayout(&win);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* bar = new AddressBar(&win);
        QStringList commits;
        AddressBar::Callbacks cb;
        cb.onBaseCommit = [&](const QString& s) { commits << s; };
        bar->setCallbacks(std::move(cb));
        bar->setState(stateWith(twoLevel()));
        auto* sibling = new QLineEdit(&win);
        lay->addWidget(bar);
        lay->addWidget(sibling);
        win.resize(800, 80);
        win.show();
        QTest::qWait(30);
        QApplication::processEvents();
        const QString shownBefore = bar->baseDisplayText();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(win.focusWidget(), bar->editWidget());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x77"));
        sibling->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        QCOMPARE(win.focusWidget(), sibling);
        if (bar->isEditing()) {
            // Not the active window (the hidden desktop): the focus chain
            // moved but no FocusOut was delivered. Hand the overlay the
            // event the click would have carried.
            QFocusEvent out(QEvent::FocusOut, Qt::MouseFocusReason);
            QApplication::sendEvent(bar->editWidget(), &out);
        }
        QVERIFY(!bar->isEditing());
        QVERIFY(commits.isEmpty());
        QCOMPARE(bar->baseDisplayText(), shownBefore);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(win.focusWidget(), sibling);                    // no focus steal on a click-away
    }

    void testStatePushDuringEditIsDeferred() {
        // A live tick mid-edit must never stomp the typed text or move the
        // overlay: the state is stored, the relayout waits for the edit to
        // end, and then applies.
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        QTest::keyClicks(bar.editWidget(), QStringLiteral("0x42"));
        const QRect overlay = bar.editRect();
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());
        AddressBarState s = stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                        crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                        crumb(QStringLiteral("QWidget"), 2) });
        s.baseAddress = 0x1234; s.resolvedBase = 0x1234;
        const int n = bar.stateApplyCount();
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), n + 1);                  // stored…
        QCOMPARE(bar.state().crumbs.size(), 3);
        QCOMPARE(bar.editText(), QStringLiteral("0x42"));       // …the text untouched…
        QCOMPARE(bar.editRect(), overlay);                       // …the overlay where it was…
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());   // …the layout frozen.
        QVERIFY(bar.isEditing());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QVERIFY(!bar.itemRect(QStringLiteral("crumb:2")).isNull());  // applied now
        QCOMPARE(bar.baseDisplayText(), QStringLiteral("0x1234"));
        QCOMPARE(bar.segments().size(), 3);
    }

    void testStalePushDuringEditKeepsCellsSane() {
        // The frozen layout can outlive its crumbs: a push with FEWER crumbs
        // mid-edit leaves cells whose index is past the new trail. Hovering
        // one must not index out of range — empty tip — and the new state
        // lands after Escape.
        AddressBar bar;
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                 crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                 crumb(QStringLiteral("QWidget"), 2) }));
        showBar(bar, 900);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        const QRect stale = bar.itemRect(QStringLiteral("crumb:2"));
        QVERIFY(!stale.isNull());
        QVERIFY(stale.left() > bar.editRect().right());          // uncovered, hoverable
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor"), 0) }));
        QCOMPARE(bar.state().crumbs.size(), 1);
        hoverAt(bar, stale.center());
        QVERIFY2(bar.toolTip().isEmpty(), qPrintable(bar.toolTip()));
        hoverAt(bar, bar.itemRect(QStringLiteral("chev:2")).center());
        QVERIFY2(bar.toolTip().isEmpty(), qPrintable(bar.toolTip()));
        grabOf(bar);                                             // paints the stale cells: no crash
        // A click on a stale crumb cell is a no-op, not an out-of-range pick.
        // The press lands on the bar (StrongFocus while editing), so it is
        // also a click-away: the overlay loses focus and the base edit
        // reverts, which applies the lone-crumb state. On the thawed
        // layout the point sits on the empty stretch — the field's own
        // click-to-type cell — so the field re-opens there as a PATH edit
        // (the same field, the other scope), never as a pick.
        int picked = -1;
        AddressBar::Callbacks cb;
        cb.onCrumb = [&](int i) { picked = i; };
        bar.setCallbacks(std::move(cb));
        const QPoint at = bar.itemRect(QStringLiteral("crumb:1")).center();
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(picked, -1);
        QVERIFY(!bar.isBaseEditing());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());      // the lone-crumb state applied
        QVERIFY(bar.itemRect(QStringLiteral("crumb:1")).isNull());
        QCOMPARE(bar.segments(), QStringList{ QStringLiteral("RcxEditor") });
        QVERIFY(bar.isPathEditing());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        QCOMPARE(bar.itemIdAt(at), QStringLiteral("space"));            // what the press landed on
    }

    void testDownOpensPlacesMenuAndPickInserts() {
        // Down in the edit lists the Goto recents, the document's bookmarks
        // and the source's modules, fuzzy-filtered by the typed text; a
        // pick fills the overlay and does NOT commit. Driven before any
        // event pump: the hidden desktop closes a popup at the first one.
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0x7FF60000"));
        GotoAddressDialog::pushRecent(QStringLiteral("<RCTarget.exe>+0x40"));   // most recent first
        Bookmark b; b.name = QStringLiteral("spawn"); b.addressFormula = QStringLiteral("<RCTarget.exe>+0x100");
        m_doc->tree.bookmarks.append(b);
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QVERIFY(!visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu")));
        bar->editWidget()->clear();                              // empty text: unfiltered
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu"));
        QVERIFY(menu);
        QVERIFY(menu->isVisible());
        const QStringList texts = actionTexts(menu);
        QVERIFY2(texts.contains(QStringLiteral("<RCTarget.exe>+0x40")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("0x7FF60000")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("spawn  <RCTarget.exe>+0x100")), qPrintable(texts.join('|')));
        // Recent before bookmarks, most recent first.
        QVERIFY(texts.indexOf(QStringLiteral("<RCTarget.exe>+0x40")) < texts.indexOf(QStringLiteral("0x7FF60000")));
        QVERIFY(texts.indexOf(QStringLiteral("0x7FF60000")) < texts.indexOf(QStringLiteral("spawn  <RCTarget.exe>+0x100")));
        QVERIFY(!texts.contains(QStringLiteral("Clear recent")));       // the edit's menu inserts only
        QAction* pick = actionWithText(menu, QStringLiteral("<RCTarget.exe>+0x40"));
        QVERIFY(pick);
        pick->trigger();
        menu->hide();
        QTest::qWait(10);                                        // runs the menu's deleteLater
        QCOMPARE(bar->editText(), QStringLiteral("<RCTarget.exe>+0x40"));
        QVERIFY(bar->isEditing());                               // inserted, not committed
        QCOMPARE(commit.count(), 0);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        // Filtered: a substring of the bookmark's name leaves only it
        // (rcx::fuzzyScore is the strict matcher: contiguous substring or
        // word-start initials — "spa" finds "spawn", "spwn" finds nothing).
        bar->editWidget()->setText(QStringLiteral("spa"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu"));
        QVERIFY(menu);
        QStringList entries;
        for (QAction* a : menu->actions()) if (!a->isSeparator() && a->data().isValid()) entries << a->text();
        QCOMPARE(entries, QStringList{ QStringLiteral("spawn  <RCTarget.exe>+0x100") });
        menu->hide();
        QTest::qWait(10);
        // Nothing matches → no menu at all.
        bar->editWidget()->setText(QStringLiteral("zzzz"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QVERIFY(!visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu")));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    void testLineZeroHasNoAddressCell() {
        // Line 0 is the class header alone and the bar is the only base
        // edit. A click where the address cell used to be (right after the
        // chevron) starts nothing — not the bar's edit, not a Scintilla one
        // — and the bar's own base cell still edits.
        QApplication::processEvents();
        QsciScintilla* sci = m_editor->scintilla();
        const QString line = lineZeroText();
        QVERIFY2(!line.contains(QStringLiteral("0x")), qPrintable(line));
        QCOMPARE(line, buildCommandRowText(QStringLiteral("struct"), QStringLiteral("RcxEditor"), false));
        const ColumnSpan chev = commandRowChevronSpan(line);
        QVERIFY(chev.valid);
        QCOMPARE(commandRowRootTypeSpan(line).start, chev.end);
        // Character column → byte position (the row holds a ▸), then to
        // a viewport point on that glyph's row.
        const int col = chev.end + 1;
        const long pos = (long)sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)0)
                       + line.left(col).toUtf8().size();
        const int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
        const int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
        const int lh = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(!bar->isEditing());
        QTest::mouseClick(sci->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x + 2, y + lh / 2));
        QApplication::processEvents();
        QVERIFY2(!bar->isEditing(), "a line-0 click opened the bar's edit — there is no address cell");
        QVERIFY2(!m_editor->isEditing(), "a Scintilla inline edit started on the keyword");
        // The bar's base cell is the edit.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QApplication::processEvents();
        QVERIFY2(bar->isBaseEditing(), "the bar's base cell did not open the edit");
        QCOMPARE(bar->editText(), QStringLiteral("0x0"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QVERIFY(!m_editor->isEditing());
    }

    void testLineZeroStaysTheHeaderWhenTheBaseChanges() {
        // The base lives in the bar only: a rebase changes the bar's state
        // and leaves line 0 byte-identical — the row never showed it.
        QApplication::processEvents();
        const QString header = buildCommandRowText(QStringLiteral("struct"), QStringLiteral("RcxEditor"), false);
        QCOMPARE(lineZeroText(), header);
        AddressBar* bar = m_editor->addressBar();
        QCOMPARE(bar->state().baseAddress, 0ull);
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x1000")));
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0x1000ull);
        QCOMPARE(bar->state().baseAddress, 0x1000ull);
        QCOMPARE(bar->baseDisplayText(), QStringLiteral("0x1000"));
        QCOMPARE(lineZeroText(), header);
        QVERIFY(!lineZeroText().contains(QStringLiteral("1000")));
        // A formula base is the bar's too — line 0 does not elide, print or
        // parse it.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x10*2+0x4")));
        QApplication::processEvents();
        QCOMPARE(bar->state().baseFormula, QStringLiteral("0x10*2+0x4"));
        QCOMPARE(lineZeroText(), header);
    }

    void testRecentCellMenuGotoClearAndPickRebases() {
        // The recent cell: the same places, unfiltered, plus "Go to
        // address…" and "Clear recent"; a pick rebases through the
        // controller, undoably.
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0x7FF60000"));
        Bookmark b; b.name = QStringLiteral("spawn"); b.addressFormula = QStringLiteral("0x40");
        m_doc->tree.bookmarks.append(b);
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy pickSpy(m_editor, &RcxEditor::recentPickRequested);
        QSignalSpy gotoSpy(m_editor, &RcxEditor::gotoDialogRequested);
        const int undoBefore = m_doc->undoStack.count();
        const QRect recent = bar->itemRect(QStringLiteral("recent"));
        QVERIFY(!recent.isNull());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarRecentMenu"));
        QVERIFY(menu);
        const QStringList texts = actionTexts(menu);
        QVERIFY2(texts.contains(QStringLiteral("0x7FF60000")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("spawn  0x40")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("Go to address\u2026\tCtrl+G")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("Clear recent")), qPrintable(texts.join('|')));
        QVERIFY(actionWithText(menu, QStringLiteral("Clear recent"))->isEnabled());
        // The cell keeps its hover fill while its menu is up (grab before the pump).
        QImage img = bar->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(countColour(img, devRect(img, recent), t.hover) > 0, "recent cell not hover-filled while its menu is up");
        // "Go to address…" is a request the editor forwards; main.cpp wires
        // it in a later phase, so here only the signal is checked.
        actionWithText(menu, QStringLiteral("Go to address\u2026\tCtrl+G"))->trigger();
        QCOMPARE(gotoSpy.count(), 1);
        // A bookmark pick → recentPickRequested(formula) → rebaseTo.
        actionWithText(menu, QStringLiteral("spawn  0x40"))->trigger();
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QTest::qWait(10);
        QCOMPARE(pickSpy.count(), 1);
        QCOMPARE(pickSpy.at(0).at(0).toString(), QStringLiteral("0x40"));
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(bar->state().baseAddress, 0x40ULL);
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, recent), t.hover) < 16, "recent cell stayed filled after its menu hid");
        // Clear recent empties the shared list (restored by cleanup()).
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarRecentMenu"));
        QVERIFY(menu);
        actionWithText(menu, QStringLiteral("Clear recent"))->trigger();
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QTest::qWait(10);
        QVERIFY(GotoAddressDialog::loadRecent().isEmpty());
    }

    void testUpCellCollapsesOneLevel() {
        // Up = the parent crumb: one undoable collapse of the deepest hop,
        // the trail one shorter, the cell disabled again at the root.
        drillTwoLevels();
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(bar->state().canUp);
        QSignalSpy spy(m_editor, &RcxEditor::navUpRequested);
        const int undoBefore = m_doc->undoStack.count();
        QCOMPARE(segments().size(), 3);
        const QRect up = bar->itemRect(QStringLiteral("up"));
        QVERIFY(!up.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!collapsed(m_id.vptr));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"), QStringLiteral("QWidgetPrivate") }));
        QVERIFY(bar->state().canUp);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QVERIFY(m_ctrl->focusPath().isEmpty());
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QVERIFY(!bar->state().canUp);
        // Disabled now: the click is ignored.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 2);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        // Undo restores the trail through the push.
        m_doc->undoStack.undo();
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(!collapsed(m_id.parent));
    }

    void testStaleDotIsNeverTheAccent() {
        // vs.json shipped without focusGlow, and Theme::fromJson's default
        // for it is borderFocused — which there IS the accent — so a stale
        // source's dot painted purple. The theme data now names a warning
        // amber everywhere; pinned under both polarities.
        for (const QString& name : { QStringLiteral("tw"), QStringLiteral("vs") }) {
            const Theme theme = loadTheme(name);
            QVERIFY2(theme.background.isValid(), qPrintable(name + QStringLiteral(".json not found under RCX_SOURCE_DIR")));
            QVERIFY2(theme.focusGlow != theme.indHoverSpan, qPrintable(name));
            AddressBar bar;
            bar.applyTheme(theme);
            AddressBarState s = stateWith(twoLevel());
            s.liveness = liveness::Stale;
            bar.setState(s);
            showBar(bar, 800);
            const QImage img = grabOf(bar);
            QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), theme.indHoverSpan), 0);
            QVERIFY2(countColour(img, devRect(img, bar.livenessDotRect()), theme.focusGlow) > 0, qPrintable(name));
        }
    }

    void testChipTooltipWithoutSavedKind() {
        // A live attach with no saved-source entry (self-attach, MCP,
        // kernel) has no identifier: the tip is the bare name, not
        // kindLabelFor("")'s "Plugin".
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.sourceKindId.clear();
        bar.setState(s);
        showBar(bar, 800);
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        const QString tip = bar.toolTip();
        QVERIFY2(tip.startsWith(QStringLiteral("RCTarget.exe")), qPrintable(tip));
        QVERIFY2(!tip.contains(QStringLiteral("Plugin")), qPrintable(tip));
        QVERIFY(tip.contains(QStringLiteral("Live")));
    }

    // ── P4: sideways navigation + the path edit ──

    void testChevronMenuListsSiblingsWithCurrentChecked() {
        // chev:0 lists every drillable field of the root class in memory
        // order — expanded or not — with the trail's hop checked; a pick
        // asks for the switch.
        const uint64_t ptr2 = addSibling();
        drillOneLevel();                                        // RcxEditor.vptr › QWidgetPrivate
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy pick(m_editor, &RcxEditor::siblingPickRequested);
        const QRect chev = bar->itemRect(QStringLiteral("chev:0"));
        QVERIFY(!chev.isNull());
        QMenu* menu = openMenuOn(bar, QStringLiteral("chev:0"), QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        // The rows are the controller's entries (the model's list plus the
        // address each field leads to) through the one row-text rule.
        const QVector<SiblingEntry> sibs = m_ctrl->siblingsForCrumb(0);
        QStringList expected;
        for (const SiblingEntry& e : sibs) expected << AddressBar::siblingActionRowText(e);
        QCOMPARE(actionTexts(menu), expected);
        QCOMPARE(expected.size(), 2);                          // vptr, ptr2 — dptr (no refId) is no hop
        {
            // Same fields, same order as the pure model query.
            const QVector<SiblingEntry> model = siblingFieldsOf(m_doc->tree, m_id.editor, m_id.vptr);
            QCOMPARE(model.size(), sibs.size());
            for (int i = 0; i < model.size(); ++i) {
                QCOMPARE(sibs[i].id, model[i].id);
                QCOMPARE(sibs[i].current, model[i].current);
            }
        }
        QVERIFY2(expected[0].startsWith(QStringLiteral("vptr")), qPrintable(expected[0]));
        QVERIFY2(expected[0].contains(QStringLiteral("QWidgetPrivate")), qPrintable(expected[0]));
        QVERIFY2(expected[1].startsWith(QStringLiteral("ptr2")), qPrintable(expected[1]));
        QVERIFY(!expected.join('|').contains(QStringLiteral("dptr")));
        QVERIFY(menu->actions()[0]->isCheckable());
        QVERIFY(menu->actions()[0]->isChecked());
        QVERIFY(!menu->actions()[1]->isChecked());
        QVERIFY(menu->actions()[1]->isEnabled());              // collapsed siblings are not dimmed
        // The chevron keeps its hover fill while its menu is up (grab before the pump).
        QImage img = bar->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(countColour(img, devRect(img, chev), t.hover) > 0, "chevron not hover-filled while its menu is up");
        menu->actions()[1]->trigger();
        closeMenuOn(bar, QStringLiteral("chev:0"), menu);
        QCOMPARE(pick.count(), 1);
        QCOMPARE(pick.at(0).at(0).toInt(), 0);
        QCOMPARE(pick.at(0).at(1).toULongLong(), (qulonglong)ptr2);
        // …and the controller switched.
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ ptr2 }));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.ptr2"), QStringLiteral("QWidget") }));
        img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, chev), t.hover) < 16, "chevron stayed filled after its menu hid");
        // The trailing chevron (after the deepest crumb) lists the deepest
        // class's fields with nothing checked: drill further. QWidget has
        // none in this chain, so the menu says so and offers nothing.
        menu = openMenuOn(bar, QStringLiteral("chev:1"), QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 1);
        QVERIFY(!menu->actions()[0]->isEnabled());
        closeMenuOn(bar, QStringLiteral("chev:1"), menu);
    }

    void testSwitchSiblingIsOneUndoStep() {
        const uint64_t ptr2 = addSibling();
        drillOneLevel();
        const int undoBefore = m_doc->undoStack.count();
        m_ctrl->switchSibling(0, ptr2);
        QApplication::processEvents();
        QVERIFY(collapsed(m_id.vptr));
        QVERIFY(!collapsed(ptr2));
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ ptr2 }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);     // ONE entry for both halves
        QCOMPARE(m_doc->undoStack.text(undoBefore), QStringLiteral("Switch to ptr2"));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.ptr2"), QStringLiteral("QWidget") }));
        QCOMPARE(m_editor->addressBar()->state().trailPath, QStringLiteral("RcxEditor.ptr2"));
        // Choosing the hop already in the trail is a no-op: no entry.
        m_ctrl->switchSibling(0, ptr2);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ ptr2 }));
        // Undo reverses BOTH halves; redo re-applies both.
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(!collapsed(m_id.vptr));
        QVERIFY(collapsed(ptr2));
        m_doc->undoStack.redo();
        QApplication::processEvents();
        QVERIFY(collapsed(m_id.vptr));
        QVERIFY(!collapsed(ptr2));
        // A non-hop (no refId) and an unknown level are refused outright.
        const int n = m_doc->undoStack.count();
        m_ctrl->switchSibling(0, m_id.dptr);
        m_ctrl->switchSibling(7, m_id.vptr);
        QCOMPARE(m_doc->undoStack.count(), n);
    }

    void testSwitchSiblingAtTheEndDrillsFurther() {
        // level == focusPath.size() appends: the deepest chevron's pick.
        drillOneLevel();                                        // {vptr}; parent collapsed
        const int undoBefore = m_doc->undoStack.count();
        m_ctrl->switchSibling(1, m_id.parent);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(!collapsed(m_id.vptr));                          // nothing collapsed
        QVERIFY(!collapsed(m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate.parent"),
                                           QStringLiteral("QWidget") }));
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!collapsed(m_id.vptr));
    }

    // ── Renaming the class from the bar ──
    //
    // The deepest crumb names the class you are looking at, and until now it
    // was the one thing on the bar you could see but not touch: a click
    // scrolled the pane a little and nothing said the name was live. It is a
    // field now, committing the same undoable command line 0's class-name
    // edit pushes — on the class the CRUMB names, which is why a drilled
    // crumb renames the drilled class and not the view root.
    void testDeepestCrumbRenamesTheClassItNames() {
        drillTwoLevels();                       // RcxEditor.vptr › QWidgetPrivate.parent › QWidget
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QCOMPARE(segments().last(), QStringLiteral("QWidget"));
        const int undoBefore = m_doc->undoStack.count();
        const uint64_t rootBefore = m_ctrl->viewRootId();

        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier,
                          bar->itemRect(QStringLiteral("crumb:2")).center());
        QVERIFY(bar->isClassNameEditing());
        QCOMPARE(bar->editText(), QStringLiteral("QWidget"));
        QCOMPARE(bar->editWidget()->selectedText(), QStringLiteral("QWidget"));

        bar->editWidget()->setText(QStringLiteral("Renamed"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QVERIFY2(!bar->isEditing(), "a good rename left the overlay open");

        // The drilled class, not the view root, and one undo entry.
        const int idx = m_doc->tree.indexOfId(m_id.widget);
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].structTypeName, QStringLiteral("Renamed"));
        QCOMPARE(m_ctrl->viewRootId(), rootBefore);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(segments().last(), QStringLiteral("Renamed"));

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(m_id.widget)].structTypeName,
                 QStringLiteral("QWidget"));
        QCOMPARE(segments().last(), QStringLiteral("QWidget"));
    }

    // The root case — the one in build/issue.png: nothing drilled, so the
    // deepest crumb IS the root class, and line 0 (which renames the same
    // node) must agree afterwards.
    void testRootCrumbRenameAgreesWithLineZero() {
        AddressBar* bar = m_editor->addressBar();
        QCOMPARE(segments(), QStringList{ QStringLiteral("RcxEditor") });
        QVERIFY(lineZeroText().contains(QStringLiteral("RcxEditor")));

        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier,
                          bar->itemRect(QStringLiteral("crumb:0")).center());
        QVERIFY(bar->isClassNameEditing());
        bar->editWidget()->setText(QStringLiteral("PlayerBase"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QVERIFY(!bar->isEditing());
        QCOMPARE(segments(), QStringList{ QStringLiteral("PlayerBase") });
        QVERIFY2(lineZeroText().contains(QStringLiteral("PlayerBase")), qPrintable(lineZeroText()));
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(m_id.editor)].structTypeName,
                 QStringLiteral("PlayerBase"));
    }

    // A class needs a name — the one rule, the same one line 0 enforces. A
    // refusal keeps the overlay open with its reason where the preview goes,
    // exactly as a refused base formula does.
    void testRenameRefusesAnEmptyNameAndKeepsTheOverlay() {
        AddressBar* bar = m_editor->addressBar();
        const int undoBefore = m_doc->undoStack.count();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier,
                          bar->itemRect(QStringLiteral("crumb:0")).center());
        QVERIFY(bar->isClassNameEditing());

        bar->editWidget()->setText(QString());
        QApplication::processEvents();
        QVERIFY2(!bar->editTextValid(), "an empty class name reads as valid");
        QCOMPARE(bar->editPreviewText(), QStringLiteral("a class needs a name"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QVERIFY2(bar->isClassNameEditing(), "a refused rename closed the overlay");
        QCOMPARE(m_doc->undoStack.count(), undoBefore);

        // Typing answers the refusal, and re-committing the name it already
        // has is a no-op, not a refusal: the overlay closes, nothing is
        // pushed.
        bar->editWidget()->setText(QStringLiteral("RcxEditor"));
        QApplication::processEvents();
        QVERIFY(bar->editTextValid());
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QVERIFY(!bar->isEditing());
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(segments(), QStringList{ QStringLiteral("RcxEditor") });
    }

    // The rename field is sized like every other overlay on the bar: fitted
    // to what it holds, growing only.
    void testRenameFieldFitsTheClassName() {
        AddressBar* bar = m_editor->addressBar();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier,
                          bar->itemRect(QStringLiteral("crumb:0")).center());
        QVERIFY(bar->isClassNameEditing());
        const QRect crumbCell = bar->itemRect(QStringLiteral("crumb:0"));
        // The cell, to the pixel on the left and within the caret's gap on
        // the right — the two are paper against paper.
        QCOMPARE(bar->editRect().left(), crumbCell.left());
        QVERIFY(qAbs(bar->editRect().width() - crumbCell.width())
                    <= QFontMetrics(bar->font()).horizontalAdvance(QLatin1Char(' ')));
        QCOMPARE(bar->editRect().width(), fittedEditW(bar->editText(), bar->font()));
        const int opened = bar->editRect().width();
        bar->editWidget()->setText(QStringLiteral("AVeryLongClassNameIndeedYesReally"));
        QApplication::processEvents();
        QVERIFY(bar->editRect().width() > opened);
        bar->editWidget()->setText(QStringLiteral("Ab"));
        QApplication::processEvents();
        QVERIFY2(bar->editRect().width() > opened, "the rename field shrank under the caret");
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    // The crumb's context menu carries what the click no longer does, plus
    // the class chooser the bar's own flat dropdown used to be a worse copy
    // of — this is the route to it when line 0 has scrolled away.
    void testDeepestCrumbMenuOffersRenameAndTheRealChooser() {
        drillOneLevel();
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QSignalSpy chooser(m_editor, &RcxEditor::typeSelectorRequested);
        const QRect deep = bar->itemRect(QStringLiteral("crumb:1"));
        QVERIFY(!deep.isNull());
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, deep.center(),
                              bar->mapToGlobal(deep.center()));
        QApplication::sendEvent(bar, &ctx);
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarCellMenu"));
        QVERIFY(menu);
        const QStringList texts = actionTexts(menu);
        for (const QString& want : {QStringLiteral("Rename class\tF2"),
                                    QStringLiteral("Scroll to top"),
                                    QStringLiteral("Other classes…")})
            QVERIFY2(texts.contains(want), qPrintable(want + QStringLiteral(" | ") + texts.join(QLatin1Char('/'))));
        QAction* other = nullptr;
        for (QAction* a : menu->actions())
            if (a->text() == QStringLiteral("Other classes…")) other = a;
        QVERIFY(other);
        other->trigger();
        menu->hide();
        QTest::qWait(10);
        QCOMPARE(chooser.count(), 1);           // the filtered chooser, not a flat menu

        // An ANCESTOR crumb keeps the plain copy menu: its label is a path
        // segment, and there is no name there to rename.
        const QRect anc = bar->itemRect(QStringLiteral("crumb:0"));
        QContextMenuEvent ctx2(QContextMenuEvent::Mouse, anc.center(),
                               bar->mapToGlobal(anc.center()));
        QApplication::sendEvent(bar, &ctx2);
        QMenu* m2 = visibleMenu(bar, QStringLiteral("rcxAddressBarCellMenu"));
        QVERIFY(m2);
        QVERIFY(!actionTexts(m2).contains(QStringLiteral("Rename class\tF2")));
        m2->hide();
        QTest::qWait(10);
    }

    // ── The bar has no class dropdown ──
    //
    // `root.chev` used to sit between the source chip and the address: a
    // chevron whose menu was a flat, unsorted, uncapped, filter-less QMenu of
    // every root class. On a project with thousands of classes that is not a
    // chooser, it is a wall — and it was a second, worse copy of the type
    // chooser the line-0 chevron already opens (TypePopupMode::Root, with a
    // filter box). Its position made it worse: a chevron in the run of
    // separators, opening something that was neither the source before it nor
    // the address after it.
    //
    // So it is gone, and this pins that it stays gone AND that nothing went
    // with it: the chooser is still one click from line 0, and switching the
    // root still leaves exactly one history entry.
    void testTheBarHasNoClassDropdown() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QVERIFY2(bar->itemRect(QStringLiteral("root.chev")).isNull(),
                 "root.chev is laid out again");
        QVERIFY(!bar->traversalIds().contains(QStringLiteral("root.chev")));
        // No chevron cell survives between the chip and the address at any
        // width — the chip's own chevron is the last thing before the base.
        for (int w : {1200, 900, 600, 420, 300, AddressBar::kNarrowFloorW}) {
            bar->resize(w, AddressBar::kAddressBarHeight);
            QApplication::processEvents();
            const QRect chip = bar->itemRect(QStringLiteral("src.chev"));
            const QRect base = bar->itemRect(QStringLiteral("base"));
            if (chip.isNull() || base.isNull()) continue;
            for (const QString& id : bar->traversalIds()) {
                const QRect r = bar->itemRect(id);
                QVERIFY2(!(r.left() >= chip.right() && r.right() <= base.left()),
                         qPrintable(QStringLiteral("%1 sits between the chip and the base at %2")
                                        .arg(id).arg(w)));
            }
        }

        // Line 0 still opens the real chooser: the chevron span is there, and
        // it is what asks for it.
        QSignalSpy chooser(m_editor, &RcxEditor::typeSelectorRequested);
        const QString line = lineZeroText();
        QVERIFY2(commandRowChevronSpan(line).valid, qPrintable(line));

        // And the gesture that menu performed still records one history entry.
        const int before = m_ctrl->backEntries().size();
        m_ctrl->pickViewRoot(m_id.widget, m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.widget);
        QCOMPARE(m_ctrl->backEntries().size(), before + 1);
        QVERIFY(m_ctrl->focusPath().isEmpty());                 // a jump: fresh trail
        QCOMPARE(segments(), QStringList{ QStringLiteral("QWidget") });
        // Re-picking the root you are already on is not a gesture.
        m_ctrl->pickViewRoot(m_id.widget, m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->backEntries().size(), before + 1);
    }
    void testSpaceClickOpensPathEditOnTheTrail() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QCOMPARE(bar->state().trailPath, QStringLiteral("RcxEditor.vptr"));
        const QRect space = bar->itemRect(QStringLiteral("space"));
        QVERIFY(!space.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, space.center());
        QApplication::processEvents();
        QVERIFY(bar->isEditing());
        QVERIFY(bar->isPathEditing());
        QVERIFY(!bar->isBaseEditing());
        QCOMPARE(bar->editScope(), AddressBar::EditScope::Path);
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.vptr"));
        QCOMPARE(bar->editWidget()->selectedText(), QStringLiteral("RcxEditor.vptr"));
        QVERIFY(bar->editTextValid());
        QCOMPARE(bar->window()->focusWidget(), bar->editWidget());
        // Geometry: the field starts where the root crumb's label did (so
        // the text does not jump) and reaches short of the recent cell;
        // what it covers starts at the base's right edge.
        const QRect base = bar->itemRect(QStringLiteral("base"));
        const QRect recent = bar->itemRect(QStringLiteral("recent"));
        QCOMPARE(bar->editRect().left(), base.right() + 1);
        QVERIFY(bar->editRect().right() < recent.left());
        QVERIFY(bar->editRect().width() >= AddressBar::kEditMinW);
        QCOMPARE(bar->editRect(), bar->editWidget()->geometry());
        QCOMPARE(bar->editCoveredRect().left(), base.right() + 1);
        QCOMPARE(bar->editCoveredRect().right(), recent.left() - 1);
        // Enter on a deeper path → pathCommitRequested → the controller
        // navigates, the overlay closes, focus returns to the document.
        QSignalSpy commit(m_editor, &RcxEditor::pathCommitRequested);
        const int undoBefore = m_doc->undoStack.count();
        bar->editWidget()->setText(QStringLiteral("RcxEditor.vptr.parent"));
        QVERIFY(bar->editTextValid());
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(commit.count(), 1);
        QCOMPARE(commit.at(0).at(0).toString(), QStringLiteral("RcxEditor.vptr.parent"));
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(!collapsed(m_id.vptr));
        QVERIFY(!collapsed(m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QVERIFY(!bar->isEditing());
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate.parent"),
                                           QStringLiteral("QWidget") }));
        QCOMPARE(bar->state().trailPath, QStringLiteral("RcxEditor.vptr.parent"));
    }

    void testNavigateToDrillPathExpandsHopsUndoably() {
        QVERIFY(collapsed(m_id.vptr));
        QVERIFY(collapsed(m_id.parent));
        const int undoBefore = m_doc->undoStack.count();
        QString err;
        QVERIFY(m_ctrl->navigateToDrillPath(QStringLiteral("RcxEditor.vptr.parent"), &err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(!collapsed(m_id.vptr));
        QVERIFY(!collapsed(m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);     // both expands, ONE entry
        QCOMPARE(m_doc->undoStack.text(undoBefore), QStringLiteral("Navigate to path"));
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);            // same root: no view switch
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate.parent"),
                                           QStringLiteral("QWidget") }));
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(collapsed(m_id.vptr));
        QVERIFY(collapsed(m_id.parent));
        QCOMPARE(segments(), QStringList{ QStringLiteral("RcxEditor") });   // the trail reconciled away
        // A path into another root switches the view first, then drills.
        QVERIFY(m_ctrl->navigateToDrillPath(QStringLiteral("QWidgetPrivate.parent")));
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.parent }));
        QVERIFY(!collapsed(m_id.parent));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("QWidgetPrivate.parent"), QStringLiteral("QWidget") }));
        // Re-committing the trail as it stands changes nothing: no entry.
        const int n = m_doc->undoStack.count();
        QVERIFY(m_ctrl->navigateToDrillPath(QStringLiteral("QWidgetPrivate.parent")));
        QCOMPARE(m_doc->undoStack.count(), n);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.parent }));
    }

    void testNavigateToUnknownSegmentIsRefused() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const int undoBefore = m_doc->undoStack.count();
        const bool vptrWas = collapsed(m_id.vptr), parentWas = collapsed(m_id.parent);
        QString err;
        QVERIFY(!m_ctrl->navigateToDrillPath(QStringLiteral("RcxEditor.nope"), &err));
        QVERIFY2(err.contains(QStringLiteral("'nope'")), qPrintable(err));
        QCOMPARE(hint.count(), 1);
        QVERIFY2(hint.at(0).at(0).toString().contains(QStringLiteral("'nope'")),
                 qPrintable(hint.at(0).at(0).toString()));
        QCOMPARE(collapsed(m_id.vptr), vptrWas);                // tree untouched
        QCOMPARE(collapsed(m_id.parent), parentWas);
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        // Through the bar: the resolver already says no while typing (seam
        // markerError); Enter is refused and the overlay stays open.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("space")).center());
        QVERIFY(bar->isPathEditing());
        bar->editWidget()->setText(QStringLiteral("RcxEditor.nope"));
        QVERIFY(!bar->editTextValid());
        QVERIFY2(bar->editPreviewText().contains(QStringLiteral("'nope'")), qPrintable(bar->editPreviewText()));
        QImage img = grabOf(*bar);
        const QRect seam = seamRowUnder(img, bar->editRect());
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QVERIFY(bar->isPathEditing());                          // refused: still open
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.nope"));
        QCOMPARE(hint.count(), 2);
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(bar->window()->focusWidget(), bar->editWidget());
        img = grabOf(*bar);
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        // Fixing the text turns the seam back.
        bar->editWidget()->setText(QStringLiteral("RcxEditor.vptr"));
        QVERIFY(bar->editTextValid());
        img = grabOf(*bar);
        QVERIFY(countColour(img, seam, t.borderFocused) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.markerError), 0);
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
    }

    void testDownInPathEditCompletesTheLastSegment() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("space")).center());
        QVERIFY(bar->isPathEditing());
        // The partial last segment filters the container's drillable fields.
        bar->editWidget()->setText(QStringLiteral("RcxEditor.v"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarPathMenu"));
        QVERIFY(menu);
        QCOMPARE(actionData(menu), QStringList{ QStringLiteral("vptr") });   // dptr: no refId; leaf: a value
        QVERIFY2(actionTexts(menu)[0].startsWith(QStringLiteral("vptr")), qPrintable(actionTexts(menu)[0]));
        QVERIFY(!actionTexts(menu).join('|').contains(QStringLiteral("dptr")));
        menu->actions()[0]->trigger();
        menu->hide();
        QTest::qWait(10);
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.vptr"));       // the segment replaced
        QVERIFY(bar->isPathEditing());                                      // completed, not committed
        // A trailing dot: the fields of the class the path reaches, all of them.
        bar->editWidget()->setText(QStringLiteral("RcxEditor.vptr."));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarPathMenu"));
        QVERIFY(menu);
        QCOMPARE(actionData(menu), QStringList{ QStringLiteral("parent") });
        menu->actions()[0]->trigger();
        menu->hide();
        QTest::qWait(10);
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.vptr.parent"));
        // No dot yet: the roots, filtered by what is typed.
        bar->editWidget()->setText(QStringLiteral("QW"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarPathMenu"));
        QVERIFY(menu);
        QCOMPARE(actionData(menu), (QStringList{ QStringLiteral("QWidgetPrivate"), QStringLiteral("QWidget") }));
        menu->hide();
        QTest::qWait(10);
        // Nothing matches → no menu at all.
        bar->editWidget()->setText(QStringLiteral("RcxEditor.zzz"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QVERIFY(!visibleMenu(bar, QStringLiteral("rcxAddressBarPathMenu")));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));   // nothing committed
    }

    void testAltDAndCtrlLOpenThePathEdit() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QsciScintilla* sci = m_editor->scintilla();
        sci->setFocus();
        QCOMPARE(m_editor->window()->focusWidget(), sci);
        QTest::keyClick(sci, Qt::Key_D, Qt::AltModifier);
        QVERIFY2(bar->isPathEditing(), "Alt+D did not open the path edit");
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.vptr"));
        QCOMPARE(bar->window()->focusWidget(), bar->editWidget());
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QCOMPARE(m_editor->window()->focusWidget(), sci);
        QTest::keyClick(sci, Qt::Key_L, Qt::ControlModifier);
        QVERIFY2(bar->isPathEditing(), "Ctrl+L did not open the path edit");
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QCOMPARE(m_editor->window()->focusWidget(), sci);
        // Plain D and plain L stay the document's: no edit opens.
        QTest::keyClick(sci, Qt::Key_D);
        QTest::keyClick(sci, Qt::Key_L);
        QVERIFY(!bar->isEditing());
        // One overlay: Alt+D over an open base edit swaps the scope.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isBaseEditing());
        bar->beginPathEdit();
        QVERIFY(bar->isPathEditing());
        QCOMPARE(bar->editText(), QStringLiteral("RcxEditor.vptr"));
        QCOMPARE(bar->findChildren<QLineEdit*>().size(), 1);
        bar->beginBaseEdit();
        QVERIFY(bar->isBaseEditing());
        QCOMPARE(bar->editText(), QStringLiteral("0x0"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    void testDeepestCrumbScrollToTopMutatesNothing() {
        // A short pane, so the document is taller than the viewport and a
        // scroll can land the deepest hop's row at the top.
        m_splitter->resize(800, 120);
        QApplication::processEvents();
        drillTwoLevels();                                       // RcxEditor.vptr › QWidgetPrivate.parent › QWidget
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QsciScintilla* sci = m_editor->scintilla();
        QSignalSpy crumb(m_editor, &RcxEditor::crumbClicked);
        QSignalSpy pick(m_editor, &RcxEditor::siblingPickRequested);
        const int undoBefore = m_doc->undoStack.count();
        sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 0UL);
        QCOMPARE((int)sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE), 0);
        const QRect deep = bar->itemRect(QStringLiteral("crumb:2"));
        QVERIFY(!deep.isNull());
        // The scroll is the crumb menu's "Scroll to top" now — a click opens
        // the rename instead, and a non-mutating convenience is what a
        // context menu is for.
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, deep.center(),
                              bar->mapToGlobal(deep.center()));
        QApplication::sendEvent(bar, &ctx);
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarCellMenu"));
        QVERIFY2(menu, "the deepest crumb has no context menu");
        QAction* scroll = nullptr;
        for (QAction* a : menu->actions())
            if (a->text() == QStringLiteral("Scroll to top")) scroll = a;
        QVERIFY2(scroll, qPrintable(actionTexts(menu).join(QStringLiteral(" | "))));
        scroll->trigger();
        menu->hide();
        QTest::qWait(10);
        QApplication::processEvents();
        QCOMPARE(crumb.count(), 0);
        QCOMPARE(pick.count(), 0);
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(!collapsed(m_id.vptr));
        QVERIFY(!collapsed(m_id.parent));
        // The hop's row (the header of the class the deepest crumb names)
        // is the first visible line — as far up as Scintilla lets a short
        // document scroll (end-at-last-line: lines - linesOnScreen).
        const int hopLine = lineOf(m_id.parent);
        QVERIFY(hopLine > 0);
        const int lines = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
        const int onScreen = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
        const int maxFirst = qMax(0, lines - onScreen);
        QVERIFY2(maxFirst > 0, "pane not short enough to scroll — the assertion below would be vacuous");
        const int first = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        QCOMPARE(first, qMin(hopLine, maxFirst));
        QVERIFY(first > 0);
    }

    void testDoubleClickOnLineZeroStartsNoBaseEdit() {
        // The address cell left line 0: a double-click where it used to be
        // (the keyword column) opens neither the bar's edit nor a Scintilla
        // one. (Before the demotion the single press was redirected to the
        // bar and a double-click still slipped into the legacy edit.)
        QApplication::processEvents();
        QsciScintilla* sci = m_editor->scintilla();
        const QString line = lineZeroText();
        const ColumnSpan chev = commandRowChevronSpan(line);
        QVERIFY2(chev.valid, qPrintable(line));
        const int col = chev.end + 1;
        const long pos = (long)sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)0)
                       + line.left(col).toUtf8().size();
        const int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
        const int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
        const int lh = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(!bar->isEditing());
        // The double-click event itself, as the viewport's filter sees it.
        const QPoint at(x + 2, y + lh / 2);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, at, sci->viewport()->mapToGlobal(at),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(sci->viewport(), &dbl);
        QApplication::processEvents();
        QVERIFY2(!bar->isBaseEditing(), "a line-0 double-click opened the bar's edit — there is no address cell");
        QVERIFY2(!m_editor->isEditing(), "a Scintilla inline edit started on the keyword");
        QCOMPARE(lineZeroText(), line);
    }

    void testCoveredCellsGoQuietWhileEditing() {
        // Phase-3 follow-up: the crumbs under the overlay's paper painted
        // as nothing but still answered hover with a hand and a tooltip.
        // The paper is the PATH edit's now — that scope stands in for the whole
        // trail. A base edit stands in for one cell and leaves the trail alone.
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        const QRect c0 = bar.itemRect(QStringLiteral("crumb:0"));
        const QRect recent = bar.itemRect(QStringLiteral("recent"));
        QVERIFY(!c0.isNull());
        hoverAt(bar, c0.center());
        QCOMPARE(bar.cursor().shape(), Qt::PointingHandCursor);
        QVERIFY(bar.toolTip().contains(QStringLiteral("RcxEditor.vptr")));

        // Opening the BASE edit covers the base cell and nothing else: the
        // crumbs stay on screen, so they stay live.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isBaseEditing());
        QVERIFY2(!bar.editCoveredRect().contains(c0.center()),
                 "a base edit papered over the trail");
        QCOMPARE(bar.itemIdAt(c0.center()), QStringLiteral("crumb:0"));
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());

        // The PATH edit does cover them — it is standing in for them.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("space")).center());
        QVERIFY(bar.isPathEditing());
        QVERIFY(bar.editCoveredRect().contains(c0.center()));
        QCOMPARE(bar.editCoveredRect().right(), recent.left() - 1);
        hoverAt(bar, c0.center());
        QVERIFY(bar.cursor().shape() != Qt::PointingHandCursor);
        QVERIFY2(bar.toolTip().isEmpty(), qPrintable(bar.toolTip()));
        QVERIFY(bar.itemIdAt(c0.center()).isEmpty());
        // Past the covered range the recent cell is still a cell — it hits, it
        // takes the click, and (this scope showing no help) it still talks.
        QCOMPARE(bar.itemIdAt(recent.center()), QStringLiteral("recent"));
        hoverAt(bar, recent.center());
        QVERIFY(!bar.toolTip().isEmpty());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        hoverAt(bar, c0.center());
        QCOMPARE(bar.cursor().shape(), Qt::PointingHandCursor);   // back
        QCOMPARE(bar.itemIdAt(c0.center()), QStringLiteral("crumb:0"));
    }

    void testLineZeroHasNoSourceControlTheChipHas() {
        // The 'name'▾ that used to open the source chooser from line 0
        // moved to the bar's chip. The row now goes chevron → keyword: no
        // ▾, no address, the keyword starts where the chevron ends. A
        // click on that cell is nothing; only the chip asks for the popup.
        QApplication::processEvents();
        QsciScintilla* sci = m_editor->scintilla();
        const int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        QVERIFY(len > 0);
        QByteArray buf(len + 1, '\0');
        sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)0, (void*)buf.data());
        QString line = QString::fromUtf8(buf.constData(), len);
        while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
        QVERIFY2(!line.contains(QChar(0x25BE)), qPrintable(line));
        const ColumnSpan chev = commandRowChevronSpan(line);
        const ColumnSpan rt = commandRowRootTypeSpan(line);
        QVERIFY(chev.valid && rt.valid);
        QCOMPARE(rt.start, chev.end);
        QVERIFY(!line.contains(QStringLiteral("0x")));
        QCOMPARE(line, buildCommandRowText(QStringLiteral("struct"),
                                           QStringLiteral("RcxEditor"), false));
        QSignalSpy spy(m_editor, &RcxEditor::sourcePopupRequested);
        // The cell right after the chevron — where the source label was.
        const long pos = (long)sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)0)
                       + line.left(rt.start + 1).toUtf8().size();
        const int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
        const int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
        const int lh = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
        AddressBar* bar = m_editor->addressBar();
        QTest::mouseClick(sci->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x + 1, y + lh / 2));
        QApplication::processEvents();
        QCOMPARE(spy.count(), 0);
        QVERIFY2(!bar->isBaseEditing(), "the cell after the chevron is the keyword: nothing to edit");
        QVERIFY(!bar->isEditing());
        QVERIFY(!m_editor->isEditing());
        // The chip still opens the popup.
        const QRect src = bar->itemRect(QStringLiteral("src"));
        QVERIFY(!src.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, src.center());
        QCOMPARE(spy.count(), 1);
        auto* popup = m_editor->findChild<SourceChooserPopup*>();
        QVERIFY(popup);
        popup->hide();
        QApplication::processEvents();
    }

    void testPressOnVisibleCellAfterEditActsInOneClick() {
        // Phase-4 follow-up: the press that ended an edit was spent on
        // EVERY cell but base/space, so a click on "recent" while the base
        // edit was open closed the edit and did nothing else — P3 had it
        // close the edit AND open the places menu. Only a press on a cell
        // the overlay's paper covered is spent; recent stayed visible.
        QWidget win;
        auto* lay = new QVBoxLayout(&win);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* bar = new AddressBar(&win);
        int crumbClicks = 0;
        AddressBar::Callbacks cb;
        cb.onCrumb = [&](int) { ++crumbClicks; };
        bar->setCallbacks(std::move(cb));
        bar->setState(stateWith(twoLevel()));
        lay->addWidget(bar);
        win.resize(800, 80);
        win.show();
        QTest::qWait(30);
        QApplication::processEvents();
        const QRect recent = bar->itemRect(QStringLiteral("recent"));
        const QRect c0 = bar->itemRect(QStringLiteral("crumb:0"));
        QVERIFY(!recent.isNull() && !c0.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isBaseEditing());
        QVERIFY(!bar->editCoveredRect().contains(recent.center()));
        // The click-away as Qt delivers it: focus leaves the overlay
        // (MouseFocusReason) BEFORE the press reaches the bar.
        QFocusEvent out(QEvent::FocusOut, Qt::MouseFocusReason);
        QApplication::sendEvent(bar->editWidget(), &out);
        QVERIFY(!bar->isEditing());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarRecentMenu"));
        QVERIFY2(menu, "the press that closed the edit did not open the places menu");
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QTest::qWait(10);
        QVERIFY(!bar->isEditing());
        // A press on a crumb the paper covered is still spent — the PATH
        // edit's paper, since that is the scope that covers the trail.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("space")).center());
        QVERIFY(bar->isPathEditing());
        QVERIFY(bar->editCoveredRect().contains(c0.center()));
        QFocusEvent out2(QEvent::FocusOut, Qt::MouseFocusReason);
        QApplication::sendEvent(bar->editWidget(), &out2);
        QVERIFY(!bar->isEditing());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, c0.center());
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, c0.center());
        QCOMPARE(crumbClicks, 0);
        QVERIFY(!bar->isEditing());
        // ...and the next click on it, with no edit in the way, is a click.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, c0.center());
        QCOMPARE(crumbClicks, 1);
    }

    void testAltDAndCtrlLAreInertDuringAnInlineEdit() {
        // Phase-4 follow-up: mid-edit, Ctrl+L reached QScintilla's own
        // keymap (LineCut) and Alt+D fell through to the bar. Both are
        // swallowed while a Scintilla inline edit is open.
        AddressBar* bar = m_editor->addressBar();
        QsciScintilla* sci = m_editor->scintilla();
        sci->setFocus();
        const int ln = lineOf(m_id.leaf);
        QVERIFY(ln > 0);
        QVERIFY(m_editor->beginInlineEdit(EditTarget::Name, ln));
        QVERIFY(m_editor->isEditing());
        const QString before = sci->text(ln);
        QTest::keyClick(sci, Qt::Key_L, Qt::ControlModifier);
        QVERIFY2(m_editor->isEditing(), "Ctrl+L ended the inline edit");
        QCOMPARE(sci->text(ln), before);
        QVERIFY2(!bar->isEditing(), "Ctrl+L opened the bar's edit over an inline edit");
        QTest::keyClick(sci, Qt::Key_D, Qt::AltModifier);
        QVERIFY2(m_editor->isEditing(), "Alt+D ended the inline edit");
        QCOMPARE(sci->text(ln), before);
        QVERIFY2(!bar->isEditing(), "Alt+D opened the bar's edit over an inline edit");
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());
    }

    void testSwitchSiblingRefusesAHopOfAnotherClass() {
        // Phase-4 follow-up: the id has to be one of the hops the crumb's
        // menu lists (a field of the class at that level). `parent` is
        // QWidgetPrivate's, not RcxEditor's: refused at level 0, no undo
        // entry, the trail untouched — and accepted at its own level.
        drillOneLevel();
        const QVector<uint64_t> before = m_ctrl->focusPath();
        const int n = m_doc->undoStack.count();
        QVERIFY(!m_ctrl->switchSibling(0, m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), n);
        QCOMPARE(m_ctrl->focusPath(), before);
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!m_ctrl->switchSibling(0, m_id.dptr));              // not a hop at all
        QVERIFY(!m_ctrl->switchSibling(0, m_id.vptr));              // already there: no-op
        QCOMPARE(m_doc->undoStack.count(), n);
        QVERIFY(m_ctrl->switchSibling(1, m_id.parent));
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QCOMPARE(m_doc->undoStack.count(), n + 1);
    }

    // A show-all view (root 0) renders every root and borrows the first
    // root's name for its root crumb, so nothing is "current". The bar's
    // class menu used to be where that showed (no row checked); the state
    // is what carries it now, and picking a root out of a show-all view
    // still lands on that root.
    void testShowAllViewNamesNoCurrentRoot() {
        m_ctrl->setViewRootId(0);
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QCOMPARE(bar->state().viewRootId, 0ULL);
        QCOMPARE(segments(), QStringList{ QStringLiteral("RcxEditor") });   // borrowed, not current
        m_ctrl->pickViewRoot(m_id.editor, m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QCOMPARE(bar->state().viewRootId, m_id.editor);
    }
    void testFallbackThemeCarriesFocusGlow() {
        // Phase-3 follow-up: without it Theme::fromJson defaulted focusGlow
        // to borderFocused, and the stale dot wore the focus ring's colour.
        const Theme t = address_bar_detail::fallbackTheme();
        QCOMPARE(t.focusGlow, QColor(QStringLiteral("#E5A00D")));
        QVERIFY(t.focusGlow != t.borderFocused);
        QVERIFY(t.focusGlow != t.indHoverSpan);
    }

    // ── P5: history + keyboard ──
    //
    // The rule every slot below pins: one gesture = at most ONE undo entry
    // + ONE history entry; a Back / Forward = ZERO of both, except one
    // "Reopen path" macro when a hop on the restored trail was collapsed;
    // undo / redo never move history.
private:
    // F12 on the vptr row: the definition jump, recorded at its call site.
    void jumpToDefinitionOf(uint64_t hopId) {
        QsciScintilla* sci = m_editor->scintilla();
        sci->setCursorPosition(lineOf(hopId), 0);
        QTest::keyClick(sci, Qt::Key_F12);
        QApplication::processEvents();
    }
    // The device rows / columns the four edge fills pick for a logical rect
    // at the grab's scale — qFloor(edge ± 0.5), paintutil's rule, NOT a
    // rounded rect's edges: at 125 % the two disagree on the bottom row
    // (a 22-px cell at y 2 ends on device row 29, where qRound puts 30).
    struct RingEdges { int top, bottom, left, right; };
    static RingEdges ringEdgesOf(const QImage& img, const QRect& logical) {
        const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
        const QRectF dev(logical.left() * dpr, logical.top() * dpr,
                         logical.width() * dpr, logical.height() * dpr);
        return { qFloor(dev.top() + 0.5), qFloor(dev.bottom() - 0.5),
                 qFloor(dev.left() + 0.5), qFloor(dev.right() - 0.5) };
    }
    static QRect rowAcross(const RingEdges& e, int y) { return QRect(e.left, y, e.right - e.left + 1, 1); }
    static QRect colAlong(const RingEdges& e, int x)  { return QRect(x, e.top, 1, e.bottom - e.top + 1); }

private slots:
    void testF12ThenBackRestoresRootAndTrail() {
        drillOneLevel();                                        // RcxEditor.vptr › QWidgetPrivate
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(!m_ctrl->canGoBack());
        QVERIFY(!bar->state().canBack);
        QSignalSpy hist(m_ctrl, &RcxController::historyChanged);
        const int undoBefore = m_doc->undoStack.count();
        jumpToDefinitionOf(m_id.vptr);
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        QVERIFY(m_ctrl->focusPath().isEmpty());                 // a jump: fresh trail
        QVERIFY(m_ctrl->canGoBack());
        QVERIFY(!m_ctrl->canGoForward());
        QVERIFY(bar->state().canBack);
        QVERIFY(!bar->state().canForward);
        QCOMPARE(hist.count(), 1);
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        const NavEntry left = m_ctrl->backEntries().first();
        QCOMPARE(left.viewRootId, m_id.editor);
        QCOMPARE(left.focusPath, (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(left.baseAddress, 0ULL);
        QCOMPARE(left.label, QStringLiteral("RcxEditor.vptr  @ 0x0"));
        QCOMPARE(m_doc->undoStack.count(), undoBefore);         // F12 is not undoable
        // Back: root and trail restored; the hop was still open, so no
        // macro — and no history entry either (it is a restore).
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(!collapsed(m_id.vptr));
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QVERIFY(!m_ctrl->canGoBack());
        QVERIFY(m_ctrl->canGoForward());
        QVERIFY(!bar->state().canBack);
        QVERIFY(bar->state().canForward);
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"), QStringLiteral("QWidgetPrivate") }));
        QCOMPARE(m_ctrl->forwardEntries().size(), 1);
        QCOMPARE(m_ctrl->forwardEntries().first().viewRootId, m_id.priv);
        // Forward: the jump again.
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        QVERIFY(m_ctrl->focusPath().isEmpty());
        QVERIFY(m_ctrl->canGoBack());
        QVERIFY(!m_ctrl->canGoForward());
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        // The cells drive the same path through the editor's signals.
        QSignalSpy back(m_editor, &RcxEditor::navBackRequested);
        QSignalSpy fwd(m_editor, &RcxEditor::navForwardRequested);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("back")).center());
        QApplication::processEvents();
        QCOMPARE(back.count(), 1);
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("fwd")).center());
        QApplication::processEvents();
        QCOMPARE(fwd.count(), 1);
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        // A Back / Forward is never an undo entry.
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
    }

    void testRebaseThenBackIsRawAndUndoStillUndoesTheRebase() {
        // DECIDED: Back is a RAW restore. The exact sequence:
        //   rebaseTo("0x40")  base 0x40, undo +1 (cmd::ChangeBase), history +1
        //   goBack            base 0x0 written directly — undo count and
        //                     index UNCHANGED: nothing pushed, nothing undone
        //   undoStack.undo()  undoes the ORIGINAL rebase: base 0x0 (already
        //                     there, so the number does not move), formula
        //                     empty, index back one
        //   undoStack.redo()  base 0x40 again — and Forward still leads to
        //                     0x40: undo / redo never touched history
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        const int undoBefore = m_doc->undoStack.count();
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x40")));
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QVERIFY(m_ctrl->canGoBack());
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        QCOMPARE(m_ctrl->backEntries().last().baseAddress, 0ULL);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);     // raw: nothing pushed
        QCOMPARE(m_doc->undoStack.index(), undoBefore + 1);     // nothing undone either
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(m_editor->addressBar()->state().baseAddress, 0ULL);
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);                // the rebase's old base: already in place
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());
        QCOMPARE(m_doc->undoStack.index(), undoBefore);
        QVERIFY(m_ctrl->canGoForward());                        // history untouched by undo
        m_doc->undoStack.redo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(m_ctrl->forwardEntries().last().baseAddress, 0x40ULL);
        // A formula rebase: Back restores formula AND value, Forward brings
        // both back, still with nothing on the undo stack for either.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QApplication::processEvents();
        const int n = m_doc->undoStack.count();
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));
        QVERIFY(!m_ctrl->canGoForward());                       // a new gesture truncates forward
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());
        QCOMPARE(m_doc->undoStack.count(), n);
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));
        QCOMPARE(m_doc->undoStack.count(), n);
        // Rebasing to where you are records nothing.
        const int h = m_ctrl->backEntries().size();
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QCOMPARE(m_ctrl->backEntries().size(), h);
    }

    void testSourceSwitchThenBackRestoresTheSource() {
        // Two File sources on disk (a File switch is loadData: a real
        // path). Switching records the place left; Back switches back and
        // restores that place's base over the entry's own; a source whose
        // file is gone degrades to the current one with a hint.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto writeFile = [&](const QString& name, char fill) {
            const QString p = dir.filePath(name);
            QFile f(p);
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(64, fill));
            f.close();
            return p;
        };
        const QString a = writeFile(QStringLiteral("a.bin"), 'A');
        const QString b = writeFile(QStringLiteral("b.bin"), 'B');
        SavedSourceEntry ea; ea.kind = QStringLiteral("File"); ea.displayName = QStringLiteral("a.bin"); ea.filePath = a;
        SavedSourceEntry eb; eb.kind = QStringLiteral("File"); eb.displayName = QStringLiteral("b.bin"); eb.filePath = b;
        eb.baseAddress = 0x100;
        m_ctrl->copySavedSources({ ea, eb }, 0);
        m_doc->loadData(a);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 0);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("a.bin"));
        const int histBefore = m_ctrl->backEntries().size();
        m_ctrl->switchSource(1);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("b.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0x100ULL);            // the entry's base came with it
        QCOMPARE(m_ctrl->backEntries().size(), histBefore + 1);
        QCOMPARE(m_ctrl->backEntries().last().activeSourceIdx, 0);
        QCOMPARE(m_editor->addressBar()->state().sourceName, QStringLiteral("b.bin"));
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 0);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("a.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QCOMPARE(m_ctrl->backEntries().size(), histBefore);     // a restore records nothing
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(m_editor->addressBar()->state().sourceName, QStringLiteral("a.bin"));
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("b.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0x100ULL);
        // Degrade: a.bin vanishes; Back keeps b.bin, says so, and still
        // restores the rest of the place (the base).
        QVERIFY(QFile::remove(a));
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("b.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(hint.count() >= 1);
        QVERIFY2(hint.at(0).at(0).toString().contains(QStringLiteral("a.bin")),
                 qPrintable(hint.at(0).at(0).toString()));
    }

    void testCrumbClickRecordsOneEntryAndBackReopensInOneMacro() {
        drillTwoLevels();                                       // [vptr, parent]
        const int undoBefore = m_doc->undoStack.count();
        const int histBefore = m_ctrl->backEntries().size();
        m_ctrl->collapseToFocus(1);                             // the crumb click: collapse below QWidgetPrivate
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(collapsed(m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);     // one undo entry
        QCOMPARE(m_ctrl->backEntries().size(), histBefore + 1); // one history entry
        QCOMPARE(m_ctrl->backEntries().last().focusPath, (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        // A click past the end moves nothing and records nothing.
        m_ctrl->collapseToFocus(5);
        QCOMPARE(m_ctrl->backEntries().size(), histBefore + 1);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        // Back: `parent` is collapsed now — reopened through ONE macro, the
        // trail restored; still no history entry.
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(!collapsed(m_id.parent));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QCOMPARE(m_doc->undoStack.text(undoBefore + 1), QStringLiteral("Reopen path"));
        QCOMPARE(m_ctrl->backEntries().size(), histBefore);
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate.parent"),
                                           QStringLiteral("QWidget") }));
        // Undoing the reopen collapses the hop again (the trail trims with
        // it) and leaves history exactly where it was.
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(collapsed(m_id.parent));
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(m_ctrl->backEntries().size(), histBefore);
        m_doc->undoStack.redo();
        QApplication::processEvents();
        QVERIFY(!collapsed(m_id.parent));
        // Up is the same gesture as the crumb click: one of each.
        const int n = m_doc->undoStack.count(), hn = m_ctrl->backEntries().size();
        m_ctrl->goUp(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), n + 1);
        QCOMPARE(m_ctrl->backEntries().size(), hn + 1);
    }

    void testRemovedSourceIsForgottenByHistory() {
        // NavEntry.activeSourceIdx is a raw index into the saved-source
        // list: removing a source shifts the later ones and Clear All drops
        // them all — the history follows (NavHistory::forgetSource /
        // forgetAllSources), or Back silently switched to whichever source
        // slid into the recorded slot.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto writeFile = [&](const QString& name, char fill) {
            const QString p = dir.filePath(name);
            QFile f(p);
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(64, fill));
            f.close();
            return p;
        };
        const QString a = writeFile(QStringLiteral("a.bin"), 'A');
        const QString b = writeFile(QStringLiteral("b.bin"), 'B');
        const QString c = writeFile(QStringLiteral("c.bin"), 'C');
        SavedSourceEntry ea; ea.kind = QStringLiteral("File"); ea.displayName = QStringLiteral("a.bin"); ea.filePath = a;
        SavedSourceEntry eb; eb.kind = QStringLiteral("File"); eb.displayName = QStringLiteral("b.bin"); eb.filePath = b;
        SavedSourceEntry ec; ec.kind = QStringLiteral("File"); ec.displayName = QStringLiteral("c.bin"); ec.filePath = c;
        ec.baseAddress = 0x200;
        m_ctrl->copySavedSources({ ea, eb, ec }, 1);
        m_doc->loadData(b);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);
        m_ctrl->switchSource(2);                                // records the place left: b.bin, index 1
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 2);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("c.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0x200ULL);
        QCOMPARE(m_ctrl->backEntries().last().activeSourceIdx, 1);
        // Remove a source BEFORE the recorded one: the entry follows b.bin
        // down to index 0, and Back lands on the SAME source.
        m_ctrl->removeSavedSource(0);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);               // c.bin, shifted
        QCOMPARE(m_ctrl->backEntries().last().activeSourceIdx, 0);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 0);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("b.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(m_ctrl->canGoForward());
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 1);
        QCOMPARE(m_doc->provider->name(), QStringLiteral("c.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0x200ULL);
        // Remove the recorded source itself: Back keeps the current one,
        // says so, and still restores the rest of the place (the base).
        QCOMPARE(m_ctrl->backEntries().last().activeSourceIdx, 0);   // b.bin
        m_ctrl->removeSavedSource(0);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 0);               // c.bin again, shifted
        QCOMPARE(m_ctrl->backEntries().last().activeSourceIdx, kNavSourceRemoved);
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const Provider* prov = m_doc->provider.get();
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), 0);
        QCOMPARE(m_doc->provider.get(), prov);                  // no source switch
        QCOMPARE(m_doc->provider->name(), QStringLiteral("c.bin"));
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);                // the place's base came back
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QVERIFY(hint.count() >= 1);
        QVERIFY2(hint.at(0).at(0).toString().contains(QStringLiteral("removed")),
                 qPrintable(hint.at(0).at(0).toString()));
        // Clear All forgets every recorded source the same way: Forward
        // restores c.bin's place (base 0x200) under no source at all.
        m_ctrl->clearSources();
        QApplication::processEvents();
        QCOMPARE(m_ctrl->forwardEntries().last().activeSourceIdx, kNavSourceRemoved);
        hint.clear();
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->activeSourceIndex(), -1);
        QVERIFY(m_doc->provider->name().isEmpty());
        QCOMPARE(m_doc->tree.baseAddress, 0x200ULL);
        QVERIFY(hint.count() >= 1);
    }

    void testLineZeroRootPickRecordsHistory() {
        // The command row's class chooser (TypePopupMode::Root) is a root
        // pick like the bar's root.chev: the place left is recorded, so
        // Back returns; re-picking the current root records nothing.
        QVERIFY(m_ctrl->backEntries().isEmpty());
        TypeEntry pick;
        pick.entryKind   = TypeEntry::Composite;
        pick.category    = TypeEntry::CatType;
        pick.structId    = m_id.widget;
        pick.displayName = QStringLiteral("QWidget");
        m_ctrl->applyTypePopupResult(TypePopupMode::Root, -1, pick, pick.displayName);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.widget);
        QVERIFY(m_ctrl->canGoBack());
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        QCOMPARE(m_ctrl->backEntries().last().viewRootId, m_id.editor);
        QVERIFY(m_editor->addressBar()->state().canBack);
        m_ctrl->applyTypePopupResult(TypePopupMode::Root, -1, pick, pick.displayName);   // same root: nothing
        QApplication::processEvents();
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QVERIFY(m_ctrl->canGoForward());
    }

    void testStaleEntriesAreSkipped() {
        // The place an entry names must still exist: a deleted root leaves
        // a stale entry that Back steps over to the previous valid one, and
        // a stale Forward with nothing behind it does nothing at all.
        jumpToDefinitionOf(m_id.vptr);                          // editor → priv   (A: editor)
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        m_ctrl->pickViewRoot(m_id.widget, m_editor);            // priv → widget   (B: priv)
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.widget);
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        m_ctrl->deleteRootStruct(m_id.priv);                    // B's root is gone
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.widget);
        QCOMPARE(m_ctrl->backEntries().size(), 1);              // B is left out of the listing...
        QCOMPARE(m_ctrl->backEntries().last().viewRootId, m_id.editor);
        QVERIFY(m_ctrl->canGoBack());                           // ...A is restorable
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);            // B skipped, A restored
        QVERIFY(!m_ctrl->canGoBack());
        QVERIFY(m_ctrl->backEntries().isEmpty());               // the stale entry was discarded
        QVERIFY(m_ctrl->canGoForward());
        QCOMPARE(m_ctrl->forwardEntries().size(), 1);
        QCOMPARE(m_ctrl->forwardEntries().first().viewRootId, m_id.widget);
        // Now the Forward place goes: the flag says no, the cell is off,
        // and Forward is a no-op.
        m_ctrl->deleteRootStruct(m_id.widget);
        QApplication::processEvents();
        QVERIFY(!m_ctrl->canGoForward());
        QVERIFY(!m_editor->addressBar()->state().canForward);
        m_ctrl->goForward(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QVERIFY(m_ctrl->forwardEntries().isEmpty());
    }

    void testHistoryMenuRowsSkipStaleEntries() {
        // A stale entry between two live ones: the menu lists only the live
        // ones, nearest first, and a row's step count is what its pick
        // walks — jumpToHistory discards the stale entry on the way, so an
        // unfiltered listing put the far row one step short of its place
        // (a pick beyond the stale row landed one place further).
        AddressBar* bar = m_editor->addressBar();
        jumpToDefinitionOf(m_id.vptr);                          // A: RcxEditor @ 0x0       (now: priv)
        m_ctrl->pickViewRoot(m_id.widget, m_editor);            // B: QWidgetPrivate @ 0x0  (now: widget)
        QApplication::processEvents();
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x40")));      // C: QWidget @ 0x0         (now: widget @ 0x40)
        QApplication::processEvents();
        QCOMPARE(m_ctrl->backEntries().size(), 3);
        m_ctrl->deleteRootStruct(m_id.priv);                    // B's root is gone
        QApplication::processEvents();
        QCOMPARE(m_ctrl->viewRootId(), m_id.widget);
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        QMenu* menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        // …under the current place's checked row (see
        // testHistoryMenuMarksTheCurrentPlace).
        QCOMPARE(actionTexts(menu), (QStringList{ QStringLiteral("QWidget  @ 0x0"),
                                                  QStringLiteral("RcxEditor  @ 0x0"),
                                                  QStringLiteral("QWidget  @ 0x40") }));
        QCOMPARE(menu->actions()[0]->data().toInt(), -1);
        QCOMPARE(menu->actions()[1]->data().toInt(), -2);
        QVERIFY(!menu->actions()[2]->isEnabled());
        menu->actions()[1]->trigger();
        closeMenuOn(bar, QStringLiteral("hist"), menu);
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);            // the row named A and landed on A
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(m_ctrl->backEntries().isEmpty());
        QCOMPARE(m_ctrl->forwardEntries().size(), 2);           // C and the place left; B discarded
    }

    void testOneGestureOneEntry() {
        drillOneLevel();                                        // a click-built trail: not a gesture
        QVERIFY(m_ctrl->backEntries().isEmpty());
        const uint64_t ptr2 = addSibling();
        const int undoBefore = m_doc->undoStack.count();
        QVERIFY(m_ctrl->switchSibling(0, ptr2));
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        QCOMPARE(m_ctrl->backEntries().last().focusPath, (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(m_ctrl->navigateToDrillPath(QStringLiteral("RcxEditor.vptr.parent")));
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        QCOMPARE(m_ctrl->backEntries().last().focusPath, (QVector<uint64_t>{ ptr2 }));
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        // The trail re-committed as it stands: no entry of either kind.
        QVERIFY(m_ctrl->navigateToDrillPath(QStringLiteral("RcxEditor.vptr.parent")));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        // A refused switch (the hop already there): nothing.
        QVERIFY(!m_ctrl->switchSibling(0, m_id.vptr));
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        // Back twice: the cursor moves, the undo stack does not — every
        // hop on both restored trails is still open.
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ ptr2 }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QCOMPARE(m_ctrl->backEntries().size(), 1);
        QCOMPARE(m_ctrl->forwardEntries().size(), 1);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QVERIFY(m_ctrl->backEntries().isEmpty());
        QCOMPARE(m_ctrl->forwardEntries().size(), 2);
    }

    void testBackCellFollowsTheControllerFlags() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy back(m_editor, &RcxEditor::navBackRequested);
        const QRect cell = bar->itemRect(QStringLiteral("back"));
        QVERIFY(!cell.isNull());
        QVERIFY(!bar->state().canBack);
        const double inkOff = inkAmount(grabOf(*bar), cell, editorPaperColor(t));
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, cell.center());
        QApplication::processEvents();
        QCOMPARE(back.count(), 0);                              // disabled: the click is ignored
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        jumpToDefinitionOf(m_id.vptr);
        QVERIFY(bar->state().canBack);
        const double inkOn = inkAmount(grabOf(*bar), cell, editorPaperColor(t));
        QVERIFY2(inkOn > 3.0, qPrintable(QStringLiteral("enabled Back paints no ink (%1)").arg(inkOn)));
        QVERIFY2(inkOff < inkOn * 0.6,
                 qPrintable(QStringLiteral("disabled Back didn't dim: on=%1 off=%2").arg(inkOn).arg(inkOff)));
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, cell.center());
        QApplication::processEvents();
        QCOMPARE(back.count(), 1);
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        // Used up: off again, and the accent is nowhere on the strip.
        QVERIFY(!bar->state().canBack);
        QImage img = grabOf(*bar);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
    }

    void testHistoryMenuListsEntriesAndJumps() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        jumpToDefinitionOf(m_id.vptr);                          // A: RcxEditor.vptr @ 0x0
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x40")));      // B: QWidgetPrivate @ 0x0
        QApplication::processEvents();
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        QSignalSpy jump(m_editor, &RcxEditor::historyJumpRequested);
        // Nearest first, then the current place (checked, disabled — see
        // testHistoryMenuMarksTheCurrentPlace): a row's data is the step
        // count its pick asks for.
        QMenu* menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(actionTexts(menu), (QStringList{ QStringLiteral("QWidgetPrivate  @ 0x0"),
                                                  QStringLiteral("RcxEditor.vptr  @ 0x0"),
                                                  QStringLiteral("QWidgetPrivate  @ 0x40") }));
        QCOMPARE(menu->actions()[0]->data().toInt(), -1);
        QCOMPARE(menu->actions()[1]->data().toInt(), -2);
        QVERIFY(!menu->actions()[2]->isEnabled());              // you are here
        menu->actions()[1]->trigger();
        closeMenuOn(bar, QStringLiteral("hist"), menu);
        QCOMPARE(jump.count(), 1);
        QCOMPARE(jump.at(0).at(0).toInt(), -2);
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        // Stepped, not indexed: both places walked over are on Forward now,
        // nearest first under the current row, and the stack behind is empty.
        QVERIFY(m_ctrl->backEntries().isEmpty());
        QCOMPARE(m_ctrl->forwardEntries().size(), 2);
        menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(actionTexts(menu), (QStringList{ QStringLiteral("RcxEditor.vptr  @ 0x0"),
                                                  QStringLiteral("QWidgetPrivate  @ 0x0"),
                                                  QStringLiteral("QWidgetPrivate  @ 0x40") }));
        QVERIFY(!menu->actions()[0]->isEnabled());
        QCOMPARE(menu->actions()[1]->data().toInt(), 1);
        QCOMPARE(menu->actions()[2]->data().toInt(), 2);
        menu->actions()[2]->trigger();
        closeMenuOn(bar, QStringLiteral("hist"), menu);
        QCOMPARE(jump.at(1).at(0).toInt(), 2);
        QCOMPARE(m_ctrl->viewRootId(), m_id.priv);
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        QVERIFY(m_ctrl->forwardEntries().isEmpty());
        // Both stacks populated: Back rows, the current row, Forward rows —
        // no separator, the checked row is the divider.
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 3);
        QCOMPARE(menu->actions()[0]->text(), QStringLiteral("RcxEditor.vptr  @ 0x0"));
        QVERIFY(!menu->actions()[1]->isSeparator());
        QCOMPARE(menu->actions()[1]->text(), QStringLiteral("QWidgetPrivate  @ 0x0"));
        QVERIFY(menu->actions()[1]->isChecked());
        QCOMPARE(menu->actions()[2]->text(), QStringLiteral("QWidgetPrivate  @ 0x40"));
        closeMenuOn(bar, QStringLiteral("hist"), menu);
        // The undo stack saw exactly the one rebase through all of it.
        QCOMPARE(m_doc->undoStack.count(), 1);
    }

    void testHoldOrRightClickOnBackOpensTheHistory() {
        // The narrow-width fallback: press-and-hold (DelayedPopup) or a
        // right-click on Back opens the same list; a quick click is Back.
        AddressBar bar;
        int backs = 0;
        AddressBar::Callbacks cb;
        cb.onBack = [&] { ++backs; };
        NavEntry e; e.label = QStringLiteral("RcxEditor  @ 0x0");
        cb.backEntries = [e] { return QVector<NavEntry>{ e }; };
        bar.setCallbacks(std::move(cb));
        AddressBarState s = stateWith(twoLevel());
        s.canBack = true;
        bar.setState(s);
        showBar(bar, 800);
        bar.setHoldDelayMs(40);
        const QRect back = bar.itemRect(QStringLiteral("back"));
        QVERIFY(!back.isNull());
        QCOMPARE(bar.historyMenuOpenCount(), 0);
        // Hold: the menu opens while the button is still down, and the
        // release that follows is the hold's end, not a click.
        QTest::mousePress(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QTest::qWait(bar.holdDelayMs() * 4);
        QCOMPARE(bar.historyMenuOpenCount(), 1);
        QTest::mouseRelease(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QApplication::processEvents();
        QCOMPARE(backs, 0);
        // Whether the platform closed the popup at the pump or left it up,
        // it is done with: the later steps look for menus of their own.
        auto closeHistoryMenus = [&] {
            for (QMenu* m : bar.findChildren<QMenu*>(QStringLiteral("rcxAddressBarHistoryMenu")))
                m->hide();
            QTest::qWait(10);
        };
        closeHistoryMenus();
        // Quick click: Back, no menu.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QTest::qWait(bar.holdDelayMs() * 4);
        QCOMPARE(backs, 1);
        QCOMPARE(bar.historyMenuOpenCount(), 1);
        // Right-click: the list, with the entry, inspected before any pump.
        QContextMenuEvent ctx(QContextMenuEvent::Mouse, back.center(), bar.mapToGlobal(back.center()));
        QApplication::sendEvent(&bar, &ctx);
        QMenu* menu = visibleMenu(&bar, QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(actionTexts(menu), QStringList{ QStringLiteral("RcxEditor  @ 0x0") });
        QCOMPARE(bar.historyMenuOpenCount(), 2);
        closeHistoryMenus();
        QCOMPARE(backs, 1);
        // Dragging off before the delay is not a hold.
        QTest::mousePress(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&bar, &leave);
        QTest::qWait(bar.holdDelayMs() * 4);
        QCOMPARE(bar.historyMenuOpenCount(), 2);
        // ...and the release off the strip is not a click either.
        {
            const QPointF off(-5, -5);
            QMouseEvent rel(QEvent::MouseButtonRelease, off, off, QPointF(bar.mapToGlobal(off.toPoint())),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&bar, &rel);
        }
        QCOMPARE(backs, 1);
        // With no history at all the right-click opens nothing.
        s.canBack = false;
        bar.setState(s);
        QApplication::sendEvent(&bar, &ctx);
        QVERIFY(!visibleMenu(&bar, QStringLiteral("rcxAddressBarHistoryMenu")));
        QCOMPARE(bar.historyMenuOpenCount(), 2);
    }

    void testKeyboardModeWalksEnabledCells() {
        AddressBar bar;
        int backs = 0, focusReturns = 0;
        AddressBar::Callbacks cb;
        cb.onBack = [&] { ++backs; };
        cb.onFocusReturn = [&] { ++focusReturns; };
        cb.backEntries = [] { NavEntry e; e.label = QStringLiteral("x"); return QVector<NavEntry>{ e }; };
        bar.setCallbacks(std::move(cb));
        AddressBarState s = stateWith(twoLevel());
        s.canBack = true;                                       // fwd and up stay disabled
        bar.setState(s);
        showBar(bar, 800);
        QVERIFY(!bar.inKeyboardMode());
        QVERIFY(bar.focusId().isEmpty());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        bar.enterKeyboardMode();
        QVERIFY(bar.inKeyboardMode());
        QCOMPARE(bar.focusPolicy(), Qt::StrongFocus);
        QCOMPARE(bar.focusId(), QStringLiteral("crumb:1"));     // the deepest crumb: you are here
        QCOMPARE(focusReturns, 0);                              // focus arriving is the point
        // The walk: enabled cells only, no stretch, one stop for the chip.
        const QStringList ids = bar.traversalIds();
        QCOMPARE(ids, (QStringList{ QStringLiteral("back"), QStringLiteral("hist"), QStringLiteral("src"),
                                    QStringLiteral("base"),
                                    QStringLiteral("crumb:0"), QStringLiteral("chev:0"),
                                    QStringLiteral("crumb:1"), QStringLiteral("chev:1"),
                                    QStringLiteral("recent") }));
        QTest::keyClick(&bar, Qt::Key_Right);
        QCOMPARE(bar.focusId(), QStringLiteral("chev:1"));
        QTest::keyClick(&bar, Qt::Key_Right);
        QCOMPARE(bar.focusId(), QStringLiteral("recent"));
        QTest::keyClick(&bar, Qt::Key_Right);                   // clamped, not wrapped
        QCOMPARE(bar.focusId(), QStringLiteral("recent"));
        QTest::keyClick(&bar, Qt::Key_Left);
        QCOMPARE(bar.focusId(), QStringLiteral("chev:1"));
        QTest::keyClick(&bar, Qt::Key_Home);
        QCOMPARE(bar.focusId(), QStringLiteral("back"));
        QTest::keyClick(&bar, Qt::Key_Tab);                     // Tab walks too (the focus chain is claimed)
        QCOMPARE(bar.focusId(), QStringLiteral("hist"));
        QTest::keyClick(&bar, Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(bar.focusId(), QStringLiteral("back"));
        QVERIFY(bar.inKeyboardMode());
        QTest::keyClick(&bar, Qt::Key_End);
        QCOMPARE(bar.focusId(), QStringLiteral("recent"));
        // Enter on Back = the click path: the callback fires.
        QTest::keyClick(&bar, Qt::Key_Home);
        QTest::keyClick(&bar, Qt::Key_Return);
        QCOMPARE(backs, 1);
        QVERIFY(bar.inKeyboardMode());                          // a synthesised press does not end the mode
        QTest::keyClick(&bar, Qt::Key_Space);
        QCOMPARE(backs, 2);
        // Down on a chevron opens its menu (no tree queries here: the
        // "nothing to open" row), inspected before any pump.
        QTest::keyClick(&bar, Qt::Key_End);
        QTest::keyClick(&bar, Qt::Key_Left);
        QCOMPARE(bar.focusId(), QStringLiteral("chev:1"));
        QTest::keyClick(&bar, Qt::Key_Down);
        QMenu* menu = visibleMenu(&bar, QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        menu->hide();
        QTest::qWait(10);
        QVERIFY(bar.inKeyboardMode());                          // a popup coming and going keeps the mode
        // Down on hist: the history list.
        QTest::keyClick(&bar, Qt::Key_Home);
        QTest::keyClick(&bar, Qt::Key_Right);
        QCOMPARE(bar.focusId(), QStringLiteral("hist"));
        QTest::keyClick(&bar, Qt::Key_Down);
        QVERIFY(visibleMenu(&bar, QStringLiteral("rcxAddressBarHistoryMenu")));
        QCOMPARE(bar.historyMenuOpenCount(), 1);
        visibleMenu(&bar, QStringLiteral("rcxAddressBarHistoryMenu"))->hide();
        QTest::qWait(10);
        // Down on a crumb: nothing to drop.
        QTest::keyClick(&bar, Qt::Key_End);
        QTest::keyClick(&bar, Qt::Key_Left);
        QTest::keyClick(&bar, Qt::Key_Left);
        QCOMPARE(bar.focusId(), QStringLiteral("crumb:1"));
        QTest::keyClick(&bar, Qt::Key_Down);
        QVERIFY(!visibleMenu(&bar, QStringLiteral("rcxAddressBarSiblingMenu")));
        // F2 on the DEEPEST crumb: its rename, since that crumb's label is a
        // bare class name. Keyboard mode yields to the overlay either way.
        QTest::keyClick(&bar, Qt::Key_F2);
        QVERIFY2(bar.isClassNameEditing(), "F2 on the deepest crumb did not open the rename");
        QVERIFY(!bar.inKeyboardMode());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        // F2 on an ANCESTOR: the path edit — "Class.field" is a path segment,
        // not a name.
        bar.enterKeyboardMode();
        while (bar.focusId() != QStringLiteral("crumb:0")) QTest::keyClick(&bar, Qt::Key_Left);
        QTest::keyClick(&bar, Qt::Key_F2);
        QVERIFY2(bar.isPathEditing(), "F2 on an ancestor crumb did not open the path edit");
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        // F2 on the base: the base edit.
        bar.enterKeyboardMode();
        while (bar.focusId() != QStringLiteral("base")) QTest::keyClick(&bar, Qt::Key_Left);
        QTest::keyClick(&bar, Qt::Key_F2);
        QVERIFY(bar.isBaseEditing());
        QVERIFY(!bar.inKeyboardMode());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        // Escape: out, NoFocus, the document asked to take focus back.
        bar.enterKeyboardMode();
        const int returnsBefore = focusReturns;
        QTest::keyClick(&bar, Qt::Key_Escape);
        QVERIFY(!bar.inKeyboardMode());
        QVERIFY(bar.focusId().isEmpty());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        QCOMPARE(focusReturns, returnsBefore + 1);
        // A real mouse press anywhere ends the mode too. On an ANCESTOR
        // crumb: the deepest one is a field now, and a field takes the focus
        // the mode was holding rather than handing it back.
        bar.enterKeyboardMode();
        QVERIFY(bar.inKeyboardMode());
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("crumb:0")).center());
        QVERIFY(!bar.inKeyboardMode());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        // Outside the mode the keys are nobody's: no walk, no activation.
        QTest::keyClick(&bar, Qt::Key_Right);
        QVERIFY(bar.focusId().isEmpty());
        QTest::keyClick(&bar, Qt::Key_Return);
        QCOMPARE(backs, 2);
    }

    void testKeyboardFocusRingPixels() {
        // ONE device-exact 1-px ring in borderFocused around the focused
        // cell — its top and bottom device rows, its left and right device
        // columns — and none of it outside keyboard mode. No accent.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QImage img = grabOf(bar);
        const QRect all(0, 0, img.width(), img.height());
        QCOMPARE(countColour(img, all, t.borderFocused), 0);
        bar.enterKeyboardMode();
        QCOMPARE(bar.focusId(), QStringLiteral("crumb:1"));
        const QRect cell = bar.itemRect(QStringLiteral("crumb:1"));
        img = grabOf(bar);
        RingEdges e = ringEdgesOf(img, cell);
        const int w = e.right - e.left + 1, h = e.bottom - e.top + 1;
        QVERIFY(countColour(img, rowAcross(e, e.top), t.borderFocused) >= w - 2);
        QVERIFY(countColour(img, rowAcross(e, e.bottom), t.borderFocused) >= w - 2);
        QVERIFY(countColour(img, colAlong(e, e.left), t.borderFocused) >= h - 2);
        QVERIFY(countColour(img, colAlong(e, e.right), t.borderFocused) >= h - 2);
        // Exactly one row / column thick: just inside the ring there is none.
        QCOMPARE(countColour(img, QRect(e.left + 2, e.top + 1, w - 4, 1), t.borderFocused), 0);
        QCOMPARE(countColour(img, QRect(e.left + 2, e.bottom - 1, w - 4, 1), t.borderFocused), 0);
        QCOMPARE(countColour(img, QRect(e.left + 1, e.top + 2, 1, h - 4), t.borderFocused), 0);
        QCOMPARE(countColour(img, QRect(e.right - 1, e.top + 2, 1, h - 4), t.borderFocused), 0);
        QCOMPARE(countColour(img, all, t.indHoverSpan), 0);
        // The ring follows the focus. Home is the first ENABLED stop — with
        // no history that is the source chip, not the disabled Back cell.
        QTest::keyClick(&bar, Qt::Key_Home);
        QCOMPARE(bar.focusId(), bar.traversalIds().first());
        QCOMPARE(bar.focusId(), QStringLiteral("src"));
        const QRect first = bar.itemRect(bar.focusId());
        img = grabOf(bar);
        const RingEdges eb = ringEdgesOf(img, first);
        QCOMPARE(countColour(img, rowAcross(e, e.top), t.borderFocused), 0);
        QVERIFY(countColour(img, rowAcross(eb, eb.top), t.borderFocused) >= (eb.right - eb.left + 1) - 2);
        QVERIFY(countColour(img, rowAcross(eb, eb.bottom), t.borderFocused) >= (eb.right - eb.left + 1) - 2);
        // Leaving the mode takes the ring away.
        QTest::keyClick(&bar, Qt::Key_Escape);
        img = grabOf(bar);
        QCOMPARE(countColour(img, all, t.borderFocused), 0);
    }

    void testKeyboardFocusRingIsOneDeviceRowAt125Percent() {
        // The fractional-scale half of the ring rule (test_hairline_dpr's
        // subject, without its custom main): the bar rendered onto a 1.25
        // device-pixel-ratio image, where a 1-logical-px pen would land on
        // two device rows. The ring must be exactly one device row at each
        // edge, on the row the edge fills pick (qFloor(edge ± 0.5)).
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        bar.enterKeyboardMode();
        const QRect cell = bar.itemRect(QStringLiteral("crumb:1"));
        const qreal dpr = 1.25;
        QImage img(qRound(bar.width() * dpr), qRound(bar.height() * dpr), QImage::Format_ARGB32);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::black);
        {
            QPainter p(&img);
            bar.render(&p);
        }
        const QRectF dev(cell.left() * dpr, cell.top() * dpr, cell.width() * dpr, cell.height() * dpr);
        const int topY    = qFloor(dev.top() + 0.5);
        const int bottomY = qFloor(dev.bottom() - 0.5);
        const int leftX   = qFloor(dev.left() + 0.5);
        const int rightX  = qFloor(dev.right() - 0.5);
        QVERIFY2(topY != cell.top() && bottomY != cell.bottom(),
                 "the 1.25 phase should move the ring rows off the logical rows");
        // Which device rows across the cell's width hold the ring: the top
        // and bottom rows only.
        QVector<int> ringRows;
        for (int y = qFloor(dev.top()) - 1; y <= qCeil(dev.bottom()) + 1; ++y)
            if (countColour(img, QRect(leftX + 2, y, rightX - leftX - 4, 1), t.borderFocused) >= rightX - leftX - 6)
                ringRows << y;
        QCOMPARE(ringRows, (QVector<int>{ topY, bottomY }));
        QVector<int> ringCols;
        for (int x = qFloor(dev.left()) - 1; x <= qCeil(dev.right()) + 1; ++x)
            if (countColour(img, QRect(x, topY + 2, 1, bottomY - topY - 4), t.borderFocused) >= bottomY - topY - 6)
                ringCols << x;
        QCOMPARE(ringCols, (QVector<int>{ leftX, rightX }));
    }

    void testMenuOpenCellIsHoverNotSelected() {
        // The chip while the source chooser is up: t.hover, never
        // pressedFill. On tw.json pressedFill is t.selected (button ==
        // background), so the shared state boxed the chip in pale blue for
        // as long as the popup stayed open (build/issue.png); the mouse-down
        // flash keeps pressedFill, a hung dropdown reads as hover.
        AddressBar bar;
        bar.applyTheme(loadTheme(QStringLiteral("tw")));
        const Theme& t = bar.theme();
        QVERIFY(pressedFill(t) != t.hover);
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        const QRect src = bar.itemRect(QStringLiteral("src"));
        QVERIFY(!src.isNull());
        bar.setSourceMenuOpen(true);
        QImage img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, src), t.hover) > 0, "menu-open chip has no hover fill");
        QCOMPARE(countColour(img, devRect(img, src), pressedFill(t)), 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
        bar.setSourceMenuOpen(false);
        img = grabOf(bar);
        QCOMPARE(countColour(img, devRect(img, src), t.hover), 0);
    }

    void testSourceChooserHangsOnTheSeamAtBothScales() {
        // The anchor is the bar's bottom edge under the chip: the popup's
        // top frame row is the device row right after the bar's seam row and
        // its left frame column is the chip cell's left device column — at
        // dpr 1.0 and at 1.25, where the edge fills pick qFloor(edge ± 0.5)
        // and floor(x + 0.5) is always floor(x - 0.5) + 1. Rendered onto one
        // image, the bar at the origin and the popup at (chip.left, barH),
        // the way the real windows stack.
        AddressBar bar;
        bar.applyTheme(loadTheme(QStringLiteral("tw")));
        const Theme& t = bar.theme();
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        const QRect src = bar.itemRect(QStringLiteral("src"));
        QVERIFY(!src.isNull());
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        {
            QVector<SourceEntry> entries;
            SourceEntry h; h.entryKind = SourceEntry::SectionHeader; h.displayName = QStringLiteral("Connected"); h.enabled = false;
            entries.append(h);
            SourceEntry e; e.entryKind = SourceEntry::SavedSource; e.displayName = QStringLiteral("RCTarget.exe");
            e.providerIdentifier = QStringLiteral("processmemory"); e.kindLabel = QStringLiteral("Process");
            e.iconPath = iconForProvider(e.providerIdentifier); e.pid = QStringLiteral("34856");
            e.arch = QStringLiteral("x64"); e.savedIndex = 0; e.isActive = true;
            entries.append(e);
            popup.setSources(entries);
        }
        const QPoint anchor = bar.mapToGlobal(QPoint(src.left(), bar.height()));
        popup.popup(anchor);
        QTest::qWait(30);
        QApplication::processEvents();
        // The popup sits at the anchor unless the screen edge pushes it in.
        const QRect screen = QApplication::screenAt(anchor) ? QApplication::screenAt(anchor)->availableGeometry()
                                                            : QApplication::primaryScreen()->availableGeometry();
        QCOMPARE(popup.pos().x(), qBound(screen.left(), anchor.x(), screen.right() - popup.width()));
        QCOMPARE(popup.pos().y(), anchor.y());
        for (const qreal dpr : { 1.0, 1.25 }) {
            const int barH = bar.height();
            const int w = qMax(bar.width(), src.left() + popup.width());
            QImage img(qRound(w * dpr), qRound((barH + popup.height()) * dpr), QImage::Format_ARGB32);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::black);
            {
                QPainter p(&img);
                bar.render(&p);
                popup.render(&p, QPoint(src.left(), barH));
            }
            const int seamRow  = qFloor(barH * dpr - 0.5);
            const int frameRow = qFloor(barH * dpr + 0.5);
            QCOMPARE(frameRow, seamRow + 1);
            const int barW = qRound(bar.width() * dpr);
            QVERIFY2(countColour(img, QRect(0, seamRow, barW, 1), containerBorderColor(t)) >= barW - 2,
                     qPrintable(QStringLiteral("dpr %1: the bar's seam is not on row %2").arg(dpr).arg(seamRow)));
            const QRectF pdev(src.left() * dpr, barH * dpr, popup.width() * dpr, popup.height() * dpr);
            const int left  = qFloor(pdev.left() + 0.5);
            const int right = qFloor(pdev.right() - 0.5);
            const int bottom = qFloor(pdev.bottom() - 0.5);
            QCOMPARE(countColour(img, QRect(left, frameRow, right - left + 1, 1), t.border), right - left + 1);
            QCOMPARE(countColour(img, QRect(left, frameRow, right - left + 1, 1), containerBorderColor(t)), 0);
            QCOMPARE(countColour(img, QRect(left, frameRow, right - left + 1, 1), t.indHoverSpan), 0);
            QCOMPARE(countColour(img, QRect(left, frameRow, 1, bottom - frameRow + 1), t.border), bottom - frameRow + 1);
            // The column just left of the popup is the ground under the bar's
            // window, so nothing of the frame leaks a device column left.
            QCOMPARE(countColour(img, QRect(left - 1, frameRow + 1, 1, bottom - frameRow), t.border), 0);
        }
        popup.hide();
        QApplication::processEvents();
    }

    void testDocumentShortcutsReachTheBar() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QsciScintilla* sci = m_editor->scintilla();
        sci->setFocus();
        QSignalSpy back(m_editor, &RcxEditor::navBackRequested);
        QSignalSpy fwd(m_editor, &RcxEditor::navForwardRequested);
        QSignalSpy up(m_editor, &RcxEditor::navUpRequested);
        QTest::keyClick(sci, Qt::Key_Left, Qt::AltModifier);
        QCOMPARE(back.count(), 1);
        QTest::keyClick(sci, Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(fwd.count(), 1);
        QTest::keyClick(sci, Qt::Key_Up, Qt::AltModifier);
        QCOMPARE(up.count(), 1);
        QApplication::processEvents();
        QVERIFY(m_ctrl->focusPath().isEmpty());                 // Up collapsed the hop
        // A plain arrow is the document's (Up moves the caret; a plain
        // Left would cycle the caret node's type, so it is not sent here).
        QTest::keyClick(sci, Qt::Key_Up);
        QCOMPARE(back.count(), 1);
        QCOMPARE(up.count(), 1);
        // The mouse thumb buttons on the document.
        auto thumb = [&](Qt::MouseButton b) {
            const QPointF at(20, 40);
            QMouseEvent press(QEvent::MouseButtonPress, at, at,
                              QPointF(sci->viewport()->mapToGlobal(at.toPoint())), b, b, Qt::NoModifier);
            QApplication::sendEvent(sci->viewport(), &press);
        };
        thumb(Qt::BackButton);
        QCOMPARE(back.count(), 2);
        thumb(Qt::ForwardButton);
        QCOMPARE(fwd.count(), 2);
        // F6: keyboard mode; Esc: focus back in the document.
        QTest::keyClick(sci, Qt::Key_F6);
        QVERIFY(bar->inKeyboardMode());
        QCOMPARE(bar->window()->focusWidget(), bar);
        QTest::keyClick(bar, Qt::Key_Escape);
        QVERIFY(!bar->inKeyboardMode());
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), sci);
        // During a Scintilla inline edit the arrows are the caret's and the
        // thumb buttons are inert: no navigation under a half-typed value.
        const int ln = lineOf(m_id.leaf);
        QVERIFY(m_editor->beginInlineEdit(EditTarget::Name, ln));
        QVERIFY(m_editor->isEditing());
        QTest::keyClick(sci, Qt::Key_Left, Qt::AltModifier);
        QTest::keyClick(sci, Qt::Key_Right, Qt::AltModifier);
        thumb(Qt::BackButton);
        QCOMPARE(back.count(), 2);
        QCOMPARE(fwd.count(), 2);
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();
    }

    void testReturnOnBackInKeyboardModeGoesBack() {
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        QsciScintilla* sci = m_editor->scintilla();
        jumpToDefinitionOf(m_id.vptr);
        QVERIFY(bar->state().canBack);
        QSignalSpy back(m_editor, &RcxEditor::navBackRequested);
        sci->setFocus();
        QTest::keyClick(sci, Qt::Key_F6);
        QVERIFY(bar->inKeyboardMode());
        QCOMPARE(bar->focusId(), QStringLiteral("crumb:0"));   // the lone crumb of the jumped-to root
        QTest::keyClick(bar, Qt::Key_Home);
        QCOMPARE(bar->focusId(), QStringLiteral("back"));
        QTest::keyClick(bar, Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(back.count(), 1);
        QCOMPARE(m_ctrl->viewRootId(), m_id.editor);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        // Back is spent: the state push moved the ring off the disabled
        // cell onto the deepest crumb of the restored trail; still in mode.
        QVERIFY(bar->inKeyboardMode());
        QVERIFY(!bar->state().canBack);
        QCOMPARE(bar->focusId(), QStringLiteral("crumb:1"));
        QTest::keyClick(bar, Qt::Key_Escape);
        QVERIFY(!bar->inKeyboardMode());
        QCOMPARE(m_editor->window()->focusWidget(), sci);
    }

    void testSplitPanesShareTheTrailAndOnlyTheSenderScrolls() {
        // Controller-global history and trail (pinned): both bars show the
        // same state; a crumb click in pane B rewrites both trails but
        // scrolls only pane B. Wide enough that a 700-px pane lays the
        // whole three-crumb trail out (no fold), short enough to scroll.
        m_splitter->resize(1400, 120);
        RcxEditor* pane2 = m_ctrl->addSplitEditor(m_splitter);
        QApplication::processEvents();
        drillTwoLevels();
        QApplication::processEvents();
        AddressBar* bar1 = m_editor->addressBar();
        AddressBar* bar2 = pane2->addressBar();
        QVERIFY(bar1->state() == bar2->state());
        QCOMPARE(bar2->segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                                 QStringLiteral("QWidgetPrivate.parent"),
                                                 QStringLiteral("QWidget") }));
        QsciScintilla* sci1 = m_editor->scintilla();
        QsciScintilla* sci2 = pane2->scintilla();
        sci1->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 0UL);
        sci2->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 0UL);
        QSignalSpy crumb2(pane2, &RcxEditor::crumbClicked);
        const int undoBefore = m_doc->undoStack.count();
        const int histBefore = m_ctrl->backEntries().size();
        const QRect c1 = bar2->itemRect(QStringLiteral("crumb:1"));
        QVERIFY(!c1.isNull());
        QTest::mouseClick(bar2, Qt::LeftButton, Qt::NoModifier, c1.center());
        QApplication::processEvents();
        QCOMPARE(crumb2.count(), 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(m_ctrl->backEntries().size(), histBefore + 1);
        QVERIFY(bar1->state() == bar2->state());
        QCOMPARE(bar1->segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"), QStringLiteral("QWidgetPrivate") }));
        // Pane B scrolled its hop's row up; pane A did not move.
        const int hopLine = lineOf(m_id.vptr);
        QVERIFY(hopLine > 0);
        const int lines = (int)sci2->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
        const int onScreen = (int)sci2->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
        const int maxFirst = qMax(0, lines - onScreen);
        QVERIFY2(maxFirst > 0, "pane not short enough to scroll — the assertion below would be vacuous");
        QCOMPARE((int)sci2->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE), qMin(hopLine, maxFirst));
        QCOMPARE((int)sci1->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE), 0);
        // Back from pane A restores the trail on both and scrolls pane A.
        sci2->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, 0UL);
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QVERIFY(bar1->state() == bar2->state());
        QCOMPARE((int)sci2->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE), 0);
        // The sender (pane A) scrolled to the restored hop's row: the entry
        // was anchored on pane B's first visible row — the command row,
        // which is no node — so the restore falls back to the deepest hop
        // of the restored trail, `parent`.
        const int parentLine = lineOf(m_id.parent);
        QVERIFY(parentLine > 0);
        const int lines1 = (int)sci1->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
        const int onScreen1 = (int)sci1->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
        const int maxFirst1 = qMax(0, lines1 - onScreen1);
        QVERIFY2(maxFirst1 > 0, "pane A not short enough to scroll — the assertion below would be vacuous");
        QCOMPARE((int)sci1->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE), qMin(parentLine, maxFirst1));
    }

    // ── The history menu's "you are here" row ──

    void testHistoryMenuMarksTheCurrentPlace() {
        // Explorer parity: between the Back rows and the Forward rows sits
        // the place the user is at — checked, disabled, worded exactly as
        // the controller would record it now (trailPathText + the base).
        drillOneLevel();
        AddressBar* bar = m_editor->addressBar();
        jumpToDefinitionOf(m_id.vptr);                          // leaves RcxEditor.vptr @ 0x0
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x40")));      // leaves QWidgetPrivate @ 0x0
        QApplication::processEvents();
        QCOMPARE(m_ctrl->backEntries().size(), 2);
        const QString here = m_ctrl->currentNavLabel();
        QCOMPARE(here, trailPathText(m_doc->tree, m_ctrl->viewRootId(), m_ctrl->focusPath())
                           + QStringLiteral("  @ 0x40"));
        QCOMPARE(here, m_ctrl->currentNavEntry(m_editor).label);
        QMenu* menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(actionTexts(menu), (QStringList{ QStringLiteral("QWidgetPrivate  @ 0x0"),
                                                  QStringLiteral("RcxEditor.vptr  @ 0x0"), here }));
        QAction* cur = menu->actions()[2];
        QVERIFY(cur->isCheckable());
        QVERIFY(cur->isChecked());
        QVERIFY(!cur->isEnabled());
        QCOMPARE(cur->data().toInt(), 0);
        for (QAction* a : menu->actions()) QVERIFY(!a->isSeparator());
        QVERIFY(!menu->actions()[0]->isChecked());
        QVERIFY(!menu->actions()[1]->isChecked());
        closeMenuOn(bar, QStringLiteral("hist"), menu);
        // One step back: the row moves with the user — the place just left
        // sits on Forward below it, unchecked and live.
        m_ctrl->goBack(m_editor);
        QApplication::processEvents();
        menu = openMenuOn(bar, QStringLiteral("hist"), QStringLiteral("rcxAddressBarHistoryMenu"));
        QVERIFY(menu);
        QCOMPARE(actionTexts(menu), (QStringList{ QStringLiteral("RcxEditor.vptr  @ 0x0"),
                                                  QStringLiteral("QWidgetPrivate  @ 0x0"),
                                                  QStringLiteral("QWidgetPrivate  @ 0x40") }));
        QCOMPARE(menu->actions()[1]->text(), m_ctrl->currentNavLabel());
        QVERIFY(menu->actions()[1]->isChecked());
        QVERIFY(!menu->actions()[1]->isEnabled());
        QVERIFY(!menu->actions()[2]->isChecked());
        QVERIFY(menu->actions()[2]->isEnabled());
        QCOMPARE(menu->actions()[2]->data().toInt(), 1);
        closeMenuOn(bar, QStringLiteral("hist"), menu);
    }

    // ── Sibling rows say where each pointer leads ──

    // A pointer whose VALUE reads fine but whose target does not (past the
    // buffer) is placed nowhere: compose stamps ptrBase 0 for it once
    // expanded, so the collapsed row must say the same.
    void testDanglingSiblingPointerIsUnreadable() {
        QByteArray bytes = chainBytes();
        qToLittleEndian<quint64>(0x1000, bytes.data() + 0x18);   // ptr2 -> outside the 64 bytes
        m_doc->provider = std::make_unique<BufferProvider>(bytes);
        const uint64_t ptr2 = addSibling();
        const QVector<SiblingEntry> sibs = m_ctrl->siblingsForCrumb(0);
        QCOMPARE(sibs.size(), 2);
        QCOMPARE(sibs[1].id, ptr2);
        QCOMPARE(sibs[1].address, 0ULL);
        QCOMPARE(sibs[0].address, 0x20ULL);                     // the readable one is unaffected
        AddressBar* bar = m_editor->addressBar();
        QMenu* menu = openMenuOn(bar, QStringLiteral("chev:0"), QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions()[1]->text(), AddressBar::siblingActionText(sibs[1]));
        QVERIFY2(menu->actions()[1]->toolTip().endsWith(QStringLiteral("(unreadable)")), qPrintable(menu->actions()[1]->toolTip()));
        closeMenuOn(bar, QStringLiteral("chev:0"), menu);
    }

    void testSiblingRowsSayWhereEachPointerLeads() {
        // The bytes hold real pointers: RcxEditor@0 .vptr = 0x20, .ptr2 =
        // 0x38, QWidgetPrivate@0x20 .parent = 0x30. A collapsed pointer's
        // address is ONE provider read at container + offset; an expanded
        // one is what compose dereferenced (ptrBase inside it) — the same
        // number, off the data the crumbs use.
        QByteArray bytes = chainBytes();
        qToLittleEndian<quint64>(0x38, bytes.data() + 0x18);
        m_doc->provider = std::make_unique<BufferProvider>(bytes);
        const uint64_t ptr2 = addSibling();                     // ptr2 at +0x18, collapsed
        QVector<SiblingEntry> sibs = m_ctrl->siblingsForCrumb(0);
        QCOMPARE(sibs.size(), 2);
        QCOMPARE(sibs[0].id, m_id.vptr);
        QVERIFY(!sibs[0].expanded);
        QCOMPARE(sibs[0].address, 0x20ULL);                     // read, not rendered
        QCOMPARE(sibs[1].id, ptr2);
        QCOMPARE(sibs[1].address, 0x38ULL);
        // The pure model query has no memory: its entries are unplaced.
        for (const SiblingEntry& e : siblingFieldsOf(m_doc->tree, m_id.editor, 0))
            QCOMPARE(e.address, 0ULL);
        // An expanded hop: compose's ptrBase; the level under it reads at
        // that frame.
        drillOneLevel();                                        // vptr open, trail RcxEditor.vptr
        sibs = m_ctrl->siblingsForCrumb(0);
        QVERIFY(sibs[0].expanded);
        QCOMPARE(sibs[0].address, 0x20ULL);
        QCOMPARE(sibs[1].address, 0x38ULL);
        const QVector<SiblingEntry> inner = m_ctrl->siblingsForCrumb(1);   // QWidgetPrivate's fields
        QCOMPARE(inner.size(), 1);
        QCOMPARE(inner[0].id, m_id.parent);
        QCOMPARE(inner[0].address, 0x30ULL);                    // read at 0x20 + 0
        // The row carries it: "@ 0x…" in the tab column and in the tooltip.
        AddressBar* bar = m_editor->addressBar();
        QMenu* menu = openMenuOn(bar, QStringLiteral("chev:0"), QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions()[0]->text(), AddressBar::siblingActionText(sibs[0]) + QStringLiteral("\t@ 0x20"));
        QCOMPARE(menu->actions()[1]->text(), AddressBar::siblingActionText(sibs[1]) + QStringLiteral("\t@ 0x38"));
        QVERIFY2(menu->actions()[0]->toolTip().endsWith(QStringLiteral("@ 0x20")), qPrintable(menu->actions()[0]->toolTip()));
        QVERIFY2(menu->actions()[1]->toolTip().endsWith(QStringLiteral("@ 0x38")), qPrintable(menu->actions()[1]->toolTip()));
        closeMenuOn(bar, QStringLiteral("chev:0"), menu);
        // A null provider: nothing can be read, so nothing is placed, and
        // the rows say so in their tooltip only — the tab column stays empty.
        m_doc->provider = std::make_unique<NullProvider>();
        m_ctrl->refresh();
        sibs = m_ctrl->siblingsForCrumb(0);
        QCOMPARE(sibs.size(), 2);
        QCOMPARE(sibs[0].address, 0ULL);
        QCOMPARE(sibs[1].address, 0ULL);
        menu = openMenuOn(bar, QStringLiteral("chev:0"), QStringLiteral("rcxAddressBarSiblingMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions()[0]->text(), AddressBar::siblingActionText(sibs[0]));
        QVERIFY(!menu->actions()[0]->text().contains(QLatin1Char('\t')));
        QVERIFY2(menu->actions()[0]->toolTip().endsWith(QStringLiteral("@ (unreadable)")), qPrintable(menu->actions()[0]->toolTip()));
        closeMenuOn(bar, QStringLiteral("chev:0"), menu);
    }

    // ── The edit overlay follows a pane resize ──

    void testEditOverlayFollowsAResize() {
        // A resize while an edit is up moves the overlay with the cells —
        // the field a fresh open would take at the new width — and leaves
        // the text, caret and selection alone; Esc still restores.
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.baseFormula  = QStringLiteral("<RCTarget.exe>+0x1234");
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        showBar(bar, 800);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isBaseEditing());
        const QString text = bar.editText();
        bar.editWidget()->setSelection(2, 5);
        for (int w : {1000, 500, 800}) {
            bar.resize(w, AddressBar::kAddressBarHeight);
            QApplication::processEvents();
            QCOMPARE(bar.width(), w);
            const QRect base   = bar.itemRect(QStringLiteral("base"));     // the cell at the new width
            const QRect recent = bar.itemRect(QStringLiteral("recent"));
            QVERIFY2(bar.isBaseEditing(), qPrintable(QString::number(w)));
            QCOMPARE(bar.editRect(), bar.editWidget()->geometry());
            QCOMPARE(bar.editRect().left(), base.left());
            // Fitted, not full-bleed: the field is the size of its value at
            // every width, so the dead air never comes back on a resize.
            QCOMPARE(bar.editRect().width(), fittedEditW(text, bar.font()));
            QVERIFY2(bar.rect().contains(bar.editRect()), qPrintable(QString::number(w)));
            QVERIFY2(bar.editRect().right() < recent.left(), qPrintable(QString::number(w)));
            QCOMPARE(bar.editCoveredRect().left(), bar.editRect().left());
            // The paper is the CELL's, not the strip's: with the formula's
            // resolved suffix folded into the preview, what it hides is the
            // base cell and nothing past it. The crumbs stay on screen.
            QVERIFY2(bar.editCoveredRect().right() < recent.left() - 1,
                     qPrintable(QStringLiteral("the paper still runs to `recent` at %1").arg(w)));
            QVERIFY(bar.editCoveredRect().right() >= base.right());
            QCOMPARE(bar.editText(), text);
            QCOMPARE(bar.editWidget()->selectedText(), text.mid(2, 5));
        }
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QCOMPARE(bar.state().baseFormula, s.baseFormula);
        // The path scope follows the same way: it starts at the base's right
        // edge and is fitted to the trail, at every width that has room.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("space")).center());
        QVERIFY(bar.isPathEditing());
        const QString path = bar.editText();
        for (int w : {1000, 800}) {
            bar.resize(w, AddressBar::kAddressBarHeight);
            QApplication::processEvents();
            const QRect base   = bar.itemRect(QStringLiteral("base"));
            const QRect recent = bar.itemRect(QStringLiteral("recent"));
            QCOMPARE(bar.editRect(), bar.editWidget()->geometry());
            // The field starts where the first crumb's CELL does — its own
            // left inset is kCrumbPad, so the trail's first glyph lands on
            // the column it was already on.
            QCOMPARE(bar.editRect().left(), base.right() + 1);
            // The path scope keeps a floor: it stands in for the whole trail,
            // not for one cell, and everything right of it is covered anyway.
            QCOMPARE(bar.editRect().width(),
                     qMax(AddressBar::kEditMinW, fittedEditW(path, bar.font())));
            QVERIFY(bar.editRect().right() < recent.left() - AddressBar::kChevW);
            // The path scope is the one that DOES cover the whole strip: it
            // stands in for the entire trail, not for one cell.
            QCOMPARE(bar.editCoveredRect().left(), base.right() + 1);
            QCOMPARE(bar.editCoveredRect().right(), recent.left() - 1);
            QCOMPARE(bar.editText(), path);
        }
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
    }

    // ── The formula and what it comes to ──
    //
    // The base used to read "formula  →  value" with TWO spaces before the
    // arrow and one after, which made the formula look like it carried
    // trailing space, and the arrow itself repeated the glyph this same bar
    // uses for Back and Forward — so it read as one more direction rather
    // than as arithmetic. It is an equation now, spaced evenly.
    void testBaseSeparatorIsAnEvenlySpacedEquals() {
        QCOMPARE(AddressBar::baseSepText(), QStringLiteral("= "));

        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.baseFormula  = QStringLiteral("<RCTarget.exe>+0x1234");
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        showBar(bar, 900);

        // What the cell actually holds: formula, separator, value — and no
        // arrow of any kind, in either direction. The space before the `=` is
        // GEOMETRY (baseSepGap, one space wide), reserved so the suffix
        // already stands where the edit overlay's preview will.
        const QString shown = bar.baseDisplayText() + QStringLiteral(" ") + bar.baseSuffixShown();
        QCOMPARE(shown, QStringLiteral("<RCTarget.exe>+0x1234 = 0x7FF6DEAD1234"));
        const QRect cell = bar.itemRect(QStringLiteral("base"));
        const int sepGap = QFontMetrics(bar.font()).horizontalAdvance(QLatin1Char(' '));
        QCOMPARE(cell.width(), AddressBar::kBasePad
                                   + QFontMetrics(bar.font()).horizontalAdvance(bar.baseDisplayText())
                                   + sepGap
                                   + QFontMetrics(bar.font()).horizontalAdvance(bar.baseSuffixShown())
                                   + AddressBar::kBasePad);
        for (QChar arrow : {QChar(0x2192), QChar(0x2190), QChar(0x203a)})
            QVERIFY2(!shown.contains(arrow), qPrintable(QStringLiteral("arrow %1 back in the base")
                                                            .arg(arrow.unicode(), 4, 16, QLatin1Char('0'))));

        // A bare literal is not an equation: no separator, nothing after it.
        AddressBarState lit = stateWith(twoLevel());
        lit.baseAddress = lit.resolvedBase = 0x279B3D07010ULL;
        bar.setState(lit);
        QApplication::processEvents();
        QVERIFY2(bar.baseSuffixShown().isEmpty(), qPrintable(bar.baseSuffixShown()));
        QCOMPARE(bar.baseDisplayText(), QStringLiteral("0x279B3D07010"));
    }

    // ── Clicking in moves nothing ──
    //
    // The rule the RcxEditor's inline edit sets and this one has to match: an
    // inline edit is pixel-perfect. It replaces a span in place, so nothing
    // around it can move — and the user's word for an overlay that does move
    // things is "annoying as fuck". Rects proving the field stands where the
    // cell did are half the proof; this is the other half, and the one that
    // would have caught every version of the bug: the same bar grabbed at
    // rest and mid-edit, every differing device column counted.
    //
    // Everything that changes must lie inside the segment being edited (its
    // selection band and caret). One column outside it is the line having
    // been shoved. The bottom two device rows are excluded: the focus seam is
    // what clicking in is FOR.
    void testOpeningAnEditMovesNothingOutsideItsSegment() {
        for (int scope = 0; scope < 2; ++scope) {
            AddressBar bar;
            bar.applyTheme(ThemeManager::instance().current());
            AddressBarState s = stateWith(twoLevel());
            s.baseAddress = s.resolvedBase = 0x279B3D07010ULL;   // a literal: no suffix
            bar.setState(s);
            showBar(bar, 900);

            const QRect cell = bar.itemRect(scope == 0 ? QStringLiteral("base")
                                                       : QStringLiteral("crumb:1"));
            QVERIFY(!cell.isNull());
            const QImage rest = grabOf(bar);
            if (scope == 0) bar.beginBaseEdit(); else bar.beginClassNameEdit();
            QApplication::processEvents();
            QVERIFY(bar.isEditing());
            const QImage edit = grabOf(bar);
            QCOMPARE(rest.size(), edit.size());

            const qreal dpr = bar.devicePixelRatioF();
            const int cellL = int(cell.left() * dpr);
            const int cellR = int((cell.right() + 1) * dpr) + 1;   // +1: AA on the edge
            const int rows  = edit.height() - int(2 * dpr) - 1;    // above the focus seam
            int inside = 0, outside = 0, firstOutside = -1;
            for (int x = 0; x < edit.width(); ++x) {
                bool diff = false;
                for (int y = 0; y < rows && !diff; ++y)
                    diff = rest.pixel(x, y) != edit.pixel(x, y);
                if (!diff) continue;
                if (x >= cellL && x <= cellR) { ++inside; continue; }
                ++outside;
                if (firstOutside < 0) firstOutside = x;
            }
            QVERIFY2(inside > 0, "opening the edit changed nothing at all");
            QVERIFY2(outside == 0,
                     qPrintable(QStringLiteral("%1 opened and moved the line: %2 device columns "
                                               "changed outside [%3,%4], first at %5")
                                    .arg(scope == 0 ? QStringLiteral("the base edit")
                                                    : QStringLiteral("the class rename"))
                                    .arg(outside).arg(cellL).arg(cellR).arg(firstOutside)));
        }
    }

    // The same rule where a formula IS evaluated: the "= 0x…" the cell shows
    // at rest and the preview the overlay shows are the same phrase on the
    // same pixels. The gap before the `=` is geometry (baseSepGap) precisely
    // so it can double as the room the caret needs.
    void testTheResolvedSuffixDoesNotMoveWhenTheFieldOpens() {
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        AddressBar::Callbacks cb;
        cb.evaluate = [](const QString& s) {
            return s == QStringLiteral("<RCTarget.exe>+0x1234") ? QStringLiteral("0x7FF6DEAD1234")
                                                                : QString();
        };
        bar.setCallbacks(std::move(cb));
        AddressBarState s = stateWith(twoLevel());
        s.baseFormula  = QStringLiteral("<RCTarget.exe>+0x1234");
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        showBar(bar, 900);

        const QRect cell = bar.itemRect(QStringLiteral("base"));
        const QFontMetrics fm(bar.font());
        // Where the cell paints its suffix: text, then one space of geometry.
        const int suffixX = cell.left() + AddressBar::kBasePad
                          + fm.horizontalAdvance(bar.baseDisplayText())
                          + fm.horizontalAdvance(QLatin1Char(' '));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, cell.center());
        QVERIFY(bar.isBaseEditing());
        QCOMPARE(bar.editPreviewText(), QStringLiteral("= 0x7FF6DEAD1234"));
        // The preview starts at the field's right edge — the same column.
        QCOMPARE(bar.editRect().right() + 1, suffixX);
        QCOMPARE(bar.baseSuffixShown(), QStringLiteral("= 0x7FF6DEAD1234"));
    }

    // ── The field is the size of its value ──
    //
    // It used to open at a flat kEditMinW = 180 whatever it held, so a
    // 13-character address left ~78 px of empty field between the value and
    // the "= 0x…" preview painted after it (build/issue.png). The floor is now
    // the floor for SHORT values only; anything longer measures itself.
    void testBaseEditFieldFitsItsValue() {
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.baseAddress = s.resolvedBase = 0x279B3D07010ULL;
        bar.setState(s);
        showBar(bar, 900);

        const QRect baseCell = bar.itemRect(QStringLiteral("base"));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, baseCell.center());
        QVERIFY(bar.isBaseEditing());

        const QString text = bar.editText();
        QCOMPARE(text, QStringLiteral("0x279B3D07010"));
        QCOMPARE(bar.editRect().width(), fittedEditW(text, bar.font()));
        QVERIFY2(bar.editRect().width() < 180,
                 qPrintable(QStringLiteral("still slab-wide: %1").arg(bar.editRect().width())));
        // THE contract: the overlay stands exactly where the cell did. Same
        // left edge to the pixel, and a width that differs from the cell's
        // only by the gap the caret lives in (a space, versus the cell's
        // 6-px right padding) — paper against paper, invisible.
        QCOMPARE(bar.editRect().left(), baseCell.left());
        QVERIFY2(qAbs(bar.editRect().width() - baseCell.width())
                     <= QFontMetrics(bar.font()).horizontalAdvance(QLatin1Char(' ')),
                 qPrintable(QStringLiteral("field %1 vs cell %2")
                                .arg(bar.editRect().width()).arg(baseCell.width())));
        // The selection is the whole value — the half the user said was
        // already right, pinned so the width work cannot cost it.
        QCOMPARE(bar.editWidget()->selectedText(), text);
        // Nothing readable beyond the cell is papered over: the paper stops
        // inside the next crumb's own padding, well short of its first glyph,
        // so the trail is still there and still legible.
        QCOMPARE(bar.editCoveredRect().left(), baseCell.left());
        QVERIFY2(bar.editCoveredRect().right()
                     < bar.itemRect(QStringLiteral("crumb:0")).left() + AddressBar::kCrumbPad,
                 qPrintable(QStringLiteral("paper to %1, crumb text at %2")
                                .arg(bar.editCoveredRect().right())
                                .arg(bar.itemRect(QStringLiteral("crumb:0")).left()
                                     + AddressBar::kCrumbPad)));
    }

    // A one-character value gets a one-character field. There is deliberately
    // no minimum: the CELL is that small, and the whole contract is that the
    // overlay is the cell. A 90-px floor lived here and was the last thing
    // still inflating a field on click.
    void testShortBaseValueGetsAShortFieldNotAFloor() {
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.baseFormula = QStringLiteral("1");
        bar.setState(s);
        showBar(bar, 900);
        const QRect cell = bar.itemRect(QStringLiteral("base"));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, cell.center());
        QVERIFY(bar.isBaseEditing());
        QCOMPARE(bar.editText(), QStringLiteral("1"));
        // The cell here also carries the resolved suffix, so only the field's
        // own share of it is the comparison: same left edge, fitted width.
        QCOMPARE(bar.editRect().left(), cell.left());
        QCOMPARE(bar.editRect().width(), fittedEditW(bar.editText(), bar.font()));
        QVERIFY2(bar.editRect().width() < AddressBar::kEditMinW,
                 qPrintable(QStringLiteral("a one-char field is %1 px wide")
                                .arg(bar.editRect().width())));
        // ...and it grows the moment there is something to grow for.
        const int opened = bar.editRect().width();
        bar.editWidget()->setText(QStringLiteral("<RCTarget.exe>+0x1234"));
        QApplication::processEvents();
        QVERIFY(bar.editRect().width() > opened);
    }

    // Growth is one-way for as long as the overlay is up. A field that
    // resized in both directions would reflow the box under the caret and
    // drag the preview with every keystroke; a field that never grew would
    // pin the caret to its right edge on the longest strings in the app.
    void testEditFieldGrowsWithTypingAndNeverShrinks() {
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.baseAddress = s.resolvedBase = 0x279B3D07010ULL;
        bar.setState(s);
        showBar(bar, 900);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier,
                          bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isBaseEditing());
        const int opened = bar.editRect().width();

        bar.editWidget()->setText(QStringLiteral("<RCTarget.exe> + 0x1A0 + 0x58"));
        QApplication::processEvents();
        const int grown = bar.editRect().width();
        QVERIFY2(grown > opened, qPrintable(QStringLiteral("%1 -> %2").arg(opened).arg(grown)));
        QCOMPARE(grown, fittedEditW(bar.editText(), bar.font()));

        // Back to the short value: the box does NOT close up around it.
        bar.editWidget()->setText(QStringLiteral("0x10"));
        QApplication::processEvents();
        QCOMPARE(bar.editRect().width(), grown);

        // Longer than the strip can hold: the field stops where the preview
        // beside it would stop fitting and the QLineEdit scrolls internally.
        bar.editWidget()->setText(QString(400, QLatin1Char('A')));
        QApplication::processEvents();
        const int limit = bar.itemRect(QStringLiteral("recent")).left() - AddressBar::kChevW;
        QCOMPARE(bar.editRect().right(), limit - AddressBar::kEditPreviewMinW - 1);

        // A fresh open measures its own value again — the high-water mark
        // belongs to one edit session, not to the bar.
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier,
                          bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isBaseEditing());
        QCOMPARE(bar.editRect().width(), opened);
    }

    // ── The grammar help ──
    //
    // The worked examples that hung off the editor's command-row address
    // span until that row was demoted. They must survive TYPING, which is the
    // whole reason they are on a private RcxTooltip: the shared one is
    // cleared by GlobalTooltipBridge on every KeyPress.
    void testBaseEditShowsTheGrammarHelpWhileTyping() {
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 900);
        QVERIFY(!bar.editHelpVisible());

        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier,
                          bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isBaseEditing());
        QVERIFY2(bar.editHelpVisible(), "no expression help on opening the base edit");

        QTest::keyClicks(bar.editWidget(), QStringLiteral("0x1A0"));
        QApplication::processEvents();
        QVERIFY2(bar.editHelpVisible(), "the help died on the first keystroke");

        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QVERIFY(!bar.editHelpVisible());

        // A commit closes it too — the overlay and the help end together.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier,
                          bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.editHelpVisible());
        bar.baseCommitFinished(true);
        QVERIFY(!bar.isEditing());
        QVERIFY(!bar.editHelpVisible());
    }

    // The examples themselves: every form the parser accepts, in a body whose
    // two columns only line up in a fixed-pitch face.
    void testGrammarHelpNamesEveryFormOfTheGrammar() {
        const QString body = fmt::baseAddressHelpBody();
        QCOMPARE(fmt::baseAddressHelpTitle(), QStringLiteral("Base Address"));
        const QStringList forms = {QStringLiteral("0x7FF61234ABCD"),
                                   QStringLiteral("<app.exe>"),
                                   QStringLiteral("<app.exe> + 0x1A0"),
                                   QStringLiteral("[<app.exe> + 0x58]"),
                                   QStringLiteral("ntdll!SymbolName"),
                                   QStringLiteral("Operators: + - * << >> & | ^"),
                                   QStringLiteral("hexadecimal")};
        for (const QString& form : forms)
            QVERIFY2(body.contains(form), qPrintable(form));
        // Every description starts at column 24. Nothing enforces that but
        // this test, and a stray character makes the block read as ragged.
        int examples = 0;
        const QStringList rows = body.split(QLatin1Char('\n'));
        for (const QString& row : rows) {
            const int gap = row.indexOf(QStringLiteral("   "));   // 3+ spaces: a column break
            if (gap <= 0) continue;
            int desc = gap;
            while (desc < row.size() && row.at(desc) == QLatin1Char(' ')) ++desc;
            QVERIFY2(desc == 24, qPrintable(QStringLiteral("column %1: %2").arg(desc).arg(row)));
            ++examples;
        }
        QCOMPARE(examples, 5);
    }

    // ── A union root on line 0 ──

    void testLineZeroNamesAUnionRoot() {
        // A union viewed as the root: updateCommandRow prints its keyword
        // (resolvedClassKeyword), the line-0 parsers read it back at the
        // chevron, the name is the inline rename, and the bar's crumb and
        // root row carry the keyword too.
        m_doc->tree.nodes[idx(m_id.editor)].classKeyword = QStringLiteral("union");
        m_ctrl->refresh();
        QApplication::processEvents();
        const QString line = lineZeroText();
        QCOMPARE(line, buildCommandRowText(QStringLiteral("union"), QStringLiteral("RcxEditor"), false));
        const ColumnSpan chev = commandRowChevronSpan(line);
        QVERIFY(chev.valid);
        const ColumnSpan rt = commandRowRootTypeSpan(line);
        QVERIFY2(rt.valid, qPrintable(line));
        QCOMPARE(rt.start, chev.end);
        QCOMPARE(line.mid(rt.start, rt.end - rt.start), QStringLiteral("union"));
        const ColumnSpan rn = commandRowRootNameSpan(line);
        QVERIFY2(rn.valid, qPrintable(line));
        QCOMPARE(line.mid(rn.start, rn.end - rn.start), QStringLiteral("RcxEditor"));
        QCOMPARE(crumbs()[0].keyword, QStringLiteral("union"));
        const RootEntry& r0 = rootClassEntries(m_doc->tree)[0];
        QCOMPARE(r0.keyword, QStringLiteral("union"));
        QCOMPARE(r0.label, QStringLiteral("RcxEditor"));
        QVERIFY(m_editor->beginInlineEdit(EditTarget::RootClassName, 0));
        QVERIFY(m_editor->isEditing());
        m_editor->cancelInlineEdit();
        QVERIFY(!m_editor->isEditing());
    }
};

QTEST_MAIN(TestBreadcrumb)
#include "test_breadcrumb.moc"
