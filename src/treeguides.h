#pragma once

// ── Tree lines, drawn ──
//
// The editor's tree lines are painted over Scintilla's text in device pixels.
// They used to be box-drawing glyphs written into the indent, which broke
// between rows at some zooms and never sat on the row bands. The document text
// is now the same with the option on or off; this header turns the composed
// rows into line spans, and the spans into device-pixel rectangles.
//
// Structure. Every "elbow row" at depth D (a header, a field, an array element
// separator, a member row) is a child of the nearest row above it at depth D-1.
// A parent's vertical runs in the cell of its own first type character, c(k)
// for level k = D-1, from the top edge of its first child row down to the arm
// of its LAST child — never past it. Deeper levels are the children's own
// verticals, so the tree draws itself recursively. Footers, brace-wrap `{`
// rows and matrix continuation rows never get an arm; a vertical just passes
// through them.
//
// Geometry. Scintilla fills rows as aliased rectangles in logical px, which
// round in absolute device space, and the editor viewport's device origin is
// fractional at 125 %. So every coordinate here is computed from the painter's
// device transform (origin included) and snapped once, in device px: a vertical
// is ONE rectangle however many rows it crosses, its x depends only on its
// level, and an arm sits at a constant offset inside its row band. Nothing
// depends on the dirty rect, so a partial repaint reproduces the same pixels.
//
// The same geometry draws the tree columns (TreeColumns, below): dashes in the
// separator cells between the type, name and value columns.
//
// QtCore only (plus core.h) — unit-tested without a window.

#include "core.h"

#include <QHash>
#include <QRect>
#include <QString>
#include <QStringView>
#include <QVector>
#include <QtMath>
#include <algorithm>

namespace rcx {

// One parent's vertical: from the top edge of `topLine` (its first child row)
// to the bottom of the arm on `endLine` (its last child row).
struct TreeSpan {
    int topLine = 0;
    int endLine = 0;
    bool operator==(const TreeSpan& o) const { return topLine == o.topLine && endLine == o.endLine; }
    bool operator!=(const TreeSpan& o) const { return !(*this == o); }
};

struct TreeGuides {
    int lineCount = 0;
    QVector<qint16> elbowLevel;          // per line: the level of the row's arm, -1 = none
    QVector<QVector<TreeSpan>> byLevel;  // per level: sorted by topLine, disjoint

