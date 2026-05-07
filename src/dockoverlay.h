#pragma once
#include "themes/theme.h"
#include <QWidget>
#include <QDockWidget>
#include <QPainter>
#include <QMainWindow>
#include <QStatusBar>
#include <QTabBar>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>
#include <cmath>

namespace rcx {

// ── Drop zone identifiers ──
// Per-dock split zones (Left/Right/Top/Bottom) are deliberately retained
// in the enum for ABI/numeric stability but are NEVER produced by the
// hit-tester or drawn as diamonds. Splitting a sidebar in half is
// nonsensical UX (e.g. splitting a 280-px workspace into two 140-px
// columns) — the user wanted these gone.
enum class DropZone {
    None,
    Left, Right, Top, Bottom,  // [removed from UI — kept for value stability]
    Center,                     // Tabify with hovered dock group
    Float,                      // Leave floating
    EdgeLeft, EdgeRight, EdgeTop, EdgeBottom  // Outer window frame zones
};

// ── DockOverlay ──
// Transparent overlay covering QMainWindow during dock drag.
// Shows drop zone targets (diamond pattern + edge strips) and a blue preview rectangle.

class DockOverlay : public QWidget {
    Q_OBJECT
public:
    explicit DockOverlay(QMainWindow* parent)
        : QWidget(parent), m_mainWindow(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setCursor(Qt::ClosedHandCursor);
        hide();
    }

    void setAccentColor(const QColor& c) { m_accent = c; }

    // Wire theme colours so the overlay adapts to light/dark themes instead
    // of using hard-coded RGB. Call this from MainWindow::applyTheme.
    void setTheme(const Theme& t) {
        m_theme = t;
        // Accent defaults to the theme's focus border if not otherwise set.
        if (!m_accentOverride) m_accent = t.borderFocused;
    }
    void setAccentColorOverride(const QColor& c) {
        m_accent = c;
        m_accentOverride = true;
    }

    void beginDrag(QDockWidget* dock, const QString& title) {
        m_draggedDock = dock;
        m_dragTitle = title;
        m_activeZone = DropZone::None;
        m_hoveredDock = nullptr;
        setGeometry(m_mainWindow->rect());
        raise();
        show();
        grabMouse();
        setFocus();
    }

    void endDrag() {
        releaseMouse();
        hide();
        m_draggedDock = nullptr;
        m_hoveredDock = nullptr;
        m_activeZone = DropZone::None;
    }

    DropZone activeZone() const { return m_activeZone; }
    QDockWidget* draggedDock() const { return m_draggedDock; }
    QDockWidget* hoveredDock() const { return m_hoveredDock; }

protected:
    void mouseMoveEvent(QMouseEvent* e) override {
        m_cursorPos = e->pos();
        updateDropTarget(e->pos());
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        DropZone zone = m_activeZone;
        QDockWidget* target = m_hoveredDock;
        QDockWidget* source = m_draggedDock;
        endDrag();
        if (source && zone != DropZone::None)
            emit dropRequested(source, target, zone);
        else if (source)
            emit dragCancelled(source);
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Escape) {
            QDockWidget* source = m_draggedDock;
            endDrag();
            if (source) emit dragCancelled(source);
        }
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // The full-rect scrim was dropped — it composed weirdly with Qt's
        // own drag-preview windows and produced a visible odd-color block
        // across the editor area. The edge strips, diamond, and preview
        // rect provide all the visual feedback needed without dimming
        // the rest of the window.

        // Edge zone strips
        drawEdgeZones(p);

        // Diamond target (Center / Tabify) — only when hovering a doc dock
        if (m_hoveredDock && m_hoveredDock != m_draggedDock)
            drawDiamondTargets(p);

        // Preview rectangle for the currently-active zone
        if (m_activeZone != DropZone::None && m_activeZone != DropZone::Float)
            drawPreviewRect(p);

