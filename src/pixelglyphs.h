#pragma once
// 5×7 variable-width pixel bitmap font + the handful of fixed bitmaps the
// ribbon icons are
// composed from. Everything here paints in DEVICE pixels: callers hand us a
// painter attached to an unscaled QImage sized to the device cell and we
// fillRect whole `scale × scale` blocks with antialiasing off, so every ink
// pixel is a full device pixel at 100 / 125 / 150 / 200 %.
//
// The grid was 3×5 (shapes traced off the original ReClassEx toolbar bitmaps)
// until 2026-09-06, when the user called the result "not very sharp". It was
// sharp — every ink pixel landed on a whole device pixel — but 3 columns and 5
// rows cannot DRAW a legible digit: `6` read as `b`, `4` as `y`, `2` as `Z`,
// and `U` and `V` were the same glyph but for one row. 5×7 with a per-glyph
// advance fixes the letterforms without widening a single ribbon cell.

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QSettings>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <QtMath>
#include <cstdint>

namespace rcx {

struct PixelBitmap {
    int w;
    int h;
    const char* rows[8];
};

// Green "+" of the Add-N icons (top-left corner).
inline constexpr PixelBitmap kPlus5x5{5, 5, {
    "..#..",
    "..#..",
    "#####",
    "..#..",
    "..#.."}};

// The Insert-N arch arrow: up from the left, over, and down onto the row.
inline constexpr PixelBitmap kHookArrow7x7{7, 7, {
    ".###...",
    "#...#..",
    "#...#..",
    "....#..",
    "..#####",
    "...###.",
    "....#.."}};

// Delete ✕.
inline constexpr PixelBitmap kCross7x7{7, 7, {
    "#.....#",
    ".#...#.",
    "..#.#..",
    "...#...",
    "..#.#..",
    ".#...#.",
    "#.....#"}};

// Row of three byte "cells" above the fill label (000 / FFF / ???). Two rows
// + a 1-row gap + the 5-row label = 8 font rows, so the block is 8·s tall and
// fits a 16-device cell at 100 % (s = 2) — a 3-row band overflowed it.
inline constexpr PixelBitmap kSquaresRow11x2{11, 2, {
    "###.###.###",
    "###.###.###"}};
inline constexpr int kSquaresGap = 1;   // rows between the squares and the label

// The ▾ that marks a ribbon button which opens a menu instead of acting.
// Solid, not an outline: at k = 1 (100 / 125 %) an outlined triangle is three
// disconnected pixels.
inline constexpr PixelBitmap kChevronDown7x4{7, 4, {
    "#######",
    ".#####.",
    "..###..",
    "...#..."}};

// The red "*" that turns the class glyph into "pointer to class" (ReClassEx C*).
inline constexpr PixelBitmap kStar5x5{5, 5, {
    "#.#.#",
    ".###.",
    "..#..",
    ".###.",
    "#.#.#"}};

// Width in font pixels of a label at scale 1: the sum of the per-glyph
// advances plus one gap between each pair (variable width, so this is no
// longer a formula in the character count).
// ── Text: Departure Mono, a real pixel font ──
// Replaced a hand-drawn 5x7 bitmap table on 2026-09-07. That table was crisp
// but had to be drawn at 2x to be legible, so every "pixel" was a 2x2 block —
// the user's "blocky", and at that grid a 4 read as a 9 and U was a V.
// Departure Mono (Helena Zhang, SIL OFL 1.1, no Reserved Font Name; licence in
// src/fonts/DepartureMono-LICENSE.txt) was drawn on an 11 px em, so at 11
// DEVICE px it draws 1-device-pixel strokes: same physical size as the old 2x
// blocks, four times the detail, and ~25 % narrower.
//
// The design size is documentation, NOT the rendering rule. "Only whole
// multiples of 11" was the rule here until 2026-09-08; measuring the face
// through drawPixelLabel's own threshold showed it is the wrong rule. What
// actually decides whether a size is clean is that EVERY vertical stem
// thresholds to the SAME width — a size that mixes 1 px and 2 px stems is the
// one that looks broken. Stem-run histogram over "H64" / "U16" / "FN*" /
// "WSTR" (cap height, advance per 3 chars, stem widths):
//   11 px  cap 8   adv 21   1 px x31                  uniform (the old size)
//   12 px  cap 9   adv 23   1 px x18 + 2 px x19       MIXED — visibly bad
//   13 px  cap 9   adv 25   2 px x32 + 1 px x3        near-uniform
//   14 px  cap 10  adv 27   2 px x42 + 1 px x1        uniform
//   15 px  cap 11  adv 29   2 px x44                  uniform, but chunky
//   16/18/20 px                                        MIXED
//   22 px  cap 16  adv 42   2 px x66                  uniform (2x the design)
// So 13 / 14 / 15 / 22 are all on-grid in the sense that matters, and the
// choice between them is taste, not correctness.
inline constexpr int kPixelFontDesign = 11;   // the em the OLD pixel face was drawn on

// Type-glyph label size in LOGICAL px (H64, U32, PTR, +4, WSTR).
inline constexpr int kRibbonLabelLogicalPx = 11;

// The font is drawn into the icon painters' DEVICE-resolution canvas, so the
// size is chosen in device px and antialiasing is off — the two things a pixel
// font needs to stay pixel-perfect at fractional DPI (a logical pixelSize at
// dpr 1.25 would land on 13.75 device px and blur).
// The font at an explicit DEVICE size. Antialiasing off and no hinting: both
// are what a pixel font needs to stay on its grid (the drawing path also hard-
// thresholds, because these two are only hints an OTF rasteriser may ignore).
inline QFont pixelLabelFontAt(int sizeDev) {
    // The app's own monospace face, the one the editor body and every ribbon
    // caption already use -- NOT a pixel font any more (2026-09-08).
    //
    // A pixel font is drawn for exactly one size: Departure Mono has 1-device-px
    // stems at its 11 px em and 2 px at every size above it, so "a little
    // bigger" and "not blocky" could not both be true, which is what the user
    // hit. An outline face has no such size: it is hinted and antialiased at
    // whatever the DPI asks for, the icons beside these labels are already
    // vector SVG, and this makes the whole strip one typeface.
    static const QString family = []() -> QString {
        const QString want = QSettings(QStringLiteral("RC"), QStringLiteral("RC"))
                                 .value(QStringLiteral("font"), QStringLiteral("JetBrains Mono"))
                                 .toString();
        if (QFontDatabase::families().contains(want)) return want;
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
        return QFontDatabase::families().contains(want) ? want
                                                        : QStringLiteral("Courier New");
    }();
    QFont f(family);
    f.setPixelSize(qMax(1, sizeDev));
    f.setFixedPitch(true);
    return f;
}

// Advance width / cap height in DEVICE px at an EXPLICIT device font size.
// The dpr forms below are these two with the strip's size table applied; the
// square QAction cell (pixelFontSizeSquareDev) needs its own size, so the
// measurement and the size choice have to be separable.
inline int pixelLabelWidthDevAt(QStringView label, int sizeDev) {
    return QFontMetrics(pixelLabelFontAt(sizeDev)).horizontalAdvance(label.toString());
}

inline int pixelLabelHeightDevAt(int sizeDev) {
    const QFontMetrics fm(pixelLabelFontAt(sizeDev));
    return fm.capHeight() > 0 ? int(fm.capHeight()) : fm.ascent();
}

// Glyph size in DEVICE px: a MEASURED two-tier table, not a formula.
//
// The rule is uniform stem width (see the histogram above), and within the
// sizes that satisfy it the pick is the one the user can actually read. 11 px
// was too small ("a little larger"); 15 px is uniform but every stem is 2 px
// on an 11 px em, which reads as doubled blocks — the user's "gross as fuck
// and blocky". 14 px is the clean step: +25 % cap height over 11 (8 -> 10),
// one stem width throughout, and — the reason it is free — the advance grows
// 21 -> 27 device px, which still sits under the 28 logical px that the 150 %
// tier already forces pixelLabelCellWidth to reserve. The cells do not move.
//
// Two tiers, not `design x round(dpr)`: at round(dpr) >= 2 the display is
// dense enough that 22 px (uniform, 2x the em) is the physically small option.
// The table also keeps the PHYSICAL size steadier across DPIs than 11/22 did
// — 14 logical px at 100 %, 11.2 at 125 %, 14.7 at 150 %, 11 at 200 %.
inline int pixelFontSizeDev(qreal dpr) {
    // Free to be any size now that the face is an outline one. 11 logical px
    // sits a touch under the 9 pt panel captions above the strip, which is the
    // "slightly larger than the old pixel labels" the user asked for: the pixel
    // face rendered 11 DEVICE px, i.e. 8.8 logical at 125 %.
    const qreal d = dpr > 0 ? dpr : 1.0;
    return qMax(1, qRound(kRibbonLabelLogicalPx * d));
}

inline QFont pixelLabelFont(qreal dpr) { return pixelLabelFontAt(pixelFontSizeDev(dpr)); }

// Advance width of `label` in DEVICE px at this dpr.
inline int pixelLabelWidthDev(QStringView label, qreal dpr) {
    return pixelLabelWidthDevAt(label, pixelFontSizeDev(dpr));
}

// Ink height in DEVICE px — the cap box, used to centre a label in its cell.
inline int pixelLabelHeightDev(qreal dpr) {
    return pixelLabelHeightDevAt(pixelFontSizeDev(dpr));
}

// ── The square QAction / QMenu cell has its own size ──
//
// ribbon_icons.h's `wide = false` path draws these same labels into a SQUARE
// 16-logical cell (QMenu clamps an action icon to PM_SmallIconSize, and handing
// it the strip's 24- / 32-wide pixmap would only get it smooth-scaled into a
// blur). The strip's size does not fit that cell: "H64" is 27 device px of
// advance at 14 px against a 20 device px cell at 125 %. Until 2026-09-08 it
// was drawn at the strip size anyway and simply CLIPPED — (20 - 27) / 2 = -3,
// so both edges of every 3-character label were cut off — while the header
// comment on that path claimed it was "shrunk to fit". This is the shrink.
//
// The size is capped at a REFERENCE label length, not fitted to each label on
// its own. Per-label fitting would put a 1-character "F" at the full strip size
// (cap 10) next to a "H64" at cap 4 IN THE SAME MENU — exactly the neighbouring-
// glyph inconsistency the one-scale rule exists to prevent. The reference is
// the MODAL length, 3 characters: H64 / I32 / PTR / STR / FN* / 000 are almost
// the whole vocabulary, so almost the whole vocabulary shares one size, and a
// longer label steps down a notch instead of clipping. (Fitting the LONGEST
// label instead — 4 characters, "WSTR" — was tried and rendered: it drags every
// glyph at 100 % down to 6 px, where "H64" is an unreadable blob. Making 20
// glyphs mushy to keep one in step is the worse trade.)
//
// "Clean" is pixelFontSizeDev's stem-uniformity rule, measured the same way
// (1-px vs 2-px stem runs over "H64" / "U16" / "FN*" / "WSTR" / "000" / "PTR"):
//   6 px   adv3 11  adv4 15  cap 4    1 px x44,  2 px x32    mixed — last resort
//   8 px   adv3 15  adv4 20  cap 6    1 px x100, 2 px x23    stems agree
//   9 px   adv3 17  adv4 23  cap 7    1 px x82,  2 px x62    mixed
//   10 px  adv3 19  adv4 25  cap 7    1 px x50,  2 px x90    mixed
//   11 px  adv3 21  adv4 28  cap 8    1 px x164, 2 px x9     stems agree
//   12 px  adv3 23                    1 px x89,  2 px x115   mixed
//   14 px  adv3 27  adv4 36  cap 10   1 px x23,  2 px x216   stems agree
//   16-21                             mixed
//   22 px  adv3 42  adv4 56  cap 16   1 px x0,   2 px x392   stems agree
// What that yields, against the 16 / 20 / 24 / 32 device px square cells:
//   3 chars and under   8 / 8 / 11 / 14 device px   (cap 6 / 6 / 8 / 10)
//   4 chars ("WSTR")    6 / 8 /  8 / 11 device px   (cap 4 / 6 / 6 /  8)
// i.e. ~5 logical px of cap in a 16 logical px cell at every DPI, with the one
// long label a single notch behind. Below 8 px the stems stop agreeing (6 px
// is 44 / 32), but a 16 device px cell cannot hold 4 characters at 8 px
// (advance 20), so 100 % spends 6 px on WSTR alone — a blunt glyph beats a
// cut-off one, and that is the only cell in the whole table that pays it.
inline constexpr int kSquareGlyphRefChars = 3;   // the modal label length

inline int pixelFontSizeSquareDev(QStringView label, int cellWDev, qreal dpr) {
    // Monospace, so a run of any character measures the reference width.
    const QString ref(kSquareGlyphRefChars, QLatin1Char('0'));
    int best = 0;
    for (int size : {6, 8, 11, 14, 22}) {
        if (size > pixelFontSizeDev(dpr)) break;   // never LARGER than the strip
        if (pixelLabelWidthDevAt(ref, size) <= cellWDev
         && pixelLabelWidthDevAt(label, size) <= cellWDev) best = size;
    }
    // Nothing fits: the floor still draws, and test_pixel_glyphs pins the
    // shipped vocabulary short enough that this branch is unreachable.
    return best > 0 ? best : 6;
}

// Logical width of the ribbon cell that holds a pixel label. THE one rule:
// ribbonIconCellWidth (layout) and the icon painters both call this.
//
// Sized from the DEVICE advance at the design size, unscaled. Layout is in
// logical px and must not depend on dpr, and the worst case (widest in logical
// terms) is dpr 1.0, where 1 device px IS 1 logical px. So this is exact at
// 100 % and leaves slack at every higher DPI.
inline int pixelLabelCellWidth(QStringView label) {
    // Layout is LOGICAL and must not depend on the DPI, but the font size does
    // (pixelFontSizeDev is a per-DPI table), so a single measurement is not
    // enough: a 3-char label is 14 logical px at 100 % (14 device / 1.0) but
    // 14.67 at 150 % (22 device / 1.5). Take the widest the label can be in
    // logical terms across every shipped DPI and size for that — which is why
    // the 11 -> 14 device step at the low tier cost the layout nothing: the
    // 150 % tier was already the binding case.
    qreal widest = 0;
    for (qreal dpr : {1.0, 1.25, 1.5, 2.0})
        widest = qMax(widest, pixelLabelWidthDev(label, dpr) / dpr);
    return qMax(16, qCeil(widest) + 6);
}


// Scale for the fixed BITMAPS above (the plus, the hook arrow, the rail
// chevron, the fill squares). Text no longer uses this — it is a real font
// now — but these shapes are still hand-drawn grids and want whole device
// pixels per grid cell: 2 up to 150 %, 3 at 200 %.
inline int pixelBitmapScale(qreal dpr) {
    return qMax(1, qRound(1.6 * dpr));
}

inline void drawBitmap(QPainter& p, int xDev, int yDev, const PixelBitmap& bm,
                       int scale, const QColor& ink) {
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    for (int r = 0; r < bm.h; ++r) {
        const char* row = bm.rows[r];
        for (int c = 0; c < bm.w; ++c)
            if (row[c] == '#')
                p.fillRect(xDev + c * scale, yDev + r * scale, scale, scale, ink);
    }
}

// Paints `label` (upper-cased) starting at device position (xDev, yDev).
// Paints `label` at DEVICE position (xDev, yDev) — yDev is the TOP of the cap
// box, matching how the old bitmap API was called.
//
// The text is rendered into a scratch image and its alpha HARD-THRESHOLDED
// before it is composited. Departure Mono ships as an OTF, i.e. CFF outlines,
// so Qt rasterises it through the normal vector path and antialiases the edges
// even with NoAntialias + TextAntialiasing off — both are hints the font engine
// may ignore. Measured: stray alpha 26 pixels along the stems. A pixel font
// with soft edges is just a small blurry font, which is the thing this change
// set out to fix, so the threshold is not optional polish — it is the feature.
// At a uniform-stem size (see pixelFontSizeDev) the outlines land on the grid,
// so thresholding snaps to the pixels the designer drew rather than inventing any.
//
// Takes the DEVICE size, not the dpr: the square QAction cell draws the same
// labels at pixelFontSizeSquareDev instead of the strip's table.
inline void drawPixelLabelAt(QPainter& p, int xDev, int yDev, QStringView label,
                             int sizeDev, const QColor& ink) {
    // (xDev, yDev) is the TOP-LEFT of the cap box, which is the contract every
    // caller centres against. The mask-and-threshold pass this used to run
    // existed only to keep a pixel face on its grid; an outline face wants the
    // rasteriser's own antialiasing, so it draws straight onto the canvas.
    const QFont f = pixelLabelFontAt(sizeDev);
    const QFontMetrics fm(f);
    const int cap = fm.capHeight() > 0 ? int(fm.capHeight()) : fm.ascent();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(f);
    p.setPen(ink);
    p.drawText(QPointF(xDev, yDev + cap), label.toString());
    p.restore();
}

inline void drawPixelLabel(QPainter& p, int xDev, int yDev, QStringView label,
                           qreal dpr, const QColor& ink) {
    drawPixelLabelAt(p, xDev, yDev, label, pixelFontSizeDev(dpr), ink);
}

}  // namespace rcx
