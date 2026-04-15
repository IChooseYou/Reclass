#pragma once
#include <QWidget>
#include <QDockWidget>
#include <QPainter>
#include <QMainWindow>
#include <QTabBar>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>
#include <cmath>

namespace rcx {

// ── Drop zone identifiers ──
enum class DropZone {
    None,
    Left, Right, Top, Bottom,  // Split zones relative to hovered dock
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

        // Soft scrim
        p.fillRect(rect(), QColor(0, 0, 0, 20));

        // Highlight the hovered dock widget area
        if (m_hoveredDock && m_hoveredDock != m_draggedDock) {
            QRect dr = m_hoveredDock->geometry();
            p.fillRect(dr, QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 15));
        }

        // Edge zone strips
        drawEdgeZones(p);

        // Diamond targets
        if (m_hoveredDock && m_hoveredDock != m_draggedDock)
            drawDiamondTargets(p);

        // Preview rectangle
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

    static constexpr int kEdgeW      = 36;
    static constexpr int kTargetSz   = 28;
    static constexpr int kTargetDist = 52;
    static constexpr int kHitR       = 20;

    QDockWidget* findDockAt(QPoint localPos) const {
        for (auto* child : m_mainWindow->findChildren<QDockWidget*>()) {
            if (child == m_draggedDock) continue;
            if (!child->isVisible() || child->isFloating()) continue;
            if (child->objectName().startsWith(QStringLiteral("_sentinel_"))) continue;
            if (child->geometry().contains(localPos))
                return child;
        }
        return nullptr;
    }

    void updateDropTarget(QPoint pos) {
        QRect wr = rect();

        // Edge zones
        if (pos.x() < kEdgeW) { m_activeZone = DropZone::EdgeLeft; m_hoveredDock = nullptr; return; }
        if (pos.x() > wr.width() - kEdgeW) { m_activeZone = DropZone::EdgeRight; m_hoveredDock = nullptr; return; }
        if (pos.y() < kEdgeW) { m_activeZone = DropZone::EdgeTop; m_hoveredDock = nullptr; return; }
        if (pos.y() > wr.height() - kEdgeW) { m_activeZone = DropZone::EdgeBottom; m_hoveredDock = nullptr; return; }

        m_hoveredDock = findDockAt(pos);
        if (!m_hoveredDock) { m_activeZone = DropZone::Float; return; }

        // Diamond hit test
        QPoint center = m_hoveredDock->geometry().center();
        QPoint rel = pos - center;

        struct { int dx; int dy; DropZone zone; } targets[] = {
            { 0, -kTargetDist, DropZone::Top },
            { 0,  kTargetDist, DropZone::Bottom },
            {-kTargetDist, 0,  DropZone::Left },
            { kTargetDist, 0,  DropZone::Right },
            { 0, 0,            DropZone::Center },
        };
        for (const auto& t : targets) {
            int dx = rel.x() - t.dx, dy = rel.y() - t.dy;
            if (dx * dx + dy * dy < kHitR * kHitR) {
                m_activeZone = t.zone;
                return;
            }
        }

        // Quadrant fallback
        if (qAbs(rel.x()) > qAbs(rel.y()))
            m_activeZone = (rel.x() < 0) ? DropZone::Left : DropZone::Right;
        else
            m_activeZone = (rel.y() < 0) ? DropZone::Top : DropZone::Bottom;
    }

    void drawEdgeZones(QPainter& p) {
        QColor bg(m_accent.red(), m_accent.green(), m_accent.blue(), 12);
        QColor hl(m_accent.red(), m_accent.green(), m_accent.blue(), 50);
        QRect wr = rect();
        int ew = kEdgeW;

        auto draw = [&](QRect r, DropZone z) {
            bool active = (m_activeZone == z);
            p.fillRect(r, active ? hl : bg);
            if (active) {
                // Thin accent line on the edge
                switch (z) {
                case DropZone::EdgeLeft:   p.fillRect(r.left(), r.top(), 3, r.height(), m_accent); break;
                case DropZone::EdgeRight:  p.fillRect(r.right()-2, r.top(), 3, r.height(), m_accent); break;
                case DropZone::EdgeTop:    p.fillRect(r.left(), r.top(), r.width(), 3, m_accent); break;
                case DropZone::EdgeBottom: p.fillRect(r.left(), r.bottom()-2, r.width(), 3, m_accent); break;
                default: break;
                }
            }
        };

        draw(QRect(0, ew, ew, wr.height()-2*ew), DropZone::EdgeLeft);
        draw(QRect(wr.width()-ew, ew, ew, wr.height()-2*ew), DropZone::EdgeRight);
        draw(QRect(ew, 0, wr.width()-2*ew, ew), DropZone::EdgeTop);
        draw(QRect(ew, wr.height()-ew, wr.width()-2*ew, ew), DropZone::EdgeBottom);
    }

