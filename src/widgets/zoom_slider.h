#pragma once

#include "themes/theme.h"
#include <QMouseEvent>
#include <QSlider>
#include <QStyleOptionSlider>

namespace rcx {

// Keep the thin track, but make the entire strip a click-and-drag target.
// Native QSlider only accepts its styled subcontrols and page-steps on a
// left-click, which left dead space around this 3px groove.
class ZoomSlider : public QSlider {
public:
    static constexpr int kMinimum = -8; // keep at least 4pt of the 12pt base
    static constexpr int kMaximum = 20; // QsciScintilla::zoomTo / Ctrl+wheel limit

    explicit ZoomSlider(QWidget* parent = nullptr) : QSlider(Qt::Horizontal, parent) {
        setObjectName(QStringLiteral("paneZoomSlider"));
        setAccessibleName(QStringLiteral("Zoom"));
        setRange(kMinimum, kMaximum);
        setFixedSize(90, 26);
        // Same visual baseline as the old 18px slider centred in a 26px row.
        setContentsMargins(0, 6, 0, 0);
        setToolTip(QStringLiteral("Zoom (also Ctrl+scroll)"));
    }

    void applyTheme(const Theme& t) {
        setStyleSheet(QStringLiteral(
            "QSlider::groove:horizontal { height: 3px; background: %1; border-radius: 0px; }"
            "QSlider::handle:horizontal { background: %2; width: 3px; height: 11px;"
            " margin: -4px 0; border-radius: 0px; }"
            "QSlider::handle:horizontal:hover, QSlider::handle:horizontal:focus { background: %3; }")
            .arg(t.border.name(), t.textMuted.name(), t.text.name()));
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || event->buttons() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        event->accept();
        if (!isEnabled() || minimum() == maximum()) return;
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderHandle, this);
        const int x = event->pos().x();
        // Preserve the grab point on the thumb. Everywhere else seeks to
        // the click and immediately starts dragging, without page-step gaps.
        m_grabOffset = x >= handle.left() && x <= handle.right()
            ? x - handle.left() : handle.width() / 2;
        setSliderDown(true);
        moveTo(x);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (isSliderDown() && (event->buttons() & Qt::LeftButton)) {
            moveTo(event->pos().x());
            event->accept();
        } else {
            QSlider::mouseMoveEvent(event);
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && isSliderDown()) {
            moveTo(event->pos().x());
            setSliderDown(false);
            event->accept();
        } else {
            QSlider::mouseReleaseEvent(event);
        }
    }

private:
    void moveTo(int x) {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderHandle, this);
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(),
            x - m_grabOffset - groove.left(), qMax(0, groove.width() - handle.width()), opt.upsideDown));
    }

    int m_grabOffset = 0;
};

} // namespace rcx
