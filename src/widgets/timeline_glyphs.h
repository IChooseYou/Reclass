#pragma once

// ── The timeline's glyphs: Record / Stop, Back to live ──
//
// Painted, not codicons: record.svg and debug-*.svg carry their own fills,
// which a theme tint cannot reach. The colour is the caller's, the way the
// context menu tints its icons — Record and Stop red (markerPtr), Back to
// live green (indHintGreen). Straight edges land on whole
// device pixels; the round ones are antialiased. `box` is the icon square
// (14 px on the address bar), already centred in its cell.

#include "paintutil.h"

#include <QPainter>
#include <QPolygonF>
#include <QRectF>
#include <cmath>

namespace rcx::timeline {

// `r` with every edge rounded onto the device grid, so a fill has no
// half-covered rows or columns at 125 %.
inline QRectF deviceSnapped(const QPainter& p, const QRectF& r) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF snapped(QPointF(std::round(dev.left()), std::round(dev.top())),
                         QPointF(std::round(dev.right()), std::round(dev.bottom())));
    return dt.inverted().mapRect(snapped);
}

// Whole device pixels for a size given in logical px (at least `minDev`).
inline int devicePx(const QPainter& p, qreal logical, int minDev = 1) {
    const qreal dpr = std::max<qreal>(1.0, std::abs(p.deviceTransform().m11()));
    return std::max(minDev, int(std::lround(logical * dpr)));
}

// Fill a rect given in DEVICE pixels.
inline void fillDeviceRect(QPainter& p, int x, int y, int w, int h, const QColor& c) {
    p.fillRect(p.deviceTransform().inverted().mapRect(QRectF(x, y, w, h)), c);
}

// A dot to record; a square while recording (press it to stop).
inline void paintRecordGlyph(QPainter& p, const QRectF& box, bool recording, const QColor& c) {
    const QPointF ctr = box.center();
    if (recording) {
        const QPointF dev = p.deviceTransform().map(ctr);
        const int s = devicePx(p, box.width() * 8.0 / 14.0, 2);
        fillDeviceRect(p, int(std::lround(dev.x() - s / 2.0)), int(std::lround(dev.y() - s / 2.0)), s, s, c);
        return;
    }
    const qreal r = box.width() * 5.0 / 14.0;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(ctr, r, r);
    p.restore();
}

// Back to live: a play triangle, nudged right by a sixth of its width so it
// reads centred (its ink mass sits left of its box).
inline void paintPlayGlyph(QPainter& p, const QRectF& box, const QColor& c) {
    const QPointF ctr = box.center();
    const qreal h = box.height() * 10.0 / 14.0;
    const qreal w = h * 0.87;
    const qreal left = ctr.x() - w / 2 + w / 6;
    const QPolygonF tri({QPointF(left, ctr.y() - h / 2), QPointF(left, ctr.y() + h / 2),
                         QPointF(left + w, ctr.y())});
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPolygon(tri);
    p.restore();
}

// ── The address bar's chips ──

// A filled dot: LIVE (green) and REC (red).
inline void paintDotGlyph(QPainter& p, const QPointF& center, qreal radius, const QColor& c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(center, radius, radius);
    p.restore();
}

// A small clock — a ring and two hands: the view is showing a past moment.
inline void paintClockGlyph(QPainter& p, const QPointF& center, qreal radius, const QColor& c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c, 1.25);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(center, radius, radius);
    p.drawLine(center, center + QPointF(0, -radius * 0.62));
    p.drawLine(center, center + QPointF(radius * 0.48, 0));
    p.restore();
}

// A square of `side` logical px on whole device pixels: REC while recording.
inline void paintSquareGlyph(QPainter& p, const QPointF& center, qreal side, const QColor& c) {
    const QPointF dev = p.deviceTransform().map(center);
    const int s = devicePx(p, side, 2);
    fillDeviceRect(p, int(std::lround(dev.x() - s / 2.0)), int(std::lround(dev.y() - s / 2.0)), s, s, c);
}

} // namespace rcx::timeline
