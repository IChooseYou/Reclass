// Visual regression test for the doc-tab source icon.
// Renders the icon into a known QImage, samples pixels at the
// expected location, and asserts:
//   1. Pixels exist at iconRect (x=8..22, vertical center band).
//   2. Color matches the requested tint (selected=white, unselected=dim).
//   3. Live=false dims the alpha to ~40%.
//   4. Two different icon paths produce visually different pixmaps
//      (proves changing the source-icon path changes the render).
//
// Uses the SAME drawTabSourceIcon helper that the live MenuBarStyle
// calls — so what passes here is exactly what the live tab paints.

#include <QtTest/QTest>
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QColor>
#include <cstdio>

#include "tab_source_icon.h"

#define LOG(...) do { std::fprintf(stdout, __VA_ARGS__); std::fflush(stdout); } while (0)

class TestTabSourceIcon : public QObject {
    Q_OBJECT
private slots:
    void rendersAtCorrectLocationAndSize();
    void tintMatchesRequestedColor();
    void liveFalseDimsOpacity();
    void differentIconPathsRenderDifferently();
    void iconAndTextBaselinesAlignInLiveTabContext();
};

static QImage renderIcon(const QString& iconPath, bool live, const QColor& tint,
                         const QRect& iconRect, int canvasW = 80, int canvasH = 30)
{
    QImage img(canvasW, canvasH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    rcx::drawTabSourceIcon(&p, iconRect, iconPath, live, tint);
    p.end();
    return img;
}

// Average UN-PREMULTIPLIED pixel color across the icon rect.
// Format_ARGB32_Premultiplied stores RGB as RGB*A/255, so to compare
// to the requested tint we divide each pixel's RGB by its alpha.
static QColor avgColor(const QImage& im, const QRect& r) {
    QImage straight = im.convertToFormat(QImage::Format_ARGB32);
    long sumR = 0, sumG = 0, sumB = 0, sumA = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < straight.height(); ++y) {
        for (int x = r.left(); x <= r.right() && x < straight.width(); ++x) {
            QRgb px = straight.pixel(x, y);
            int a = qAlpha(px);
            if (a == 0) continue;
            sumR += qRed(px); sumG += qGreen(px); sumB += qBlue(px); sumA += a;
            ++n;
        }
    }
    if (!n) return QColor();
    return QColor(int(sumR/n), int(sumG/n), int(sumB/n), int(sumA/n));
}

static double avgAlpha(const QImage& im, const QRect& r) {
    long sum = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < im.height(); ++y) {
        for (int x = r.left(); x <= r.right() && x < im.width(); ++x) {
            sum += qAlpha(im.pixel(x, y));
            ++n;
        }
    }
    return n ? double(sum) / n : 0.0;
}

void TestTabSourceIcon::rendersAtCorrectLocationAndSize()
{
    QRect iconRect(8, 8, 14, 14);
    QImage img = renderIcon(":/vsicons/file-binary.svg", true, Qt::white, iconRect);

    // Inside iconRect: should have pixels.
    double aIn = avgAlpha(img, iconRect);
    // Outside iconRect (right of icon, before text region): should be transparent.
    QRect outside(30, 8, 14, 14);
    double aOut = avgAlpha(img, outside);

    LOG("\nrendersAtCorrectLocationAndSize:\n");
    LOG("  inside iconRect avg alpha: %.1f\n", aIn);
    LOG("  outside iconRect avg alpha: %.1f\n", aOut);

    QVERIFY2(aIn > 10.0,
        qPrintable(QString("icon area is empty (avg alpha %1)").arg(aIn)));
    QVERIFY2(aOut < 5.0,
        qPrintable(QString("icon bled outside its rect (avg alpha %1)").arg(aOut)));
}

void TestTabSourceIcon::tintMatchesRequestedColor()
{
    QRect iconRect(8, 8, 14, 14);

    QImage selected = renderIcon(":/vsicons/file-binary.svg", true, QColor(240, 240, 240), iconRect);
    QImage dim      = renderIcon(":/vsicons/file-binary.svg", true, QColor(120, 120, 120), iconRect);

    QColor cSelected = avgColor(selected, iconRect);
    QColor cDim      = avgColor(dim, iconRect);

    LOG("\ntintMatchesRequestedColor:\n");
    LOG("  selected (req 240,240,240): rendered (%d,%d,%d)\n",
        cSelected.red(), cSelected.green(), cSelected.blue());
    LOG("  unselected (req 120,120,120): rendered (%d,%d,%d)\n",
        cDim.red(), cDim.green(), cDim.blue());

    // Selected variant should be brighter (closer to 240).
    QVERIFY2(cSelected.red() > cDim.red() + 50,
        qPrintable(QString("selected not brighter — sel.r=%1 dim.r=%2")
                   .arg(cSelected.red()).arg(cDim.red())));
    // Selected color near requested tint.
    QVERIFY2(qAbs(cSelected.red() - 240) < 20,
        qPrintable(QString("selected red %1 not near 240").arg(cSelected.red())));
    QVERIFY2(qAbs(cDim.red() - 120) < 20,
        qPrintable(QString("dim red %1 not near 120").arg(cDim.red())));
}

