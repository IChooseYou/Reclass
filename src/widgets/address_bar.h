#pragma once
// rcx::AddressBar — the strip above the command row that answers Explorer's
// three questions for a class view: where am I reading from (the source
// chip), where in it (the base address and the drill-down trail) and where
// can I go (Back / Forward / history / Up, a chevron per level, recent
// places). It replaces BreadcrumbBar, which could only answer the middle one.
//
// ONE custom-painted QWidget on the RibbonBar template instead of a layout of
// QLabels and QToolButtons: per-item hit rects, hover fills, pressed and
// menu-open states, device-exact hairlines (a QSS border is two device rows
// at 125 %) and a single paintEvent — and no widget tree to tear down on
// every live-refresh tick. Intentionally no Q_OBJECT (mirrors EnumPickerPopup
// and the bar this replaces): outbound events are a struct of std::function
// callbacks that RcxEditor bridges to real signals, so this header does not
// enter every test target's AUTOMOC. It is still a QObject via QWidget, so
// the QMenu connects below work.
//
// State contract: the controller pushes one AddressBarState per refresh;
// setState() early-returns on operator== so an unchanged tick costs nothing.
// Geometry is a lazily recomputed Layout (paint and every query go through
// ensureLayout(); resize, font and state changes mark it dirty), and every
// cell is addressed by a string id — the surface the tests and the render
// harness drive:
//     back  fwd  hist  up | src  src.chev  base
//     [overflow]  crumb:<i>  chev:<i> ...  space  recent
//
// Overflow, applied in this order until the strip fits: shrink the base
// (elide the formula, then drop its resolved-address suffix; floor 90 px),
// elide the source (floor 80 px), fold crumbs into « from the ROOT (never
// the deepest), middle-elide the deepest to 60 px, drop `up` and `hist`,
// drop the trailing chevron. Each knob turns only as far as the overshoot
// needs — a 30-px squeeze costs the base three characters, not half of it.
// Below the ~395 px those six reach with a process source and a formula
// base, the narrow-pane steps keep only what names the place: the chip
// goes icon-only (the name moves to its tooltip), the base
// shows its bare literal elided to 60 px, Forward goes, and last Back and
// the divider go too. The deepest crumb and `recent` are never laid out past
// the right edge down to kNarrowFloorW (~230 px); the deepest crumb is
// never dropped at any width, because it is the one thing the bar exists
// to name.
//
// Chrome rules: the band is editorPaperColor (it sits inside the document
// column — paper, not a third chrome strip), the seam under it is one device
// row of containerBorderColor, hover is t.hover, the mouse-down flash is
// pressedFill(t), a cell whose menu is up stays t.hover,
// tones walk text > textDim > textMuted > textFaint, and there is no accent
// anywhere on the bar — purple is spent on document tabs and real selection,
// not on a navigation aid.
//
// Tooltips: the widget's toolTip property mirrors the hovered cell and the
// app's GlobalTooltipBridge shows it. QEvent::ToolTip is never handled here
// and nothing is dismissed on a hover change (the bridge repositions on the
// next MouseMove); doing both strobed the ribbon's tips before it learned
// the same rule (src/ribbon.cpp refreshToolTip).
//
// Landed so far: layout, paint, crumbs, the « menu, tooltips, context menus
// (P2a); the source chip with its liveness dot and the popup click, and the
// base segment with its resolved-address suffix (P2b); the base edit overlay
// with live validation, the resolved-address preview and the recent /
// bookmarks / modules menu, the recent cell and Up (P3); the chevron menus
// (a crumb's drillable fields, the other roots), the path edit with its
// completion menu, and Alt+D / Ctrl+L (P4); the nav cluster — Back /
// Forward / Up enabled per the controller's flags, the history menu on
// `hist` and, so it stays reachable when the narrow-width overflow drops
// `hist` and `up`, on a right-click or a press-and-hold of Back
// (QToolButton's DelayedPopup) — and keyboard mode (P5).
//
// Keyboard mode (F6 from the document, enterKeyboardMode()): the bar takes
// StrongFocus for as long as it lasts and walks a traversal list of the
// enabled, laid-out cells left → right (Left / Right / Tab / Shift+Tab,
// Home / End), starting at the deepest crumb — "you are here". Enter and
// Space synthesise a press + release at the focused cell's centre so the
// ordinary click path runs (menus included); Down opens the focused cell's
// menu when it has one; F2 opens the base edit on the base and the path
// edit on a crumb; Esc hands focus back to the document. A real mouse
// press anywhere on the bar, or focus leaving it for anything but a popup,
// ends the mode. The focused cell wears ONE device-exact 1-px ring in
// borderFocused — the same four edge fills every hairline here uses.
//
// The edits are the one place the bar owns a child widget: a hidden
// QLineEdit shown in one of two scopes. The BASE edit sits over the base
// segment (grown to kEditMinW) on the FULL formula, never the elided
// display text — the command row used to feed its ellipsis to the parser
// and silently no-op. The PATH edit spans from the base's right edge to
// the recent cell on the dotted trail ("RcxEditor.vptr.parent"); Down
// completes the last segment from the fields of the class the segments
// before it reach. Both wear PanelSearchField's interior rule
// (panelFieldInteriorQss); the bar paints the focus seam under the field
// itself, borderFocused while the text parses / resolves and markerError
// while it does not (or the controller refused it). Enter is the only
// commit (onBaseCommit / onPathCommit; the controller answers with
// baseCommitFinished / pathCommitFinished); Esc and losing focus restore,
// the Explorer / Goto-dialog rule. While the overlay is up the layout is
// frozen: a state push is stored and its relayout waits for the edit to
// end, and the cells the overlay covers stop answering to hover.
//
// The bar has no NodeTree. What its menus and the path edit need from the
// tree (a crumb's siblings, the roots, the fields a path lands in, whether
// a path resolves) arrives as TreeQueries the controller installs through
// the editor, pulled only when a menu opens or a key is typed.

#include "core.h"
#include "paintutil.h"
#include "rcxtooltip.h"
#include "sourcechooserpopup.h"   // iconForProvider / kindLabelFor
#include "svgicon.h"
#include "tab_source_icon.h"
#include "themes/thememanager.h"
#include "address_bar_model.h"
#include "chrome_fallback_theme.h"
#include "timeline_glyphs.h"          // Record / Stop / Back to live, painted
#include "nav_history.h"          // NavEntry — the history menu's rows
#include "dock_header.h"          // chromeFont
#include "fuzzy_match.h"          // the places menu filter
#include "gotoaddressdialog.h"    // loadRecent / clearRecent (the shared recent list)
#include "panel_search_field.h"   // kFieldHeight, panelFieldInteriorQss

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <utility>

namespace rcx {

namespace address_bar_detail {

// Sensible colours when ThemeManager has nothing loaded (tests, harnesses
// run from a directory without themes/). The same VS2022 Dark values
// RibbonBar falls back to, so the two strips agree even in a bare target.
inline Theme fallbackTheme() {
    return chromeFallbackTheme();
}

inline QString hex(uint64_t v) {
    return QStringLiteral("0x") + QString::number(v, 16).toUpper();
}

// A crumb label is "Class.field" for an ancestor and a bare "Class" for the
// deepest; these split it back. Class names never carry a dot, so the first
// one is the seam.
inline QString classHalf(const QString& label) {
    const int d = label.indexOf(QLatin1Char('.'));
    return d < 0 ? label : label.left(d);
}
inline QString fieldHalf(const QString& label) {
    const int d = label.indexOf(QLatin1Char('.'));
    return d < 0 ? QString() : label.mid(d + 1);
}

// The overlay field. A QLineEdit whose cursorRect() is reachable: the bar
// measures the inset Qt keeps inside the widget rather than assuming it, and
// that measurement is what makes an opening overlay land on the cell's own
// pixels. No signals, no slots, no moc.
struct EditField : QLineEdit {
    using QLineEdit::QLineEdit;
    using QLineEdit::cursorRect;
};

}  // namespace address_bar_detail

class AddressBar : public QWidget {
public:
    // The bar is a field: the same height as the house text field beside it
    // (the panel filter boxes), so the two read as one control family.
    static constexpr int kAddressBarHeight = 26;
    static_assert(kAddressBarHeight == PanelSearchField::kFieldHeight,
                  "the address bar must match PanelSearchField's height");

    // Outbound events. RcxEditor bridges each to a real signal; the phase a
    // callback goes live in is noted — until then its cell is painted and
    // its click is a no-op.
    struct Callbacks {
        std::function<void(int)>           onCrumb;          // ancestor crumb / « pick → collapse to index
        std::function<void()>              onCurrentCrumb;   // the deepest crumb: scroll its header up (non-mutating)
        std::function<void(QPoint)>        onSourceClick;    // chip click — global anchor: the bar's bottom edge under the chip
        std::function<void(int, uint64_t)> onSiblingPick;    // chev:<level> menu pick → switch the hop at that level
        std::function<void(QString)>       onBaseCommit;     // Enter in the base edit
        std::function<void(QString)>       onPathCommit;     // Enter in the path edit
        std::function<void(QString)>       onRecentPick;     // recent-places menu pick
        // The deepest crumb renamed in place: the class NODE id (Crumb::classId,
        // so a drilled crumb renames the class it names, not the view root) and
        // the new name. The controller answers through classRenameFinished.
        std::function<void(uint64_t, QString)> onClassRename;
        // The real class chooser — the filtered type popup line 0's chevron
        // opens. The bar used to carry a second, flat, unfiltered copy of it
        // on a chevron of its own; this is the crumb context menu's route to
        // the good one.
        std::function<void()>              onClassChooser;
        std::function<void()>              onBack;           // Back cell (also Alt+Left, XButton1)
        std::function<void()>              onForward;        // Forward cell
        std::function<void()>              onUp;             // Up cell — the parent crumb
        // History menu pick: negative = that many steps back, positive =
        // forward. Stepped, not indexed, so the controller's stacks end up
        // exactly as a run of single Back / Forward presses would leave them.
        std::function<void(int)>           onHistoryJump;
        std::function<void()>              onRefresh;        // source context menu
        std::function<void()>              onTimelineBackToLive;  // "Back to live" beside the address
        std::function<void()>              onTimelineRecord; // Record / Stop beside it
        std::function<void()>              onGotoDialog;     // recent menu "Go to address…" (Ctrl+G)
        // An edit ended from the keyboard (Enter accepted, Esc): the document
        // takes focus back. Not called when focus already went elsewhere.
        std::function<void()>              onFocusReturn;
        // What the base edit reads, pulled on demand (never per refresh):
        // the controller's formula evaluator — the same one the Scintilla
        // inline edit uses — returning "0x…" or "" when the text does not
        // resolve; the document's bookmarks; the source's cached module
        // names (Provider::modulesCached(), never the enumerating syscall).
        std::function<QString(const QString&)> evaluate;
        std::function<QVector<Bookmark>()>     bookmarks;
        std::function<QStringList()>           modules;
        // The history menu's rows, pulled when it opens: the controller's
        // Back and Forward stacks, oldest first (NavHistory's order — the
        // menu lists each nearest first).
        std::function<QVector<NavEntry>()>     backEntries;
        std::function<QVector<NavEntry>()>     forwardEntries;
        // The label the controller would record for the place the user is
        // at now (NavEntry::label's shape, "Trail.path  @ 0x…"): the
        // history menu's checked, disabled "you are here" row between the
        // Back rows and the Forward rows — Explorer's list.
        std::function<QString()>               currentLabel;
    };

    // What the chevron menus and the path edit read from the tree, pulled
    // on demand (a menu opening, a keystroke) — never per refresh. The bar
    // has no NodeTree; the controller answers through the editor. The
    // struct itself lives in the model header so editor.h can carry one
    // without this widget header.
    using TreeQueries = AddressBarTreeQueries;
    void setTreeQueries(TreeQueries q) { m_treeQ = std::move(q); }