    void drawDiamondTargets(QPainter& p) {
        QPoint center = m_hoveredDock->geometry().center();
        int d = kTargetDist;
        int sz = kTargetSz;
        int half = sz / 2;

        struct Target { int dx; int dy; DropZone zone; };
        Target targets[] = {
            { 0, -d, DropZone::Top },
            { 0,  d, DropZone::Bottom },
            {-d,  0, DropZone::Left },
            { d,  0, DropZone::Right },
            { 0,  0, DropZone::Center },
        };

        for (const auto& t : targets) {
            QPoint tp(center.x() + t.dx, center.y() + t.dy);
            QRect tr(tp.x() - half, tp.y() - half, sz, sz);
            bool active = (m_activeZone == t.zone);

            // Rounded rect background
            QColor bg = active ? QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 200)
                               : QColor(20, 20, 20, 180);
            QColor border = active ? m_accent : QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 100);
            p.setPen(QPen(border, 1.5));
            p.setBrush(bg);
            p.drawRoundedRect(tr, 5, 5);

            // Arrow icon via polygon
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, active ? 240 : 140));
            int as = active ? 6 : 5;  // arrow size
            int cx = tp.x(), cy = tp.y();

            switch (t.zone) {
            case DropZone::Top:
                p.drawPolygon(QPolygon({QPoint(cx, cy-as), QPoint(cx-as, cy+as/2), QPoint(cx+as, cy+as/2)}));
                break;
            case DropZone::Bottom:
                p.drawPolygon(QPolygon({QPoint(cx, cy+as), QPoint(cx-as, cy-as/2), QPoint(cx+as, cy-as/2)}));
                break;
            case DropZone::Left:
                p.drawPolygon(QPolygon({QPoint(cx-as, cy), QPoint(cx+as/2, cy-as), QPoint(cx+as/2, cy+as)}));
                break;
            case DropZone::Right:
                p.drawPolygon(QPolygon({QPoint(cx+as, cy), QPoint(cx-as/2, cy-as), QPoint(cx-as/2, cy+as)}));
                break;
            case DropZone::Center:
                // Tabify icon: overlapping squares
                p.fillRect(cx-4, cy-4, 7, 7, QColor(255, 255, 255, active ? 220 : 120));
                p.fillRect(cx-1, cy-1, 7, 7, QColor(255, 255, 255, active ? 180 : 80));
                break;
            default: break;
            }
        }

        // Connect targets with faint lines (diamond shape)
        p.setPen(QPen(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 40), 1));
        p.setBrush(Qt::NoBrush);
        QPoint ct = center;
        p.drawLine(ct.x(), ct.y()-d, ct.x()+d, ct.y());
        p.drawLine(ct.x()+d, ct.y(), ct.x(), ct.y()+d);
        p.drawLine(ct.x(), ct.y()+d, ct.x()-d, ct.y());
        p.drawLine(ct.x()-d, ct.y(), ct.x(), ct.y()-d);
    }

    void drawPreviewRect(QPainter& p) {
        QRect preview = computePreviewRect();
        if (preview.isNull()) return;

        p.setPen(QPen(m_accent, 2));
        p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 35));
        p.drawRect(preview.adjusted(1, 1, -1, -1));

        // Zone label centered in preview
        QString label;
        switch (m_activeZone) {
        case DropZone::Left:       label = QStringLiteral("Split Left"); break;
        case DropZone::Right:      label = QStringLiteral("Split Right"); break;
        case DropZone::Top:        label = QStringLiteral("Split Top"); break;
        case DropZone::Bottom:     label = QStringLiteral("Split Bottom"); break;
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
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 180));
            p.drawRoundedRect(lr, 4, 4);
            p.setPen(Qt::white);
            p.drawText(lr, Qt::AlignCenter, label);
        }
    }

    QRect computePreviewRect() const {
        QRect wr = rect();
        switch (m_activeZone) {
        case DropZone::EdgeLeft:   return QRect(0, 0, wr.width()/4, wr.height());
        case DropZone::EdgeRight:  return QRect(wr.width()*3/4, 0, wr.width()/4, wr.height());
        case DropZone::EdgeTop:    return QRect(0, 0, wr.width(), wr.height()/4);
        case DropZone::EdgeBottom: return QRect(0, wr.height()*3/4, wr.width(), wr.height()/4);
        default: break;
        }
        if (!m_hoveredDock) return {};
        QRect dr = m_hoveredDock->geometry();
        switch (m_activeZone) {
        case DropZone::Left:   return QRect(dr.left(), dr.top(), dr.width()/2, dr.height());
        case DropZone::Right:  return QRect(dr.center().x(), dr.top(), dr.width()/2, dr.height());
        case DropZone::Top:    return QRect(dr.left(), dr.top(), dr.width(), dr.height()/2);
        case DropZone::Bottom: return QRect(dr.left(), dr.center().y(), dr.width(), dr.height()/2);
        case DropZone::Center: return dr;
        default: return {};
        }
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
        p.setBrush(QColor(25, 25, 25, 220));
        p.drawRoundedRect(lr, 4, 4);
        p.setPen(QColor(200, 200, 200));
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