void TestTabSourceIcon::liveFalseDimsOpacity()
{
    QRect iconRect(8, 8, 14, 14);

    QImage liveImg = renderIcon(":/vsicons/file-binary.svg", true, Qt::white, iconRect);
    QImage deadImg = renderIcon(":/vsicons/file-binary.svg", false, Qt::white, iconRect);

    double aLive = avgAlpha(liveImg, iconRect);
    double aDead = avgAlpha(deadImg, iconRect);

    LOG("\nliveFalseDimsOpacity:\n");
    LOG("  live alpha:%.1f, dead alpha:%.1f, ratio:%.2f\n", aLive, aDead, aDead / qMax(1.0, aLive));

    QVERIFY2(aDead < aLive * 0.6,
        qPrintable(QString("muted state didn't dim opacity: live=%1 dead=%2")
                   .arg(aLive).arg(aDead)));
}

void TestTabSourceIcon::differentIconPathsRenderDifferently()
{
    QRect iconRect(8, 8, 14, 14);

    QImage iA = renderIcon(":/vsicons/file-binary.svg", true, Qt::white, iconRect);
    QImage iB = renderIcon(":/vsicons/server-process.svg", true, Qt::white, iconRect);

    int diffPixels = 0;
    for (int y = iconRect.top(); y <= iconRect.bottom(); ++y) {
        for (int x = iconRect.left(); x <= iconRect.right(); ++x) {
            if (iA.pixel(x, y) != iB.pixel(x, y)) ++diffPixels;
        }
    }
    int totalPixels = iconRect.width() * iconRect.height();

    LOG("\ndifferentIconPathsRenderDifferently:\n");
    LOG("  diff pixels: %d / %d (%.1f%%)\n",
        diffPixels, totalPixels, 100.0 * diffPixels / totalPixels);

    QVERIFY2(diffPixels > totalPixels / 5,
        qPrintable(QString("two different SVGs rendered too similarly: %1/%2 differ")
                   .arg(diffPixels).arg(totalPixels)));
}

