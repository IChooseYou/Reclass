#pragma once
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QApplication>
#include <QMouseEvent>
#include <functional>

namespace rcx {

// ── Modern arrow tooltip ──
// Draws a rounded-rect body with a triangular arrow whose tip touches
// the anchor point (center of the dwell area).
//
// Bypasses Fusion/CSS/DWM entirely — everything is manual QPainter on a
// WA_TranslucentBackground layered window.  The DarkTitleBar property is
// pre-set to prevent DarkApp::notify from calling DwmSetWindowAttribute
// (which was the root cause of the previous transparent-window failure).
//
// Usage:
//   tip->setTheme(bg, border, titleCol, bodyCol, sepCol);
//   tip->populate("Title", "line1\nline2", font);
//   tip->showAt(QPoint(midX, lineBottom));  // arrow tip at this point
//   tip->dismiss();

class RcxTooltip : public QWidget {
public:
    static constexpr int kArrowH = 8;
    static constexpr int kArrowW = 14;
    static constexpr int kRadius = 6;
    static constexpr int kPad    = 10;
    static constexpr int kGap    = 4;
    static constexpr int kMaxW   = 550;

    std::function<void(QMouseEvent*)> onMouseMove;

    explicit RcxTooltip(QWidget* parent = nullptr)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        // ── Key fix: prevent DwmSetWindowAttribute on this window ──
        // DarkApp::notify checks this property and skips DWM calls.
        // Without this, DWMWA_USE_IMMERSIVE_DARK_MODE breaks WS_EX_LAYERED
        // alpha compositing on Windows 10/11.
        setProperty("DarkTitleBar", true);

        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setMouseTracking(true);
    }

    void setTheme(const QColor& bg, const QColor& border,
                  const QColor& title, const QColor& body, const QColor& sep) {
        m_bg = bg; m_border = border;
        m_titleCol = title; m_bodyCol = body; m_sepCol = sep;
    }

    void populate(const QString& title, const QString& body, const QFont& font) {
        if (title == m_title && body == m_body && isVisible()) return;
        m_title = title; m_body = body;
        m_lines = body.split('\n');
        m_font = font;
        m_font.setPointSizeF(font.pointSizeF() * 0.9);
        m_bold = m_font; m_bold.setBold(true);
        recalc();
    }

    // `anchor`: global screen point where the arrow tip touches.
    // Typically the center-bottom of the hovered span.
    void showAt(const QPoint& anchor) {
        QRect scr = screenAt(anchor);
        int w = m_bw, h = m_bh + kArrowH;
        m_up = (anchor.y() + h <= scr.bottom());
        int x = qBound(scr.left() + 2, anchor.x() - w / 2, scr.right() - w - 2);
        int y = m_up ? anchor.y() : anchor.y() - h;
        m_ax = qBound(kRadius + kArrowW/2 + 1, anchor.x() - x,
                       w - kRadius - kArrowW/2 - 1);
        setFixedSize(w, h);
        move(x, y);
        if (!isVisible()) show();
        update();
    }

    void dismiss() { if (isVisible()) hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Body rect (excludes arrow space)
        QRectF b(0.5, m_up ? kArrowH + 0.5 : 0.5,
                 width() - 1.0, m_bh - 1.0);
        qreal r = kRadius, ax = m_ax, ah = kArrowW / 2.0;

        // ── Single contiguous path: rounded rect + arrow notch ──
        // No QPainterPath::united() — that causes junction artifacts.
        // Clockwise from top-left, inserting the arrow inline.
        QPainterPath pp;
        pp.moveTo(b.left() + r, b.top());
        if (m_up) {
            pp.lineTo(ax - ah, b.top());
            pp.lineTo(ax, 0.5);
            pp.lineTo(ax + ah, b.top());
        }
        pp.lineTo(b.right() - r, b.top());
        pp.arcTo(b.right() - 2*r, b.top(), 2*r, 2*r, 90, -90);
        pp.lineTo(b.right(), b.bottom() - r);
        pp.arcTo(b.right() - 2*r, b.bottom() - 2*r, 2*r, 2*r, 0, -90);
        if (!m_up) {
            pp.lineTo(ax + ah, b.bottom());
            pp.lineTo(ax, height() - 0.5);
            pp.lineTo(ax - ah, b.bottom());
        }
        pp.lineTo(b.left() + r, b.bottom());
        pp.arcTo(b.left(), b.bottom() - 2*r, 2*r, 2*r, 270, -90);
        pp.lineTo(b.left(), b.top() + r);
        pp.arcTo(b.left(), b.top(), 2*r, 2*r, 180, -90);
        pp.closeSubpath();

        p.setPen(QPen(m_border, 1));
        p.setBrush(m_bg);
        p.drawPath(pp);

        // ── Content: title + separator + body ──
        qreal cy = (m_up ? kArrowH : 0) + kPad;
        QFontMetrics tf(m_bold), bf(m_font);

        if (!m_title.isEmpty()) {
            p.setFont(m_bold); p.setPen(m_titleCol);
            p.drawText(QPointF(kPad, cy + tf.ascent()), m_title);
            cy += tf.height() + kGap;
            p.setPen(m_sepCol);
            p.drawLine(QPointF(kPad, cy), QPointF(width() - kPad, cy));
            cy += 1 + kGap;
        }
        p.setFont(m_font); p.setPen(m_bodyCol);
        for (const auto& l : m_lines) {
            p.drawText(QPointF(kPad, cy + bf.ascent()), l);
            cy += bf.lineSpacing();
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (onMouseMove) onMouseMove(e); else QWidget::mouseMoveEvent(e);
    }

private:
    static QRect screenAt(const QPoint& pt) {
        auto* s = QApplication::screenAt(pt);
        return s ? s->availableGeometry() : QRect(0, 0, 1920, 1080);
    }

    void recalc() {
        QFontMetrics tf(m_bold), bf(m_font);
        int maxW = m_title.isEmpty() ? 0 : tf.horizontalAdvance(m_title);
        for (const auto& l : m_lines) maxW = qMax(maxW, bf.horizontalAdvance(l));
        m_bw = qMin(maxW + 2 * kPad, kMaxW);
        m_bh = kPad + (m_title.isEmpty() ? 0 : tf.height() + kGap + 1 + kGap)
             + m_lines.size() * bf.lineSpacing() + kPad;
    }

    QString m_title, m_body;
    QStringList m_lines;
    QFont m_font, m_bold;
    QColor m_bg{30, 30, 30}, m_border{60, 60, 60};
    QColor m_titleCol{220, 220, 220}, m_bodyCol{180, 180, 180}, m_sepCol{60, 60, 60};
    bool m_up = true;
    int m_ax = 0, m_bw = 0, m_bh = 0;
};

} // namespace rcx
