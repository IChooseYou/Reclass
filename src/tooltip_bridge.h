#pragma once
#include "rcxtooltip.h"
#include <QApplication>
#include <QHelpEvent>
#include <QPointer>

namespace rcx {

// App-wide tooltip bridge: replaces Qt's default tooltip widget with
// rcx::RcxTooltip for every setToolTip("…") property call in the app.
// One filter installed on qApp catches every QEvent::ToolTip, reads
// widget->toolTip(), and shows the shared RcxTooltip instead. The 37
// existing setToolTip call sites get themed RcxTooltip rendering with
// zero per-site edits.
//
// Dismissal is intentionally narrow. Qt's default tooltip auto-hides
// on any mouse move / leave; mirroring that broadly here caused a
// "still cursor flicker" — the user pauses on a button, Qt fires
// ToolTip, we show, some unrelated child widget fires Leave, we
// dismiss, Qt fires ToolTip again (cursor still parked on the parent),
// we show, repeat. Bridge now tracks WHICH widget the current tip
// belongs to and only dismisses on a Leave event for THAT widget (or
// click / window deactivate / focus-out).
class GlobalTooltipBridge : public QObject {
    // No Q_OBJECT — bridge has no signals/slots, only an eventFilter
    // override, and skipping the macro lets us live in a header-only
    // file without an AUTOMOC dance on every test target that pulls
    // tooltip_bridge.h in.
public:
    explicit GlobalTooltipBridge(QObject* parent = nullptr) : QObject(parent) {}

    // Test accessor — the live app doesn't need this but the flicker
    // detection test reads it to verify the bridge actually consumed
    // a given QEvent::ToolTip event.
    QWidget* tooltipTarget() const { return m_target.data(); }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        const auto t = e->type();
        if (t == QEvent::ToolTip) {
            auto* w = qobject_cast<QWidget*>(obj);
            if (!w) return false;
            QString tip = w->toolTip();
            if (tip.isEmpty()) {
                // Empty tooltip on the target widget — clear and let
                // Qt do nothing.
                m_target = nullptr;
                rcx::dismissRcxTooltip();
                return false;
            }
            auto* he = static_cast<QHelpEvent*>(e);
            // Same widget + same text + already visible: idempotent —
            // skip the populate/move call entirely. Qt re-fires
            // QEvent::ToolTip every time its hover-stay timer ticks
            // (~0.7s by default) even when the cursor hasn't moved;
            // re-publishing the same tip caused per-tick visual
            // recomputes that read as flicker on a stationary mouse.
            if (m_target == w && m_lastText == tip
                && rcx::sharedRcxTooltip()->isVisible())
                return true;
            m_target = w;
            m_lastText = tip;
            rcx::showRcxTooltip(he->globalPos(), tip, w->font());
            return true;  // suppress Qt's default tooltip widget
        }
        // Only dismiss when the widget the tip belongs to (or one of
        // its ancestors — Leave on a child of m_target shouldn't kill
        // the tip) actually gets Leave / WindowDeactivate / FocusOut.
        // Earlier code dismissed on ANY widget's Leave; an unrelated
        // child widget firing Leave (mouse tracking moving between
        // sub-widgets of the same control) ate the tip every tick.
        if (t == QEvent::WindowDeactivate || t == QEvent::FocusOut) {
            m_target = nullptr;
            m_lastText.clear();
            rcx::dismissRcxTooltip();
        } else if (t == QEvent::MouseButtonPress) {
            // Clicking anywhere dismisses. Don't clear m_target/text —
            // the next ToolTip event on the same widget should still
            // show (the user can click + re-hover).
            rcx::dismissRcxTooltip();
        } else if (t == QEvent::Leave) {
            if (m_target && obj == m_target.data()) {
                m_target = nullptr;
                m_lastText.clear();
                rcx::dismissRcxTooltip();
            }
        }
        return false;
    }

private:
    QPointer<QWidget> m_target;
    QString           m_lastText;
};

} // namespace rcx
