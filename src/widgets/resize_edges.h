#pragma once

// ── Resize zones of the frameless main window ──
//
// The window has no native border, so thin see-through widgets along every
// edge and corner hand a press to QWindow::startSystemResize (ResizeEdge in
// main.cpp). Over the title bar only a thin band along the very top resizes
// (the top edge and its two corners), the way a native window's border sits
// over its caption; the side strips start below the title bar, so Close and the
// wordmark keep their edges. A maximized or full-screen window has no zones:
// its top edge belongs to the title bar again (drag down to restore) and Close
// reaches the screen corner.
//
// Pure geometry, unit-tested (tests/test_resize_edges.cpp).

#include <QRect>
#include <qnamespace.h>

namespace rcx {

inline constexpr int kResizeEdgeThickness = 5;   // edge strip thickness
inline constexpr int kResizeCornerSize    = 12;  // corner square size

// The zone for `e` (one edge, or two for a corner) in a w×h window whose title
// bar is `captionH` tall.
inline QRect resizeEdgeRect(Qt::Edges e, int w, int h, int captionH = 0) {
    const int E = kResizeEdgeThickness, C = kResizeCornerSize;
    const bool L = e.testFlag(Qt::LeftEdge), R = e.testFlag(Qt::RightEdge);
    const bool T = e.testFlag(Qt::TopEdge),  B = e.testFlag(Qt::BottomEdge);
    if ((L || R) && T)                                     // top corner: a thin band, clear of Close
        return QRect(L ? 0 : w - C, 0, C, E);
    if ((L || R) && B)                                     // bottom corner
        return QRect(L ? 0 : w - C, h - C, C, C);
    const int top = qMax(C, captionH);                     // sides start below the title bar
    if (L) return QRect(0,     top, E, h - top - C);       // left strip
    if (R) return QRect(w - E, top, E, h - top - C);       // right strip
    if (T) return QRect(C, 0,     w - 2 * C, E);           // top strip
    if (B) return QRect(C, h - E, w - 2 * C, E);           // bottom strip
    return {};
}

inline bool resizeZonesEnabled(Qt::WindowStates s) {
    return !(s & (Qt::WindowMaximized | Qt::WindowFullScreen));
}

} // namespace rcx
