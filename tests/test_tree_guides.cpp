// Drawn tree lines, without a window: the spans built from composed rows
// (checked against an independent scan and hand-read goldens), the text a copy
// puts in their place, and the device-pixel geometry swept over display scale,
// device-origin phase, zoom-sized advances, row spacing, horizontal scroll and
// first visible line.

#include <QtTest/QTest>

#include "core.h"
#include "treeguides.h"
#include "providers/buffer_provider.h"
#include "tree_fixture.h"

#include <algorithm>
#include <tuple>

using namespace rcx;

namespace {

int levelKey(const LineMeta& lm) {
    return lm.lineKind == LineKind::CommandRow ? 0 : qMax(0, lm.depth);
}

// An independent reading of the rule, one level at a time: a row belongs to
// the group of the nearest arm at that level above it (nothing shallower in
// between), and the group continues past it if another arm at that level
// follows before anything shallower does.
QString oraclePrefix(const QVector<LineMeta>& meta, int line) {
    const LineMeta& lm = meta[line];
    if (lm.lineKind == LineKind::CommandRow || lm.depth <= 0) return {};
    QString out;
    for (int d = 0; d < lm.depth; ++d) {
        auto armAt = [&](int i) { return isTreeElbowRow(meta[i]) && levelKey(meta[i]) - 1 == d; };
        bool inGroup = false;
        for (int j = line; j >= 0; --j) {
            if (armAt(j)) { inGroup = true; break; }
            if (levelKey(meta[j]) <= d) break;
        }
        bool more = false;
        if (inGroup) {
            for (int s = line + 1; s < meta.size(); ++s) {
                if (armAt(s)) { more = true; break; }
                if (levelKey(meta[s]) <= d) break;
            }
        }
        const bool own = isTreeElbowRow(lm) && d == lm.depth - 1;
        QChar ch = QLatin1Char(' ');
        if (inGroup)
            ch = own ? (more ? QChar(0x251C) : QChar(0x2514)) : (more ? QChar(0x2502) : QLatin1Char(' '));
        out += ch;
        out += QLatin1Char(' ');
    }
    return out;
}

// Every structural promise the painter relies on. Empty = all hold.
QString invariantFailure(const QVector<LineMeta>& meta, const TreeGuides& g) {
    if (g.lineCount != meta.size()) return QStringLiteral("lineCount");
    for (int i = 0; i < meta.size(); ++i) {
        const int want = isTreeElbowRow(meta[i]) ? levelKey(meta[i]) - 1 : -1;
        if (g.elbowLevel[i] != want)
            return QStringLiteral("elbowLevel[%1]=%2 want %3").arg(i).arg(g.elbowLevel[i]).arg(want);
    }
    for (int level = 0; level < g.byLevel.size(); ++level) {
        int prevEnd = -1;
        for (const TreeSpan& s : g.byLevel[level]) {
            if (s.topLine > s.endLine || s.topLine <= prevEnd)
                return QStringLiteral("level %1: span %2..%3 unsorted or overlapping").arg(level).arg(s.topLine).arg(s.endLine);
            prevEnd = s.endLine;
            for (int end : {s.topLine, s.endLine})
                if (g.elbowLevel[end] != level)
                    return QStringLiteral("level %1: span end %2 is not an arm at that level").arg(level).arg(end);
            for (int i = s.topLine + 1; i < s.endLine; ++i)
                if (levelKey(meta[i]) <= level)
                    return QStringLiteral("level %1: row %2 inside span %3..%4 is shallower").arg(level).arg(i).arg(s.topLine).arg(s.endLine);
            if (s.topLine > 0 && levelKey(meta[s.topLine - 1]) > level)
                return QStringLiteral("level %1: span at %2 has no parent row above").arg(level).arg(s.topLine);
        }
    }
    for (int i = 0; i < meta.size(); ++i) {
        if (g.elbowLevel[i] >= 0 && treeSpanAt(g, g.elbowLevel[i], i) < 0)
            return QStringLiteral("arm row %1 in no span").arg(i);
        const QString got = treePrefixAscii(g, meta, i);
        const QString want = oraclePrefix(meta, i);
        if (got != want)
            return QStringLiteral("row %1 prefix [%2] want [%3]").arg(i).arg(got, want);
    }
    return {};
}

QString dump(const ComposeResult& cr, const TreeGuides& g) {
    const QStringList lines = cr.text.split(QLatin1Char('\n'));
    QString out;
    for (int i = 0; i < cr.meta.size(); ++i)
        out += QStringLiteral("[%1] d=%2 k=%3 |%4|%5\n").arg(i, 2).arg(cr.meta[i].depth)
                   .arg(int(cr.meta[i].lineKind)).arg(treePrefixAscii(g, cr.meta, i), lines.value(i));
    return out;
}

using RectKey = std::tuple<int, int, int, int>;
QVector<RectKey> keys(const QVector<QRect>& rects) {
    QVector<RectKey> k;
    for (const QRect& r : rects) k.append({r.x(), r.y(), r.width(), r.height()});
    std::sort(k.begin(), k.end());
    return k;
}

} // namespace

class TestTreeGuides : public QObject {
    Q_OBJECT

private:
    struct Composed { ComposeResult cr; TreeGuides g; TreeColumns cols; };
    static Composed composeFixture(const treefix::Rich& fx, uint64_t viewRoot, bool brace,
                                   bool compact = false) {
        BufferProvider prov(fx.data);
        Composed c;
        c.cr = compose(fx.tree, prov, viewRoot, compact, /*treeLines=*/true, brace);
        c.g = buildTreeGuides(c.cr.meta);
        c.cols = buildTreeColumns(c.cr.meta, c.cr.text);
        return c;
    }

private slots:
    void theTwoRootShapeReadsAsBefore() {
        const auto fx = treefix::twoRoots();
        const auto c = composeFixture(fx, 0, false);
        // The shape testTreeLinesDepth2 traced: root rows, an expanded pointer
        // whose three children hang off it, the pointer's `}` crossed by the
        // root's vertical, then the second root on its own.
        const QStringList want = {
            QString(),
            QStringLiteral("├ "),
            QStringLiteral("├ "),
            QStringLiteral("│ ├ "),
            QStringLiteral("│ ├ "),
            QStringLiteral("│ └ "),
            QStringLiteral("│ "),
            QStringLiteral("└ "),
            QString(),
            QString(),
            QStringLiteral("├ "),
            QStringLiteral("├ "),
            QStringLiteral("└ "),
            QString(),
        };
        QStringList got;
        for (int i = 0; i < c.cr.meta.size(); ++i) got << treePrefixAscii(c.g, c.cr.meta, i);
        QVERIFY2(got == want, qPrintable(dump(c.cr, c.g)));
        QVERIFY2(invariantFailure(c.cr.meta, c.g).isEmpty(), qPrintable(invariantFailure(c.cr.meta, c.g)));
    }

