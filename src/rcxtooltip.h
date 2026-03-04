#pragma once
#include "themes/thememanager.h"
#include <QWidget>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QPropertyAnimation>
#include <QCursor>
#include <cstdio>

#define TIP_LOG(...) do { \
    FILE* _f = fopen("E:/game_dev/util/reclass2027-main/build/tip_trace.log", "a"); \
    if (_f) { fprintf(_f, __VA_ARGS__); fclose(_f); } \
} while(0)

namespace rcx {

class RcxTooltip : public QWidget {
public:
    static RcxTooltip* instance() {
        static RcxTooltip* s = nullptr;
        if (!s) {
            s = new RcxTooltip;
            QObject::connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                             s, [](const rcx::Theme&) { /* colors read live in paintEvent */ });
        }
        return s;
    }

    void showFor(QWidget* trigger, const QString& text) {
        if (!trigger || text.isEmpty()) {
            TIP_LOG("[TIP] showFor: null trigger or empty text -- dismiss\n");
            dismiss(); return;
        }

        // Same widget+text already showing — do nothing (prevents teleport)
        if (m_trigger == trigger && m_text == text && isVisible()) {
            TIP_LOG("[TIP] showFor: same widget+text, already visible -- skip\n");
            return;
        }

        TIP_LOG("[TIP] showFor: text='%s' trigger=%p class=%s\n",
               qPrintable(text), (void*)trigger, trigger->metaObject()->className());

        // Cancel pending dismiss
        if (m_dismissTimer) m_dismissTimer->stop();

        m_trigger = trigger;
        m_text    = text;

        m_label->setText(text);
        m_label->adjustSize();

        // ── Size: label + padding + arrow ──
        const int pad = 8;
        const int vpad = 4;
        int bodyW = m_label->sizeHint().width()  + pad * 2;
        int bodyH = m_label->sizeHint().height() + vpad * 2;
        int totalW = bodyW;
        int totalH = bodyH + kArrowH;

        // ── Position relative to trigger widget ──
        QRect trigGlobal = QRect(trigger->mapToGlobal(QPoint(0, 0)), trigger->size());
        int trigCenterX  = trigGlobal.center().x();

        QScreen* screen = QApplication::screenAt(trigGlobal.center());
        QRect scr = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

        // Default: above the trigger
        m_arrowDown = true;
        int x = trigCenterX - totalW / 2;
        int y = trigGlobal.top() - totalH - kGap;

        // Flip below if not enough room above
        if (y < scr.top()) {
            m_arrowDown = false;
            y = trigGlobal.bottom() + kGap;
        }

        // Clamp horizontally
        if (x < scr.left()) x = scr.left() + 2;
        if (x + totalW > scr.right()) x = scr.right() - totalW - 2;

        // Arrow X in local coords
        m_arrowLocalX = trigCenterX - x;
        m_arrowLocalX = qBound(kArrowHalfW + 4, m_arrowLocalX, totalW - kArrowHalfW - 4);

        // Position label inside the body
        if (m_arrowDown)
            m_label->move(pad, vpad);
        else
            m_label->move(pad, kArrowH + vpad);

        m_bodyRect = m_arrowDown
            ? QRect(0, 0, bodyW, bodyH)
            : QRect(0, kArrowH, bodyW, bodyH);

        setFixedSize(totalW, totalH);
        move(x, y);

        if (!isVisible()) {
            TIP_LOG("[TIP] showFor: showing at (%d,%d) size=%dx%d arrowDown=%d arrowX=%d\n",
                   x, y, totalW, totalH, m_arrowDown, m_arrowLocalX);
            setWindowOpacity(0.0);
            show();
            raise();
            // Fade in
            auto* anim = new QPropertyAnimation(this, "windowOpacity", this);
            anim->setDuration(80);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            anim->start(QAbstractAnimation::DeleteWhenStopped);
        } else {
            TIP_LOG("[TIP] showFor: already visible, updating\n");
            update();
        }
    }

    void dismiss() {
        TIP_LOG("[TIP] dismiss: wasVisible=%d\n", isVisible());
        if (m_dismissTimer) m_dismissTimer->stop();
        if (isVisible()) hide();
        m_trigger = nullptr;
    }

