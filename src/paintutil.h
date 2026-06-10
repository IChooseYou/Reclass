#pragma once
#include <QPainter>
#include <QRectF>
#include <QTransform>
#include <QColor>
#include <QtMath>

namespace rcx {

// ── Device-pixel-exact edge fills ──
// A 1-logical-px fillRect covers 1.25 device px at DPR 1.25 and snaps to TWO
// filled rows/columns — a "1px" line next to a device-exact one (the editor
// paints its borders in device pixels) reads as a double line. These map the
// target rect through the painter's deviceTransform, pick the edge device
// row/column, and fill exactly that, at any DPR.
//
// Edge selection is half-a-pixel INWARD (qFloor(edge - 0.5), not
// qCeil(edge) - 1): Qt rounds the widget's system clip with qRound
// semantics, so at fractional DPR a .25-phase edge puts the outermost
// device row/col OUTSIDE the clip and the fill vanishes entirely
// (empirically: DPR 1.25, widget width 181 → no line at all). Picking the
// row/col that the rect overlaps by ≥ half a device px is identical at
// integer edges and stays inside the clip in every fractional phase.
// Regression-pinned by tests/test_hairline_dpr.cpp (real-window phase sweep
// at QT_SCALE_FACTOR=1.25).

inline void fillTopDeviceRowOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devRow(dev.left(), qFloor(dev.top() + 0.5), dev.width(), 1.0);
    p.fillRect(dt.inverted().mapRect(devRow), c);
}

inline void fillBottomDeviceRowOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devRow(dev.left(), qFloor(dev.bottom() - 0.5), dev.width(), 1.0);
    p.fillRect(dt.inverted().mapRect(devRow), c);
}

inline void fillLeftDeviceColOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devCol(qFloor(dev.left() + 0.5), dev.top(), 1.0, dev.height());
    p.fillRect(dt.inverted().mapRect(devCol), c);
}

inline void fillRightDeviceColOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devCol(qFloor(dev.right() - 0.5), dev.top(), 1.0, dev.height());
    p.fillRect(dt.inverted().mapRect(devCol), c);
}

} // namespace rcx