        // Floating label near cursor showing dock title
        drawCursorLabel(p);
    }

signals:
    void dropRequested(QDockWidget* source, QDockWidget* target, DropZone zone);
    void dragCancelled(QDockWidget* source);

private:
    QMainWindow* m_mainWindow;
    QDockWidget* m_draggedDock = nullptr;
    QDockWidget* m_hoveredDock = nullptr;
    DropZone     m_activeZone  = DropZone::None;
    QPoint       m_cursorPos;
    QString      m_dragTitle;
    QColor       m_accent{70, 130, 220};
    bool         m_accentOverride = false;
    Theme        m_theme;  // populated by setTheme; fields empty until wired

    // Small alpha helpers — keep alphas readable; themes supply RGB.
    QColor dim(const QColor& c, int alpha) const {
        return QColor(c.red(), c.green(), c.blue(), alpha);
    }
    QColor textOrWhite(int alpha) const {
        const QColor& c = m_theme.text.isValid() ? m_theme.text : QColor(255, 255, 255);
        return dim(c, alpha);
    }
    QColor surfaceOrDark(int alpha) const {
        const QColor& c = m_theme.backgroundAlt.isValid()
            ? m_theme.backgroundAlt : QColor(20, 20, 20);
        return dim(c, alpha);
    }

    static constexpr int kEdgeW      = 36;
    static constexpr int kTargetSz   = 28;
    static constexpr int kTargetDist = 52;
    static constexpr int kHitR       = 20;

    // The overlay covers the entire QMainWindow including its custom
    // title bar and menu bar, but Qt's QMainWindow refuses to dock into
    // that chrome area — so edge strips drawn at y=0..kEdgeW would
    // either be invisible (covered by menu bar) or visually wrong
    // (poking out under the menu bar). Compute a "content rect" that
    // excludes the chrome, and use it for BOTH drawing and hit-testing
    // so the two stay in lockstep.
    QRect contentRect() const {
        QRect r = rect();
        // Skip past any QMainWindow chrome: top widget + menu bar.
        int top = 0;
        if (auto* tb = m_mainWindow->findChild<QWidget*>(QStringLiteral("TitleBarWidget")))
            if (tb->isVisible()) top = qMax(top, tb->geometry().bottom() + 1);
        if (auto* mb = m_mainWindow->menuWidget())
            if (mb->isVisible()) top = qMax(top, mb->geometry().bottom() + 1);
        if (top > 0) r.setTop(top);
        // Skip past the status bar at the bottom.
        if (auto* sb = m_mainWindow->statusBar())
            if (sb->isVisible()) r.setBottom(sb->geometry().top() - 1);
        return r;
    }

    // Only doc-tab docks (objectName "DocDock_*") are valid Center-tabify
    // targets. Tabifying with a sidebar (workspace/scanner/symbols/
    // bookmarks) would hide that sidebar behind the new dock — every
    // user complaint about "drop on workspace replaced workspace" was
    // this. Sidebars are reachable via EdgeX zones instead.
    static bool isTabbableTarget(QDockWidget* d) {
        return d && d->objectName().startsWith(QStringLiteral("DocDock_"));
    }

    QDockWidget* findDockAt(QPoint localPos) const {
        for (auto* child : m_mainWindow->findChildren<QDockWidget*>()) {
            if (child == m_draggedDock) continue;
            if (!child->isVisible() || child->isFloating()) continue;
            if (child->objectName().startsWith(QStringLiteral("_sentinel_"))) continue;
            if (!isTabbableTarget(child)) continue;
            if (child->geometry().contains(localPos))
                return child;
        }
        return nullptr;
    }

    // Map an edge zone to its dock area for allowedAreas() filtering.
    Qt::DockWidgetArea edgeZoneArea(DropZone z) const {
        switch (z) {
        case DropZone::EdgeLeft:   return Qt::LeftDockWidgetArea;
        case DropZone::EdgeRight:  return Qt::RightDockWidgetArea;
        case DropZone::EdgeTop:    return Qt::TopDockWidgetArea;
        case DropZone::EdgeBottom: return Qt::BottomDockWidgetArea;
        default: return Qt::NoDockWidgetArea;
        }
    }

