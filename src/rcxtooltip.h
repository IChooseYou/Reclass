#pragma once
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QApplication>
#include <QMouseEvent>
#include <functional>
#include "themes/thememanager.h"

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

// Rich text span for per-segment coloring in tooltip body
struct TipSpan {
    QString text;
    QColor  color;  // invalid = use default body color
    bool    bold = false;
    bool    keyCap = false;  // draw as outlined keyboard key
};
using TipLine = QVector<TipSpan>;

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
        // Pass every mouse event through to the window under the tooltip.
        // Without this the tooltip sits on top of the cursor's actual
        // target, the original widget receives a Leave event, the
        // hover-state-driven owner re-evaluates and (in the editor's
        // case) decides to dismiss the chip-tooltip — which then makes
        // the cursor re-enter the widget, MouseMove fires, tooltip is
        // shown again. Non-stop flicker. WA_TransparentForMouseEvents
        // cuts the loop at the OS level: the cursor never "enters" this
        // window for hit-test purposes, so the underlying widget keeps
        // its hover state.
        setAttribute(Qt::WA_TransparentForMouseEvents);
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
        m_richLines.clear();
        m_font = font;
        m_font.setPointSizeF(font.pointSizeF() * 0.9);
        m_bold = m_font; m_bold.setBold(true);
        recalc();
    }

    void populateRich(const QString& title, const QVector<TipLine>& richBody, const QFont& font) {
        m_title = title;
        m_richLines = richBody;
        m_body.clear();
        m_lines.clear();
        // Build plain lines for width calculation
        for (const auto& rl : richBody) {
            QString plain;
            for (const auto& s : rl) plain += s.text;
            m_lines.append(plain);
        }
        m_font = font;
        m_font.setPointSizeF(font.pointSizeF() * 0.9);
        m_bold = m_font; m_bold.setBold(true);
        recalc();
    }

    // `anchor`: global screen point where the arrow tip touches.
    // Typically the center-bottom of the hovered span.
    //
    // `preferAbove`: when true, force the tooltip body to render ABOVE
    // the anchor (arrow points downward into the anchor) whenever there
    // is room above. Falls back to below if the top of the screen is
    // hit. When false (default), the legacy behaviour applies — body
    // below if it fits, else above. Used by the byte-selection tooltip
    // so it pops upward off the selected row (anchored at the top of
    // the line) and doesn't cover other hex rows below.
    void showAt(const QPoint& anchor, bool preferAbove = false) {
        QRect scr = screenAt(anchor);
        int w = m_bw, h = m_bh + kArrowH;
        if (preferAbove) {
            m_up = (anchor.y() - h < scr.top());  // flip down only if no room above
        } else {
            m_up = (anchor.y() + h <= scr.bottom());
        }
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
        if (!m_richLines.isEmpty()) {
            QFont boldBody = m_font; boldBody.setBold(true);
            QFont keyFont = m_font;
            // Keycap label sits at 70% of body font — smaller than the
            // value text it sits next to. The previous 80% looked
            // chunky against compact body interps.
            keyFont.setPointSizeF(m_font.pointSizeF() * 0.70);
            QFontMetrics kfm(keyFont);
            // Tight inner padding: +4 px (was +10). Outer +2 around
            // the cap is implicit from the line-height max() below.
            int keyH = kfm.height() + 4;
            int keyRowH = qMax((int)bf.lineSpacing(), keyH + 2);
            int textRowH = bf.lineSpacing();
            for (int li = 0; li < m_richLines.size(); li++) {
                // Per-line height — text-only rows stay compact; rows
                // that contain a keycap span use the taller keyRowH.
                // Lets us stack 4 keycap rows + a text interp row
                // without blowing every line up to keycap height.
                bool lineHasKey = false;
                for (const auto& s : m_richLines[li])
                    if (s.keyCap) { lineHasKey = true; break; }
                int lineH = lineHasKey ? keyRowH : textRowH;
                qreal cx = kPad;
                for (const auto& span : m_richLines[li]) {
                    QColor col = span.color.isValid() ? span.color : m_bodyCol;
                    if (span.keyCap) {
                        // Draw keyboard key: rounded rect with centered symbol.
                        // Width is uniform across the tooltip (m_maxKeyW set
                        // by recalc) so a column of caps lines up and the
                        // action labels that follow start at the same x.
                        int kw = m_maxKeyW > 0 ? m_maxKeyW
                                               : qMax(kfm.horizontalAdvance(span.text) + 8, keyH);
                        qreal ky = cy + (lineH - keyH) / 2.0;
                        QRectF kr(cx, ky, kw, keyH);
                        // Subtle fill for the key face
                        QColor keyBg = m_bg.lighter(130);
                        p.setPen(QPen(col.darker(120), 1.0));
                        p.setBrush(keyBg);
                        p.drawRoundedRect(kr, 3, 3);
                        // Key label
                        p.setFont(keyFont);
                        p.setPen(col);
                        p.drawText(kr, Qt::AlignCenter, span.text);
                        cx += kw + 4;  // gap after key
                    } else {
                        p.setFont(span.bold ? boldBody : m_font);
                        p.setPen(col);
                        QFontMetrics sfm(span.bold ? boldBody : m_font);
                        // Vertically center text in the line
                        qreal textY = cy + (lineH - sfm.height()) / 2.0 + sfm.ascent();
                        p.drawText(QPointF(cx, textY), span.text);
                        cx += sfm.horizontalAdvance(span.text);
                    }
                }
                cy += lineH;
            }
        } else {
            for (const auto& l : m_lines) {
                p.drawText(QPointF(kPad, cy + bf.ascent()), l);
                cy += bf.lineSpacing();
            }
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
        QFont keyFont = m_font;
        // Keep these in sync with the draw path above (0.70 scale,
        // +4 inner padding). Changes here without matching draw-side
        // changes will mis-size the tooltip rectangle.
        keyFont.setPointSizeF(m_font.pointSizeF() * 0.70);
        QFontMetrics kfm(keyFont);
        int keyH = kfm.height() + 4;
        int keyRowH = qMax((int)bf.lineSpacing(), keyH + 2);
        int textRowH = bf.lineSpacing();
        int maxW = m_title.isEmpty() ? 0 : tf.horizontalAdvance(m_title);
        int totalLinesH = 0;
        // Pass 1: find the widest keycap. Every keycap in the
        // tooltip then renders at this width so columns line up.
        m_maxKeyW = 0;
        for (const auto& rl : m_richLines) {
            for (const auto& s : rl) {
                if (!s.keyCap) continue;
                int tw = kfm.horizontalAdvance(s.text);
                m_maxKeyW = qMax(m_maxKeyW, qMax(tw + 8, keyH));
            }
        }
        if (!m_richLines.isEmpty()) {
            for (const auto& rl : m_richLines) {
                int lineW = 0;
                bool lineHasKey = false;
                for (const auto& s : rl) {
                    if (s.keyCap) {
                        lineHasKey = true;
                        lineW += m_maxKeyW + 4;
                    } else {
                        QFontMetrics sfm(s.bold ? tf : bf);
                        lineW += sfm.horizontalAdvance(s.text);
                    }
                }
                maxW = qMax(maxW, lineW);
                totalLinesH += lineHasKey ? keyRowH : textRowH;
            }
        } else {
            for (const auto& l : m_lines) maxW = qMax(maxW, bf.horizontalAdvance(l));
            totalLinesH = m_lines.size() * textRowH;
        }
        m_bw = qMin(maxW + 2 * kPad, kMaxW);
        m_bh = kPad + (m_title.isEmpty() ? 0 : tf.height() + kGap + 1 + kGap)
             + totalLinesH + kPad;
    }

    QString m_title, m_body;
    QStringList m_lines;
    QVector<TipLine> m_richLines;
    QFont m_font, m_bold;
    QColor m_bg{30, 30, 30}, m_border{60, 60, 60};
    QColor m_titleCol{220, 220, 220}, m_bodyCol{180, 180, 180}, m_sepCol{60, 60, 60};
    bool m_up = true;
    int m_ax = 0, m_bw = 0, m_bh = 0;
    // Widest keycap across all rich lines, computed by recalc() and
    // applied to every keycap by the draw path so columns of caps
    // (Ctrl+C / Ctrl+V / Del / Enter) line up and the action labels
    // that follow them start at the same x position.
    int m_maxKeyW = 0;
};

