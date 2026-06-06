#pragma once

#include <QFont>
#include <QFontInfo>

namespace rcx {

// QFont::pointSize() returns -1 when the font was set with setPixelSize(). Code
// that derives a size from it — `derived.setPointSize(qMax(FLOOR, base.pointSize() - N))`
// or an embedded `font-size:<pointSize()>pt` — then collapses every derived size
// to the FLOOR (or to 1pt / negative where there's no floor) for pixel-sized
// fonts, rendering text tiny/invisible. This resolves a concrete point size via
// QFontInfo first. It's a no-op for the point-sized fonts the app normally uses
// (QFontDialog produces those), so routing derivations through it is purely
// robustness, not a visual change.
//
// See memory `reference_pixel_font_pointsize_bug_class` for the (still partial)
// list of call sites across the codebase.
inline int resolvedPointSize(const QFont& f) {
    int ps = f.pointSize();
    if (ps > 0) return ps;
    int info = QFontInfo(f).pointSize();
    return info > 0 ? info : 9;  // last-resort sane default
}

}  // namespace rcx