    bool isEmpty() const { return lineCount == 0; }
    bool operator==(const TreeGuides& o) const {
        return lineCount == o.lineCount && elbowLevel == o.elbowLevel && byLevel == o.byLevel;
    }
    bool operator!=(const TreeGuides& o) const { return !(*this == o); }
};

// A row that hangs off its parent's vertical with an arm.
inline bool isTreeElbowRow(const LineMeta& lm) {
    if (lm.depth <= 0 || lm.isContinuation) return false;
    return lm.lineKind == LineKind::Header
        || lm.lineKind == LineKind::Field
        || lm.lineKind == LineKind::ArrayElementSeparator;
}

// O(lines). A row at depth k closes every group at level >= k (its own
// children's and anything deeper); an elbow row then joins — or opens — the
// group at level k-1. Continuation rows sit at their matrix's depth and have no
// children, so they close nothing that is still open.
inline TreeGuides buildTreeGuides(const QVector<LineMeta>& meta) {
    TreeGuides g;
    g.lineCount = meta.size();
    g.elbowLevel.fill(-1, meta.size());
    QVector<int> open;  // per level: index into byLevel[level], -1 = closed
    for (int i = 0; i < meta.size(); ++i) {
        const LineMeta& lm = meta[i];
        const int k = lm.lineKind == LineKind::CommandRow ? 0 : qMax(0, lm.depth);
        for (int l = k; l < open.size(); ++l) open[l] = -1;
        if (!isTreeElbowRow(lm) || k - 1 > 0x7ffe) continue;
        const int level = k - 1;
        while (open.size() <= level) open.append(-1);
        while (g.byLevel.size() <= level) g.byLevel.append(QVector<TreeSpan>());
        g.elbowLevel[i] = qint16(level);
        if (open[level] < 0) {
            open[level] = g.byLevel[level].size();
            g.byLevel[level].append(TreeSpan{i, i});
        } else {
            g.byLevel[level][open[level]].endLine = i;
        }
    }
    return g;
}

// Index of the span at `level` whose rows include `line`, or -1.
inline int treeSpanAt(const TreeGuides& g, int level, int line) {
    if (level < 0 || level >= g.byLevel.size()) return -1;
    const auto& spans = g.byLevel[level];
    auto it = std::upper_bound(spans.begin(), spans.end(), line,
                               [](int ln, const TreeSpan& s) { return ln < s.topLine; });
    if (it == spans.begin()) return -1;
    --it;
    return line <= it->endLine ? int(it - spans.begin()) : -1;
}

// The indent of `line` as box-drawing text ("│ ├ " …), kTreeIndent chars per
// level — what copying a row puts in place of the drawn lines. Empty for rows
// without an indent.
inline QString treePrefixAscii(const TreeGuides& g, const QVector<LineMeta>& meta, int line) {
    if (line < 0 || line >= meta.size() || line >= g.lineCount) return {};
    const LineMeta& lm = meta[line];
    if (lm.lineKind == LineKind::CommandRow || lm.depth <= 0) return {};
    const int own = g.elbowLevel[line];
    QString out;
    out.reserve(lm.depth * kTreeIndent);
    for (int d = 0; d < lm.depth; ++d) {
        QChar ch = QLatin1Char(' ');
        const int si = treeSpanAt(g, d, line);
        if (si >= 0) {
            const TreeSpan& s = g.byLevel[d][si];
            if (d == own)
                ch = line < s.endLine ? QChar(0x251C) : QChar(0x2514);  // ├ └
            else if (line < s.endLine)
                ch = QChar(0x2502);                                      // │
        }
        out += ch;
        for (int p = 1; p < kTreeIndent; ++p) out += QLatin1Char(' ');
    }
    return out;
}

// ── Fold boxes ──
//
// A row with children carries a drawn [+] (closed) or [−] (open) box on its
// elbow: centred on its parent's vertical, where the arm leaves it, so the
// arm runs from the box to the row's type text and the row's own children hang
// from the line that starts under it. Drawn whether or not the tree lines are
// on. A depth-0 row (a second root) has no parent line; its box sits in the
// one-column gutter at column 0.
struct FoldBoxes {
    int lineCount = 0;
    int count = 0;              // rows with a box
    QVector<qint8> state;       // per line: -1 none, 0 open (−), 1 closed (+)
    QVector<qint16> col;        // per line: the document column the box is centred in

    bool isEmpty() const { return count == 0; }
    bool operator==(const FoldBoxes& o) const {
        return lineCount == o.lineCount && count == o.count && state == o.state && col == o.col;
    }
    bool operator!=(const FoldBoxes& o) const { return !(*this == o); }
};

inline bool isFoldBoxRow(const LineMeta& lm) {
    return lm.foldHead && lm.lineKind != LineKind::CommandRow;
}

// The cell a row's box is centred in: its parent's line (the parent's first
// type character), or column 0 for a row at depth 0.
inline int foldBoxCol(int depth) {
    return depth >= 1 ? kFoldCol + (depth - 1) * kTreeIndent : 0;
}

// The columns that toggle a row: its box's cell and the arm's cell after it,
// up to the row's type text.
inline ColumnSpan foldSlotFor(const LineMeta& lm) {
    if (!isFoldBoxRow(lm)) return {};
    const int typeStart = kFoldCol + qMax(0, lm.depth) * kTreeIndent;
    return {qMax(0, typeStart - kTreeIndent), typeStart, true};
}

inline FoldBoxes buildFoldBoxes(const QVector<LineMeta>& meta) {
    FoldBoxes b;
    b.lineCount = meta.size();
    b.state.fill(-1, meta.size());
    b.col.fill(0, meta.size());
    for (int i = 0; i < meta.size(); ++i) {
        const LineMeta& lm = meta[i];
        if (!isFoldBoxRow(lm)) continue;
        b.state[i] = lm.foldCollapsed ? 1 : 0;
        b.col[i] = qint16(qMin(foldBoxCol(lm.depth), 0x7fff));
        ++b.count;
    }
    return b;
}

// ── Tree columns ──
//
// Quiet vertical lines between a row's type, name and value columns (View ▸
// Tree Columns, independent of the tree lines), drawn like the tree lines: one
// continuous line per run of consecutive rows sharing a separator cell — the
// blank cell the formatters put before a name and before a value. A run is
// carried across a row at the same depth whose cell is blank (a struct header
// before its brace, a collapsed struct) when the column resumes right after;
// anything else in that cell ends it, so a nested block, whose columns sit
// further right, draws its own.
//
// A row has a cell only where its composed text really has an empty separator:
// a compact-mode type that overflows moves the row's cells with it (its
// effectiveTypeW), and nothing is ever drawn over a character.

struct TreeColumnRuns {
    int col = 0;               // document column of the separator cell
    QVector<TreeSpan> runs;    // rows [topLine, endLine], sorted, disjoint
    bool operator==(const TreeColumnRuns& o) const { return col == o.col && runs == o.runs; }
    bool operator!=(const TreeColumnRuns& o) const { return !(*this == o); }
};

struct TreeColumns {
    int lineCount = 0;
    QVector<int> start;    // per line: first index into cols; start[lineCount] == cols.size()
    QVector<qint16> cols;  // document columns of the separator cells, per line in order
    QVector<TreeColumnRuns> byCol;   // the lines to draw, sorted by column

