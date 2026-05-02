#pragma once
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QRect>
#include <QString>
#include <QColor>
#include <QPaintDevice>

namespace rcx {

// Renders an SVG into the painter at iconRect, tinted to match `tint`.
// Crisp at any DPI: pixmap is sized at logical * dpr and tagged with
// devicePixelRatio so Qt downscales correctly. Tint is applied via
// CompositionMode_SourceIn so the icon takes on `tint`'s color.
// `live=false` reduces opacity to 0.40 (the muted/disconnected look).
//
// Used by both the live doc-tab paint path (MenuBarStyle::drawControl
// in main.cpp) and by tests that grab the rendered tab pixmap and
// sample pixels — guarantees test and live render match.
inline void drawTabSourceIcon(QPainter* p, const QRect& iconRect,
                               const QString& iconPath, bool live,
                               const QColor& tint) {
    if (!p || iconPath.isEmpty()) return;
    QSvgRenderer renderer(iconPath);
    if (!renderer.isValid()) return;

    qreal dpr = (p->device() ? p->device()->devicePixelRatioF() : 1.0);
    QPixmap pm(iconRect.size() * dpr);
    // CRITICAL: dpr must be set BEFORE the painter is attached.
    // QPainter caches the painter coordinate system at construction,
    // so a later setDevicePixelRatio leaves the SVG rendered at 1:1
    // physical (half-size at dpr=2) in the top-left corner of an
    // oversized pixmap — then drawPixmap upscales the half-rendered
    // content. That's what made the icon look blurry.
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing, true);
        pp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&pp, QRectF(0, 0,
                                     iconRect.width(),
                                     iconRect.height()));
        pp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        pp.fillRect(QRect(0, 0, iconRect.width(), iconRect.height()), tint);
    }
    qreal prev = p->opacity();
    if (!live) p->setOpacity(prev * 0.40);
    p->drawPixmap(iconRect, pm);
    p->setOpacity(prev);
}

} // namespace rcx