    // The text a chevron / completion row shows: "field  →  Class", or the
    // field alone when the class repeats it (an auto-named pointer). One
    // rule, so the tests build their expectations from the same entries.
    static QString siblingActionText(const SiblingEntry& e) {
        return (e.classLabel.isEmpty() || e.classLabel == e.field)
            ? e.field
            : e.field + QStringLiteral("  ") + QChar(0x2192) + QStringLiteral("  ") + e.classLabel;
    }
    // Where the field leads, as the sibling menu says it: "@ 0x…", or
    // "@ (unreadable)" when the controller could not place it (no source,
    // a null or unreadable pointer, an unknown frame). The crumb tooltip's
    // words, so a row and the crumb it would become agree.
    static QString siblingAddressText(const SiblingEntry& e) {
        return e.address != 0 ? QStringLiteral("@ ") + address_bar_detail::hex(e.address)
                              : QStringLiteral("@ (unreadable)");
    }
    // A chevron-menu row: the text above plus, after a tab, the address
    // when it is known. QMenu lays the part after a tab out in its
    // shortcut column — right-aligned, clear of the labels — for free (the
    // places menu's "Go to address…\tCtrl+G" already relies on it). It is
    // NOT one tone down: the style paints that column in the item's own
    // pen, and dimming it would take a QStyle of the bar's own, which is
    // not worth a suffix. An unknown address is left to the tooltip, so
    // a column of "(unreadable)" never crowds a menu under a dead source.
    static QString siblingActionRowText(const SiblingEntry& e) {
        return e.address != 0 ? siblingActionText(e) + QLatin1Char('\t') + siblingAddressText(e)
                              : siblingActionText(e);
    }
    explicit AddressBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("rcxAddressBar"));
        // NoFocus at rest: a click on the bar must not steal focus from the
        // document (P5's keyboard mode takes StrongFocus only while it is on).
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kAddressBarHeight);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        // The bar is chrome, so it wears the chrome face — the user's editor
        // family at 10 pt — not whatever font the parent happens to have.
        setFont(chromeFont());
        m_theme = ThemeManager::instance().current();
        if (!m_theme.background.isValid() || !m_theme.text.isValid())
            m_theme = address_bar_detail::fallbackTheme();
        // The one child widget: the base-edit overlay, hidden at rest. A
        // plain QLineEdit in PanelSearchField's interior (see
        // panelFieldInteriorQss) — no frame, no box: the bar paints the
        // focus seam under the field the way PanelSearchField paints its
        // own, so the two fields ring alike. Its keys and focus are read
        // through eventFilter (a virtual, so no Q_OBJECT is needed).
        m_edit = new address_bar_detail::EditField(this);
        m_edit->setObjectName(QStringLiteral("rcxAddressBarEdit"));
        m_edit->setFrame(false);
        m_edit->setFont(font());
        m_edit->hide();
        m_edit->installEventFilter(this);
        connect(m_edit, &QLineEdit::textChanged, this, [this](const QString&) { onEditTextChanged(); });
        applyEditStyle();
        // Press-and-hold on Back opens the history list (QToolButton's
        // DelayedPopup): armed by the press, disarmed by the release or the
        // pointer leaving; a quick click stays a plain Back.
        m_holdTimer.setSingleShot(true);
        connect(&m_holdTimer, &QTimer::timeout, this, [this] { onHoldElapsed(); });
        // RcxTooltip expires itself (capped at 20 s by expiryMs), and a base
        // formula can take longer than that to type. keepAlive() only re-arms
        // a tooltip that is already showing, so a stray tick is harmless.
        m_helpKeepAlive.setInterval(kHelpKeepAliveMs);
        connect(&m_helpKeepAlive, &QTimer::timeout, this,
                [this] { if (m_helpTip) m_helpTip->keepAlive(); });
    }

    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── Keyboard mode ──
    // F6 lands here. StrongFocus for the duration; the focused cell is
    // named by id (the same namespace itemRect() speaks) and walked over
    // traversalIds(). An open edit is dropped first: one focus owner.
    void enterKeyboardMode() {
        if (m_editVisible) endEdit();
        const QStringList ids = traversalIds();
        if (ids.isEmpty()) return;
        m_kbFocusId = deepestCrumbId(ids);
        m_kbMode = true;   // before setFocus: focusInEvent hands focus back otherwise
        setFocusPolicy(Qt::StrongFocus);
        setFocus(Qt::OtherFocusReason);
        m_hoverId.clear();
        refreshToolTip();
        dismissRcxTooltip();
        update();
    }
    // Esc (returnFocus: the document takes focus back), or a mouse press /
    // focus leaving (the new owner already has it).
    void leaveKeyboardMode(bool returnFocus) {
        if (!m_kbMode) return;
        m_kbMode = false;
        m_kbFocusId.clear();
        setFocusPolicy(Qt::NoFocus);
        update();
        if (returnFocus && m_cb.onFocusReturn) m_cb.onFocusReturn();
    }
    bool inKeyboardMode() const { return m_kbMode; }
    QString focusId() const { return m_kbMode ? m_kbFocusId : QString(); }
    // The cells keyboard mode walks, left → right: every laid-out, ENABLED
    // cell but the stretch and the chip's chevron (the chip is one stop —
    // both open the same popup). The divider is not a cell.
    QStringList traversalIds() const {
        ensureLayout();
        QStringList ids;
        for (const LaidItem& li : m_layout.items) {
            if (!li.enabled || li.kind == Cell::Space || li.kind == Cell::SrcChev) continue;
            ids << li.id;
        }
        return ids;
    }
    // Test hooks for the Back cell's hold-to-open: the delay (500 ms, the
    // platform's DelayedPopup feel) and how many times the history menu
    // has opened — a popup on the hidden test desktop is closed at the
    // first event pump, so "did it open" is counted rather than seen.
    // What a laid-out cell reads ("" when it is not laid out) — the chips' words.
    QString cellText(const QString& id) const {
        const LaidItem* li = itemById(id);
        return li ? li->text : QString();
    }
    void setHoldDelayMs(int ms) { m_holdDelayMs = qMax(1, ms); }
    int  holdDelayMs() const { return m_holdDelayMs; }
    int  historyMenuOpenCount() const { return m_historyMenuOpens; }
    static constexpr int kHoldDelayMs = 500;

    // ── Edits (two scopes, one overlay) ──
    // Base   — the address formula.
    // Path   — the whole trail as one dotted string.
    // Class  — the name of the class the deepest crumb names, renamed in
    //          place. The bar names the class you are looking at, so that
    //          is where its name is edited; before this the segment was the
    //          one thing on the bar you could see but not touch.
    enum class EditScope { None, Base, Path, Class };

    // BASE: open the overlay over the base segment, grown to at least
    // kEditMinW, on the FULL formula (or "0x…") with everything selected.
    // Enter → onBaseCommit; the controller answers through
    // baseCommitFinished. Esc and losing focus restore (revert, never
    // commit). The bar holds StrongFocus only while the overlay is up. A
    // path edit in progress is dropped first (one overlay, one scope).
    void beginBaseEdit() {
        if (m_editVisible && m_editScope == EditScope::Base) { m_edit->setFocus(); m_edit->selectAll(); return; }
        if (m_editVisible) endEdit();
        const QString text = baseFullText();
        QRect r;
        int coveredLeft = 0;
        if (!editGeometryFor(EditScope::Base, text, &r, &coveredLeft)) return;
        openEdit(EditScope::Base, r, coveredLeft, text);
        showEditHelp();
    }

    // CLASS: the deepest crumb turned into a field on its own name, all
    // selected. It is the deepest crumb and no other because that is the only
    // crumb whose label is a bare class name — ancestors read "Class.field"
    // (src/controller.cpp addressBarState), which is not a thing you can
    // rename — and because the deepest crumb is never folded away, so the
    // target cannot vanish at a narrow width the way crumb:0 can. The click
    // that used to scroll the class header to the top of the pane now opens
    // this; the scroll is on the crumb's context menu.
    void beginClassNameEdit() {
        if (m_editVisible && m_editScope == EditScope::Class) { m_edit->setFocus(); m_edit->selectAll(); return; }
        if (m_state.crumbs.isEmpty()) return;
        if (m_editVisible) endEdit();
        const Crumb& c = m_state.crumbs.last();
        if (c.classId == 0) return;         // a crumb whose class did not resolve
        const QString text = address_bar_detail::classHalf(c.label);
        QRect r;
        int coveredLeft = 0;
        if (!editGeometryFor(EditScope::Class, text, &r, &coveredLeft)) return;
        m_classEditId = c.classId;          // pinned: the state may be pushed under the overlay
        openEdit(EditScope::Class, r, coveredLeft, text);
    }

    // The overlay's rect for `scope` at the CURRENT layout, and the left
    // edge of what it covers. One rule for opening an edit and for
    // following a pane resize while one is up (resizeEvent), so the
    // field sits where a fresh open would put it at the new width. False
    // when the base is not laid out (an empty bar).
    // Width the field needs to hold `text` whole: the cell's left padding,
    // the text, and the right inset the caret lives in. For a cell's OWN text
    // this is the cell's own width to the pixel (kBasePad + advance +
    // kBasePad), which is the point — opening an edit must not resize
    // anything. Measured on the FULL string: the base cell middle-elides its
    // display at kBaseMaxW but the edit always opens on the whole formula, so
    // a formula too long to have been shown whole is the one case where the
    // field is wider than the cell, because you cannot edit what you cannot
    // see.
    int editFitWidth(EditScope scope, const QString& text) const {
        const QFontMetrics fm(editFont(scope));
        return kBasePad + fm.horizontalAdvance(text) + baseSepGap();
    }

    // The face the overlay wears: the cell's own. The deepest crumb is
    // painted DemiBold from depth 2 (the tone ladder's single weight step),
    // and a field that opened on it in the regular face would re-set the text
    // a pixel narrower and a shade lighter the instant you clicked — the same
    // class of tell as a box that resizes.
    QFont editFont(EditScope scope) const {
        const bool deepIsBold = m_state.crumbs.size() > 1;
        return (scope == EditScope::Class && deepIsBold) ? deepestFont() : font();
    }
    QFont editFont() const { return editFont(m_editScope); }

    // The width an overlay holding `text` should have: the text's, and
    // nothing else. There is deliberately NO minimum — a short value gets a
    // short field because the CELL it replaces is exactly that short, and the
    // whole contract is that opening an edit resizes nothing. (A floor of 90
    // lived here and was the last thing still inflating a field on click: a
    // seven-letter class name opened 22 px wider than its crumb.)
    //
    // While an edit is open m_editGrownW is a floor, so the field can GROW as
    // you type and never shrink back: one that resized on every keystroke in
    // both directions would jiggle under the caret and drag the preview with
    // it, and backspacing would reflow the box you are typing in. Growing is
    // for text you ADD. Opening grows nothing.
    int editWidthFor(EditScope scope, const QString& text, int left, int limit) const {
        const int want = qMax(editFitWidth(scope, text), m_editVisible ? m_editGrownW : 0);
        // The one hard bound: never so wide that the preview after it has
        // nowhere to go. Past this the QLineEdit scrolls internally, which is
        // the honest answer for a formula longer than the strip.
        const int room = qMax(kEditMinW, limit - left - kEditPreviewMinW);
        return qMin(want, room);
    }

    bool editGeometryFor(EditScope scope, const QString& text,
                         QRect* r, int* coveredLeft) const {
        const int limit = editLimit();
        if (scope == EditScope::Class) {
            // Over the crumb itself, fitted to the name. Anchored on the
            // crumb and not on the base, so this arm is the one that works
            // on a bar with no address laid out at all.
            const QRect cell = itemRect(deepestCrumbId());
            if (cell.isNull()) return false;
            QRect g = cell;
            g.setWidth(editWidthFor(scope, text, g.left(), limit));
            if (g.right() >= limit) {
                g.setRight(limit - 1);
                // CLIPPED by the strip's edge — not merely short. Only then is
                // it worth covering `recent` to keep the field usable; a short
                // name gets a short field, because that is the cell.
                if (g.width() < kBaseMinW) g.setWidth(kBaseMinW);
            }
            *r = g;
            *coveredLeft = g.left();
            return true;
        }
        const QRect base = itemRect(QStringLiteral("base"));
        if (base.isNull()) return false;
        if (scope == EditScope::Base) {
            // The base cell resized to its value, stopping short of the
            // recent cell. The crumbs under it are simply covered — the
            // layout is frozen while the overlay is up, so nothing shifts.
            QRect g = base;
            g.setWidth(editWidthFor(scope, text, g.left(), limit));
            if (g.right() >= limit) {
                g.setRight(limit - 1);
                if (g.width() < kBaseMinW) g.setWidth(kBaseMinW);   // clipped by a narrow strip: cover `recent` rather than shrink to nothing
            }
            *r = g;
            *coveredLeft = g.left();
            return true;
        }
        const int covered = base.right() + 1;
        // The field's own left inset IS kCrumbPad (syncEditTextMargins), and
        // the first crumb's cell starts here — so the field starts here too
        // and the trail's first glyph does not move when the overlay opens.
        QRect g(covered, kCellTop, 0, kCellH);
        // Fitted like the base one. The trail behind it is covered either way
        // (m_coveredRect still runs to the recent cell), so the only thing the
        // old full-bleed width bought was distance between the path and the
        // resolver's answer painted after it.
        // kEditMinW IS a floor here, and only here: this scope stands in for
        // the whole trail rather than for one cell, so there is no cell width
        // to match — and a trail that is currently empty would otherwise open
        // as a 12-px sliver. Everything to its right is covered anyway, so a
        // floor costs nothing visible.
        g.setWidth(qMax(kEditMinW, editWidthFor(scope, text, g.left(), limit)));
        if (g.right() >= limit) g.setRight(limit - 1);
        // A narrow strip: keep the field usable — grow back over the base,
        // then past the recent cell, before shrinking below the minimum.
        if (g.width() < kEditMinW) g.setLeft(qMax(base.left(), limit - kEditMinW));
        if (g.width() < kBaseMinW) g.setWidth(kBaseMinW);
        *r = g;
        *coveredLeft = qMin(covered, g.left());
        return true;
    }

    // PATH: the overlay from the base's right edge to the recent cell on
    // the dotted trail (state().trailPath, "RcxEditor.vptr.parent"), all
    // selected; the first glyph lands where the root crumb's did, so the
    // trail reads as having turned editable in place. Down completes the
    // last segment; Enter → onPathCommit; the controller answers through
    // pathCommitFinished. Alt+D and Ctrl+L in the document, or a click on
    // the empty stretch, land here.
    void beginPathEdit() {
        if (m_editVisible && m_editScope == EditScope::Path) { m_edit->setFocus(); m_edit->selectAll(); return; }
        if (m_editVisible) endEdit();
        QRect r;
        int coveredLeft = 0;
        if (!editGeometryFor(EditScope::Path, m_state.trailPath, &r, &coveredLeft)) return;
        openEdit(EditScope::Path, r, coveredLeft, m_state.trailPath);
    }

    // The cell the class-name edit opens over. Empty on a bar with no
    // crumbs; never a folded one (the deepest is never folded).
    QString deepestCrumbId() const {
        const int n = m_state.crumbs.size();
        return n > 0 ? QStringLiteral("crumb:%1").arg(n - 1) : QString();
    }

    bool isEditing() const { return m_editVisible; }
    bool isPathEditing() const { return m_editVisible && m_editScope == EditScope::Path; }
    bool isClassNameEditing() const { return m_editVisible && m_editScope == EditScope::Class; }
    bool isBaseEditing() const { return m_editVisible && m_editScope == EditScope::Base; }
    EditScope editScope() const { return m_editVisible ? m_editScope : EditScope::None; }
    QString editText() const { return m_edit->text(); }
    QLineEdit* editWidget() const { return m_edit; }       // test hook
    QRect editRect() const { return m_editVisible ? m_editRect : QRect(); }
    // The base-address grammar help (see showEditHelp) is on screen.
    bool editHelpVisible() const { return m_helpTip && m_helpTip->isVisible(); }
    // Everything the open edit hides: the overlay plus the paper painted
    // from it to the recent cell. Cells under it stop answering to hover.
    QRect editCoveredRect() const { return m_editVisible ? m_coveredRect : QRect(); }
    // The seam colour under the field while editing: borderFocused while
    // the text parses / resolves and no commit was refused, markerError
    // otherwise.
    bool editTextValid() const { return m_editValid && m_commitError.isEmpty(); }
    // What is painted after the overlay: "= 0x…" or the parser's words.
    QString editPreviewText() const { return m_editPreview; }

    // The controller's verdict on the last onBaseCommit / onPathCommit. A
    // success closes the overlay and applies the state push the navigation
    // produced (deferred while the overlay was up); a refusal keeps it
    // open, seam markerError, `err` where the preview goes (when there is
    // room), until the text changes.
    void baseCommitFinished(bool ok, const QString& err = QString()) { commitFinished(ok, err); }
    void pathCommitFinished(bool ok, const QString& err = QString()) { commitFinished(ok, err); }
    void classRenameFinished(bool ok, const QString& err = QString()) { commitFinished(ok, err); }
    void commitFinished(bool ok, const QString& err) {
        if (!m_editVisible) return;
        if (ok) {
            endEdit();
            if (m_cb.onFocusReturn) m_cb.onFocusReturn();
            return;
        }
        m_commitError = err.isEmpty() ? QStringLiteral("refused") : err;
        m_editPreview = m_commitError;
        m_edit->setFocus();
        update();
    }

    // One snapshot per refresh. Equal states are free; a changed one relays
    // out lazily. While the inline edit overlay is up (P3) the state is
    // stored but the relayout waits for commit / cancel, so a live tick can
    // never stomp what the user is typing.
    void setState(const AddressBarState& s) {
        if (s == m_state) return;
        m_state = s;
        ++m_stateApplyCount;
        if (m_editVisible) m_relayoutDeferred = true;
        else               markLayoutDirty();
        refreshToolTip();   // the hovered cell may have moved or gone
        reconcileKeyboardFocus();   // ...and so may the keyboard-focused one
        update();
    }

    void applyTheme(const Theme& t) {
        m_theme = t;
        if (!m_theme.background.isValid() || !m_theme.text.isValid())
            m_theme = address_bar_detail::fallbackTheme();
        // A font change writes the setting and then re-themes, so this is
        // where the bar picks the new chrome face up (PanelSearchField does
        // the same). setFont → changeEvent(FontChange) → markLayoutDirty.
        setFont(chromeFont());
        m_edit->setFont(editFont());
        applyEditStyle();
        // A theme or font change while the field is open re-shows the help in
        // the new colours rather than leaving the old ones on screen.
        if (editHelpVisible()) showEditHelp();
        update();
    }
    const Theme& theme() const { return m_theme; }

    // Liveness alone changes no geometry (the chip's dot and icon opacity),
    // so it bypasses the relayout the full state push would trigger. Fed by
    // the controller's sourceStatusChanged, so the dot turns the moment a
    // read fails rather than on the next refresh tick.
    void setLiveness(int liveness) {
        if (m_state.liveness == liveness) return;
        m_state.liveness = liveness;
        refreshToolTip();
        update();
    }

    // The source chooser is the one dropdown the bar does not own (the
    // controller opens the shared SourceChooserPopup), so the controller
    // says when it is up and when it went away; in between the chip stays
    // t.hover, exactly like a cell with its own QMenu open.
    void setSourceMenuOpen(bool open) {
        const QString id = QStringLiteral("src");
        if (open) { if (m_menuOpenId == id) return; m_menuOpenId = id; }
        else      { if (m_menuOpenId != id) return; m_menuOpenId.clear(); }
        update();
    }

    // ── Geometry queries (the test + harness surface) ──
    QString itemIdAt(QPoint pos) const {
        ensureLayout();
        // While an edit is up, what it covers is not there: the layout is
        // frozen, so the cells still exist, but a crumb hidden under the
        // paper must not offer a hand cursor and a tooltip over nothing.
        if (m_editVisible && m_coveredRect.contains(pos)) return QString();
        for (const LaidItem& li : m_layout.items)
            if (li.rect.contains(pos)) return li.id;
        return QString();
    }
    QRect itemRect(const QString& id) const {   // null when not laid out
        const LaidItem* li = itemById(id);
        return li ? li->rect : QRect();
    }

    // ── Test hooks ──
    // The rendered trail left→right: « when crumbs are folded behind it,
    // then the visible crumb labels (full, not the elided display text).
    QStringList segments() const {
        ensureLayout();
        QStringList out;
        if (m_layout.fold > 0) out << QStringLiteral("«");
        for (int i = m_layout.fold; i < m_state.crumbs.size(); ++i)
            out << m_state.crumbs[i].label;
        return out;
    }
    // The crumbs the « menu would list, top-down (root first).
    QStringList overflowMenuLabels() const {
        ensureLayout();
        QStringList out;
        for (int i = 0; i < m_layout.fold && i < m_state.crumbs.size(); ++i)
            out << m_state.crumbs[i].label;
        return out;
    }
    const AddressBarState& state() const { return m_state; }
    int stateApplyCount() const { return m_stateApplyCount; }

    // The dotted path down to crumb i — "RcxEditor.vptr.parent" for the third
    // crumb of "RcxEditor.vptr › QWidgetPrivate.parent › QWidget": the class
    // half of the root crumb, then the field half of every crumb above i.
    // Built from the labels the controller emitted, so it agrees with
    // trailPathText() without the bar knowing the tree. What "Copy path"
    // puts on the clipboard.
    QString crumbPathText(int i) const {
        using namespace address_bar_detail;
        if (m_state.crumbs.isEmpty()) return QString();
        QString out = classHalf(m_state.crumbs[0].label);
        for (int k = 0; k < i && k < m_state.crumbs.size(); ++k) {
            const QString f = fieldHalf(m_state.crumbs[k].label);
            if (!f.isEmpty()) out += QLatin1Char('.') + f;
        }
        return out;
    }

    // What the chip and the base segment actually DRAW — elided to their
    // width budgets — as opposed to the full strings in state(). The bug
    // these pin: the command row used to elide a long formula for display
    // and then edit (and evaluate) the elided form, ellipsis and all.

    // The one glyph that says "this formula evaluates to". It is `=`, and
    // deliberately not an arrow: this same bar carries ← and → as its Back and
    // Forward buttons, so a third arrow mid-strip read as one more direction
    // rather than as arithmetic. It cannot be misread as the › that separates
    // crumbs either. One space after it; the space BEFORE it is baseSepGap.
    static QString baseSepText() { return QStringLiteral("= "); }

    // The gap between a formula and its `=`. One space wide — so the phrase is
    // evenly spaced — but GEOMETRY rather than a space inside the string,
    // because it is also the room the caret needs when the field opens over
    // the formula. Reserved at rest, it means the suffix is already standing
    // where the preview will be, and clicking in moves it by nothing.
    int baseSepGap() const {
        return QFontMetrics(font()).horizontalAdvance(QLatin1Char(' '));
    }

    QString sourceDisplayText() const {
        const LaidItem* li = itemById(QStringLiteral("src"));
        return li ? li->text : QString();
    }
    QString baseDisplayText() const {
        const LaidItem* li = itemById(QStringLiteral("base"));
        return li ? li->text : QString();
    }
    // The " = 0xADDR" painted after the formula, or empty beside a bare
    // literal (there it would only repeat the number). Separate from
    // baseDisplayText because the two are drawn in different tones.
    QString baseSuffixShown() const {
        const LaidItem* li = itemById(QStringLiteral("base"));
        return li ? li->text2 : QString();
    }
    // The chip's icon square and its liveness dot, logical px. The dot rect
    // is null when nothing is painted there (static file, no source).
    QRect sourceIconRect() const {
        const LaidItem* li = itemById(QStringLiteral("src"));
        return li ? chipIconRect(li->rect) : QRect();
    }
    QRect livenessDotRect() const {
        const QRect icon = sourceIconRect();
        return (icon.isNull() || !livenessDotColor().isValid()) ? QRect() : dotRectFor(icon);
    }

    // ── Metrics (logical px; the strip is 26 tall, cells 22 tall at y 2) ──
    static constexpr int kNavBtnW      = 22;
    static constexpr int kHistW        = 14;
    static constexpr int kNavGap       = 2;
    static constexpr int kNavIconPx    = 14;
    static constexpr int kChevIconPx   = 12;
    static constexpr int kDividerPad   = 6;    // either side of the field divider
    static constexpr int kChipPad      = 4;
    static constexpr int kChipIconPx   = 16;
    static constexpr int kChipTextGap  = 4;
    static constexpr int kChipChevGap  = 2;
    static constexpr int kDotPx        = 6;    // liveness dot on the chip icon
    static constexpr int kDotRing      = 1;    // paper ring that lifts it off the icon
    static constexpr int kChevW        = 14;   // every chevron cell
    static constexpr int kSrcMaxW      = 160;
    static constexpr int kSrcMinW      = 80;
    static constexpr int kBasePad      = 6;
    static constexpr int kBaseMaxW     = 200;
    static constexpr int kBaseMinW     = 90;
    static constexpr int kCrumbPad     = 6;
    static constexpr int kDeepestMinW  = 60;
    static constexpr int kOverflowW    = 18;
    static constexpr int kRecentW      = 16;
    static constexpr int kTlChipPad    = 6;    // either side of a timeline chip's ink
    static constexpr int kTlGlyphW     = 10;   // the chip's dot / clock / square
    static constexpr int kTlGlyphGap   = 5;    // glyph to words
    static constexpr int kRightMargin  = 6;
    static constexpr int kCellTop      = 2;
    static constexpr int kCellH        = 22;
    static constexpr int kBaseBareMinW = 60;   // the bare literal, narrow-pane step 7c
    // NOT a floor on the field's width (see editWidthFor — the field is the
    // size of its cell, full stop). This is the narrow-STRIP fallback: the
    // width below which the geometry arms stop trying to keep the overlay
    // clear of its neighbours and start growing it back over them instead.
    static constexpr int kEditMinW     = 90;
    // The overlay is the CELL, to the pixel. Its left inset is the cell's own
    // kBasePad, so the first glyph lands on the device column the cell painted
    // it on; its width is kBasePad + the text + baseSepGap(), and a cell with
    // no suffix is exactly that wide, so clicking in resizes nothing. The
    // caret lives in the gap, minus this — the field's own right padding, the
    // only part of the box the text may not use.
    static constexpr int kEditRightInset = 2;
    static constexpr int kHelpKeepAliveMs = 5000;   // well inside RcxTooltip's 20 s cap
    static constexpr int kEditPreviewMinW = 48; // room the preview beside the overlay needs to be worth painting
    static constexpr double kDisabledOpacity = 0.40;
    // Where the narrow-pane steps bottom out with a folded trail: the chip's
    // icon on the gutter, its chevron, the bare base, «, the deepest crumb
    // at its floor and `recent`. Down to this width the deepest crumb and
    // `recent` end inside the strip; narrower, nothing can hold them.
    static constexpr int kNarrowFloorW =
        (kGutter - kChipPad) + (kChipPad + kChipIconPx + kChipChevGap) + kChevW + kChipPad
        + (kBasePad + kBaseBareMinW + kBasePad) + kOverflowW
        + (kCrumbPad + kDeepestMinW + kCrumbPad) + kRecentW + kRightMargin;