    bool isEmpty() const { return lineCount == 0; }
    int firstCol(int line) const { return start[line]; }
    int endCol(int line) const { return start[line + 1]; }
    bool operator==(const TreeColumns& o) const {
        return lineCount == o.lineCount && start == o.start && cols == o.cols && byCol == o.byCol;
    }
    bool operator!=(const TreeColumns& o) const { return !(*this == o); }
};

// A row laid out as type | name | value. Members (enum, bitfield, code),
// separators, footers and braces are free text.
inline bool isTreeColumnRow(const LineMeta& lm) {
    if (lm.isMemberLine) return false;
    return lm.lineKind == LineKind::Field
        || lm.lineKind == LineKind::Continuation
        || lm.lineKind == LineKind::Header;
}

// `text` is the composed document (lines joined by '\n'), `meta` its rows.
inline TreeColumns buildTreeColumns(const QVector<LineMeta>& meta, const QString& text) {
    TreeColumns c;
    c.lineCount = meta.size();
    c.start.reserve(meta.size() + 1);
    QVector<QStringView> rows;
    rows.reserve(meta.size());
    int pos = 0;
    for (int i = 0; i < meta.size(); ++i) {
        c.start.append(c.cols.size());
        int end = text.indexOf(QLatin1Char('\n'), pos);
        if (end < 0) end = text.size();
        const QStringView line = QStringView(text).mid(qMin(pos, int(text.size())), qMax(0, end - pos));
        rows.append(line);
        pos = end + 1;

        const LineMeta& lm = meta[i];
        if (!isTreeColumnRow(lm)) continue;
        const LineGeometry geo = LineGeometry::forLine(lm);
        const int typeName = geo.typeStart() + geo.typeColumnWidth;
        // A hex row's ASCII preview is never cut to the name column: Hex128's
        // 16 characters run past a narrower one, and its separator with them.
        const int nameW = isHexPreview(lm.nodeKind) && lm.lineKind == LineKind::Field
            ? qMax(geo.nameColumnWidth, sizeForKind(lm.nodeKind))
            : geo.nameColumnWidth;
        const int nameValue = typeName + kSepWidth + nameW;
        // An empty separator with the row's text continuing after it.
        auto separator = [&](int col) {
            return col >= 0 && col + 1 < line.size() && line[col] == QLatin1Char(' ');
        };
        auto ink = [&](int col) { return col < line.size() && !line[col].isSpace(); };
        const bool header = lm.lineKind == LineKind::Header;
        // A header's name follows its type (an anonymous struct's type runs
        // straight into its brace: no name, no column); only a pointer header
        // has a value.
        if (separator(typeName)
            && (!header || (ink(typeName + 1) && line[typeName + 1] != QLatin1Char('{'))))
            c.cols.append(qint16(typeName));
        const bool pointerHeader = header && (lm.nodeKind == NodeKind::Pointer32
                                              || lm.nodeKind == NodeKind::Pointer64);
        if (separator(nameValue) && (!header || (pointerHeader && ink(nameValue + 1))))
            c.cols.append(qint16(nameValue));
    }
    c.start.append(c.cols.size());

    // Runs. An open run ends only on its column's rows, so rows carried across
    // at the end of a scope never extend it.
    struct Open { int index; int run; int depth; };
    QHash<int, Open> open;
    QHash<int, int> indexOf;
    for (int i = 0; i < meta.size(); ++i) {
        const int b = c.start[i], e = c.start[i + 1];
        auto hasCol = [&](int col) {
            for (int k = b; k < e; ++k)
                if (c.cols[k] == col) return true;
            return false;
        };
        for (auto it = open.begin(); it != open.end();) {
            const int col = it.key();
            const QStringView line = rows[i];
            const bool blank = col >= line.size() || line[col] == QLatin1Char(' ');
            if (hasCol(col) || (meta[i].depth == it->depth && blank)) ++it;
            else it = open.erase(it);
        }
        for (int k = b; k < e; ++k) {
            const int col = c.cols[k];
            auto it = open.find(col);
            if (it != open.end()) {
                c.byCol[it->index].runs[it->run].endLine = i;
                continue;
            }
            int index = indexOf.value(col, -1);
            if (index < 0) {
                index = c.byCol.size();
                indexOf.insert(col, index);
                TreeColumnRuns runs;
                runs.col = col;
                c.byCol.append(runs);
            }
            c.byCol[index].runs.append(TreeSpan{i, i});
            open.insert(col, Open{index, int(c.byCol[index].runs.size()) - 1, meta[i].depth});
        }
    }
    std::sort(c.byCol.begin(), c.byCol.end(),
              [](const TreeColumnRuns& a, const TreeColumnRuns& b) { return a.col < b.col; });
    return c;
}

// ── Geometry ──

struct TreeGuideGeom {
    qreal sx = 1, sy = 1;  // device px per logical px (the painter's scale)
    qreal ox = 0, oy = 0;  // the viewport's origin in device px (absolute; fractional at 125 %)
    int   first = 0;       // first visible document line
    int   lh = 1;          // line height, logical px (Scintilla's is an integer)
    int   ea = 0, ed = 0;  // extra ascent / descent, logical px
    int   xOrigin = 0;     // x of document column 0, logical, viewport-local (margin - xOffset)
    qreal adv = 8;         // advance of one column, logical px
    int   t = 1;           // stroke width, device px
    QRect clipDev;         // where lines may land, device px (the text area, never the margin)
};

// Lines keep pace with the text: one device px at the default size up to
// 150 %, two at 175–200 % or high zoom, three at the largest zoom.
inline int treeStrokeDevPx(qreal sx, qreal adv) {
    return qMax(1, qRound(sx * adv / 11.0));
}

namespace treegeom {

// The one rounding rule for row edges; the same one Qt applies to Scintilla's
// aliased row fills, so a vertical starts on the first device row of its band.
inline int snapEdge(qreal v) { return qRound(v); }

inline int rowTop(const TreeGuideGeom& g, int line) {
    return snapEdge(g.oy + g.sy * qreal(line - g.first) * g.lh);
}

// An arm's offset below its row top: centred on the font box (between the
// extra ascent and descent), the same for every row, and always inside the
// shortest band a row can round to.
inline int armOffset(const TreeGuideGeom& g) {
    const qreal centre = g.ea + (g.lh - g.ea - g.ed) / 2.0;
    const int band = qFloor(g.sy * g.lh);
    return qBound(0, qFloor(g.sy * centre - g.t / 2.0 + 0.5), qMax(0, band - g.t));
}

inline qreal colX(const TreeGuideGeom& g, qreal col) {
    return g.ox + g.sx * (g.xOrigin + col * g.adv);
}

inline int levelCol(int level) { return kFoldCol + level * kTreeIndent; }

// Left device column of a t-wide line centred in document column `col`.
inline int cellLineX(const TreeGuideGeom& g, int col) {
    return qFloor(colX(g, col) + g.sx * g.adv / 2.0 - g.t / 2.0 + 0.5);
}

// Left device column of the vertical for `level`: centred in cell c(level).
inline int railX(const TreeGuideGeom& g, int level) {
    return cellLineX(g, levelCol(level));
}

// Right end (exclusive) of an arm at `level`: the centre of the next cell,
// half a cell short of the child's type text.
inline int armEnd(const TreeGuideGeom& g, int level) {
    return snapEdge(colX(g, levelCol(level) + 1) + g.sx * g.adv * 0.5);
}

// Half a fold box's side, not counting its centre stroke: about half a cell,
// never taller than the row band allows. The side is 2h + t, so the centre
// stroke of the + sits exactly in the middle — on the parent's line.
inline int foldBoxHalf(const TreeGuideGeom& g) {
    const int cell = qFloor(g.sx * g.adv);
    const int band = qFloor(g.sy * g.lh);
    const int h = qMax(g.t + 1, (qMin(cell, band - 2) - g.t) / 2);
    return qMax(1, qMin(h, (band - g.t) / 2));
}

// A row's box: centred on the line in column `col`, its − stroke on the row's
// arm (pulled inside the row band when the band is too short to centre it).
inline QRect foldBoxRect(const TreeGuideGeom& g, int line, int col) {
    const int h = foldBoxHalf(g);
    const int s = 2 * h + g.t;
    const int top0 = rowTop(g, line);
    const int band = rowTop(g, line + 1) - top0;
    const int top = qBound(top0, top0 + armOffset(g) - h, qMax(top0, top0 + band - s));
    return QRect(cellLineX(g, col) - h, top, s, s);
}

} // namespace treegeom

// The strokes of one fold box, appended to `out` clipped to g.clipDev: the
// outline as four rects (top, bottom, then the sides between them) and the
// sign — a − for an open row, a + (the − and two halves of a vertical) for a
// closed one. No two rects overlap.
inline void appendFoldBoxStrokes(const TreeGuideGeom& g, const QRect& box, bool closed,
                                 QVector<QRect>& out) {
    auto add = [&](int x, int y, int w, int h) {
        const QRect c = QRect(x, y, w, h).intersected(g.clipDev);
        if (!c.isEmpty()) out.append(c);
    };
    const int t = g.t, s = box.width(), x = box.left(), y = box.top();
    const int h = (s - t) / 2;
    add(x, y, s, t);
    add(x, y + s - t, s, t);
    if (s > 2 * t) {
        add(x, y + t, t, s - 2 * t);
        add(x + s - t, y + t, t, s - 2 * t);
    }
    // The sign keeps a gap from the outline once there is room for one.
    const int gap = (h - 2 * t >= 1) ? qMax(t, qRound(h / 3.0)) : 0;
    const int a = h - t - gap;   // the sign's reach from its centre stroke
    if (a < 1) return;
    add(x + t + gap, y + h, 2 * a + t, t);
    if (closed) {
        add(x + h, y + t + gap, t, a);
        add(x + h, y + h + t, t, a);
    }
}

// Device-px strokes of every fold box on rows [firstLine, lastLine]. The box
// on `hoverLine` goes to `hoverOut` instead, when given.
inline void foldBoxDeviceRects(const TreeGuideGeom& g, const FoldBoxes& fb,
                               int firstLine, int lastLine, QVector<QRect>& out,
                               int hoverLine = -1, QVector<QRect>* hoverOut = nullptr) {
    out.clear();
    if (hoverOut) hoverOut->clear();
    if (fb.isEmpty() || g.t <= 0 || g.lh <= 0 || g.clipDev.isEmpty()) return;
    firstLine = qMax(0, firstLine);
    lastLine = qMin(fb.lineCount - 1, lastLine);
    for (int line = firstLine; line <= lastLine; ++line) {
        if (fb.state[line] < 0) continue;
        const QRect box = treegeom::foldBoxRect(g, line, fb.col[line]);
        appendFoldBoxStrokes(g, box, fb.state[line] == 1,
                             (hoverOut && line == hoverLine) ? *hoverOut : out);
    }
}

// Device-px rectangles for every vertical and arm touching rows
// [firstLine, lastLine], clipped to g.clipDev. Rectangles never overlap. With
// `boxes`, a vertical breaks around each box that sits on it and that row's
// arm starts at the box's right edge, so lines and boxes never overlap either.
inline void treeGuideDeviceRects(const TreeGuideGeom& g, const TreeGuides& tg,
                                 int firstLine, int lastLine, QVector<QRect>& out,
                                 const FoldBoxes* boxes = nullptr) {
    out.clear();
    if (tg.lineCount <= 0 || g.t <= 0 || g.lh <= 0 || g.clipDev.isEmpty()) return;
    firstLine = qMax(0, firstLine);
    lastLine = qMin(tg.lineCount - 1, lastLine);
    if (lastLine < firstLine) return;
    if (boxes && (boxes->lineCount != tg.lineCount || boxes->isEmpty())) boxes = nullptr;

    const int t = g.t;
    const int armOff = treegeom::armOffset(g);
    auto add = [&](const QRect& r) {
        const QRect c = r.intersected(g.clipDev);
        if (!c.isEmpty()) out.append(c);
    };
    // The box on `line` when it sits on the vertical of `level`.
    auto boxOn = [&](int line, int level, QRect* box) {
        if (!boxes || boxes->state[line] < 0 || tg.elbowLevel[line] != level
            || boxes->col[line] != treegeom::levelCol(level))
            return false;
        *box = treegeom::foldBoxRect(g, line, boxes->col[line]);
        return true;
    };

    for (int level = 0; level < tg.byLevel.size(); ++level) {
        const auto& spans = tg.byLevel[level];
        if (spans.isEmpty()) continue;
        const int x = treegeom::railX(g, level);
        // Disjoint and sorted, so endLine is sorted too.
        auto it = std::lower_bound(spans.begin(), spans.end(), firstLine,
                                   [](const TreeSpan& s, int ln) { return s.endLine < ln; });
        for (; it != spans.end() && it->topLine <= lastLine; ++it) {
            int top = treegeom::rowTop(g, it->topLine);
            const int bottom = treegeom::rowTop(g, it->endLine) + armOff + t;
            if (boxes) {
                const int lo = qMax(it->topLine, firstLine), hi = qMin(it->endLine, lastLine);
                QRect box;
                for (int line = lo; line <= hi; ++line) {
                    if (!boxOn(line, level, &box)) continue;
                    const int cutEnd = qMin(bottom, box.top());
                    if (cutEnd > top) add(QRect(x, top, t, cutEnd - top));
                    top = qMax(top, box.top() + box.height());
                }
            }
            if (bottom > top) add(QRect(x, top, t, bottom - top));
        }
    }

    for (int line = firstLine; line <= lastLine; ++line) {
        const int level = tg.elbowLevel[line];
        if (level < 0) continue;
        QRect box;
        const int x0 = boxOn(line, level, &box) ? box.left() + box.width()
                                                : treegeom::railX(g, level) + t;
        const int x1 = treegeom::armEnd(g, level);
        if (x1 <= x0) continue;
        add(QRect(x0, treegeom::rowTop(g, line) + armOff, x1 - x0, t));
    }
}

// Device-px rectangles for the column lines touching rows [firstLine,
// lastLine], clipped to g.clipDev: one t-wide rect per run, from the top of its
// first row band to the bottom of its last. A run through `skipLine` with a
// column at or right of `skipFromCol` leaves that row's band out — the row an
// inline edit is shifting, whose text no longer matches its composed cells.
inline void treeColumnDeviceRects(const TreeGuideGeom& g, const TreeColumns& tc,
                                  int firstLine, int lastLine, QVector<QRect>& out,
                                  int skipLine = -1, int skipFromCol = 0x7fffffff) {
    out.clear();
    if (tc.lineCount <= 0 || g.t <= 0 || g.lh <= 0 || g.clipDev.isEmpty()) return;
    firstLine = qMax(0, firstLine);
    lastLine = qMin(tc.lineCount - 1, lastLine);
    if (lastLine < firstLine) return;
    for (const TreeColumnRuns& cl : tc.byCol) {
        const int x = treegeom::cellLineX(g, cl.col);
        const bool skipping = skipLine >= 0 && cl.col >= skipFromCol;
        auto add = [&](int top, int end) {
            if (end < top) return;
            const int y0 = treegeom::rowTop(g, top);
            const int y1 = treegeom::rowTop(g, end + 1);
            const QRect r = QRect(x, y0, g.t, y1 - y0).intersected(g.clipDev);
            if (!r.isEmpty()) out.append(r);
        };
        // Disjoint and sorted, so endLine is sorted too.
        auto it = std::lower_bound(cl.runs.begin(), cl.runs.end(), firstLine,
                                   [](const TreeSpan& s, int ln) { return s.endLine < ln; });
        for (; it != cl.runs.end() && it->topLine <= lastLine; ++it) {
            if (skipping && skipLine >= it->topLine && skipLine <= it->endLine) {
                add(it->topLine, skipLine - 1);
                add(skipLine + 1, it->endLine);
            } else {
                add(it->topLine, it->endLine);
            }
        }
    }
}

} // namespace rcx