    // Schedule dismiss with a delay — but only if the cursor has truly
    // left the trigger+tooltip zone.  Qt fires synthetic Leave events
    // when a tooltip window appears above the trigger; we must ignore those.
    void scheduleDismiss() {
        if (m_trigger) {
            QPoint cursor = QCursor::pos();
            QRect trigRect(m_trigger->mapToGlobal(QPoint(0, 0)), m_trigger->size());
            QRect tipRect(pos(), size());
            QRect zone = trigRect.united(tipRect).adjusted(-4, -4, 4, 4);
            bool inside = zone.contains(cursor);
            TIP_LOG("[TIP] scheduleDismiss: cursor=(%d,%d) zone=(%d,%d %dx%d) inside=%d\n",
                   cursor.x(), cursor.y(),
                   zone.x(), zone.y(), zone.width(), zone.height(), inside);
            if (inside)
                return;  // cursor still inside — ignore spurious Leave
        }
        if (!m_dismissTimer) {
            m_dismissTimer = new QTimer(this);
            m_dismissTimer->setSingleShot(true);
            connect(m_dismissTimer, &QTimer::timeout, this, &RcxTooltip::dismiss);
        }
        m_dismissTimer->start(100);
    }

    QWidget* currentTrigger() const { return m_trigger; }

    // ── Geometry accessors (for testing) ──
    bool     arrowPointsDown()    const { return m_arrowDown; }
    int      arrowLocalX()        const { return m_arrowLocalX; }
    QRect    bodyRect()           const { return m_bodyRect; }
    QString  currentText()        const { return m_text; }

    // Constants exposed for testing
    static constexpr int kArrowH     = 6;
    static constexpr int kArrowHalfW = 6;
    static constexpr int kGap        = 2;

protected:
    void paintEvent(QPaintEvent*) override {
        TIP_LOG("[TIP] paintEvent: size=%dx%d bodyRect=(%d,%d %dx%d)\n",
               width(), height(),
               m_bodyRect.x(), m_bodyRect.y(), m_bodyRect.width(), m_bodyRect.height());
        const auto& theme = ThemeManager::instance().current();

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Fill entire widget with the tooltip background first
        // (no WA_TranslucentBackground, so unpainted areas would be opaque garbage)
        p.fillRect(rect(), theme.backgroundAlt);

        // Build path: rounded body + triangle arrow
        QPainterPath path;
        path.addRoundedRect(QRectF(m_bodyRect), 4.0, 4.0);

        // Triangle arrow
        QPolygonF arrow;
        if (m_arrowDown) {
            int ay = m_bodyRect.bottom();
            arrow << QPointF(m_arrowLocalX - kArrowHalfW, ay)
                  << QPointF(m_arrowLocalX, ay + kArrowH)
                  << QPointF(m_arrowLocalX + kArrowHalfW, ay);
        } else {
            int ay = kArrowH;
            arrow << QPointF(m_arrowLocalX - kArrowHalfW, ay)
                  << QPointF(m_arrowLocalX, 0)
                  << QPointF(m_arrowLocalX + kArrowHalfW, ay);
        }
        QPainterPath arrowPath;
        arrowPath.addPolygon(arrow);
        arrowPath.closeSubpath();
        path = path.united(arrowPath);

        // Stroke the shape border
        p.setPen(QPen(theme.border, 1.0));
        p.setBrush(theme.backgroundAlt);
        p.drawPath(path);
    }

private:
    explicit RcxTooltip()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        // NOTE: WA_TranslucentBackground removed — it breaks under DWM dark mode
        // (DwmSetWindowAttribute DWMWA_USE_IMMERSIVE_DARK_MODE kills layered compositing)
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAutoFillBackground(false);  // we paint everything ourselves in paintEvent

        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignCenter);
        updateLabelStyle();
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, [this](const rcx::Theme&) { updateLabelStyle(); });
    }

    void updateLabelStyle() {
        const auto& theme = ThemeManager::instance().current();
        m_label->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: transparent; padding: 0; }")
                .arg(theme.text.name()));
    }

    QLabel*   m_label        = nullptr;
    QWidget*  m_trigger      = nullptr;
    QString   m_text;
    QTimer*   m_dismissTimer = nullptr;
    bool      m_arrowDown    = true;
    int       m_arrowLocalX  = 0;
    QRect     m_bodyRect;
};

} // namespace rcx