protected:
    void paintEvent(QPaintEvent*) override {
        ensureLayout();
        const Theme& t = m_theme;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), editorPaperColor(t));
        // The field divider: one device column between the nav cluster and
        // the field, inset 4 px top and bottom so it reads as a separator
        // inside the strip rather than a cut through it.
        if (m_layout.dividerX >= 0)
            fillLeftDeviceColOfRect(p, QRectF(m_layout.dividerX, 4, 1, kAddressBarHeight - 8),
                                    containerBorderColor(t));
        // ...and its mirror before the timeline's buttons.
        if (m_layout.tlDividerX >= 0)
            fillLeftDeviceColOfRect(p, QRectF(m_layout.tlDividerX, 4, 1, kAddressBarHeight - 8),
                                    containerBorderColor(t));
        for (const LaidItem& li : m_layout.items) paintItem(p, li);
        // Keyboard focus: ONE device-exact 1-px ring around the focused
        // cell in borderFocused — the four edge fills, so it is one row /
        // one column at every scale (a QPen rect would be two at 125 %).
        if (m_kbMode && !m_kbFocusId.isEmpty()) {
            const QRect fr = itemRect(m_kbFocusId);
            if (!fr.isNull()) {
                const QRectF f(fr);
                fillTopDeviceRowOfRect(p, f, t.borderFocused);
                fillBottomDeviceRowOfRect(p, f, t.borderFocused);
                fillLeftDeviceColOfRect(p, f, t.borderFocused);
                fillRightDeviceColOfRect(p, f, t.borderFocused);
            }
        }
        // The seam: one device row, never a QSS border.
        fillBottomDeviceRowOfRect(p, QRectF(rect()), containerBorderColor(t));
        if (m_editVisible) paintEditChrome(p);
    }

    bool eventFilter(QObject* obj, QEvent* e) override {
        if (obj == m_edit && m_editVisible) {
            if (e->type() == QEvent::KeyPress) {
                auto* ke = static_cast<QKeyEvent*>(e);
                switch (ke->key()) {
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    commitEdit();
                    return true;
                case Qt::Key_Escape:
                    endEdit();
                    if (m_cb.onFocusReturn) m_cb.onFocusReturn();
                    return true;
                case Qt::Key_Down:
                    // The scope's list: places for the base, the fields of
                    // the class the typed path reaches for the path.
                    if (m_editScope == EditScope::Path)      showPathCompletionMenu();
                    else if (m_editScope == EditScope::Base) showPlacesMenu(true);
                    // Class: no list. A rename is a name, not a pick.
                    return true;
                default:
                    break;
                }
            } else if (e->type() == QEvent::FocusOut) {
                // A popup taking focus (the places menu, the source chooser)
                // or the window deactivating is not the user leaving the
                // field. Anything else is — a click elsewhere, Tab — and
                // reverts: Enter is the only commit.
                const Qt::FocusReason why = static_cast<QFocusEvent*>(e)->reason();
                if (why != Qt::PopupFocusReason && why != Qt::ActiveWindowFocusReason
                    && why != Qt::MenuBarFocusReason) {
                    // A click-away ends the edit. Qt moves focus BEFORE it
                    // delivers the press, so by the time mousePressEvent
                    // runs the overlay is gone, the layout has thawed, and
                    // the point may sit on a cell the user never saw (a
                    // chevron that was under the paper). A press THERE is
                    // spent — mousePressEvent keeps only its click-in-the-
                    // same-field case. A press on a cell that stayed
                    // visible beside the overlay (recent, the chip, the
                    // nav buttons) is a click the user aimed: it closes
                    // the edit AND acts, in one go. The covered rect is
                    // read here because endEdit() clears it.
                    const QRect covered = m_coveredRect;
                    endEdit();
                    if (why == Qt::MouseFocusReason) {
                        m_swallowNextPress = true;
                        m_swallowRect = covered;
                    }
                }
            }
        }
        return QWidget::eventFilter(obj, e);
    }

    // The bar is focusable only while editing or in keyboard mode; a click
    // on it while editing ends the edit (the overlay's FocusOut) and lands
    // focus here, where it is no use — hand it back to the document. In
    // keyboard mode focus arriving (F6, a menu giving it back) is the point.
    void focusInEvent(QFocusEvent* e) override {
        QWidget::focusInEvent(e);
        if (!m_editVisible && !m_kbMode && m_cb.onFocusReturn) m_cb.onFocusReturn();
    }

    void focusOutEvent(QFocusEvent* e) override {
        QWidget::focusOutEvent(e);
        // A menu opened from the keyboard (Down, Enter on a chevron) takes
        // focus and hands it back when it hides; the window deactivating
        // is not the user leaving. Anything else — a click elsewhere, a
        // Tab out of the strip — ends keyboard mode; the new owner keeps
        // the focus it already took.
        if (m_kbMode && e->reason() != Qt::PopupFocusReason
            && e->reason() != Qt::ActiveWindowFocusReason && e->reason() != Qt::MenuBarFocusReason)
            leaveKeyboardMode(false);
    }

    // Tab / Shift+Tab: QWidget::event runs the focus chain BEFORE
    // keyPressEvent sees the key, so the walk is claimed here; outside
    // keyboard mode the chain works as it always did.
    bool focusNextPrevChild(bool next) override {
        if (!m_kbMode) return QWidget::focusNextPrevChild(next);
        stepKeyboardFocus(next ? +1 : -1);
        return true;
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (!m_kbMode) { QWidget::keyPressEvent(e); return; }
        const QStringList ids = traversalIds();
        switch (e->key()) {
        case Qt::Key_Left:   stepKeyboardFocus(-1); break;
        case Qt::Key_Right:  stepKeyboardFocus(+1); break;
        case Qt::Key_Home:   if (!ids.isEmpty()) setKeyboardFocus(ids.first()); break;
        case Qt::Key_End:    if (!ids.isEmpty()) setKeyboardFocus(ids.last());  break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            // The ordinary click path, menus included — one activation
            // contract for mouse and keyboard.
            synthesiseClick(m_kbFocusId);
            break;
        case Qt::Key_Down:
            openMenuFor(m_kbFocusId);
            break;
        case Qt::Key_F2: {
            const LaidItem* li = itemById(m_kbFocusId);
            if (li && li->kind == Cell::Base)            beginBaseEdit();
            else if (li && li->kind == Cell::Crumb)      isDeepest(*li) ? beginClassNameEdit()
                                                                        : beginPathEdit();
            break;
        }
        case Qt::Key_Escape:
            leaveKeyboardMode(true);
            break;
        default:
            QWidget::keyPressEvent(e);
            return;
        }
        e->accept();
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        markLayoutDirty();
        // A pane resize while an edit is up: the cells re-lay out at the
        // new width (ensureLayout keys on it), so the overlay follows them
        // — the field rect a fresh open would take now, for the scope
        // that is open. The text, the caret and the selection are the
        // user's and stay untouched; only geometry moves. Without this
        // the overlay kept its opening rect while the base cell and the
        // recent cell went elsewhere.
        if (m_editVisible) followEditGeometry();
        // A narrower pane can drop the focused cell (Record, Forward…):
        // keyboard focus moves to a cell that is still there.
        else if (m_kbMode) reconcileKeyboardFocus();
    }

    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
            markLayoutDirty();
            // A font change while an edit is up re-measures every cell; the
            // overlay follows the field it covers, as it does on a resize.
            if (m_editVisible) { m_edit->setFont(editFont()); followEditGeometry(); }
        }
        // The window going inactive does NOT end the edit (ActiveWindowFocus
        // is one of the reasons the overlay survives), but the help is a
        // top-level tooltip: left up, it would sit over whatever the user
        // switched to. It comes back with the window.
        if (e->type() == QEvent::ActivationChange && m_editVisible
            && m_editScope == EditScope::Base) {
            if (isActiveWindow()) showEditHelp();
            else                  dismissEditHelp();
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        // The pointer moved: whatever press ended an edit is history. (A
        // focus-out caused by a press on another widget also arms the
        // flag; the pointer has to travel back here before it can press.)
        m_swallowNextPress = false;
        updateHover(e->pos());
        QWidget::mouseMoveEvent(e);
    }

    void mousePressEvent(QMouseEvent* e) override {
        // A real press anywhere on the bar ends keyboard mode (the mouse
        // is driving now); the press synthesised by Enter is not one.
        if (m_kbMode && !m_synthPress) leaveKeyboardMode(true);
        if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
        dismissRcxTooltip();
        const QString id = itemIdAt(e->pos());
        if (m_swallowNextPress) {
            // This press already ended an edit (see eventFilter). Under
            // the paper the overlay covered it is spent — except on the
            // field's own text cells: a click on the base or the empty
            // stretch while editing is the user clicking elsewhere in the
            // same field, and the edit re-opens there in that cell's
            // scope (the release does it). Menus and nav never fire off
            // a press whose target the user did not see; a cell that was
            // visible the whole time answers as it always does.
            m_swallowNextPress = false;
            const bool wasCovered = m_swallowRect.contains(e->pos());
            m_swallowRect = QRect();
            const LaidItem* li = itemById(id);
            const bool editable = li && (li->kind == Cell::Base || li->kind == Cell::Space
                                         || (li->kind == Cell::Crumb && isDeepest(*li)));
            if (wasCovered && !editable) {
                e->accept();
                return;
            }
        }
        if (id == QLatin1String("overflow")) {
            // Menus open on press (like every other dropdown in the app);
            // the cell stays t.hover through m_menuOpenId until it hides.
            m_pressedId.clear();
            showOverflowMenu();
            e->accept();
            return;
        }
        if (id == QLatin1String("recent")) {
            m_pressedId.clear();
            showPlacesMenu(false);
            e->accept();
            return;
        }
        if (id == QLatin1String("hist")) {
            m_pressedId.clear();
            if (const LaidItem* li = itemById(id); li && li->enabled) showHistoryMenu(id);
            e->accept();
            return;
        }
        if (const LaidItem* li = itemById(id); li && li->kind == Cell::Chev) {
            m_pressedId.clear();
            showSiblingMenu(li->index);
            e->accept();
            return;
        }
        if (!id.isEmpty()) {
            m_pressedId = id;
            // Holding Back opens the history list — whenever there is one,
            // even with Back itself disabled (only Forward entries left).
            if (id == QLatin1String("back") && historyAvailable())
                m_holdTimer.start(m_holdDelayMs);
            update();
        }
        e->accept();
    }

    // Press-then-release on the SAME cell is a click; dragging off cancels.
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
        m_holdTimer.stop();
        if (m_holdFired) {
            // The hold already opened the menu: this release is its end,
            // not a click on Back.
            m_holdFired = false;
            e->accept();
            return;
        }
        if (!m_pressedId.isEmpty()) {
            const QString id = m_pressedId;
            m_pressedId.clear();
            update();
            if (itemIdAt(e->pos()) == id) activate(id);
        }
        e->accept();
    }

    void leaveEvent(QEvent* e) override {
        QWidget::leaveEvent(e);
        m_holdTimer.stop();   // dragging off Back is not a hold
        m_swallowNextPress = false;
        if (!m_hoverId.isEmpty()) {
            m_hoverId.clear();
            refreshToolTip();
            unsetCursor();
            update();
        }
        dismissRcxTooltip();
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        using namespace address_bar_detail;
        const QString id = itemIdAt(e->pos());
        const LaidItem* li = itemById(id);
        if (!li) { e->ignore(); return; }
        if (li->kind == Cell::Back) {
            // Right-click on Back = the history list (Explorer, every
            // browser). The narrow-width fallback: below ~420 px the
            // overflow rule drops `hist`, and this keeps the list one
            // gesture away. Hung under the cell like the other dropdowns
            // — not exec()'d — so it behaves like the `hist` press.
            if (historyAvailable()) showHistoryMenu(id);
            e->accept();
            return;
        }
        auto copy = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
        // A popup, not exec(): every other menu on the bar is one (the
        // history list, the sibling lists, the places list), it keeps a
        // blocking event loop out of an event handler, and it is the only
        // shape a test can drive.
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("rcxAddressBarCellMenu"));
        switch (li->kind) {
        case Cell::Crumb: {
            // The layout is frozen while the overlay is up, so a cell may
            // outlive its crumb: index through the CURRENT state only.
            const int i = li->index;
            if (i < 0 || i >= m_state.crumbs.size()) { delete menu; e->ignore(); return; }
            const Crumb& c = m_state.crumbs[i];
            menu->addAction(QStringLiteral("Copy path"), this,
                           [this, i, copy] { copy(crumbPathText(i)); });
            QAction* addr = menu->addAction(QStringLiteral("Copy address"), this,
                                           [c, copy] { copy(hex(c.address)); });
            addr->setEnabled(c.address != 0 || i == 0);   // the root is base, never "unknown"
            menu->addAction(QStringLiteral("Copy class name"), this,
                           [c, copy] { copy(classHalf(c.label)); });
            if (isDeepest(*li)) {
                menu->addSeparator();
                QAction* ren = menu->addAction(QStringLiteral("Rename class\tF2"), this,
                                              [this] { beginClassNameEdit(); });
                ren->setEnabled(c.classId != 0);
                // The scroll a click used to do, kept where it does not cost
                // the segment its edit gesture.
                menu->addAction(QStringLiteral("Scroll to top"), this,
                               [this] { if (m_cb.onCurrentCrumb) m_cb.onCurrentCrumb(); });
                // The bar has no class dropdown of its own any more — this
                // opens the real chooser (the filtered type popup), not the
                // flat menu the old root chevron carried.
                QAction* other = menu->addAction(QStringLiteral("Other classes\u2026"), this,
                                                [this] { if (m_cb.onClassChooser) m_cb.onClassChooser(); });
                other->setEnabled(bool(m_cb.onClassChooser));
            }
            break;
        }
        case Cell::Base: {
            menu->addAction(QStringLiteral("Copy address"), this,
                           [this, copy] { copy(hex(m_state.resolvedBase ? m_state.resolvedBase
                                                                        : m_state.baseAddress)); });
            QAction* formula = menu->addAction(QStringLiteral("Copy formula"), this,
                                              [this, copy] { copy(m_state.baseFormula); });
            formula->setEnabled(!m_state.baseFormula.isEmpty());
            menu->addSeparator();
            menu->addAction(QStringLiteral("Edit address"), this, [this] { beginBaseEdit(); });
            menu->addAction(QStringLiteral("Go to address\u2026\tCtrl+G"), this,
                           [this] { if (m_cb.onGotoDialog) m_cb.onGotoDialog(); });
            break;
        }
        case Cell::Src:
        case Cell::SrcChev: {
            QAction* name = menu->addAction(QStringLiteral("Copy source name"), this,
                                           [this, copy] { copy(m_state.sourceName); });
            name->setEnabled(!m_state.sourceName.isEmpty());
            menu->addAction(QStringLiteral("Change source\u2026"), this,
                           [this] { activate(QStringLiteral("src")); });
            menu->addAction(QStringLiteral("Refresh\tF5"), this,
                           [this] { if (m_cb.onRefresh) m_cb.onRefresh(); });
            break;
        }
        default:
            delete menu;
            e->ignore();
            return;
        }
        dismissRcxTooltip();
        popupMenuFor(menu, id, e->globalPos());
        e->accept();
    }