    // True iff the dragged dock can actually be placed in the area this
    // zone targets. Used to filter zone activation so the overlay never
    // promises a drop that Qt will silently reject.
    bool zoneAllowed(DropZone z) const {
        if (!m_draggedDock) return true;
        Qt::DockWidgetAreas allowed = m_draggedDock->allowedAreas();
        if (z >= DropZone::EdgeLeft && z <= DropZone::EdgeBottom)
            return (allowed & edgeZoneArea(z)) != 0;
        // Center (tabify) and Float don't map to a specific QMainWindow
        // area — Center docks alongside the hovered peer (which already
        // lives in an allowed area, since it's docked), Float just makes
        // a top-level window.
        return true;
    }

    void updateDropTarget(QPoint pos) {
        QRect wr = contentRect();

        // Cursor in the chrome (title bar / menu bar / status bar) — no
        // drop possible there, just defer.
        if (!wr.contains(pos)) {
            m_hoveredDock = nullptr;
            m_activeZone = DropZone::None;
            return;
        }

        // Edge zones — only activate if the dragged dock's allowedAreas
        // includes that side, and use the content rect so the strips
        // line up with the dockable area (not the menu bar).
        if (pos.x() < wr.left() + kEdgeW && zoneAllowed(DropZone::EdgeLeft))   { m_activeZone = DropZone::EdgeLeft;   m_hoveredDock = nullptr; return; }
        if (pos.x() > wr.right() - kEdgeW && zoneAllowed(DropZone::EdgeRight))  { m_activeZone = DropZone::EdgeRight;  m_hoveredDock = nullptr; return; }
        if (pos.y() < wr.top() + kEdgeW && zoneAllowed(DropZone::EdgeTop))    { m_activeZone = DropZone::EdgeTop;    m_hoveredDock = nullptr; return; }
        if (pos.y() > wr.bottom() - kEdgeW && zoneAllowed(DropZone::EdgeBottom)) { m_activeZone = DropZone::EdgeBottom; m_hoveredDock = nullptr; return; }

        m_hoveredDock = findDockAt(pos);
        if (!m_hoveredDock) { m_activeZone = DropZone::Float; return; }

        // Diamond hit test — only Center remains. Per-dock split zones
        // (Left/Right/Top/Bottom) were removed: splitting a sidebar dock
        // into two halves was the cause of the recurring "split makes a
        // tiny column inside the workspace" bug.
        QPoint center = m_hoveredDock->geometry().center();
        QPoint rel = pos - center;
        if (rel.x() * rel.x() + rel.y() * rel.y() < kHitR * kHitR) {
            m_activeZone = DropZone::Center;
            return;
        }
        // Cursor over a dock body but outside the Center diamond — fall
        // back to Center (tabify with hovered dock) so dropping anywhere
        // on a dock face still does something useful.
        m_activeZone = DropZone::Center;
    }

    void drawEdgeZones(QPainter& p) {
        // Edge zones are INVISIBLE until activated (cursor near that
        // edge). The previous always-on faint-grey strips just looked
        // like graphical clutter — VS / IntelliJ show edge targets only
        // as the active highlight. Hit-testing still works for all four
        // edges; we just don't paint the inactive state anymore.
        if (m_activeZone < DropZone::EdgeLeft || m_activeZone > DropZone::EdgeBottom)
            return;

        QRect wr = contentRect();
        int ew = kEdgeW;
        int L = wr.left(), T = wr.top(), R = wr.right(), B = wr.bottom();
        int W = wr.width(), H = wr.height();

        QRect r;
        switch (m_activeZone) {
        case DropZone::EdgeLeft:   r = QRect(L,            T + ew,    ew,        H - 2*ew); break;
        case DropZone::EdgeRight:  r = QRect(R - ew + 1,   T + ew,    ew,        H - 2*ew); break;
        case DropZone::EdgeTop:    r = QRect(L + ew,       T,         W - 2*ew,  ew);       break;
        case DropZone::EdgeBottom: r = QRect(L + ew,       B - ew + 1, W - 2*ew, ew);       break;
        default: return;
        }

        // Filled translucent accent + 3px solid accent stripe along the
        // inside edge. Visible without being noisy.
        QColor hl(m_accent.red(), m_accent.green(), m_accent.blue(), 60);
        p.fillRect(r, hl);
        switch (m_activeZone) {
        case DropZone::EdgeLeft:   p.fillRect(r.left(),     r.top(),    3, r.height(), m_accent); break;
        case DropZone::EdgeRight:  p.fillRect(r.right()-2,  r.top(),    3, r.height(), m_accent); break;
        case DropZone::EdgeTop:    p.fillRect(r.left(),     r.top(),    r.width(), 3, m_accent); break;
        case DropZone::EdgeBottom: p.fillRect(r.left(),     r.bottom()-2, r.width(), 3, m_accent); break;
        default: break;
        }
    }