// Reproduce the EXACT live tab geometry, render icon + text together,
// save the result to a PNG so the user can inspect, AND analyze the
// vertical centers of the icon vs text glyphs to report the delta.
// If the icon and text are visually misaligned, this tells us by
// exactly how many pixels and in which direction.
void TestTabSourceIcon::iconAndTextBaselinesAlignInLiveTabContext()
{
    // Reproduce the live tab cell: 37px tall, with a 2px top accent
    // strip and 1px bottom border (matches the sentinel "+" tab math
    // in MenuBarStyle::drawControl). Centering is done in the content
    // area between those bands — same as live.
    QRect tabRect(0, 0, 200, 37);

    QImage img(tabRect.width(), tabRect.height(), QImage::Format_ARGB32);
    img.fill(QColor(40, 40, 45));  // dark tab bg
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // Top accent strip (2px) + bottom border (1px) — match live look.
    p.fillRect(QRect(tabRect.left(), tabRect.top(), tabRect.width(), 2),
               QColor(80, 130, 200));     // accent
    p.fillRect(QRect(tabRect.left(), tabRect.bottom(), tabRect.width(), 1),
               QColor(20, 20, 25));       // bottom border

    // Same font as live: JetBrains Mono 10pt, fixed pitch.
    QFont f("JetBrains Mono", 10);
    f.setFixedPitch(true);
    p.setFont(f);
    QFontMetrics fm(f);

    // Same geometry as the live MenuBarStyle (main.cpp ~680). Visible
    // content area excludes the accent + border.
    const int kIconSz  = fm.height();
    const int kIconPad = 8;
    const int kIconGap = 6;
    QRect contentArea = tabRect.adjusted(0, 2, 0, -1);
    int iy = contentArea.top()
             + (contentArea.height() - kIconSz) / 2;
    QRect iconRect(tabRect.left() + kIconPad, iy, kIconSz, kIconSz);

    // Render icon (selected state — full opacity).
    rcx::drawTabSourceIcon(&p, iconRect, ":/vsicons/file-binary.svg",
                            true, QColor(220, 220, 220));

    // Text Y centering matches: same +2/-1 inset.
    int leftInset = kIconPad + kIconSz + kIconGap;
    QRect textRect = tabRect.adjusted(leftInset, 2, -8, -1);
    p.setPen(QColor(220, 220, 220));
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               "UnnamedClass0");

    // Diagnostic borders so the user can SEE the geometry:
    //   red    = iconRect
    //   magenta = textRect (the rect Qt centers the text inside)
    //   green  = visible content-area centerline (where everything
    //            should align vertically)
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::red, 1));
    p.drawRect(iconRect);
    p.setPen(QPen(QColor(255, 0, 255), 1));
    p.drawRect(textRect.adjusted(0, 0, -1, -1));
    p.setPen(QPen(Qt::green, 1));
    int contentMid = contentArea.center().y();
    p.drawLine(0, contentMid, tabRect.width(), contentMid);
    p.end();

    // Save PNG so user can inspect
    QString outPath = "tab_icon_capture.png";
    img.save(outPath);
    LOG("\nSaved capture: %s\n", outPath.toUtf8().constData());

    // SECOND capture: selected/unselected/dead row so the user can
    // visually confirm the icon dims when the tab is unselected.
    QImage row(640, 37, QImage::Format_ARGB32);
    row.fill(QColor(40, 40, 45));
    QPainter rp(&row);
    rp.setRenderHint(QPainter::Antialiasing, true);
    rp.setRenderHint(QPainter::TextAntialiasing, true);
    rp.setFont(f);

    auto drawTab = [&](int x0, const QString& label, bool selected, bool live) {
        QRect cell(x0, 0, 200, 37);
        rp.fillRect(cell, selected ? QColor(60, 60, 70) : QColor(40, 40, 45));
        // Accent + border for realism
        if (selected)
            rp.fillRect(QRect(cell.left(), cell.top(), cell.width(), 2),
                        QColor(80, 130, 200));
        rp.fillRect(QRect(cell.left(), cell.bottom(), cell.width(), 1),
                    QColor(20, 20, 25));
        QColor tint = selected ? QColor(240, 240, 240) : QColor(150, 150, 150);
        QRect ca = cell.adjusted(0, 2, 0, -1);
        QRect ir(cell.left() + kIconPad,
                 ca.top() + (ca.height() - kIconSz) / 2,
                 kIconSz, kIconSz);
        qreal stateOp = selected ? 1.0 : 0.70;
        qreal prevOp = rp.opacity();
        rp.setOpacity(prevOp * stateOp);
        rcx::drawTabSourceIcon(&rp, ir, ":/vsicons/file-binary.svg", live, tint);
        rp.setOpacity(prevOp);
        QRect tr = cell.adjusted(kIconPad + kIconSz + kIconGap, 2, -8, -1);
        rp.setPen(tint);
        rp.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, label);
    };
    drawTab(  0, "selected",     true,  true);
    drawTab(210, "unselected",   false, true);
    drawTab(420, "disconnected", false, false);
    rp.end();
    QString rowPath = "tab_states_capture.png";
    row.save(rowPath);
    LOG("Saved states capture: %s\n", rowPath.toUtf8().constData());

    // Find vertical bounds (top/bottom Y) of NON-BG pixels in the icon
    // column and text column.
    auto findVRange = [&](int x0, int x1) -> QPair<int, int> {
        int top = -1, bot = -1;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = x0; x < x1 && x < img.width(); ++x) {
                QRgb px = img.pixel(x, y);
                int dR = qAbs(qRed(px)   - 40);
                int dG = qAbs(qGreen(px) - 40);
                int dB = qAbs(qBlue(px)  - 45);
                if (dR + dG + dB > 30) {  // significantly different from bg
                    if (top < 0) top = y;
                    bot = y;
                    break;
                }
            }
        }
        return qMakePair(top, bot);
    };

    auto iconV = findVRange(iconRect.left(), iconRect.right() + 1);
    auto textV = findVRange(textRect.left(), textRect.left() + 80);

    int iconCenter = (iconV.first + iconV.second) / 2;
    int textCenter = (textV.first + textV.second) / 2;
    int delta = iconCenter - textCenter;

    LOG("Tab rect: %dx%d\n", tabRect.width(), tabRect.height());
    LOG("Icon Y range: [%d..%d] center=%d  height=%d\n",
        iconV.first, iconV.second, iconCenter, iconV.second - iconV.first + 1);
    LOG("Text Y range: [%d..%d] center=%d  height=%d\n",
        textV.first, textV.second, textCenter, textV.second - textV.first + 1);
    LOG("Vertical center delta (icon-text): %d px\n", delta);
    LOG("Icon X start: %d  Text X start: %d (gap %d)\n",
        iconRect.left(), textRect.left(),
        textRect.left() - iconRect.right());

    // Hard rule: vertical centers must agree within 1 px.
    QVERIFY2(qAbs(delta) <= 1,
        qPrintable(QString("icon and text vertical centers off by %1 px "
                           "(icon center=%2, text center=%3) — see %4")
                   .arg(delta).arg(iconCenter).arg(textCenter).arg(outPath)));
}

QTEST_MAIN(TestTabSourceIcon)
#include "test_tab_source_icon.moc"