private:
    enum class Cell { Back, Fwd, Hist, Up, Src, SrcChev, Base,
                      Crumb, Chev, Overflow, Space, Recent, Live, Record };

    struct LaidItem {
        QString id;
        Cell    kind = Cell::Space;
        int     index = -1;       // crumb index for Crumb / Chev
        QRect   rect;
        bool    enabled = true;
        QString text;             // display text (possibly elided) for text cells
        QString text2;            // base only: the " = 0x…" resolved-address suffix
    };
    struct Layout {
        QVector<LaidItem> items;
        int dividerX = -1;        // the field divider column, logical x
        int tlDividerX = -1;      // the divider before the timeline's buttons
        int fold     = 0;         // crumbs [0, fold) are behind «
        int width    = 0;         // width the strip needs with an empty `space`
        int forWidth = -1;        // widget width this layout was computed for
    };
    // The knobs the overflow rule turns, in the order it turns them.
    struct Budget {
        bool compactTimeline = false;  // step 0: the timeline buttons lose their words; tooltips keep them
        bool dropRecord    = false;  // step 8: F9 and View ▸ Timeline still record
        bool dropTimeline  = false;  // step 9: Ctrl+End still resumes
        int  baseMaxW      = kBaseMaxW;
        bool showResolved  = true;   // the base's "  → 0x…" suffix; goes with step 1
        int  srcMaxW       = kSrcMaxW;
        int  fold          = 0;
        int  deepestMaxW   = 0;   // 0 = never elide the deepest
        bool dropUpHist    = false;
        bool dropTrailChev = false;
        // The narrow-pane steps (7a–7e), for a strip under ~395 px.
        bool srcIconOnly   = false;  // the chip keeps icon, dot and chevron; the name is the tooltip's
        bool bareBase      = false;  // the literal the base resolves to, elided to kBaseBareMinW
        bool dropFwd       = false;  // Forward alone: Back stays, the history list's last on-bar route
        bool dropNav       = false;  // Back and the divider too; the chip's icon takes the gutter
    };

    // ── Layout ──

    void ensureLayout() const {
        // resize() on a hidden widget delivers no resizeEvent, so the dirty
        // flag alone cannot be trusted — the cache also remembers the width
        // it was computed for (RibbonBar's rule).
        if (m_layoutDirty || m_layout.forWidth != width()) relayout();
    }
    void markLayoutDirty() { m_layoutDirty = true; }

    QFont deepestFont() const {
        QFont f = font();
        f.setWeight(QFont::DemiBold);   // the single weight step on this surface
        return f;
    }

    QString sourceFullText() const {
        return m_state.sourceName.isEmpty() ? QStringLiteral("Select source") : m_state.sourceName;
    }
    // What a button showed when it was laid out, kept in LaidItem::index so
    // the glyph, its colour and the words always come from one snapshot (the
    // layout stays frozen while an edit is open; the state may not).
    static constexpr int kChipPast = 1, kChipRecording = 2;
    int chipFlags() const {
        int f = 0;
        if (m_state.past) f |= kChipPast;
        if (m_state.recording) f |= kChipRecording;
        return f;
    }
    // The buttons say what a click does: "Record", "Stop" while recording, and
    // "Back to live" — only while looking back at a recording. Compact: the
    // glyph alone (the tooltip keeps the words).
    QString liveChipText(bool compact) const {
        return compact ? QString() : QStringLiteral("Back to live");
    }
    QString recChipText(bool compact) const {
        if (compact) return QString();
        return m_state.recording ? QStringLiteral("Stop") : QStringLiteral("Record");
    }
    static int tlChipWidth(const QFontMetrics& fm, const QString& text) {
        return text.isEmpty() ? kTlChipPad + kTlGlyphW + kTlChipPad
                              : kTlChipPad + kTlGlyphW + kTlGlyphGap + fm.horizontalAdvance(text) + kTlChipPad;
    }
    static QRectF tlGlyphBox(const QRect& cell) {
        // Centred on the cell's true middle — the one drawIcon and
        // AlignVCenter use — not QRect::center(), which rounds down half a pixel.
        const double cy = cell.top() + cell.height() / 2.0;
        return QRectF(cell.left() + kTlChipPad, cy - kTlGlyphW / 2.0, kTlGlyphW, kTlGlyphW);
    }
    QString baseFullText() const {
        return m_state.baseFormula.isEmpty() ? address_bar_detail::hex(m_state.baseAddress)
                                             : m_state.baseFormula;
    }
    // The number the base IS right now: what a formula resolves to, else the
    // literal. The tooltip's head and the narrow-pane bare display share it.
    QString baseLiteralText() const {
        return address_bar_detail::hex(m_state.resolvedBase ? m_state.resolvedBase : m_state.baseAddress);
    }
    // What a formula resolves to right now, one tone down beside it. Only
    // for a formula — beside a literal it would repeat the number.
    QString baseSuffixText() const {
        return m_state.baseFormula.isEmpty()
            ? QString()
            : baseSepText() + address_bar_detail::hex(m_state.resolvedBase);
    }

    Layout computeLayout(const Budget& b) const {
        Layout L;
        L.fold = b.fold;
        const QFontMetrics fm(font());
        const QFontMetrics fmDeep(deepestFont());
        // Elide only when the (integer) advance exceeds the budget: elidedText
        // on a string that fits by the metric can still trim it.
        auto elide = [](const QFontMetrics& f, const QString& s, int maxW) {
            return f.horizontalAdvance(s) > maxW ? f.elidedText(s, Qt::ElideMiddle, maxW) : s;
        };
        // kGutter spent once: the first INK is the Back glyph's, on the same
        // device column as the doc-tab title's first ink. The cell starts
        // earlier by the glyph's centring pad plus the SVG's own inset (a
        // doc tab's rect starts at 0 too, with its label at kGutter) —
        // otherwise the arrow landed ~5 px right of every other strip.
        const qreal dpr = devicePixelRatioF();
        int x = qRound(kGutter - (kNavBtnW - kNavIconPx) / 2.0 - backInkInsetDev() / dpr);
        auto add = [&](const QString& id, Cell kind, int index, int w, bool enabled,
                       const QString& text = QString()) {
            LaidItem li;
            li.id = id; li.kind = kind; li.index = index;
            li.rect = QRect(x, kCellTop, w, kCellH);
            li.enabled = enabled; li.text = text;
            L.items.push_back(li);
            x += w;
        };

        // Nav cluster. Disabled cells stay laid out (Back/Forward always,
        // history + Up unless the strip is too narrow) so the field never
        // shifts left when history appears. The narrow-pane steps take it
        // apart from the right: Forward first (7d) — Back outlives it
        // because its right-click / hold is the history list's only
        // on-bar route once `hist` is gone — then Back and the divider
        // (7e): the chip's icon is the first ink then, and it takes the
        // gutter.
        if (b.dropNav) {
            x = kGutter - kChipPad;
        } else {
            add(QStringLiteral("back"), Cell::Back, -1, kNavBtnW, m_state.canBack);
            if (!b.dropFwd) {
                x += kNavGap;
                add(QStringLiteral("fwd"), Cell::Fwd, -1, kNavBtnW, m_state.canForward);
            }
            if (!b.dropUpHist) {
                x += kNavGap;
                add(QStringLiteral("hist"), Cell::Hist, -1, kHistW, m_state.canBack || m_state.canForward);
                x += kNavGap;
                add(QStringLiteral("up"), Cell::Up, -1, kNavBtnW, m_state.canUp);
            }

            // Field divider — everything right of it is "the field".
            x += kDividerPad;
            L.dividerX = x;
            x += 1 + kDividerPad;
        }

        // Source chip: icon + name, then its chevron. Two cells (so the
        // chevron can grow its own menu later) painted as ONE hover group.
        // Icon-only (7a) keeps the icon, its liveness dot and the chevron;
        // the name is still in the tooltip.
        const QString srcText = b.srcIconOnly ? QString() : elide(fm, sourceFullText(), b.srcMaxW);
        add(QStringLiteral("src"), Cell::Src, -1,
            kChipPad + kChipIconPx
                + (srcText.isEmpty() ? 0 : kChipTextGap + fm.horizontalAdvance(srcText))
                + kChipChevGap,
            true, srcText);
        add(QStringLiteral("src.chev"), Cell::SrcChev, -1, kChevW, true);
        x += kChipPad;

        // Base: the formula if one is set, else the literal, then the
        // address the formula resolves to. Elided for DISPLAY only — the
        // state carries the full string and the edit (P3) opens on that,
        // never on the elided text (the command row fed its ellipsis to
        // the parser and silently no-op'd). Bare (7c): the literal the
        // base resolves to, elided to kBaseBareMinW, no suffix.
        const QString baseText   = b.bareBase ? elide(fm, baseLiteralText(), kBaseBareMinW)
                                              : elide(fm, baseFullText(), b.baseMaxW);
        const QString baseSuffix = (b.showResolved && !b.bareBase) ? baseSuffixText() : QString();
        // baseSepGap between the two, not a space inside the suffix: the same
        // gap the edit overlay ends in, so the suffix sits where the preview
        // will and neither moves when the field opens.
        add(QStringLiteral("base"), Cell::Base, -1,
            kBasePad + fm.horizontalAdvance(baseText)
                + (baseSuffix.isEmpty() ? 0 : baseSepGap() + fm.horizontalAdvance(baseSuffix))
                + kBasePad,
            true, baseText);
        L.items.last().text2 = baseSuffix;

        // Crumbs, « in front of the run when folded, a chevron after each
        // (the trailing one drills further; it is the last thing dropped).
        const int n = m_state.crumbs.size();
        if (b.fold > 0) add(QStringLiteral("overflow"), Cell::Overflow, -1, kOverflowW, true);
        for (int i = b.fold; i < n; ++i) {
            const bool deepest = (i == n - 1);
            const QFontMetrics& f = (deepest && n > 1) ? fmDeep : fm;
            QString label = m_state.crumbs[i].label;
            if (deepest && b.deepestMaxW > 0) label = elide(f, label, b.deepestMaxW);
            add(QStringLiteral("crumb:%1").arg(i), Cell::Crumb, i,
                kCrumbPad + f.horizontalAdvance(label) + kCrumbPad, true, label);
            if (!deepest || !b.dropTrailChev)
                add(QStringLiteral("chev:%1").arg(i), Cell::Chev, i, kChevW, true);
        }

        // The stretch, then `recent` pinned to the right — or pushed past it
        // when nothing fits; the strip never drops a crumb to make room for a
        // menu button. While the timeline is on, a divider and its buttons
        // close the bar, pinned to the right margin, each named for what a
        // click does: Record (Stop while recording) and — only while looking
        // back at a recording — Back to live. Values are live otherwise; there
        // is nothing to pause. Whether the bar FITS is decided with Back to
        // live's room counted and on the widest words ("Record"), so it coming
        // and going, or Record → Stop, never folds a crumb or brings one back.
        const bool tl = m_state.timeline && !b.dropTimeline;
        const QString liveText = liveChipText(b.compactTimeline);
        const QString recText = recChipText(b.compactTimeline);
        const int liveFitW = tl ? tlChipWidth(fm, liveText) : 0;
        const int liveW = (tl && m_state.past) ? liveFitW : 0;
        const int recRoomW = (tl && !b.dropRecord) ? tlChipWidth(fm, recText) : 0;
        const int recFitW = (recRoomW == 0 || b.compactTimeline)
            ? recRoomW : qMax(tlChipWidth(fm, QStringLiteral("Record")), tlChipWidth(fm, QStringLiteral("Stop")));
        // Room for one glyph only (Record dropped): that slot is Back to live
        // while looking back, and Record's glyph — the same width — otherwise,
        // since live Record is the only timeline control there is.
        const int recW = recRoomW > 0 ? recRoomW
                       : (tl && b.dropRecord && !m_state.past) ? liveFitW : 0;
        auto clusterW = [](int live, int rec) {
            if (live + rec == 0) return 0;
            return kDividerPad + 1 + kDividerPad + live + (live > 0 && rec > 0 ? kNavGap : 0) + rec;
        };
        const int tlW = clusterW(liveW, recW);
        L.width = x + kRecentW + clusterW(liveFitW, recFitW) + kRightMargin;
        const int recentX = qMax(x, width() - kRightMargin - tlW - kRecentW);
        if (recentX > x) add(QStringLiteral("space"), Cell::Space, -1, recentX - x, true);
        x = recentX;
        add(QStringLiteral("recent"), Cell::Recent, -1, kRecentW, true);
        if (tlW > 0) {
            x += kDividerPad;
            L.tlDividerX = x;
            x += 1 + kDividerPad;
            if (liveW > 0) {
                add(QStringLiteral("tl.live"), Cell::Live, chipFlags(), liveW, true, liveText);
                if (recW > 0) x += kNavGap;
            }
            if (recW > 0)
                add(QStringLiteral("tl.rec"), Cell::Record, chipFlags(), recW, m_state.canRecord, recText);
        }
        L.forWidth = width();
        return L;
    }

    // The overflow rule: one knob per step, in order, until it fits. The two
    // text knobs turn only as far as the current overshoot needs, measured
    // on the text as laid out (elision lands a glyph or so short of its
    // budget, so a turn is re-taken up to three times before the floor —
    // never a jump to the floor for a few px).
    void relayout() const {
        m_layoutDirty = false;
        const int avail = width();
        const QFontMetrics fm(font());
        // 8. The timeline's buttons go only when the field has given all it
        //    can (steps 0–7d): down to one glyph — Back to live while looking
        //    back, Record otherwise — then none. When they go, the field is re-decided from
        //    scratch with the room they freed: a bar with the timeline on never
        //    keeps Back or the source name dropped that the same bar with it
        //    off would show. With both gone the bar is exactly the one
        //    kNarrowFloorW was measured for.
        //    The buttons' words are worth eliding the base and source names
        //    for (steps 1–2), not a folded crumb or a lost Back: past that,
        //    both keep only their glyphs (the tooltips keep the words), and
        //    the field is decided again from scratch.
        Layout L;
        for (int chips = 0; chips < 4; ++chips) {
            Budget start;
            start.compactTimeline = chips >= 1;
            start.dropRecord = chips >= 2;
            start.dropTimeline = chips >= 3;
            const bool wordsPass = m_state.timeline && chips == 0;
            if (fitField(start, avail, fm, L, wordsPass) || !m_state.timeline) break;
        }
        m_layout = L;   // when nothing fits: narrower than kNarrowFloorW — best effort
        Q_ASSERT(avail < kNarrowFloorW || namesThePlace(L));
    }

    // Steps 1–7d from `b`: true as soon as a layout fits; `L` holds the last
    // tried. `elideOnly`: stop after the two text steps (1–2).
    bool fitField(Budget b, int avail, const QFontMetrics& fm, Layout& L, bool elideOnly = false) const {
        L = computeLayout(b);
        auto fits = [&] { return L.width <= avail; };
        auto over = [&] { return L.width - avail; };
        auto laidTextW = [&](const char* id) {
            for (const LaidItem& li : L.items)
                if (li.id == QLatin1String(id)) return fm.horizontalAdvance(li.text);
            return 0;
        };
        if (fits()) return true;
        // 1. The base: elide the formula by the overshoot; when its floor
        //    cannot absorb it, drop the resolved-address suffix instead and
        //    give the formula back whatever that freed beyond the need.
        for (int turn = 0; turn < 3 && !fits(); ++turn) {
            const int cur     = laidTextW("base");
            const int suffixW = b.showResolved ? fm.horizontalAdvance(baseSuffixText()) : 0;
            if (cur - over() >= kBaseMinW) {
                b.baseMaxW = cur - over();
            } else if (suffixW > 0) {
                b.showResolved = false;
                b.baseMaxW = qMax(kBaseMinW, cur - qMax(0, over() - suffixW));
            } else if (cur > kBaseMinW) {
                b.baseMaxW = kBaseMinW;
            } else {
                break;   // already at the floor
            }
            L = computeLayout(b);
        }
        if (fits()) return true;
        b.baseMaxW = kBaseMinW; b.showResolved = false;
        L = computeLayout(b); if (fits()) return true;
        // 2. The source name, the same way.
        for (int turn = 0; turn < 3 && !fits(); ++turn) {
            const int cur = laidTextW("src");
            if (cur <= kSrcMinW) break;
            b.srcMaxW = qMax(kSrcMinW, cur - over());
            L = computeLayout(b);
        }
        if (fits()) return true;
        b.srcMaxW = kSrcMinW;
        L = computeLayout(b); if (fits()) return true;
        if (elideOnly) return false;
        for (int f = 1; f < m_state.crumbs.size(); ++f) {   // never the deepest
            b.fold = f;
            L = computeLayout(b); if (fits()) return true;
        }
        b.deepestMaxW = kDeepestMinW;
        L = computeLayout(b); if (fits()) return true;
        b.dropUpHist = true;
        L = computeLayout(b); if (fits()) return true;
        b.dropTrailChev = true;
        L = computeLayout(b); if (fits()) return true;
        // 7. Narrow panes — below the ~395 px the six steps above reach with
        //    a process source and a formula base. The field keeps only what
        //    names the place, so the deepest crumb and `recent` stay inside
        //    the strip instead of running past its right edge:
        // 7a. the chip goes icon-only (the name is the tooltip's);
        b.srcIconOnly = true;
        L = computeLayout(b); if (fits()) return true;
        // 7b. the base shows its bare literal, elided to 60 px — the state
        //     and the edit keep the full formula;
        b.bareBase = true;
        L = computeLayout(b); if (fits()) return true;
        // 7c. Forward goes (24 px) — Back stays, so the history list keeps
        //     its on-bar route (right-click / hold) one step longer;
        b.dropFwd = true;
        L = computeLayout(b); if (fits()) return true;
        // 7d. last resort for the field: Back and the divider go.
        b.dropNav = true;
        L = computeLayout(b);
        return fits();
    }

    // The invariant the narrow-pane steps exist for: the deepest crumb and
    // `recent` end inside the strip. Holds for every width down to
    // kNarrowFloorW (relayout asserts it; test_breadcrumb pins it).
    bool namesThePlace(const Layout& L) const {
        const int deepestIdx = m_state.crumbs.size() - 1;
        for (const LaidItem& li : L.items) {
            const bool named = (li.kind == Cell::Crumb && li.index == deepestIdx) || li.kind == Cell::Recent;
            if (named && (li.rect.left() < 0 || li.rect.right() >= width())) return false;
        }
        return true;
    }

    const LaidItem* itemById(const QString& id) const {
        ensureLayout();
        for (const LaidItem& li : m_layout.items)
            if (li.id == id) return &li;
        return nullptr;
    }

    bool isDeepest(const LaidItem& li) const {
        return li.kind == Cell::Crumb && li.index == m_state.crumbs.size() - 1;
    }

    // The chip and its chevron light up together.
    static bool sameGroup(const QString& a, const QString& b) {
        auto chip = [](const QString& s) {
            return s == QLatin1String("src") || s == QLatin1String("src.chev");
        };
        return a == b || (chip(a) && chip(b));
    }

    // Device columns from the arrow-left pixmap's left edge to its first
    // inked column (alpha >= 16), at the current dpr. The Back glyph is
    // drawn at the fractional x that lands that column on kGutter (see
    // computeLayout and paintItem), so the bar's first ink sits where every
    // other strip's does. Measured once per dpr: the SVG never changes.
    int backInkInsetDev() const {
        const qreal dpr = devicePixelRatioF();
        if (m_inkInsetDpr == dpr) return m_inkInsetDev;
        const QImage img = themedVsIcon(QStringLiteral(":/vsicons/arrow-left.svg"), m_theme.textDim,
                                        kNavIconPx, dpr)
                               .pixmap(QSize(kNavIconPx, kNavIconPx), dpr).toImage();
        int inset = 0;
        for (int xx = 0; xx < img.width(); ++xx) {
            bool ink = false;
            for (int yy = 0; yy < img.height() && !ink; ++yy)
                if (qAlpha(img.pixel(xx, yy)) >= 16) ink = true;
            if (ink) { inset = xx; break; }
        }
        m_inkInsetDpr = dpr;
        m_inkInsetDev = inset;
        return inset;
    }

    // "The field": everything right of the divider up to the right margin.
    // Its bottom device row is the focus seam while editing.
    QRect fieldRect() const {
        ensureLayout();
        const int left = m_layout.dividerX >= 0 ? m_layout.dividerX + 1 : 0;
        const int right = m_layout.tlDividerX >= 0 ? m_layout.tlDividerX : width() - kRightMargin;
        return QRect(left, 0, qMax(0, right - left), height());
    }

    // ── Painting ──

    void drawIconAt(QPainter& p, const QPointF& at, const char* path, int px, const QColor& tint) const {
        const qreal dpr = devicePixelRatioF();
        const QPixmap pm = themedVsIcon(QString::fromLatin1(path), tint, px, dpr)
                               .pixmap(QSize(px, px), dpr);
        drawPixmapSnapped(p, at, pm);
    }
    void drawIcon(QPainter& p, const QRect& cell, const char* path, int px, const QColor& tint) const {
        drawIconAt(p, QPointF(cell.left() + (cell.width() - px) / 2.0,
                              cell.top() + (cell.height() - px) / 2.0), path, px, tint);
    }

    // The chip's 16-px icon square: after the pad, centred in the cell.
    static QRect chipIconRect(const QRect& cell) {
        return QRect(cell.left() + kChipPad, cell.top() + (cell.height() - kChipIconPx) / 2,
                     kChipIconPx, kChipIconPx);
    }
    // The liveness dot rides the icon's bottom-right corner, 2 px over the
    // edge, so it reads as a badge ON the icon and not as a fourth glyph in
    // the row (the 2 px land in the icon-to-name gap, which stays clear).
    static QRect dotRectFor(const QRect& icon) {
        return QRect(icon.right() + 3 - kDotPx, icon.bottom() + 3 - kDotPx, kDotPx, kDotPx);
    }
    // The status-bar chip's mapping: green while reads succeed, focusGlow
    // while they fail, markerError when the source is gone — and nothing for
    // a static file or no source, where "alive" has no meaning. One
    // exception: while looking back at a recording the dot speaks
    // for the VIEW (amber, "not live"), and the chip's tooltip says so; the
    // status-bar chip keeps reporting the source.
    QColor livenessDotColor() const {
        if (m_state.sourceName.isEmpty()) return QColor();
        // Looking back: whatever the source is doing, what is on screen is
        // not live. The app's "not live" amber, as on the timeline.
        if (m_state.past) return m_theme.focusGlow;
        switch (m_state.liveness) {
        case liveness::Live:         return m_theme.indHintGreen;
        case liveness::Stale:        return m_theme.focusGlow;
        case liveness::Disconnected: return m_theme.markerError;
        default:                     return QColor();
        }
    }

    void paintItem(QPainter& p, const LaidItem& li) const {
        const Theme& t = m_theme;
        const bool hovered = li.enabled && !m_hoverId.isEmpty() && sameGroup(m_hoverId, li.id);
        // Two states, two fills: the mouse-down flash is pressedFill(t); a
        // cell whose dropdown is up (a menu, or the source chooser through
        // setSourceMenuOpen) reads as the hover it grew from. They used to
        // share pressedFill, and on tw.json pressedFill is t.selected
        // (button == background), so the chip sat in a pale-blue
        // "selected" box for the life of the popup.
        const bool pressed = li.enabled
            && hovered && !m_pressedId.isEmpty() && sameGroup(m_pressedId, li.id);
        const bool menuOpen = li.enabled
            && !m_menuOpenId.isEmpty() && sameGroup(m_menuOpenId, li.id);
        const QRect r = li.rect;
        const bool deepest = isDeepest(li);
        const qreal prevOpacity = p.opacity();
        // Disabled = the whole cell at 40 %, never textDim × 0.4.
        if (!li.enabled) p.setOpacity(prevOpacity * kDisabledOpacity);

        // Fill: the deepest crumb is inert (you are here — nothing to do),
        // the base is a text field (IBeam, no fill) and the stretch is air.
        const bool fillable = li.kind != Cell::Space && li.kind != Cell::Base && !deepest;
        if (fillable) {
            if (pressed)                  p.fillRect(r, pressedFill(t));
            else if (menuOpen || hovered) p.fillRect(r, t.hover);
        }

        p.setFont(font());
        const int textFlags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
        switch (li.kind) {
        case Cell::Back:
            // At the exact x that lands the glyph's first inked column on
            // kGutter (backInkInsetDev), not centred: the cell was placed
            // for this, and rounding to the cell's centre would drift the
            // ink a device column off the gutter at 125 %.
            drawIconAt(p, QPointF(kGutter - backInkInsetDev() / devicePixelRatioF(),
                                  r.top() + (r.height() - kNavIconPx) / 2.0),
                       ":/vsicons/arrow-left.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Fwd:
            drawIcon(p, r, ":/vsicons/arrow-right.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Up:
            drawIcon(p, r, ":/vsicons/arrow-up.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Hist:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Src: {
            // Explorer's root icon: the provider's icon, dimmed to 40 % when
            // the source is gone, with the liveness dot on its corner.
            const QRect icon = chipIconRect(r);
            const QString path = m_state.sourceName.isEmpty()
                ? QStringLiteral(":/vsicons/plug.svg") : iconForProvider(m_state.sourceKindId);
            drawTabSourceIcon(&p, icon, path, m_state.liveness != liveness::Disconnected, t.textDim);
            const QColor dot = livenessDotColor();
            if (dot.isValid()) {
                // A one-px ring of paper under the dot lifts it off the
                // icon's ink. The dot is the one round thing on the bar —
                // a badge, not a box.
                const QRect d = dotRectFor(icon);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(Qt::NoPen);
                p.setBrush(editorPaperColor(t));
                p.drawEllipse(d.adjusted(-kDotRing, -kDotRing, kDotRing, kDotRing));
                p.setBrush(dot);
                p.drawEllipse(d);
                p.setBrush(Qt::NoBrush);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(QRect(icon.right() + 1 + kChipTextGap, r.top(), r.width(), r.height()),
                       textFlags, li.text);
            break;
        }
        case Cell::SrcChev:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        case Cell::Chev:
            // Explorer's › → ˅ flip: the chevron turns down under the pointer
            // to say "this opens a list", and steps up one tone to be read.
            drawIcon(p, r, hovered ? ":/vsicons/chevron-down.svg" : ":/vsicons/chevron-right.svg",
                     kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        case Cell::Base: {
            // The formula (or literal) in the field tone, then what it
            // resolves to one rung down — until now a number only the Goto
            // dialog would show.
            const QRect tr = r.adjusted(kBasePad, 0, -kBasePad, 0);
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(tr, textFlags, li.text);
            if (!li.text2.isEmpty()) {
                p.setPen(t.textMuted);
                p.drawText(tr.adjusted(p.fontMetrics().horizontalAdvance(li.text) + baseSepGap(),
                                       0, 0, 0),
                           textFlags, li.text2);
            }
            break;
        }
        case Cell::Crumb: {
            // Tone ladder: a lone crumb only repeats what the doc tab and the
            // command row already name, so it stays textDim and regular. From
            // depth 2 the trail is navigation — ancestors textDim (text under
            // the pointer: a link cue, not the accent), the deepest text at
            // DemiBold, and the deepest ignores hover because it is inert.
            const bool lone = (m_state.crumbs.size() == 1);
            const bool primary = deepest && !lone;
            if (primary) p.setFont(deepestFont());
            p.setPen(primary ? t.text : (hovered && !deepest) ? t.text : t.textDim);
            p.drawText(r.adjusted(kCrumbPad, 0, -kCrumbPad, 0), textFlags, li.text);
            break;
        }
        case Cell::Overflow:
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(r, Qt::AlignCenter | Qt::TextSingleLine, QStringLiteral("«"));
            break;
        case Cell::Space:
            break;
        // The timeline's buttons: a glyph that carries the colour, then the
        // verb, on the same straight hover fill as every other cell — no
        // coloured box, no pulse. Back to live: a green play triangle.
        // Record: a red dot; Stop: a red square and red words, the one sign
        // that a recording is running.
        case Cell::Live: {
            const QRect words = r.adjusted(kTlChipPad + kTlGlyphW + kTlGlyphGap, 0, -kTlChipPad, 0);
            timeline::paintPlayGlyph(p, tlGlyphBox(r), t.indHintGreen);
            p.setPen(t.text);
            if (!li.text.isEmpty()) p.drawText(words, textFlags, li.text);
            break;
        }
        case Cell::Record: {
            const QRect words = r.adjusted(kTlChipPad + kTlGlyphW + kTlGlyphGap, 0, -kTlChipPad, 0);
            timeline::paintRecordGlyph(p, tlGlyphBox(r), (li.index & kChipRecording) != 0, t.markerPtr);
            p.setPen((li.index & kChipRecording) ? t.markerPtr : t.text);
            if (!li.text.isEmpty()) p.drawText(words, textFlags, li.text);
            break;
        }
        case Cell::Recent:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        }
        p.setOpacity(prevOpacity);
    }

    // What the edit adds on top of the frozen layout: paper over whatever
    // sits between the overlay and the recent cell (the crumbs it covers
    // would otherwise show their tails), the preview or the parser's words
    // there, and the focus seam under the field replacing the plain one —
    // one device row, the way PanelSearchField rings.
    void paintEditChrome(QPainter& p) const {
        const Theme& t = m_theme;
        // Paper over everything the edit covers (the overlay child paints
        // its own rect on top).
        if (m_coveredRect.width() > 0) p.fillRect(m_coveredRect, editorPaperColor(t));
        // The preview starts at the field's right edge — which, the field
        // being kBasePad + text + baseSepGap wide, is exactly the column the
        // resting "= 0x…" suffix stands on. Opening the edit does not shove
        // it; only adding characters does, which is a bump you asked for.
        p.setFont(font());
        const int after0 = m_editRect.right() + 1;
        const QRect after(after0, kCellTop, qMax(0, m_coveredRect.right() - after0 + 1), kCellH);
        // Room for the WHOLE preview, or at least a run worth reading. The
        // flat kEditPreviewMinW that used to be the only test dates from when
        // the paper ran to `recent` and the run was always enormous; now the
        // covered rect is sized to the preview, so a short one like "= 0x18"
        // measured just under the 48 and was silently dropped.
        const int need = p.fontMetrics().horizontalAdvance(m_editPreview);
        if (!m_editPreview.isEmpty() && after.width() >= qMin(need, kEditPreviewMinW)) {
            p.setPen(editTextValid() ? t.textMuted : t.markerError);
            p.drawText(after, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                       p.fontMetrics().elidedText(m_editPreview, Qt::ElideRight, after.width()));
        }
        fillBottomDeviceRowOfRect(p, QRectF(fieldRect()),
                                  editTextValid() ? t.borderFocused : t.markerError);
    }

    // ── Hover / tooltip / cursor ──

    void updateHover(const QPoint& pos) {
        const QString id = itemIdAt(pos);
        if (id == m_hoverId) return;
        m_hoverId = id;
        // Publish the new cell's text and stop. NO dismiss here: once a tip
        // is up Qt re-arms its wake-up at ~20 ms, so dismissing on every
        // hovered-cell change strobed the ribbon's tooltip as the cursor
        // crossed a row of buttons. The bridge repositions on the next move.
        refreshToolTip();
        updateCursor();
        update();
    }

    void refreshToolTip() {
        const QString tip = m_hoverId.isEmpty() ? QString() : tooltipFor(m_hoverId);
        if (tip != toolTip()) setToolTip(tip);
    }

    // ── The base-address grammar help ──
    //
    // The worked examples that used to hang off the editor's command-row
    // address span (fmt::baseAddressHelpBody). It is shown while you TYPE, not
    // on hover, which is where a beginner needs it, and that is exactly why it
    // cannot use the shared tooltip: GlobalTooltipBridge clears that one on
    // every KeyPress, so it would vanish on the first character. A private
    // instance is invisible to the bridge — the same arrangement RcxEditor's
    // m_arrowTooltip had.
    //
    // Two things still threaten it, and both are handled:
    //   * RcxTooltip expires itself (capped at 20 s), so a repeating timer
    //     re-arms it for as long as the field is open;
    //   * showAt() calls dismissOthers(), so ANY shared tooltip shown while
    //     the field is up would kill this one — hence tooltipFor() publishes
    //     nothing at all while editing.
    void showEditHelp() {
        if (!m_editVisible || m_editScope != EditScope::Base) return;
        if (!m_helpTip) m_helpTip = new RcxTooltip(this);
        m_helpTip->setTheme(m_theme.backgroundAlt, m_theme.border,
                            m_theme.text, m_theme.text, m_theme.border);
        m_helpTip->populate(fmt::baseAddressHelpTitle(), fmt::baseAddressHelpBody(), font());
        // Under the whole bar, centred on the field: clear of the value being
        // typed and of the "→ 0x…" preview beside it.
        m_helpTip->showAt(mapToGlobal(QPoint(m_editRect.center().x(), height())));
        m_helpKeepAlive.start();
    }

    void dismissEditHelp() {
        m_helpKeepAlive.stop();
        if (m_helpTip) m_helpTip->dismiss();
    }


    void updateCursor() {
        const LaidItem* li = itemById(m_hoverId);
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (li) {
            switch (li->kind) {
            case Cell::Base:
            case Cell::Space:    shape = Qt::IBeamCursor; break;          // click-to-type
            // The deepest crumb is click-to-type (its own name); the
            // ancestors above it are links.
            case Cell::Crumb:    shape = isDeepest(*li) ? Qt::IBeamCursor
                                                        : Qt::PointingHandCursor; break;
            case Cell::Back: case Cell::Fwd: case Cell::Hist: case Cell::Up:
            case Cell::Live: case Cell::Record:
                shape = li->enabled ? Qt::PointingHandCursor : Qt::ArrowCursor; break;
            default:             shape = Qt::PointingHandCursor; break;
            }
        }
        if (shape == Qt::ArrowCursor) unsetCursor();
        else                          setCursor(shape);
    }

    QString sourceTooltip() const {
        if (m_state.sourceName.isEmpty())
            return QStringLiteral("Select source  ·  click to choose one");
        // Same words as the status-bar chip, so the two never disagree.
        QString live;
        switch (m_state.liveness) {
        case liveness::Live:         live = QStringLiteral("Live — reading"); break;
        case liveness::Stale:        live = QStringLiteral("Stale — read failing"); break;
        case liveness::Disconnected: live = QStringLiteral("Disconnected"); break;
        case liveness::Static:       live = QStringLiteral("Static file"); break;
        default: break;
        }
        // The dot is amber while looking back at a recording, whatever the source is
        // doing: the words must agree with it.
        if (m_state.past) live = QStringLiteral("Looking back at a recording — values are read-only");
        // The kind word comes from the saved source's identifier; a live
        // attach that registered none (tutorial self-attach, MCP, kernel)
        // gets the bare name — kindLabelFor("") would say "Plugin".
        QString s = m_state.sourceKindId.isEmpty()
            ? m_state.sourceName
            : kindLabelFor(m_state.sourceKindId) + QLatin1Char(' ') + m_state.sourceName;
        if (!live.isEmpty()) s += QStringLiteral("  ·  ") + live;
        return s + QStringLiteral("  ·  click to change source");
    }

    QString tooltipFor(const QString& id) const {
        using namespace address_bar_detail;
        // While the BASE edit is open the grammar help owns the tooltip
        // layer: any other RcxTooltip shown now would dismiss it (showAt →
        // dismissOthers) and the user would lose the examples mid-formula.
        // The other scopes show no help, so they keep their tooltips.
        if (m_editVisible && m_editScope == EditScope::Base) return QString();
        const LaidItem* li = itemById(id);
        if (!li) return QString();
        switch (li->kind) {
        case Cell::Back:     return QStringLiteral("Back  Alt+Left");
        case Cell::Fwd:      return QStringLiteral("Forward  Alt+Right");
        case Cell::Hist:     return QStringLiteral("Recent locations");
        case Cell::Up:       return QStringLiteral("Up one level  Alt+Up");
        case Cell::Src:
        case Cell::SrcChev:  return sourceTooltip();
        case Cell::Base: {
            // The full formula lives here when the segment had to elide it.
            const QString addr = baseLiteralText();
            const QString head = m_state.baseFormula.isEmpty()
                ? QStringLiteral("Base address  %1").arg(addr)
                : QStringLiteral("Base address  %1 = %2").arg(m_state.baseFormula, addr);
            return head + QStringLiteral("\nclick to edit · Down for recent · Ctrl+G go to");
        }
        case Cell::Crumb: {
            // Where that class sits in memory — invisible everywhere else
            // once you are two pointers deep. The root's address is the base,
            // which is never "unknown". The layout is frozen while the
            // overlay is up, so the cell may outlive its crumb: check.
            if (li->index < 0 || li->index >= m_state.crumbs.size()) return QString();
            const Crumb& c = m_state.crumbs[li->index];
            const QString where = (c.address != 0 || li->index == 0)
                ? QStringLiteral("%1  @ %2").arg(c.label, hex(c.address))
                : QStringLiteral("%1  @ (unreadable)").arg(c.label);
            // The deepest crumb is the class you are looking at, and its
            // label is a bare class name: that is what a click edits.
            return isDeepest(*li) && c.classId != 0
                ? where + QStringLiteral("\nclick to rename \u00b7 right-click for more")
                : where;
        }
        case Cell::Chev:
            if (li->index < 0 || li->index >= m_state.crumbs.size()) return QString();
            return QStringLiteral("Fields of %1").arg(classHalf(m_state.crumbs[li->index].label));
        case Cell::Overflow:
            return overflowMenuLabels().join(QStringLiteral(" › "));
        case Cell::Live:
            return QStringLiteral("Back to live  Ctrl+End\n"
                                  "you are looking back at the recording; values are read-only");
        case Cell::Record:
            if (!m_state.canRecord) return QStringLiteral("Record\nthe timeline records live sources");
            return m_state.recording
                ? QStringLiteral("Stop recording  F9\nwhat was recorded stays on the timeline until you clear it")
                : QStringLiteral("Record  F9\nkeep every change of this class from now on, to look back at on the timeline");
        case Cell::Space:    return QStringLiteral("Edit the path  Alt+D");
        case Cell::Recent:   return QStringLiteral("Recent addresses and bookmarks");
        }
        return QString();
    }

    // ── Activation ──

    void activate(const QString& id) {
        const LaidItem* li = itemById(id);
        if (!li || !li->enabled) return;
        switch (li->kind) {
        case Cell::Crumb:
            if (li->index < 0 || li->index >= m_state.crumbs.size()) break;   // stale cell
            // The deepest crumb used to answer a click by scrolling its
            // header to the top of the pane — a move so quiet the user could
            // not tell the segment was live at all. It renames now, the way
            // the base segment beside it edits; the scroll is on its context
            // menu, where a non-mutating convenience belongs.
            if (isDeepest(*li)) beginClassNameEdit();
            else if (m_cb.onCrumb) m_cb.onCrumb(li->index);
            break;
        case Cell::Base:   beginBaseEdit(); break;
        // `recent` opens on PRESS (mousePressEvent), like « — never here.
        case Cell::Back:    if (m_cb.onBack)    m_cb.onBack();    break;
        case Cell::Fwd:     if (m_cb.onForward) m_cb.onForward(); break;
        case Cell::Up:      if (m_cb.onUp)      m_cb.onUp();      break;
        case Cell::Src:
        case Cell::SrcChev: {
            // The anchor is the bar's bottom edge under the chip, not the
            // cell's bottom: the popup hangs below the seam instead of
            // covering it. The controller holds the chip at t.hover while the
            // popup is up (setSourceMenuOpen) — it, not the bar, knows when
            // the popup closed.
            const QRect chip = itemRect(QStringLiteral("src"));
            if (m_cb.onSourceClick) m_cb.onSourceClick(mapToGlobal(QPoint(chip.left(), height())));
            break;
        }
        // Click-to-type on the empty stretch (Explorer): the trail turns
        // into its dotted path. chev:<i> and hist open on PRESS.
        case Cell::Space:  beginPathEdit(); break;
        case Cell::Live:   if (m_cb.onTimelineBackToLive) m_cb.onTimelineBackToLive(); break;
        case Cell::Record: if (m_cb.onTimelineRecord) m_cb.onTimelineRecord(); break;
        default: break;
        }
    }

    // ── Keyboard mode internals ──

    // "You are here": the deepest crumb, which is never dropped by the
    // overflow rule and never disabled — the one cell always there to
    // start from. Falls back to the last stop when the state has no crumbs.
    QString deepestCrumbId(const QStringList& ids) const {
        const QString deepest = QStringLiteral("crumb:%1").arg(m_state.crumbs.size() - 1);
        return ids.contains(deepest) ? deepest : ids.last();
    }
    void setKeyboardFocus(const QString& id) {
        if (id == m_kbFocusId) return;
        m_kbFocusId = id;
        update();
    }
    // Clamped, not wrapped: Home / End are the jumps, and running off an
    // end of a strip this short should feel like an end.
    void stepKeyboardFocus(int dir) {
        const QStringList ids = traversalIds();
        if (ids.isEmpty()) return;
        const int i = ids.indexOf(m_kbFocusId);
        setKeyboardFocus(ids[i < 0 ? 0 : qBound(0, i + dir, ids.size() - 1)]);
    }
    // A state push can disable or drop the focused cell (Back after the
    // last entry was used, a crumb folded away): keep the ring on a cell
    // that exists, or leave the mode when nothing is left to focus.
    void reconcileKeyboardFocus() {
        if (!m_kbMode) return;
        const QStringList ids = traversalIds();
        if (ids.isEmpty()) { leaveKeyboardMode(true); return; }
        if (!ids.contains(m_kbFocusId)) m_kbFocusId = deepestCrumbId(ids);
    }
    // Enter / Space: a press and a release at the cell's centre through
    // the real handlers, flagged so the press is not read as the mouse
    // taking over. Whatever a click there does — menu, edit, callback —
    // happens exactly as it would for the mouse.
    void synthesiseClick(const QString& id) {
        const QRect r = itemRect(id);
        if (r.isNull()) return;
        const QPointF c(r.center());
        const QPointF g(mapToGlobal(r.center()));
        m_synthPress = true;
        QMouseEvent press(QEvent::MouseButtonPress, c, c, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        mousePressEvent(&press);
        QMouseEvent release(QEvent::MouseButtonRelease, c, c, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        mouseReleaseEvent(&release);
        m_synthPress = false;
    }
    // Down: the focused cell's dropdown, when it has one. The base has
    // none outside its edit (its Down list belongs to the overlay); Back's
    // list is the hold / right-click; the rest have nothing to drop.
    void openMenuFor(const QString& id) {
        const LaidItem* li = itemById(id);
        if (!li || !li->enabled) return;
        switch (li->kind) {
        case Cell::Hist:     showHistoryMenu(id); break;
        case Cell::Recent:   showPlacesMenu(false); break;
        case Cell::Chev:     showSiblingMenu(li->index); break;
        case Cell::Overflow: showOverflowMenu(); break;
        case Cell::Src:
        case Cell::SrcChev:  activate(QStringLiteral("src")); break;
        default: break;
        }
    }

    // ── History menu ──

    bool historyAvailable() const { return m_state.canBack || m_state.canForward; }

    // Hold elapsed with Back still pressed (release and leave both stop
    // the timer): the press becomes the menu's, not a click's.
    void onHoldElapsed() {
        if (m_pressedId != QLatin1String("back")) return;
        m_pressedId.clear();
        m_holdFired = true;
        showHistoryMenu(QStringLiteral("back"));
    }

    // Explorer's list: Back entries nearest first, then the place the user
    // is at now — checked and disabled, "you are here" — then Forward
    // entries nearest first. Each row's data is the step count its pick
    // asks for (0 on the current row, which never triggers). The checked
    // row is the divider, so there is no separator: with one, "back |
    // current" read as a boundary between the past and a lone item
    // rather than as a list you are somewhere in. The stacks arrive
    // oldest first (NavHistory's order), so both are walked from the
    // end. Hung under `cellId`: `hist`, or `back` for the right-click /
    // hold fallback.
    void showHistoryMenu(const QString& cellId) {
        const QRect r = itemRect(cellId);
        if (r.isNull()) return;
        const QVector<NavEntry> back = m_cb.backEntries ? m_cb.backEntries() : QVector<NavEntry>();
        const QVector<NavEntry> fwd  = m_cb.forwardEntries ? m_cb.forwardEntries() : QVector<NavEntry>();
        const QString here = m_cb.currentLabel ? m_cb.currentLabel() : QString();
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("rcxAddressBarHistoryMenu"));
        auto addRow = [&](const NavEntry& e, int delta) {
            QAction* a = menu->addAction(e.label);
            a->setData(delta);
            connect(a, &QAction::triggered, this, [this, delta] {
                if (m_cb.onHistoryJump) m_cb.onHistoryJump(delta);
            });
        };
        for (int i = back.size() - 1; i >= 0; --i) addRow(back[i], -(back.size() - i));
        if (!here.isEmpty() && (!back.isEmpty() || !fwd.isEmpty())) {
            QAction* cur = menu->addAction(here);
            cur->setCheckable(true);
            cur->setChecked(true);
            cur->setEnabled(false);
            cur->setData(0);
        }
        for (int i = fwd.size() - 1; i >= 0; --i) addRow(fwd[i], fwd.size() - i);
        if (menu->actions().isEmpty()) {
            QAction* none = menu->addAction(QStringLiteral("No history"));
            none->setEnabled(false);
        }
        ++m_historyMenuOpens;
        dismissRcxTooltip();
        popupUnderCell(menu, cellId, r);
    }

    void showOverflowMenu() {
        const QRect r = itemRect(QStringLiteral("overflow"));
        if (r.isNull()) return;
        // Built per open and freed when it hides: the hidden set changes with
        // every fold, so there is nothing worth caching.
        auto* menu = new QMenu(this);
        for (int i = 0; i < m_layout.fold && i < m_state.crumbs.size(); ++i) {
            QAction* a = menu->addAction(m_state.crumbs[i].label);
            connect(a, &QAction::triggered, this, [this, i] { if (m_cb.onCrumb) m_cb.onCrumb(i); });
        }
        // The « cell stays t.hover while its menu is up.
        m_menuOpenId = QStringLiteral("overflow");
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            m_menuOpenId.clear();
            update();
            menu->deleteLater();
        });
        update();
        menu->popup(mapToGlobal(QPoint(r.left(), r.bottom() + 1)));
    }

    // A menu hung under a cell: the cell stays t.hover (m_menuOpenId) until
    // the menu hides, and the menu frees itself then — every dropdown the
    // bar owns is built per open, since what it lists changes underneath.
    // Hang `menu` off `cellId`: the cell holds the menu-open highlight for
    // the life of the popup, and the menu deletes itself when it closes.
    void popupMenuFor(QMenu* menu, const QString& cellId, const QPoint& globalAt) {
        m_menuOpenId = cellId;
        connect(menu, &QMenu::aboutToHide, this, [this, menu, cellId] {
            if (m_menuOpenId == cellId) m_menuOpenId.clear();
            update();
            menu->deleteLater();
        });
        update();
        menu->popup(globalAt);
    }

    void popupUnderCell(QMenu* menu, const QString& cellId, const QRect& cell) {
        popupMenuFor(menu, cellId, mapToGlobal(QPoint(cell.left(), cell.bottom() + 1)));
    }

    // chev:<level>: the drillable fields of the class at crumb `level`,
    // in memory order, the trail's hop checked. Every drillable sibling is
    // listed — expanded or not — because a pick is a switch (collapse the
    // current hop, open the chosen one) and not a jump between things
    // already open. The chevron after the deepest crumb lists that class's
    // fields with nothing checked: drill further. Each row says where its
    // field leads: the address the controller placed it at (see
    // siblingsForCrumb — compose data for an open hop, one read for a
    // closed pointer, at this open only) in the row's tab column when it
    // is known, and always in the row's tooltip, "(unreadable)" included
    // — the GlobalTooltipBridge resolves a QMenu's action tips itself.
    void showSiblingMenu(int level) {
        const QString cellId = QStringLiteral("chev:%1").arg(level);
        const QRect r = itemRect(cellId);
        if (r.isNull()) return;
        const QVector<SiblingEntry> sibs = m_treeQ.siblingsOf ? m_treeQ.siblingsOf(level)
                                                              : QVector<SiblingEntry>();
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("rcxAddressBarSiblingMenu"));
        if (sibs.isEmpty()) {
            QAction* none = menu->addAction(QStringLiteral("No classes to open here"));
            none->setEnabled(false);
        }
        for (const SiblingEntry& e : sibs) {
            QAction* a = menu->addAction(siblingActionRowText(e));
            a->setToolTip(siblingActionText(e) + QStringLiteral("  ") + siblingAddressText(e));
            a->setCheckable(true);
            a->setChecked(e.current);
            a->setData(QVariant::fromValue<qulonglong>(e.id));
            connect(a, &QAction::triggered, this, [this, level, id = e.id] {
                if (m_cb.onSiblingPick) m_cb.onSiblingPick(level, id);
            });
        }
        dismissRcxTooltip();
        popupUnderCell(menu, cellId, r);
    }

    // The path edit's Down: the drillable fields of the class the segments
    // before the last dot reach (the roots when there is no dot yet),
    // fuzzy-filtered by the partial last segment; a pick REPLACES that
    // segment and leaves the overlay open — Enter still commits. Nothing
    // to offer → no menu.
    void showPathCompletionMenu() {
        if (!m_editVisible || m_editScope != EditScope::Path) return;
        const QString text = m_edit->text();
        const int dot = text.lastIndexOf(QLatin1Char('.'));
        const QString prefix  = dot < 0 ? QString() : text.left(dot);
        const QString partial = (dot < 0 ? text : text.mid(dot + 1)).trimmed();
        const QVector<SiblingEntry> fields = m_treeQ.fieldsAtPath ? m_treeQ.fieldsAtPath(prefix)
                                                                  : QVector<SiblingEntry>();
        auto* menu = new QMenu(this);
        menu->setObjectName(QStringLiteral("rcxAddressBarPathMenu"));
        for (const SiblingEntry& e : fields) {
            if (!partial.isEmpty() && fuzzyScore(partial, e.field) <= 0) continue;
            QAction* a = menu->addAction(siblingActionText(e));
            a->setData(e.field);
            connect(a, &QAction::triggered, this, [this, prefix, field = e.field] {
                if (!m_editVisible || m_editScope != EditScope::Path) return;
                m_edit->setText(prefix.isEmpty() ? field : prefix + QLatin1Char('.') + field);
                m_edit->setFocus();
                m_edit->deselect();
                m_edit->end(false);
            });
        }
        if (menu->actions().isEmpty()) { menu->deleteLater(); return; }
        // The overlay carries its own look; no cell to hold at t.hover.
        connect(menu, &QMenu::aboutToHide, this, [this, menu] { update(); menu->deleteLater(); });
        menu->popup(mapToGlobal(QPoint(m_editRect.left(), height())));
    }

    // The places menu: Recent (the Goto dialog's list), Bookmarks (name and
    // formula), Modules (<name.exe>, from the cached list). From the base
    // edit (Down) it is fuzzy-filtered by the typed text and a pick INSERTS
    // the formula into the overlay — the user still presses Enter. From the
    // recent cell it is unfiltered, a pick rebases (onRecentPick), and it
    // carries "Go to address…" and "Clear recent". Built per open, freed on
    // hide, anchored under the seam like the source popup.
    void showPlacesMenu(bool forEdit) {
        // The places list drops from the same edge the help hangs off, so the
        // two would overlap. The list is the more specific answer; the help
        // does not come back after it (the user has moved on to picking).
        dismissEditHelp();
        const QString typed = forEdit ? m_edit->text().trimmed() : QString();
        auto matches = [&typed](const QString& s) { return typed.isEmpty() || fuzzyScore(typed, s) > 0; };
        auto* menu = new QMenu(this);
        menu->setObjectName(forEdit ? QStringLiteral("rcxAddressBarEditMenu")
                                    : QStringLiteral("rcxAddressBarRecentMenu"));
        auto pick = [this, forEdit](const QString& formula) {
            if (forEdit) {
                if (!m_editVisible) return;
                m_edit->setText(formula);
                m_edit->setFocus();
                m_edit->deselect();
                m_edit->end(false);
            } else if (m_cb.onRecentPick) {
                m_cb.onRecentPick(formula);
            }
        };
        auto addEntry = [&](const QString& shown, const QString& formula) {
            QAction* a = menu->addAction(shown);
            a->setData(formula);
            connect(a, &QAction::triggered, this, [pick, formula] { pick(formula); });
        };
        const QStringList recent = GotoAddressDialog::loadRecent();
        QStringList recentShown;
        for (const QString& r : recent) if (matches(r)) recentShown << r;
        if (!recentShown.isEmpty()) {
            menu->addSection(QStringLiteral("Recent"));
            for (const QString& r : recentShown) addEntry(r, r);
        }
        const QVector<Bookmark> marks = m_cb.bookmarks ? m_cb.bookmarks() : QVector<Bookmark>();
        bool sectioned = false;
        for (const Bookmark& b : marks) {
            if (b.addressFormula.isEmpty()) continue;
            if (!matches(b.name) && !matches(b.addressFormula)) continue;
            if (!sectioned) { menu->addSection(QStringLiteral("Bookmarks")); sectioned = true; }
            addEntry(QStringLiteral("%1  %2").arg(b.name, b.addressFormula), b.addressFormula);
        }
        const QStringList mods = m_cb.modules ? m_cb.modules() : QStringList();
        sectioned = false;
        for (const QString& m : mods) {
            if (m.isEmpty() || !matches(m)) continue;
            if (!sectioned) { menu->addSection(QStringLiteral("Modules")); sectioned = true; }
            const QString f = QLatin1Char('<') + m + QLatin1Char('>');
            addEntry(f, f);
        }
        if (forEdit) {
            if (menu->actions().isEmpty()) { menu->deleteLater(); return; }   // nothing to offer
        } else {
            if (!menu->actions().isEmpty()) menu->addSeparator();
            QAction* go = menu->addAction(QStringLiteral("Go to address\u2026\tCtrl+G"));
            connect(go, &QAction::triggered, this, [this] { if (m_cb.onGotoDialog) m_cb.onGotoDialog(); });
            QAction* clear = menu->addAction(QStringLiteral("Clear recent"));
            clear->setEnabled(!recent.isEmpty());
            connect(clear, &QAction::triggered, this, [] { GotoAddressDialog::clearRecent(); });
        }
        const QRect anchor = forEdit ? m_editRect : itemRect(QStringLiteral("recent"));
        // The recent cell stays t.hover while its menu is up; the edit's
        // menu belongs to the overlay, which carries its own state.
        m_menuOpenId = forEdit ? QString() : QStringLiteral("recent");
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            if (m_menuOpenId == QLatin1String("recent")) m_menuOpenId.clear();
            update();
            menu->deleteLater();
        });
        update();
        menu->popup(mapToGlobal(QPoint(anchor.left(), height())));
    }

    // ── Base edit internals ──

    void applyEditStyle() {
        // PanelSearchField's interior (the shared builder), so the overlay and
        // the panel filter boxes are one field family — but with ITS padding
        // dropped: the overlay's insets are the cell's, not a search box's,
        // and they are set as text margins below.
        m_edit->setStyleSheet(panelFieldInteriorQss(m_theme)
                              + QStringLiteral("QLineEdit { padding: 0px; }"));
        syncEditTextMargins();
    }

    // Land the first glyph exactly where the cell painted its text, and put
    // the caret's room in the right inset where nothing can see it.
    //
    // A QLineEdit keeps a couple of px inside itself that no stylesheet can
    // take back, and the number is Qt's, not ours — so it is MEASURED, not
    // assumed: with our own margins at zero, the caret at position 0 sits
    // exactly on that reserve. Getting this wrong by 2 px is the whole
    // difference between an overlay that appears and one that shoves the line.
    void syncEditTextMargins() {
        const int selStart = m_edit->selectionStart();
        const int selLen   = m_edit->selectedText().size();
        const int cursor   = m_edit->cursorPosition();
        m_edit->setTextMargins(0, 0, 0, 0);
        m_edit->setCursorPosition(0);
        const int own = qBound(0, m_edit->cursorRect().left(), kBasePad);
        m_edit->setTextMargins(qMax(0, kBasePad - own), 0,
                               qMax(0, kEditRightInset - own), 0);
        if (selStart >= 0) m_edit->setSelection(selStart, selLen);
        else               m_edit->setCursorPosition(cursor);
    }

    // Where an overlay may reach: short of the recent cell by one chevron
    // width (so the field never touches its glyph), or the right margin
    // when the strip is too narrow to lay `recent` out.
    int editLimit() const {
        const QRect recent = itemRect(QStringLiteral("recent"));
        return recent.isNull() ? width() - kRightMargin : recent.left() - kChevW;
    }

    // The cell the open overlay stands in for — the thing whose pixels it has
    // to hide. Null for Path, which stands in for the whole trail and says so
    // by covering out to `recent`.
    QRect editCellRect() const {
        if (m_editScope == EditScope::Base)  return itemRect(QStringLiteral("base"));
        if (m_editScope == EditScope::Class) return itemRect(deepestCrumbId());
        return QRect();
    }

    // Where the preview's last pixel falls, or the field's right edge when
    // there is no preview.
    int editPreviewRight() const {
        return m_editPreview.isEmpty()
                   ? m_editRect.right()
                   : m_editRect.right() + QFontMetrics(font()).horizontalAdvance(m_editPreview);
    }

    // The overlay's geometry: the field at `r`, and what it covers — the
    // paper that hides what was there, where hover goes quiet.
    //
    // It is the CELL, not the rest of the strip. Papering out to `recent` was
    // right when the field was a 180-px slab that half-overlapped the crumbs
    // and left their tails showing; a field that is exactly its cell overlaps
    // nothing, and blanking the trail because you clicked the address is the
    // same sin as shoving it. So: the field, the cell it replaces, and the
    // preview's run — which at open is exactly the suffix's run, and grows
    // only with what you type. Path is the exception: it stands in for the
    // whole trail, so it covers the whole trail.
    void placeEdit(const QRect& rIn, int coveredLeft) {
        const QRect recent = itemRect(QStringLiteral("recent"));
        // The timeline's divider and buttons are a hard bound: a narrow-pane
        // fallback may widen the field over `recent`, never over the timeline's buttons.
        const int hard = m_layout.tlDividerX >= 0 ? m_layout.tlDividerX : width() - kRightMargin;
        QRect r = rIn;
        if (r.right() >= hard) r.setRight(qMax(r.left(), hard - 1));
        const int stop = qMin(hard, recent.isNull() ? width() - kRightMargin : recent.left());
        m_editRect = r;
        m_edit->setGeometry(r);
        const QRect cell = editCellRect();
        int right = cell.isNull() ? stop - 1
                                  : qMax(qMax(r.right(), cell.right()), editPreviewRight());
        right = qBound(r.right(), right, stop - 1);
        m_coveredRect = QRect(coveredLeft, kCellTop, right - coveredLeft + 1, kCellH);
        // The high-water mark editWidthFor reads back as a floor. Recording it
        // here — the one place a field is ever positioned — is what makes the
        // width monotonic for as long as the overlay is up: growEditToText can
        // widen it, and nothing narrows it until endEdit clears this. A field
        // that shrank as you backspaced would reflow the box under your own
        // caret and drag the preview beside it with every character.
        m_editGrownW = r.width();
    }

    // Every keystroke: widen the field to the text if the text outgrew it.
    // Never narrows (see placeEdit), so this is safe to call on any change.
    void growEditToText() {
        if (!m_editVisible) return;
        QRect r;
        int coveredLeft = 0;
        if (!editGeometryFor(m_editScope, m_edit->text(), &r, &coveredLeft)) return;
        placeEdit(r, coveredLeft);   // unconditional: the preview may have
                                     // moved even when the field did not
    }

    // resizeEvent while an edit is up: re-place the overlay for its scope
    // at the new layout. Geometry only — the overlay child keeps its
    // text, caret and selection through setGeometry. A strip that can no
    // longer lay the base out (nothing to anchor on) leaves the overlay
    // where it is rather than closing an edit the user is typing in.
    void followEditGeometry() {
        if (!m_editVisible) return;
        QRect r;
        int coveredLeft = 0;
        if (!editGeometryFor(m_editScope, m_edit->text(), &r, &coveredLeft)) return;
        placeEdit(r, coveredLeft);
        update();
    }

    // Show the overlay in `scope` at `r` on `text`, all selected.
    void openEdit(EditScope scope, const QRect& r, int coveredLeft, const QString& text) {
        leaveKeyboardMode(false);   // one focus owner: the overlay takes over
        m_editScope = scope;
        m_editVisible = true;
        m_relayoutDeferred = false;
        m_commitError.clear();
        placeEdit(r, coveredLeft);
        m_edit->setFont(editFont(scope));   // the cell's own face, weight included
        m_edit->setText(text);
        syncEditTextMargins();   // the insets are measured on the real text
        m_edit->selectAll();
        setFocusPolicy(Qt::StrongFocus);
        m_edit->show();
        m_edit->setFocus(Qt::OtherFocusReason);
        // The pointer may be parked over a cell the overlay just covered.
        m_hoverId.clear();
        unsetCursor();
        refreshToolTip();
        dismissRcxTooltip();
        update();
    }

    // Every keystroke: the scope's verdict picks the seam colour. BASE — the
    // parser's, and a parsing formula is evaluated for the "→ 0x…" preview
    // (the number the Goto dialog alone used to show). PATH — the
    // resolver's, naming the segment that does not exist. A refused commit
    // is cleared by the first change — the user is answering it.
    void onEditTextChanged() {
        if (!m_editVisible) return;
        m_commitError.clear();
        const QString text = m_edit->text().trimmed();
        if (m_editScope == EditScope::Class) {
            // The one rule line 0's rename also enforces: a class needs a
            // name. Nothing stricter — a bar that refused what the editor
            // accepts would be the odd one out, and the generator, not this
            // field, is where a name's shape is anyone's business.
            m_editValid = !text.isEmpty();
            m_editPreview = m_editValid ? QString() : QStringLiteral("a class needs a name");
            growEditToText();
            update();
            return;
        }
        if (m_editScope == EditScope::Path) {
            const QString why = m_treeQ.validatePath ? m_treeQ.validatePath(text) : QString();
            m_editValid = why.isEmpty();
            m_editPreview = why;
            growEditToText();
            update();
            return;
        }
        const QString why = fmt::validateBaseAddress(text);
        m_editValid = why.isEmpty();
        if (m_editValid) {
            const QString r = m_cb.evaluate ? m_cb.evaluate(text) : QString();
            // The resting rule, live: a formula gets " = 0x…", a literal gets
            // nothing. `0x1000 = 0x1000` is not an evaluation, it is the same
            // number twice — and it appeared out of nowhere the moment you
            // clicked into a plain address. Compared case-insensitively
            // because the parser echoes hex in its own case.
            m_editPreview = (r.isEmpty() || r.compare(text, Qt::CaseInsensitive) == 0)
                                ? QString()
                                : baseSepText() + r;
        } else {
            m_editPreview = why;
        }
        // AFTER the preview: the covered rect has to know how far it runs.
        growEditToText();
        update();
    }

    void commitEdit() {
        const QString text = m_edit->text().trimmed();
        if (text.isEmpty()) {
            m_commitError = m_editScope == EditScope::Path  ? QStringLiteral("empty path")
                          : m_editScope == EditScope::Class ? QStringLiteral("a class needs a name")
                                                            : QStringLiteral("empty formula");
            m_editPreview = m_commitError;
            update();
            return;
        }
        // The controller answers through baseCommitFinished /
        // pathCommitFinished, synchronously (the signal chain is direct).
        // With no one listening the overlay simply stays: there is nobody
        // to accept the edit.
        if (m_editScope == EditScope::Path) { if (m_cb.onPathCommit) m_cb.onPathCommit(text); }
        else if (m_editScope == EditScope::Class) {
            if (m_cb.onClassRename) m_cb.onClassRename(m_classEditId, text);
        }
        else                                { if (m_cb.onBaseCommit) m_cb.onBaseCommit(text); }
    }

    // Close the overlay. The cells show the state's text either way — a
    // cancel "restores" by dropping the overlay's text, a success by the
    // state push the navigation already delivered, applied here.
    void endEdit() {
        if (!m_editVisible) return;
        m_editVisible = false;      // before hide(): its FocusOut must not re-enter
        m_editScope = EditScope::None;
        m_coveredRect = QRect();
        m_commitError.clear();
        m_editPreview.clear();
        m_editGrownW = 0;           // the next open measures its own value
        m_classEditId = 0;
        dismissEditHelp();
        m_edit->hide();
        setFocusPolicy(Qt::NoFocus);
        if (m_relayoutDeferred) { m_relayoutDeferred = false; markLayoutDirty(); }
        refreshToolTip();
        update();
    }

    // ── State ──

    Callbacks       m_cb;
    TreeQueries     m_treeQ;       // the menus' and the path edit's window onto the tree
    AddressBarState m_state;
    Theme           m_theme;
    int             m_stateApplyCount = 0;

    mutable Layout  m_layout;
    mutable bool    m_layoutDirty = true;

    QString m_hoverId;
    QString m_pressedId;
    QString m_menuOpenId;      // cell held at t.hover while its menu (or the source popup) is up
    bool    m_swallowNextPress = false;   // the press that ended an edit is spent...
    QRect   m_swallowRect;                // ...only inside what the overlay covered
    // Keyboard mode: the focused cell's id while it lasts; m_synthPress
    // marks the press Enter synthesises so mousePressEvent does not read
    // it as the mouse taking over.
    bool    m_kbMode = false;
    QString m_kbFocusId;
    bool    m_synthPress = false;
    // Back's press-and-hold (the history list without a `hist` cell).
    QTimer  m_holdTimer;
    bool    m_holdFired = false;          // the hold opened the menu: the release is not a click
    int     m_holdDelayMs = kHoldDelayMs;
    int     m_historyMenuOpens = 0;       // test hook: opens counted, not seen
    // The edit overlay (base or path scope). While it is visible a state
    // push is stored but not laid out (see setState); endEdit applies the
    // deferred relayout.
    address_bar_detail::EditField* m_edit = nullptr;
    QRect      m_editRect;
    QRect      m_coveredRect;      // overlay + the paper beside it: hover-quiet while up
    EditScope  m_editScope = EditScope::None;
    bool       m_editVisible = false;
    bool       m_relayoutDeferred = false;
    bool       m_editValid = true;
    int        m_editGrownW = 0;   // widest the open overlay has been: a one-way floor
    uint64_t   m_classEditId = 0;  // Class scope: the node the open rename will rename
    // The grammar help: a PRIVATE tooltip (the shared one dies on KeyPress)
    // plus the timer that keeps its own expiry from ending it mid-formula.
    RcxTooltip* m_helpTip = nullptr;
    QTimer      m_helpKeepAlive;
    QString    m_editPreview;      // "= 0x…", or the parser's / controller's words
    QString    m_commitError;      // set by a refused commit, cleared by the next keystroke
    // backInkInsetDev's cache: the SVG's ink inset at the last dpr seen.
    mutable qreal m_inkInsetDpr = -1.0;
    mutable int   m_inkInsetDev = 0;
};

}  // namespace rcx