    void drawDiamondTargets(QPainter& p) {
        // Only the Center (Tabify) diamond is drawn now. Per-dock split
        // diamonds were removed — they made nonsense layouts on small
        // sidebars (a 280-px workspace dock would offer to split itself
        // into two 140-px columns).
        QPoint tp = m_hoveredDock->geometry().center();
        int sz = kTargetSz;
        int half = sz / 2;
        QRect tr(tp.x() - half, tp.y() - half, sz, sz);
        bool active = (m_activeZone == DropZone::Center);

        QColor bg     = active ? textOrWhite(220) : surfaceOrDark(220);
        QColor border = active ? m_accent
                                : (m_theme.text.isValid() ? m_theme.text
                                                          : QColor(255, 255, 255));
        p.setPen(QPen(border, active ? 2.0 : 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(tr, 5, 5);

        p.setPen(Qt::NoPen);
        int cx = tp.x(), cy = tp.y();
        // Tabify icon: overlapping squares. Inverts when active so the
        // icon still reads against the bright text-color fill.
        p.fillRect(cx-4, cy-4, 7, 7, active ? surfaceOrDark(220) : textOrWhite(160));
        p.fillRect(cx-1, cy-1, 7, 7, active ? surfaceOrDark(160) : textOrWhite(100));
    }

    void drawPreviewRect(QPainter& p) {
        QRect preview = computePreviewRect();
        if (preview.isNull()) return;

        // Theme-text outline carries the contrast; accent provides the
        // translucent fill so the brand color still reads as a hint.
        QColor outline = m_theme.text.isValid() ? m_theme.text
                                                : QColor(255, 255, 255);
        p.setPen(QPen(outline, 2));
        p.setBrush(dim(m_accent, 60));
        p.drawRect(preview.adjusted(1, 1, -1, -1));

        // Zone label centered in preview. Per-dock split labels removed.
        QString label;
        switch (m_activeZone) {
        case DropZone::Center:     label = QStringLiteral("Tabify"); break;
        case DropZone::EdgeLeft:   label = QStringLiteral("Dock Left"); break;
        case DropZone::EdgeRight:  label = QStringLiteral("Dock Right"); break;
        case DropZone::EdgeTop:    label = QStringLiteral("Dock Top"); break;
        case DropZone::EdgeBottom: label = QStringLiteral("Dock Bottom"); break;
        default: break;
        }
        if (!label.isEmpty()) {
            QFont f = font();
            f.setPointSize(10);
            f.setBold(true);
            p.setFont(f);
            QFontMetrics fm(f);
            int tw = fm.horizontalAdvance(label) + 16;
            int th = fm.height() + 8;
            QRect lr(preview.center().x() - tw/2, preview.center().y() - th/2, tw, th);
            // Surface fill + theme-text border + theme-text foreground.
            // Guarantees the label box and text are both visible regardless
            // of the theme's accent saturation.
            QColor labelBorder = m_theme.text.isValid() ? m_theme.text
                                                        : QColor(255, 255, 255);
            p.setPen(QPen(labelBorder, 1));
            p.setBrush(surfaceOrDark(230));
            p.drawRoundedRect(lr, 4, 4);
            p.setPen(labelBorder);
            p.drawText(lr, Qt::AlignCenter, label);
        }
    }

    QRect computePreviewRect() const {
        // Edge previews live inside the content rect (no chrome) so the
        // 1/4-window highlight matches what the user actually gets after
        // the drop. Using rect() here would draw over the title/menu bar.
        QRect wr = contentRect();
        int L = wr.left(), T = wr.top(), W = wr.width(), H = wr.height();
        switch (m_activeZone) {
        case DropZone::EdgeLeft:   return QRect(L,             T,             W / 4, H);
        case DropZone::EdgeRight:  return QRect(L + W * 3 / 4, T,             W / 4, H);
        case DropZone::EdgeTop:    return QRect(L,             T,             W,     H / 4);
        case DropZone::EdgeBottom: return QRect(L,             T + H * 3 / 4, W,     H / 4);
        default: break;
        }
        if (!m_hoveredDock) return {};
        // Center is the only per-dock zone that survives — preview the
        // entire hovered dock as the tabify target.
        if (m_activeZone == DropZone::Center) return m_hoveredDock->geometry();
        return {};
    }

    void drawCursorLabel(QPainter& p) {
        if (m_dragTitle.isEmpty()) return;
        QFont f = font();
        f.setPointSize(9);
        p.setFont(f);
        QFontMetrics fm(f);

        QString text = m_dragTitle;
        if (text.size() > 30) text = text.left(28) + QStringLiteral("\u2026");

        int tw = fm.horizontalAdvance(text) + 16;
        int th = fm.height() + 8;
        int lx = m_cursorPos.x() + 16;
        int ly = m_cursorPos.y() + 20;

        // Keep on screen
        if (lx + tw > width()) lx = m_cursorPos.x() - tw - 4;
        if (ly + th > height()) ly = m_cursorPos.y() - th - 4;

        QRect lr(lx, ly, tw, th);
        p.setPen(Qt::NoPen);
        p.setBrush(surfaceOrDark(220));
        p.drawRoundedRect(lr, 4, 4);
        p.setPen(m_theme.textDim.isValid() ? m_theme.textDim : QColor(200, 200, 200));
        p.drawText(lr, Qt::AlignCenter, text);
    }
};

// ── DockDragDetector ──
// Event filter on dock tab bars. Detects drag initiation and emits dragStarted.

class DockDragDetector : public QObject {
    Q_OBJECT
public:
    explicit DockDragDetector(QMainWindow* mainWindow, QObject* parent = nullptr)
        : QObject(parent), m_mainWindow(mainWindow) {}

signals:
    void dragStarted(QDockWidget* dock, QPoint globalPos);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        auto* tabBar = qobject_cast<QTabBar*>(obj);
        if (!tabBar) return false;

        if (event->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton) {
                m_pressPos = me->pos();
                m_pressTab = tabBar->tabAt(m_pressPos);
                m_dragging = false;
            }
        }

        if (event->type() == QEvent::MouseMove && m_pressTab >= 0) {
            auto* me = static_cast<QMouseEvent*>(event);
            if (!m_dragging && (me->pos() - m_pressPos).manhattanLength() > 14) {
                m_dragging = true;
                QString title = tabBar->tabText(m_pressTab);
                if (title == QStringLiteral("\u200B")) {
                    m_pressTab = -1;
                    return false;
                }
                QDockWidget* dock = findDockByTitle(title);
                if (dock) {
                    m_pressTab = -1;
                    emit dragStarted(dock, me->globalPosition().toPoint());
                    return true;
                }
            }
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            m_pressTab = -1;
            m_dragging = false;
        }

        return false;
    }

private:
    QMainWindow* m_mainWindow;
    QPoint m_pressPos;
    int    m_pressTab = -1;
    bool   m_dragging = false;

    QDockWidget* findDockByTitle(const QString& title) const {
        for (auto* dock : m_mainWindow->findChildren<QDockWidget*>()) {
            if (dock->windowTitle() == title) return dock;
        }
        return nullptr;
    }
};

} // namespace rcx