    void everyFixtureKeepsTheInvariants_data() {
        QTest::addColumn<int>("which");
        QTest::addColumn<bool>("brace");
        const char* names[] = {"rich", "rich-last-scalar", "two-roots", "deep14", "unreadable-code"};
        for (int w = 0; w < 5; ++w)
            for (bool brace : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1%2").arg(QLatin1String(names[w]), brace ? QStringLiteral("-brace") : QString())))
                    << w << brace;
    }

    void everyFixtureKeepsTheInvariants() {
        QFETCH(int, which);
        QFETCH(bool, brace);
        treefix::Rich fx = which == 0 ? treefix::richTree(true)
                         : which == 1 ? treefix::richTree(false)
                         : which == 2 ? treefix::twoRoots()
                         : which == 3 ? treefix::deepTree(14)
                                      : treefix::unreadableCode();
        const uint64_t viewRoot = which == 2 ? 0 : fx.rootId;
        const auto c = composeFixture(fx, viewRoot, brace);
        QVERIFY(c.cr.meta.size() > 3);
        const QString why = invariantFailure(c.cr.meta, c.g);
        QVERIFY2(why.isEmpty(), qPrintable(why + QStringLiteral("\n") + dump(c.cr, c.g)));

        // The text is the same with tree lines on or off: they are drawn.
        BufferProvider prov(fx.data);
        const auto off = compose(fx.tree, prov, viewRoot, false, /*treeLines=*/false, brace);
        QCOMPARE(off.text, c.cr.text);
        for (QChar ch : {QChar(0x2502), QChar(0x251C), QChar(0x2514)})
            QVERIFY(!c.cr.text.contains(ch));
    }

    // Which rows get an arm, decided from the raw row fields per kind — never
    // through isTreeElbowRow — and every kind must actually be in the fixtures,
    // so a rule that silently drops member rows or separators fails here.
    void everyRowKindHangsOffItsParent_data() {
        QTest::addColumn<bool>("brace");
        QTest::newRow("plain") << false;
        QTest::newRow("brace") << true;
    }

    void everyRowKindHangsOffItsParent() {
        QFETCH(bool, brace);
        int enumRows = 0, bitRows = 0, codeRows = 0, unreadableRows = 0, separators = 0,
            elements = 0, headers = 0, footers = 0, continuations = 0, plain = 0;
        const treefix::Rich fixtures[] = {treefix::richTree(true), treefix::unreadableCode()};
        for (const treefix::Rich& fx : fixtures) {
            const auto c = composeFixture(fx, fx.rootId, brace);
            const auto& m = c.cr.meta;
            for (int i = 0; i < m.size(); ++i) {
                const LineMeta& lm = m[i];
                if (lm.lineKind == LineKind::CommandRow || lm.depth <= 0) continue;
                const QString p = treePrefixAscii(c.g, m, i);
                const bool arm = p.endsWith(QStringLiteral("├ ")) || p.endsWith(QStringLiteral("└ "));
                const Node* owner = lm.nodeIdx >= 0 ? &fx.tree.nodes[lm.nodeIdx] : nullptr;
                bool want = true;
                if (lm.lineKind == LineKind::Footer)                             { want = false; ++footers; }
                else if (lm.isContinuation || lm.lineKind == LineKind::Continuation) { want = false; ++continuations; }
                else if (lm.lineKind == LineKind::ArrayElementSeparator)         ++separators;
                else if (lm.lineKind == LineKind::Header)                        ++headers;
                else if (lm.isMemberLine && owner && owner->isEnum())            ++enumRows;
                else if (lm.isMemberLine && owner && owner->isBitfield())        ++bitRows;
                else if (lm.isMemberLine && owner && owner->kind == NodeKind::Asm) ++(lm.unreadable ? unreadableRows : codeRows);
                else if (lm.isArrayElement)                                      ++elements;
                else                                                             ++plain;
                QVERIFY2(arm == want, qPrintable(QStringLiteral("row %1 [%2] arm %3, want %4\n").arg(i).arg(p).arg(arm).arg(want)
                                                 + dump(c.cr, c.g)));
            }
        }
        QVERIFY2(enumRows && bitRows && codeRows && unreadableRows && separators && elements
                     && headers && footers && continuations && plain,
                 qPrintable(QStringLiteral("fixture lacks a row kind: enum %1 bit %2 code %3 unreadable %4 sep %5 elem %6 hdr %7 ftr %8 cont %9")
                                .arg(enumRows).arg(bitRows).arg(codeRows).arg(unreadableRows).arg(separators)
                                .arg(elements).arg(headers).arg(footers).arg(continuations)));
    }

    // The drawn columns against the composed TEXT: a group's vertical sits in
    // the cell of its parent's first type character, the cells it crosses are
    // blank on every row, and each arm ends where its child's text begins.
    void railsSitInTheParentsTypeColumn_data() { everyFixtureKeepsTheInvariants_data(); }

    void railsSitInTheParentsTypeColumn() {
        QFETCH(int, which);
        QFETCH(bool, brace);
        treefix::Rich fx = which == 0 ? treefix::richTree(true)
                         : which == 1 ? treefix::richTree(false)
                         : which == 2 ? treefix::twoRoots()
                         : which == 3 ? treefix::deepTree(14)
                                      : treefix::unreadableCode();
        const uint64_t viewRoot = which == 2 ? 0 : fx.rootId;
        const auto c = composeFixture(fx, viewRoot, brace);
        const QStringList lines = c.cr.text.split(QLatin1Char('\n'));
        const auto& m = c.cr.meta;
        auto charAt = [&](int row, int k) {
            const QString& t = lines.at(row);
            return k < t.size() ? t.at(k) : QLatin1Char(' ');
        };
        auto firstInk = [&](int row) {
            const QString& t = lines[row];
            for (int k = kFoldCol; k < t.size(); ++k)
                if (!t[k].isSpace()) return k;
            return -1;
        };
        int spans = 0;
        for (int level = 0; level < c.g.byLevel.size(); ++level) {
            const int col = treegeom::levelCol(level);
            for (const TreeSpan& s : c.g.byLevel[level]) {
                ++spans;
                int p = s.topLine - 1;
                while (p >= 0 && m[p].lineKind == LineKind::Footer && levelKey(m[p]) == level
                       && lines[p].trimmed() == QStringLiteral("{"))
                    --p;
                const QString ctx = QStringLiteral("level %1 span %2..%3 parent %4\n").arg(level).arg(s.topLine).arg(s.endLine).arg(p)
                                    + dump(c.cr, c.g);
                QVERIFY2(p >= 0 && levelKey(m[p]) == level, qPrintable(ctx));
                if (m[p].lineKind != LineKind::CommandRow)
                    QVERIFY2(firstInk(p) == col, qPrintable(QStringLiteral("parent text starts at %1, rail cell %2\n").arg(firstInk(p)).arg(col) + ctx));
                for (int i = s.topLine; i <= s.endLine; ++i) {
                    for (int k = 0; k < kTreeIndent; ++k)
                        QVERIFY2(charAt(i, col + k).isSpace(),
                                 qPrintable(QStringLiteral("row %1 has text in the rail cell\n").arg(i) + ctx));
                    if (c.g.elbowLevel[i] == level)
                        QVERIFY2(!charAt(i, col + kTreeIndent).isSpace(),
                                 qPrintable(QStringLiteral("row %1's text does not start where its arm ends\n").arg(i) + ctx));
                }
            }
        }
        QVERIFY(spans > 0);
    }

    // Tree columns against the composed text: free-text rows have none; every
    // dash sits in an empty cell with the row's type text before it; a plain
    // field gets both, its name starting right after the first and its value
    // right after the second; an overflowing compact type carries its cells.
    void columnsSitInTheSeparatorCells_data() {
        QTest::addColumn<int>("which");
        QTest::addColumn<bool>("brace");
        QTest::addColumn<bool>("compact");
        const char* names[] = {"rich", "rich-last-scalar", "two-roots", "deep14", "unreadable-code", "overflow"};
        for (int w = 0; w < 6; ++w)
            for (bool brace : {false, true})
                for (bool compact : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1%2%3").arg(QLatin1String(names[w]),
                                                                           brace ? QStringLiteral("-brace") : QString(),
                                                                           compact ? QStringLiteral("-compact") : QString())))
                        << w << brace << compact;
    }

    void columnsSitInTheSeparatorCells() {
        QFETCH(int, which);
        QFETCH(bool, brace);
        QFETCH(bool, compact);
        treefix::Rich fx = which == 0 ? treefix::richTree(true)
                         : which == 1 ? treefix::richTree(false)
                         : which == 2 ? treefix::twoRoots()
                         : which == 3 ? treefix::deepTree(14)
                         : which == 4 ? treefix::unreadableCode()
                                      : treefix::overflowTree();
        const uint64_t viewRoot = which == 2 ? 0 : fx.rootId;
        const auto c = composeFixture(fx, viewRoot, brace, compact);
        const QStringList lines = c.cr.text.split(QLatin1Char('\n'));
        const auto& m = c.cr.meta;
        QCOMPARE(c.cols.lineCount, m.size());
        int plainFields = 0, overflowRows = 0, headers = 0, anonymousHeaders = 0, pointerHeaders = 0,
            continuations = 0, elements = 0, foldPointers = 0;
        for (int i = 0; i < m.size(); ++i) {
            const LineMeta& lm = m[i];
            const QString& t = lines.at(i);
            const int first = c.cols.firstCol(i), n = c.cols.endCol(i) - first;
            const QString ctx = QStringLiteral("row %1 |%2| cols").arg(i).arg(t)
                + [&] { QString s; for (int k = 0; k < n; ++k) s += QStringLiteral(" %1").arg(c.cols.cols[first + k]); return s; }();
            if (lm.lineKind == LineKind::Footer || lm.lineKind == LineKind::ArrayElementSeparator
                || lm.lineKind == LineKind::CommandRow || lm.isMemberLine) {
                QVERIFY2(n == 0, qPrintable(ctx + QStringLiteral(": free text got a column")));
                continue;
            }
            const int typeStart = kFoldCol + lm.depth * kTreeIndent;
            int prev = typeStart;
            for (int k = 0; k < n; ++k) {
                const int col = c.cols.cols[first + k];
                QVERIFY2(col > prev && col + 1 < t.size() && t.at(col) == QLatin1Char(' '),
                         qPrintable(ctx + QStringLiteral(": column %1 is not an empty cell inside the row").arg(col)));
                if (k == 0 && lm.lineKind != LineKind::Continuation)
                    QVERIFY2(!t.mid(typeStart, col - typeStart).trimmed().isEmpty(),
                             qPrintable(ctx + QStringLiteral(": no type text before the first column")));
                prev = col + 1;
            }
            const Node* node = lm.nodeIdx >= 0 ? &fx.tree.nodes[lm.nodeIdx] : nullptr;
            const bool scalar = node && (node->kind == NodeKind::Int8 || node->kind == NodeKind::Int16
                                         || node->kind == NodeKind::Int32 || node->kind == NodeKind::Int64
                                         || node->kind == NodeKind::UInt8 || node->kind == NodeKind::UInt16
                                         || node->kind == NodeKind::UInt32 || node->kind == NodeKind::UInt64
                                         || node->kind == NodeKind::Float || node->kind == NodeKind::Double);
            // Every column row's count, decided from its own fields — never from
            // isTreeColumnRow — so a row kind that loses its dashes fails here.
            const bool pointerKind = lm.nodeKind == NodeKind::Pointer32 || lm.nodeKind == NodeKind::Pointer64;
            if (lm.lineKind == LineKind::Header) {
                // A named type then its name: one column; a pointer header's value
                // makes two; an anonymous struct has no name and no column.
                const bool anonymous = node && node->name.isEmpty();
                const int want = anonymous ? 0 : (pointerKind ? 2 : 1);
                QVERIFY2(n == want, qPrintable(ctx + QStringLiteral(": header has %1 columns, want %2").arg(n).arg(want)));
                if (want >= 1 && node)
                    QVERIFY2(t.mid(c.cols.cols[first] + 1, node->name.size()) == node->name,
                             qPrintable(ctx + QStringLiteral(": the header's name doesn't start after its column")));
                ++headers;
                if (anonymous) ++anonymousHeaders;
                if (pointerKind) ++pointerHeaders;
            } else if (lm.lineKind == LineKind::Continuation || lm.isContinuation) {
                int head = i;
                while (head > 0 && (m[head].lineKind == LineKind::Continuation || m[head].isContinuation)) --head;
                const int hf = c.cols.firstCol(head);
                QVERIFY2(n == 2 && c.cols.endCol(head) - hf == 2
                             && c.cols.cols[first] == c.cols.cols[hf] && c.cols.cols[first + 1] == c.cols.cols[hf + 1],
                         qPrintable(ctx + QStringLiteral(": a matrix row must keep its first row's columns")));
                ++continuations;
            } else if (lm.lineKind == LineKind::Field && lm.isArrayElement) {
                QVERIFY2(n == 2 && !t.mid(c.cols.cols[first + 1] + 1, 2).trimmed().isEmpty(),
                         qPrintable(ctx + QStringLiteral(": an array element needs both columns, its value after the second")));
                ++elements;
            } else if (lm.lineKind == LineKind::Field && lm.foldHead && pointerKind && node) {
                QVERIFY2(n == 2, qPrintable(ctx + QStringLiteral(": a collapsed pointer needs both columns")));
                QVERIFY2(t.mid(c.cols.cols[first] + 1, node->name.size()) == node->name,
                         qPrintable(ctx + QStringLiteral(": the pointer's name doesn't start after the first column")));
                QVERIFY2(!t.mid(c.cols.cols[first + 1] + 1, 2).trimmed().isEmpty(),
                         qPrintable(ctx + QStringLiteral(": the pointer's value doesn't start after the second column")));
                ++foldPointers;
            }
            if (lm.lineKind == LineKind::Field && !lm.isArrayElement && !lm.foldHead && isHexPreview(lm.nodeKind)) {
                // The ASCII preview fills the name column; the bytes the value.
                ++plainFields;
                QVERIFY2(n == 2, qPrintable(ctx + QStringLiteral(": a hex row needs both columns")));
                QVERIFY2(!t.mid(c.cols.cols[first + 1] + 1, 2).trimmed().isEmpty(),
                         qPrintable(ctx + QStringLiteral(": the bytes don't start after the second column")));
            }
            if (lm.lineKind == LineKind::Field && !lm.isArrayElement && !lm.foldHead && scalar) {
                ++plainFields;
                QVERIFY2(n == 2, qPrintable(ctx + QStringLiteral(": a plain field needs both columns")));
                QVERIFY2(t.mid(c.cols.cols[first] + 1, node->name.size()) == node->name,
                         qPrintable(ctx + QStringLiteral(": the name doesn't start after the first column")));
                QVERIFY2(!t.mid(c.cols.cols[first + 1] + 1, 2).trimmed().isEmpty(),
                         qPrintable(ctx + QStringLiteral(": the value doesn't start after the second column")));
            }
            if (compact && n > 0 && lm.lineKind != LineKind::Continuation) {
                const QString type = t.mid(typeStart, c.cols.cols[first] - typeStart).trimmed();
                if (type.size() > kCompactTypeW) {
                    ++overflowRows;
                    QVERIFY2(c.cols.cols[first] == typeStart + type.size(),
                             qPrintable(ctx + QStringLiteral(": an overflowing type must carry its column")));
                }
            }
        }
        QVERIFY(which == 4 || plainFields > 0);

        // Runs: every cell is in exactly one; a run starts and ends on rows
        // that have its cell; between them every row has the cell or is a
        // same-depth row blank there; and no run could have joined a neighbour.
        auto hasCol = [&](int row, int col) {
            for (int k = c.cols.firstCol(row); k < c.cols.endCol(row); ++k)
                if (c.cols.cols[k] == col) return true;
            return false;
        };
        int prevCol = -1;
        for (const TreeColumnRuns& cl : c.cols.byCol) {
            QVERIFY(cl.col > prevCol);
            prevCol = cl.col;
            int prevEnd = -2;
            for (const TreeSpan& r : cl.runs) {
                const QString rctx = QStringLiteral("column %1 run %2..%3").arg(cl.col).arg(r.topLine).arg(r.endLine);
                QVERIFY2(r.topLine > prevEnd + 1 && r.topLine <= r.endLine, qPrintable(rctx + QStringLiteral(": runs overlap or touch")));
                QVERIFY2(hasCol(r.topLine, cl.col) && hasCol(r.endLine, cl.col), qPrintable(rctx + QStringLiteral(": ends off its cell")));
                const int depth = m[r.topLine].depth;
                for (int row = r.topLine + 1; row < r.endLine; ++row) {
                    const QString& s = lines.at(row);
                    const bool blank = cl.col >= s.size() || s.at(cl.col) == QLatin1Char(' ');
                    QVERIFY2(hasCol(row, cl.col) || (m[row].depth == depth && blank),
                             qPrintable(rctx + QStringLiteral(": crosses row %1 |%2|").arg(row).arg(s)));
                }
                if (r.topLine > 0)
                    QVERIFY2(!hasCol(r.topLine - 1, cl.col), qPrintable(rctx + QStringLiteral(": should have joined the row above")));
                if (r.endLine + 1 < m.size())
                    QVERIFY2(!hasCol(r.endLine + 1, cl.col), qPrintable(rctx + QStringLiteral(": should have joined the row below")));
                prevEnd = r.endLine;
            }
        }
        for (int row = 0; row < m.size(); ++row) {
            for (int k = c.cols.firstCol(row); k < c.cols.endCol(row); ++k) {
                int owners = 0;
                for (const TreeColumnRuns& cl : c.cols.byCol)
                    if (cl.col == c.cols.cols[k])
                        for (const TreeSpan& r : cl.runs)
                            if (row >= r.topLine && row <= r.endLine) ++owners;
                QVERIFY2(owners == 1, qPrintable(QStringLiteral("row %1 column %2 is in %3 runs").arg(row).arg(c.cols.cols[k]).arg(owners)));
            }
        }
        const QString counts = QStringLiteral("headers %1 anonymous %2 pointer %3 matrix rows %4 elements %5 collapsed pointers %6")
            .arg(headers).arg(anonymousHeaders).arg(pointerHeaders).arg(continuations).arg(elements).arg(foldPointers);
        if (which == 0 || which == 1)
            QVERIFY2(headers && anonymousHeaders && pointerHeaders && continuations && elements && foldPointers,
                     qPrintable(QStringLiteral("fixture lacks a column row kind: ") + counts));
        if (which == 5)
            QVERIFY2(headers && foldPointers, qPrintable(QStringLiteral("fixture lacks a column row kind: ") + counts));
        if (which == 5 && compact)
            QVERIFY2(overflowRows >= 2, qPrintable(QStringLiteral("only %1 overflowing rows").arg(overflowRows)));
    }

    void matrixRowsAreOneChildNotFour() {
        for (bool lastIsMatrix : {true, false}) {
            const auto fx = treefix::richTree(lastIsMatrix);
            const auto c = composeFixture(fx, fx.rootId, false);
            int continuations = 0;
            for (int i = 0; i < c.cr.meta.size(); ++i) {
                const LineMeta& lm = c.cr.meta[i];
                if (!lm.isContinuation) continue;
                ++continuations;
                QCOMPARE(int(c.g.elbowLevel[i]), -1);
                const QString p = treePrefixAscii(c.g, c.cr.meta, i);
                QVERIFY2(!p.contains(QChar(0x251C)) && !p.contains(QChar(0x2514)),
                         qPrintable(QStringLiteral("continuation row %1 has an arm: [%2]").arg(i).arg(p)));
                // The middle matrix is crossed by the vertical; the last one is not.
                int head = i;
                while (head > 0 && c.cr.meta[head].isContinuation) --head;
                const bool isLast = treePrefixAscii(c.g, c.cr.meta, head).endsWith(QStringLiteral("└ "));
                QCOMPARE(p.at(p.size() - kTreeIndent) == QChar(0x2502), !isLast);
            }
            QVERIFY(continuations >= (lastIsMatrix ? 6 : 3));
        }
    }

    void aBraceRowLeavesNoGapUnderItsHeader() {
        const auto fx = treefix::richTree();
        const auto c = composeFixture(fx, fx.rootId, /*brace=*/true);
        int checked = 0;
        for (int level = 0; level < c.g.byLevel.size(); ++level) {
            for (const TreeSpan& s : c.g.byLevel[level]) {
                // Rows between the parent and the first child are only `{` rows,
                // and the vertical starts at the first child, not below it.
                int above = s.topLine - 1;
                while (above >= 0 && c.cr.meta[above].lineKind == LineKind::Footer
                       && levelKey(c.cr.meta[above]) == level
                       && c.cr.text.split(QLatin1Char('\n')).value(above).trimmed() == QStringLiteral("{"))
                    --above;
                if (above != s.topLine - 1) ++checked;
                QVERIFY(above < 0 || levelKey(c.cr.meta[above]) <= level);
            }
        }
        QVERIFY2(checked > 0, "no brace-wrapped parent in the fixture");
    }

    void copiedTextPutsTheTreeBackInTheIndent() {
        const auto fx = treefix::twoRoots();
        const auto c = composeFixture(fx, 0, false);
        const QString prefix = treePrefixAscii(c.g, c.cr.meta, 4);
        QCOMPARE(prefix, QStringLiteral("│ ├ "));
        QCOMPARE(prefix.size(), c.cr.meta[4].depth * kTreeIndent);
        QVERIFY(treePrefixAscii(c.g, c.cr.meta, 0).isEmpty());
    }

    void strokeKeepsPaceWithTheFont() {
        // JetBrains Mono 12 pt at 96 dpi advances 9.6 px; zoom adds points.
        QCOMPARE(treeStrokeDevPx(1.0, 9.6), 1);
        QCOMPARE(treeStrokeDevPx(1.25, 9.6), 1);
        QCOMPARE(treeStrokeDevPx(1.5, 9.6), 1);
        QCOMPARE(treeStrokeDevPx(1.75, 9.6), 2);
        QCOMPARE(treeStrokeDevPx(2.0, 9.6), 2);
        QCOMPARE(treeStrokeDevPx(1.25, 16.0), 2);    // +8
        QCOMPARE(treeStrokeDevPx(1.25, 25.6), 3);    // +20
        QCOMPARE(treeStrokeDevPx(1.0, 2.4), 1);      // never thinner than a device px
    }

    void geometryIsExactAtEveryScalePhaseAndScroll() {
        const auto fx = treefix::richTree();
        const auto c = composeFixture(fx, fx.rootId, false);
        const TreeGuides& tg = c.g;
        const int lines = tg.lineCount;

        const qreal scales[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
        const qreal phases[] = {0.0, 0.25, 0.5, 0.75};
        const qreal advs[] = {7.0, 8.4, 9.6, 12.8, 19.2, 28.8};
        const int lhExtras[] = {0, 1, 2, 5, 9};
        const QPair<int, int> spacing[] = {qMakePair(4, 2), qMakePair(1, 0)};
        const int firsts[] = {0, 3, 17};
        const QRect everywhere(-1000000, -1000000, 2000000, 2000000);
        const int margin = 41, vpW = 900, vpH = 520;
        int combos = 0;

        for (qreal s : scales)
        for (int ph = 0; ph < 4; ++ph)
        for (qreal adv : advs)
        for (int lhExtra : lhExtras)
        for (const auto& sp : spacing)
        for (int xs = 0; xs < 4; ++xs)
        for (int first : firsts) {
            ++combos;
            TreeGuideGeom g;
            g.sx = g.sy = s;
            g.ox = 37 * s + phases[ph];
            g.oy = 91 * s + phases[(ph + 1) % 4];
            g.ea = sp.first;
            g.ed = sp.second;
            g.lh = qCeil(adv * 1.3) + g.ea + g.ed + lhExtra;
            g.adv = adv;
            g.t = treeStrokeDevPx(s, adv);
            const int xOffset = xs == 3 ? qCeil(adv * 3.5) : xs;
            g.xOrigin = margin - xOffset;
            g.first = first;
            const QString ctx = QStringLiteral("scale %1 phase %2 adv %3 lh %4 ea/ed %5/%6 xOffset %7 first %8")
                .arg(s).arg(phases[ph]).arg(adv).arg(g.lh).arg(g.ea).arg(g.ed).arg(xOffset).arg(first);

            // Structure, unclipped: every span is one rect, every arm one rect.
            g.clipDev = everywhere;
            QVector<QRect> all;
            treeGuideDeviceRects(g, tg, 0, lines - 1, all);
            const int armOff = treegeom::armOffset(g);
            const int band = qFloor(g.sy * g.lh);
            QVERIFY2(armOff >= 0 && armOff + g.t <= qMax(band, g.t), qPrintable(ctx));
            const QVector<RectKey> allKeys = keys(all);
            auto has = [&](const QRect& r) {
                return std::binary_search(allKeys.begin(), allKeys.end(),
                                          RectKey{r.x(), r.y(), r.width(), r.height()});
            };
            int expected = 0;
            for (int level = 0; level < tg.byLevel.size(); ++level) {
                const int x = treegeom::railX(g, level);
                const int col = treegeom::levelCol(level);
                QVERIFY2(x >= qFloor(treegeom::colX(g, col)) && x + g.t <= qCeil(treegeom::colX(g, col + 1)),
                         qPrintable(ctx + QStringLiteral(": rail %1 leaves its cell").arg(level)));
                const int armEnd = treegeom::armEnd(g, level);
                QVERIFY2(armEnd > x + g.t && armEnd <= qFloor(treegeom::colX(g, col + 2)),
                         qPrintable(ctx + QStringLiteral(": arm %1 end %2").arg(level).arg(armEnd)));
                for (const TreeSpan& sp2 : tg.byLevel[level]) {
                    const int top = treegeom::rowTop(g, sp2.topLine);
                    const int bottom = treegeom::rowTop(g, sp2.endLine) + armOff + g.t;
                    const QRect v(x, top, g.t, bottom - top);
                    QVERIFY2(has(v), qPrintable(ctx + QStringLiteral(": vertical %1..%2").arg(sp2.topLine).arg(sp2.endLine)));
                    ++expected;
                    for (int line = sp2.topLine; line <= sp2.endLine; ++line) {
                        if (tg.elbowLevel[line] != level) continue;
                        const int rt = treegeom::rowTop(g, line);
                        const QRect arm(x + g.t, rt + armOff, armEnd - x - g.t, g.t);
                        QVERIFY2(has(arm), qPrintable(ctx + QStringLiteral(": arm row %1").arg(line)));
                        // Inside its own row band, and on rows the vertical covers.
                        QVERIFY2(arm.top() >= rt && arm.bottom() < treegeom::rowTop(g, line + 1),
                                 qPrintable(ctx + QStringLiteral(": arm row %1 leaves its band").arg(line)));
                        QVERIFY2(arm.top() >= v.top() && arm.bottom() <= v.bottom(),
                                 qPrintable(ctx + QStringLiteral(": arm row %1 misses the vertical").arg(line)));
                        ++expected;
                    }
                }
            }
            QVERIFY2(all.size() == expected, qPrintable(ctx + QStringLiteral(": %1 rects, want %2").arg(all.size()).arg(expected)));

            // Tree columns: one t-wide rect per run, unbroken from the top of its
            // first row band to the bottom of its last, centred in its cell.
            QVector<QRect> dashes;
            treeColumnDeviceRects(g, c.cols, 0, lines - 1, dashes);
            {
                QVector<RectKey> want;
                for (const TreeColumnRuns& cl : c.cols.byCol) {
                    const int x = treegeom::cellLineX(g, cl.col);
                    QVERIFY2(x >= qFloor(treegeom::colX(g, cl.col)) && x + g.t <= qCeil(treegeom::colX(g, cl.col + 1)),
                             qPrintable(ctx + QStringLiteral(": column %1 leaves its cell").arg(cl.col)));
                    for (const TreeSpan& r : cl.runs) {
                        const int top = treegeom::rowTop(g, r.topLine);
                        want.append(RectKey{x, top, g.t, treegeom::rowTop(g, r.endLine + 1) - top});
                    }
                }
                std::sort(want.begin(), want.end());
                QVERIFY2(keys(dashes) == want, qPrintable(ctx + QStringLiteral(": column lines differ from their runs")));
            }

            // No two rects share a device pixel (checked on a subset: it is O(n²)).
            if (xs == 0 && first == 0 && sp.first == 4) {
                for (int i = 0; i < all.size(); ++i)
                    for (int j = i + 1; j < all.size(); ++j)
                        QVERIFY2(!all[i].intersects(all[j]), qPrintable(ctx + QStringLiteral(": rects overlap")));
                for (const QRect& d : dashes)
                    for (const QRect& r : all)
                        QVERIFY2(!d.intersects(r), qPrintable(ctx + QStringLiteral(": a column dash touches a tree line")));
            }

            // Clipped to the text area of a viewport, only the visible rows
            // are walked — and nothing is lost by it, nothing lands on the margin.
            g.clipDev = QRect(QPoint(treegeom::snapEdge(g.ox + g.sx * margin), treegeom::snapEdge(g.oy)),
                              QPoint(treegeom::snapEdge(g.ox + g.sx * vpW) - 1,
                                     treegeom::snapEdge(g.oy + g.sy * vpH) - 1));
            QVector<QRect> full, culled;
            treeGuideDeviceRects(g, tg, 0, lines - 1, full);
            treeGuideDeviceRects(g, tg, first, first + (vpH + g.lh - 1) / g.lh, culled);
            QVERIFY2(keys(full) == keys(culled), qPrintable(ctx + QStringLiteral(": culling changed the picture")));
            for (const QRect& r : culled)
                QVERIFY2(r.left() >= g.clipDev.left(), qPrintable(ctx + QStringLiteral(": on the margin")));
            QVector<QRect> fullCols, culledCols;
            treeColumnDeviceRects(g, c.cols, 0, lines - 1, fullCols);
            treeColumnDeviceRects(g, c.cols, first, first + (vpH + g.lh - 1) / g.lh, culledCols);
            QVERIFY2(keys(fullCols) == keys(culledCols), qPrintable(ctx + QStringLiteral(": culling changed the columns")));
            for (const QRect& r : culledCols)
                QVERIFY2(r.left() >= g.clipDev.left(), qPrintable(ctx + QStringLiteral(": a column on the margin")));
        }
        QVERIFY(combos > 10000);
    }

    // Every row with children carries a box, and only those rows: centred in
    // its parent's line cell (column 0 at depth 0), with a blank slot from the
    // box to the row's type text. The fold state is never text.
    void everyRowWithChildrenHasAFoldBox_data() { everyFixtureKeepsTheInvariants_data(); }

    void everyRowWithChildrenHasAFoldBox() {
        QFETCH(int, which);
        QFETCH(bool, brace);
        treefix::Rich fx = which == 0 ? treefix::richTree(true)
                         : which == 1 ? treefix::richTree(false)
                         : which == 2 ? treefix::twoRoots()
                         : which == 3 ? treefix::deepTree(14)
                                      : treefix::unreadableCode();
        const uint64_t viewRoot = which == 2 ? 0 : fx.rootId;
        const auto c = composeFixture(fx, viewRoot, brace);
        const auto& meta = c.cr.meta;
        const FoldBoxes b = buildFoldBoxes(meta);
        QCOMPARE(b.lineCount, int(meta.size()));
        const QStringList lines = c.cr.text.split(QLatin1Char('\n'));
        int boxes = 0, headers = 0, fields = 0, depth0 = 0;
        for (int i = 0; i < meta.size(); ++i) {
            const LineMeta& lm = meta[i];
            const QString ctx = QStringLiteral("row %1 |%2|").arg(i).arg(lines.value(i));
            if (lm.lineKind != LineKind::CommandRow)
                QVERIFY2(!lines.value(i).contains(QChar(0x25B8)) && !lines.value(i).contains(QChar(0x25BE)),
                         qPrintable(ctx + QStringLiteral(": a fold glyph in the text")));
            const bool want = lm.foldHead && lm.lineKind != LineKind::CommandRow;
            QVERIFY2((b.state[i] >= 0) == want, qPrintable(ctx));
            if (!want) continue;
            ++boxes;
            if (lm.lineKind == LineKind::Header) ++headers;
            if (lm.lineKind == LineKind::Field) ++fields;
            if (lm.depth == 0) ++depth0;
            QCOMPARE(int(b.state[i]), lm.foldCollapsed ? 1 : 0);
            QCOMPARE(int(b.col[i]), lm.depth >= 1 ? treegeom::levelCol(lm.depth - 1) : 0);
            if (lm.depth >= 1)
                QVERIFY2(c.g.elbowLevel[i] == lm.depth - 1, qPrintable(ctx + QStringLiteral(": a box off the tree")));
            const ColumnSpan slot = foldSlotFor(lm);
            QVERIFY2(slot.valid && slot.start <= b.col[i] && b.col[i] < slot.end, qPrintable(ctx));
            const QString& text = lines[i];
            QVERIFY2(text.mid(slot.start, slot.end - slot.start).trimmed().isEmpty(),
                     qPrintable(ctx + QStringLiteral(": ink in the box's slot")));
            QVERIFY2(slot.end < text.size() && !text[slot.end].isSpace(),
                     qPrintable(ctx + QStringLiteral(": the slot doesn't reach the type text")));
        }
        QCOMPARE(b.count, boxes);
        if (which == 0) QVERIFY2(headers > 0 && fields >= 2, "rich lacks header or field fold rows (enum, folded pointer)");
        if (which == 2) QVERIFY2(depth0 > 0, "two-roots lacks its depth-0 box");
        if (which != 4) QVERIFY(boxes > 0);
    }

    // Box geometry against the lines, over scale, origin phase, advance, row
    // spacing and scroll: the + stroke is the parent's line, the − stroke the
    // arm, the box stays in its row band, the line breaks exactly around it,
    // the arm starts at its edge, and nothing overlaps.
    void foldBoxesSitOnTheLineAtEveryScalePhaseAndScroll() {
        const treefix::Rich rich = treefix::richTree();
        const treefix::Rich two = treefix::twoRoots();
        const Composed fixtures[] = {composeFixture(rich, rich.rootId, false),
                                     composeFixture(rich, rich.rootId, true),
                                     composeFixture(two, 0, false)};
        const qreal scales[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
        const qreal phases[] = {0.0, 0.25, 0.5, 0.75};
        const qreal advs[] = {3.0, 5.0, 7.0, 8.4, 9.6, 12.8, 19.2, 28.8};
        const int lhExtras[] = {0, 1, 2, 5, 9};
        const QPair<int, int> spacing[] = {qMakePair(4, 2), qMakePair(1, 0)};
        const QRect everywhere(-1000000, -1000000, 2000000, 2000000);
        const int margin = 41, vpW = 900, vpH = 520;
        int combos = 0, boxesChecked = 0;
        auto inked = [](const QVector<QRect>& rs, int x, int y) {
            for (const QRect& r : rs) if (r.contains(x, y)) return true;
            return false;
        };

        for (int fi = 0; fi < 3; ++fi) {
            const Composed& c = fixtures[fi];
            const TreeGuides& tg = c.g;
            const FoldBoxes fb = buildFoldBoxes(c.cr.meta);
            const int lines = tg.lineCount;
            QVERIFY(!fb.isEmpty());
            for (qreal s : scales)
            for (int ph = 0; ph < 4; ++ph)
            for (qreal adv : advs)
            for (int lhExtra : lhExtras)
            for (const auto& sp : spacing)
            for (int first : {0, 5}) {
                ++combos;
                TreeGuideGeom g;
                g.sx = g.sy = s;
                g.ox = 37 * s + phases[ph];
                g.oy = 91 * s + phases[(ph + 1) % 4];
                g.ea = sp.first;
                g.ed = sp.second;
                g.lh = qCeil(adv * 1.3) + g.ea + g.ed + lhExtra;
                g.adv = adv;
                g.t = treeStrokeDevPx(s, adv);
                g.xOrigin = margin;
                g.first = first;
                g.clipDev = everywhere;
                const int t = g.t;
                const QString ctx = QStringLiteral("fixture %1 scale %2 phase %3 adv %4 lh %5 ea/ed %6/%7 first %8")
                    .arg(fi).arg(s).arg(phases[ph]).arg(adv).arg(g.lh).arg(g.ea).arg(g.ed).arg(first);

                const int h = treegeom::foldBoxHalf(g);
                const int side = 2 * h + t;
                const int band = qFloor(g.sy * g.lh);
                QVERIFY2(h >= 1 && side <= qMax(band, t + 2), qPrintable(ctx + QStringLiteral(": side %1 band %2").arg(side).arg(band)));

                QVector<QRect> guides, boxes;
                treeGuideDeviceRects(g, tg, 0, lines - 1, guides, &fb);
                foldBoxDeviceRects(g, fb, 0, lines - 1, boxes);
                const int armOff = treegeom::armOffset(g);
                const QVector<RectKey> guideKeys = keys(guides);
                auto has = [&](const QRect& r) {
                    return std::binary_search(guideKeys.begin(), guideKeys.end(),
                                              RectKey{r.x(), r.y(), r.width(), r.height()});
                };

                for (int line = 0; line < lines; ++line) {
                    if (fb.state[line] < 0) continue;
                    const QString lctx = ctx + QStringLiteral(" row %1").arg(line);
                    const QRect box = treegeom::foldBoxRect(g, line, fb.col[line]);
                    const int rt = treegeom::rowTop(g, line), rb = treegeom::rowTop(g, line + 1);
                    QVERIFY2(box.width() == side && box.height() == side, qPrintable(lctx));
                    QVERIFY2(box.left() + h == treegeom::cellLineX(g, fb.col[line]),
                             qPrintable(lctx + QStringLiteral(": the + stroke is off its column's line")));
                    if (side <= rb - rt)
                        QVERIFY2(box.top() >= rt && box.bottom() < rb, qPrintable(lctx + QStringLiteral(": box leaves its band")));
                    const int centred = rt + armOff - h;
                    if (centred >= rt && centred + side <= rb)
                        QVERIFY2(box.top() == centred, qPrintable(lctx + QStringLiteral(": the − stroke is off the arm")));

                    QVector<QRect> strokes;
                    appendFoldBoxStrokes(g, box, fb.state[line] == 1, strokes);
                    for (int i = 0; i < strokes.size(); ++i) {
                        QVERIFY2(box.contains(strokes[i]), qPrintable(lctx + QStringLiteral(": a stroke outside the box")));
                        for (int j = i + 1; j < strokes.size(); ++j)
                            QVERIFY2(!strokes[i].intersects(strokes[j]), qPrintable(lctx + QStringLiteral(": strokes overlap")));
                    }
                    // The outline is whole; the sign is there once the box has an inside.
                    for (int k = 0; k < side; ++k) {
                        QVERIFY2(inked(strokes, box.left() + k, box.top()) && inked(strokes, box.left() + k, box.bottom())
                                     && inked(strokes, box.left(), box.top() + k) && inked(strokes, box.right(), box.top() + k),
                                 qPrintable(lctx + QStringLiteral(": outline broken at %1").arg(k)));
                    }
                    if (h >= t + 1) {
                        const int cx = box.left() + h, cy = box.top() + h;
                        QVERIFY2(inked(strokes, cx, cy), qPrintable(lctx + QStringLiteral(": no sign")));
                        QVERIFY2(inked(strokes, cx, cy - 1) == (fb.state[line] == 1),
                                 qPrintable(lctx + QStringLiteral(": + / − reads wrong")));
                    }

                    const int level = tg.elbowLevel[line];
                    if (level >= 0) {
                        QVERIFY2(box.left() + h == treegeom::railX(g, level), qPrintable(lctx + QStringLiteral(": box off its parent's line")));
                        const int x1 = treegeom::armEnd(g, level);
                        const int x0 = box.left() + side;
                        if (x1 > x0)
                            QVERIFY2(has(QRect(x0, rt + armOff, x1 - x0, t)), qPrintable(lctx + QStringLiteral(": the arm doesn't start at the box")));
                        for (const QRect& r : guides)
                            QVERIFY2(!(r.left() < box.left() + side && r.right() >= box.left() && r.intersects(box)),
                                     qPrintable(lctx + QStringLiteral(": a line runs into the box")));
                    }
                    ++boxesChecked;
                }

                // Down each line, every device row of a span is the line or a box
                // on it — exactly one (checked on a subset: it is slow).
                if (ph == 0 && sp.first == 4 && first == 0 && (lhExtra == 0 || lhExtra == 9)) {
                    for (int level = 0; level < tg.byLevel.size(); ++level) {
                        const int x = treegeom::railX(g, level);
                        for (const TreeSpan& span : tg.byLevel[level]) {
                            QVector<QRect> onLine;
                            for (int line = span.topLine; line <= span.endLine; ++line)
                                if (fb.state[line] >= 0 && tg.elbowLevel[line] == level)
                                    onLine.append(treegeom::foldBoxRect(g, line, fb.col[line]));
                            const int top = treegeom::rowTop(g, span.topLine);
                            const int bottom = treegeom::rowTop(g, span.endLine) + armOff + t;
                            for (int y = top; y < bottom; ++y) {
                                const int n = int(inked(guides, x, y)) + int(inked(onLine, x, y));
                                QVERIFY2(n == 1, qPrintable(ctx + QStringLiteral(": level %1 span %2..%3 device row %4 covered %5 times")
                                                                 .arg(level).arg(span.topLine).arg(span.endLine).arg(y).arg(n)));
                            }
                            // Nothing of the line past the last child's arm.
                            QVERIFY2(!inked(guides, x, bottom), qPrintable(ctx + QStringLiteral(": a stub below the last child")));
                        }
                    }
                    QVector<QRect> all = guides + boxes;
                    for (int i = 0; i < all.size(); ++i)
                        for (int j = i + 1; j < all.size(); ++j)
                            QVERIFY2(!all[i].intersects(all[j]), qPrintable(ctx + QStringLiteral(": rects overlap")));
                }

                // Clipped to a viewport's text area, culling changes nothing.
                g.clipDev = QRect(QPoint(treegeom::snapEdge(g.ox + g.sx * margin), treegeom::snapEdge(g.oy)),
                                  QPoint(treegeom::snapEdge(g.ox + g.sx * vpW) - 1,
                                         treegeom::snapEdge(g.oy + g.sy * vpH) - 1));
                const int last = first + (vpH + g.lh - 1) / g.lh;
                QVector<QRect> fullG, culledG, fullB, culledB;
                treeGuideDeviceRects(g, tg, 0, lines - 1, fullG, &fb);
                treeGuideDeviceRects(g, tg, first, last, culledG, &fb);
                foldBoxDeviceRects(g, fb, 0, lines - 1, fullB);
                foldBoxDeviceRects(g, fb, first, last, culledB);
                QVERIFY2(keys(fullG) == keys(culledG), qPrintable(ctx + QStringLiteral(": culling changed the lines")));
                QVERIFY2(keys(fullB) == keys(culledB), qPrintable(ctx + QStringLiteral(": culling changed the boxes")));
                for (const QRect& r : culledB)
                    QVERIFY2(g.clipDev.contains(r), qPrintable(ctx + QStringLiteral(": a box stroke outside the text area")));
            }
        }
        QVERIFY(combos > 5000);
        QVERIFY(boxesChecked > 10000);
    }
};

QTEST_GUILESS_MAIN(TestTreeGuides)
#include "test_tree_guides.moc"
