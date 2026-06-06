#pragma once

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QIODevice>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QSvgRenderer>

namespace rcx {

// Load a vsicon SVG resource, swap its baked "#C5C5C5" ink fill with `tint`
// (the theme foreground) so the glyph stays visible on light chrome, and
// rasterize to a QIcon at `logicalSize` logical px honoring `dpr`. The dpr
// handling matters: a fixed logical-size pixmap is upscaled by the platform on
// 150-200% displays and reads visibly blurry, so we render at device pixels and
// stamp setDevicePixelRatio.
//
// Results are cached by (path, tint, size, dpr). Callers re-tint on every theme
// apply/refresh and the :/vsicons resources are immutable at runtime, so every
// input that affects the pixels is in the key and the cache never needs
// invalidation. Shared by titlebar.cpp window controls and dock_tab_buttons.h
// tab close buttons (previously two divergent copies — the titlebar copy had
// lost the dpr handling, which is the blur bug this consolidation prevents).
inline QIcon themedVsIcon(const QString& path, const QColor& tint,
                          int logicalSize, qreal dpr) {
    const qreal s = dpr > 0 ? dpr : 1.0;
    const QString key = path + QLatin1Char('|') + tint.name()
                      + QLatin1Char('|') + QString::number(logicalSize)
                      + QLatin1Char('|') + QString::number(s, 'f', 3);
    static QHash<QString, QIcon> cache;
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) return it.value();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QIcon(path);
    QByteArray svg = f.readAll();
    svg.replace("#C5C5C5", tint.name().toLatin1());
    svg.replace("#c5c5c5", tint.name().toLatin1());
    QSvgRenderer r(svg);
    QPixmap pm(QSize(logicalSize, logicalSize) * s);
    // Stamp the device-pixel-ratio BEFORE painting so the painter's logical
    // coordinate space is `logicalSize` (not the raw device size). Rendering
    // into the full logical rect then fills the whole pixmap. The previous order
    // (set dpr AFTER paint) left the painter in device space, so rendering into
    // `pm.size()/s` only filled the top-left logicalSize×logicalSize corner of
    // the s-times-larger pixmap — at dpr>1 the glyph rendered small and pinned
    // to the top-left, which read as "icon too high" on HiDPI title bars/tabs.
    pm.setDevicePixelRatio(s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    r.render(&p, QRectF(0, 0, logicalSize, logicalSize));
    p.end();
    QIcon icon(pm);
    cache.insert(key, icon);
    return icon;
}

}  // namespace rcx