// Shared process-wide tooltip. The global QEvent::ToolTip bridge (set up
// in main.cpp) and the few direct callers (TypeSelectorPopup delegate)
// route through this single instance so we don't litter the heap with
// per-widget tooltip objects, and the visible tip simply repositions
// when the user moves between widgets.
//
// Lazy-init on first call; theme is read from ThemeManager each show
// so theme changes apply without rewiring.
inline RcxTooltip* sharedRcxTooltip() {
    static RcxTooltip* s_tip = nullptr;
    if (!s_tip) s_tip = new RcxTooltip(nullptr);
    return s_tip;
}

// Convenience: show plain text from a global cursor position. Caller
// passes their preferred font (usually the widget's font or the editor
// font). Theme colors pulled from ThemeManager.
inline void showRcxTooltip(const QPoint& globalAnchor,
                            const QString& text,
                            const QFont& font) {
    auto* tip = sharedRcxTooltip();
    const auto& theme = ThemeManager::instance().current();
    tip->setTheme(theme.backgroundAlt, theme.border,
                  theme.text, theme.textDim, theme.border);
    tip->populate(QString(), text, font);
    tip->showAt(globalAnchor);
}
inline void dismissRcxTooltip() {
    if (auto* t = sharedRcxTooltip(); t->isVisible()) t->dismiss();
}

} // namespace rcx
