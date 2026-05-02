#pragma once
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>

namespace rcx {

// Inline size-readout shown while the user drags a dock separator.
// Child widget of the QMainWindow — NOT a top-level Qt::ToolTip.
// That distinction matters: a top-level layered tooltip on Windows
// gets WS_EX_LAYERED + DWM compositing, whose WM_NCHITTEST routing
// breaks Qt's separator-drag mouse grab and produces per-frame
// re-composition flicker. Painted inside the parent's update batch,
// this widget has neither problem.
class DockSizeReadout : public QWidget {
public:
    explicit DockSizeReadout(QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

    void setTheme(const QColor& bg, const QColor& border,
                  const QColor& title, const QColor& body, const QColor& sep) {
        m_bg = bg; m_border = border;
        m_titleCol = title; m_bodyCol = body; m_sepCol = sep;
    }

    void updateText(const QString& title, const QString& body, const QFont& font) {
        // No early-return: `body` changes per pixel during a drag and we
        // want every call to land. The previous "skip if all three
        // strings match" optimization risked silently dropping updates
        // if Qt sent two same-size resize events in a row.
        m_title = title;
        m_body = body;
        m_font = font;
        m_bold = font; m_bold.setBold(true);
        recalc();
    }

    // Show at parent-local coordinates; clamped inside parent rect.
    void showAt(const QPoint& parentLocalPos) {
        if (!parentWidget()) return;
        QSize psz = parentWidget()->size();
        QPoint p = parentLocalPos + QPoint(12, 12);
        p.setX(qBound(2, p.x(), psz.width()  - m_w - 2));
        p.setY(qBound(2, p.y(), psz.height() - m_h - 2));
        if (size() != QSize(m_w, m_h)) setFixedSize(m_w, m_h);
        if (pos() != p) move(p);
        if (!isVisible()) show();
        // Re-raise on every tick — dock widgets re-stack themselves
        // on resize and end up over the readout otherwise. Force a
        // repaint so the freshly stored body text appears.
        raise();
        update();
    }

    void dismiss() { if (isVisible()) hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);
        QPainterPath path;
        path.addRoundedRect(r, kRadius, kRadius);
        p.setPen(QPen(m_border, 1));
        p.setBrush(m_bg);
        p.drawPath(path);

        QFontMetrics tf(m_bold), bf(m_font);
        qreal cy = kPad;
        if (!m_title.isEmpty()) {
            p.setFont(m_bold);
            p.setPen(m_titleCol);
            p.drawText(QPointF(kPad, cy + tf.ascent()), m_title);
            cy += tf.height() + kGap;
            p.setPen(m_sepCol);
            p.drawLine(QPointF(kPad, cy),
                       QPointF(width() - kPad, cy));
            cy += 1 + kGap;
        }
        p.setFont(m_font);
        p.setPen(m_bodyCol);
        p.drawText(QPointF(kPad, cy + bf.ascent()), m_body);
    }

private:
    void recalc() {
        QFontMetrics tf(m_bold), bf(m_font);
        int titleW = m_title.isEmpty() ? 0 : tf.horizontalAdvance(m_title);
        int bodyW = bf.horizontalAdvance(m_body);
        int maxW = qMax(titleW, bodyW);
        m_w = maxW + 2 * kPad;
        m_h = kPad
            + (m_title.isEmpty() ? 0 : tf.height() + kGap + 1 + kGap)
            + bf.lineSpacing()
            + kPad;
    }

    static constexpr int kRadius = 6;
    static constexpr int kPad    = 10;
    static constexpr int kGap    = 4;

    QString m_title, m_body;
    QFont m_font, m_bold;
    QColor m_bg{30, 30, 30}, m_border{60, 60, 60};
    QColor m_titleCol{220, 220, 220}, m_bodyCol{180, 180, 180}, m_sepCol{60, 60, 60};
    int m_w = 0, m_h = 0;
};

} // namespace rcx
