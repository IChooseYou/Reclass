#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QTextEdit>
#include <QImage>
#include <QPainter>
#include <QHoverEvent>
#include <QStyle>
#include <QCursor>
#include <QScreen>
#include <QEnterEvent>
#include "titlebar.h"
#include "themes/thememanager.h"
#include "svgicon.h"

class TestTitleBarBorder : public QObject {
    Q_OBJECT
private slots:

    // Regression: themedVsIcon must rasterize the glyph across the WHOLE pixmap
    // at dpr != 1, not just the top-left logical-size corner. The bug (set
    // devicePixelRatio AFTER attaching the painter, and render into size/dpr)
    // left the glyph cropped to the top-left quadrant at HiDPI — which read as
    // title-bar / dock-tab icons "too high" and small. Deterministic + headless.
    void testThemedIconFillsPixmapAtHighDpr() {
        const int logical = 20;
        const qreal dpr = 2.0;
        // chrome-close (an X) spans almost the full 16-unit viewBox, so a
        // correctly-rendered icon reaches well into the bottom-right of the
        // device pixmap; a cropped one stays inside the top-left logical×logical.
        QIcon icon = rcx::themedVsIcon(QStringLiteral(":/vsicons/chrome-close.svg"),
                                       Qt::white, logical, dpr);
        QPixmap pm = icon.pixmap(QSize(logical, logical), dpr);
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        const int W = img.width(), H = img.height();
        // dpr=2 → device pixmap is 40×40.
        QVERIFY2(W == logical * dpr && H == logical * dpr,
                 qPrintable(QString("expected %1x%1 device px, got %2x%3")
                            .arg((int)(logical * dpr)).arg(W).arg(H)));

        // Bounding box of non-transparent (glyph) pixels.
        int minX = W, minY = H, maxX = -1, maxY = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (qAlpha(img.pixel(x, y)) > 30) {
                    minX = qMin(minX, x); maxX = qMax(maxX, x);
                    minY = qMin(minY, y); maxY = qMax(maxY, y);
                }
        QVERIFY2(maxX >= 0, "icon rendered fully transparent (resource missing?)");

        // The decisive check: the glyph must extend PAST the top-left quadrant
        // boundary (logical, = W/2 here). Under the old bug every lit pixel sat
        // within the top-left logical×logical corner, so maxX/maxY <= ~logical.
        QVERIFY2(maxX > logical && maxY > logical,
                 qPrintable(QString("glyph cropped to top-left: bbox=(%1,%2)-(%3,%4) "
                            "in %5x%6 — expected it to extend past (%7,%7)")
                            .arg(minX).arg(minY).arg(maxX).arg(maxY).arg(W).arg(H).arg(logical)));

        // And it should be roughly centered (bbox center near pixmap center),
        // not biased to a corner.
        const double cx = (minX + maxX) / 2.0, cy = (minY + maxY) / 2.0;
        QVERIFY2(qAbs(cx - W / 2.0) <= W * 0.18 && qAbs(cy - H / 2.0) <= H * 0.18,
                 qPrintable(QString("glyph off-center: center=(%1,%2) pixmap=%3x%4")
                            .arg(cx).arg(cy).arg(W).arg(H)));
    }

    // Diagnose and verify: close button red hover does NOT bleed below the title bar
    void testCloseButtonHoverNoBleed() {
        const auto& theme = rcx::ThemeManager::instance().current();
        QColor redHover = theme.indHeatHot;
        QColor bgColor = theme.background;

        // Apply dark palette
        QPalette pal;
        pal.setColor(QPalette::Window, bgColor);
        pal.setColor(QPalette::WindowText, theme.text);
        pal.setColor(QPalette::Base, bgColor);
        pal.setColor(QPalette::Text, theme.text);
        QApplication::setPalette(pal);

        // Build window matching real app layout
        QMainWindow win;
        win.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
        win.resize(600, 400);

        auto* titleBar = new rcx::TitleBarWidget(&win);
        titleBar->applyTheme(theme);
        win.setMenuWidget(titleBar);

        // Add a dock widget (content below title bar)
        auto* dock = new QDockWidget("Test", &win);
        auto* emptyTitle = new QWidget(dock);
        emptyTitle->setFixedHeight(0);
        dock->setTitleBarWidget(emptyTitle);
        auto* body = new QWidget(dock);
        body->setAutoFillBackground(true);
        QPalette bodyPal;
        bodyPal.setColor(QPalette::Window, bgColor);
        body->setPalette(bodyPal);
        dock->setWidget(body);
        win.addDockWidget(Qt::TopDockWidgetArea, dock);

        win.show();
        QVERIFY(QTest::qWaitForWindowExposed(&win));
        QApplication::processEvents();

        // Find the close button position in window coordinates
        QToolButton* closeBtn = nullptr;
        for (auto* btn : titleBar->findChildren<QToolButton*>()) {
            if (btn->toolTip().contains("close", Qt::CaseInsensitive) ||
                btn->icon().name().contains("close", Qt::CaseInsensitive)) {
                closeBtn = btn;
                break;
            }
        }
        // Fallback: last QToolButton in title bar is typically close
        if (!closeBtn) {
            auto btns = titleBar->findChildren<QToolButton*>();
            if (!btns.isEmpty()) closeBtn = btns.last();
        }
        QVERIFY2(closeBtn, "Could not find close button in title bar");

        // Log geometry
        QPoint btnTopLeft = closeBtn->mapTo(&win, QPoint(0, 0));
        int btnBottom = btnTopLeft.y() + closeBtn->height();
        int titleBarBottom = titleBar->mapTo(&win, QPoint(0, titleBar->height())).y();
        qDebug() << "Title bar height:" << titleBar->height()
                 << "Title bar bottom (in win):" << titleBarBottom;
        qDebug() << "Close button geometry (in win):"
                 << QRect(btnTopLeft, closeBtn->size())
                 << "button bottom:" << btnBottom;
        qDebug() << "Red hover color:" << redHover.name();

        // Force the close button to render in hovered state
        // (WA_UnderMouse + enterEvent triggers the custom ChromeButton paintEvent)
        closeBtn->setAttribute(Qt::WA_UnderMouse, true);
        QEnterEvent enterEv(QPointF(8,8), QPointF(8,8), QPointF(8,8));
        QApplication::sendEvent(closeBtn, &enterEv);
        closeBtn->update();
        QApplication::processEvents();
        QTest::qWait(100);
        QApplication::processEvents();

        QPixmap px = win.grab();
        QImage img = px.toImage().convertToFormat(QImage::Format_ARGB32);
        img.save("titlebar_hover_test.png");

        int W = img.width();
        int H = img.height();
        qreal dpr = px.devicePixelRatio();
        qDebug() << "Image size:" << W << "x" << H << "DPR:" << dpr;

        // Helper: check if pixel is red-ish (close to indHeatHot)
        auto isRed = [&](int x, int y) -> bool {
            if (x < 0 || x >= W || y < 0 || y >= H) return false;
            QColor c(img.pixel(x, y));
            return c.red() > 150 && c.green() < 100 && c.blue() < 100;
        };

        // Scale logical coords to device pixels
        int btnMidX = (int)((btnTopLeft.x() + closeBtn->width() / 2) * dpr);
        titleBarBottom = (int)(titleBarBottom * dpr);
        btnBottom = (int)(btnBottom * dpr);
        qDebug() << "=== Vertical scan at x =" << btnMidX << "===";
        for (int y = qMax(0, titleBarBottom - 5); y <= qMin(img.height()-1, titleBarBottom + 5); y++) {
            QColor c(img.pixel(btnMidX, y));
            qDebug() << QString("  pixel(%1,%2) = %3 R%4G%5B%6 %7 %8")
                .arg(btnMidX).arg(y).arg(c.name())
                .arg(c.red()).arg(c.green()).arg(c.blue())
                .arg(isRed(btnMidX, y) ? "RED!" : "")
                .arg(y == titleBarBottom ? "<-- titlebar bottom" : "");
        }

        // THE ASSERTION: no red pixels at or below titleBarBottom
        bool bleedFound = false;
        int bleedY = -1;
        int btnLeftPx = (int)(btnTopLeft.x() * dpr);
        int btnRightPx = (int)((btnTopLeft.x() + closeBtn->width()) * dpr);
        for (int y = titleBarBottom; y <= titleBarBottom + 3; y++) {
            for (int x = btnLeftPx; x < btnRightPx; x++) {
                if (isRed(x, y)) {
                    bleedFound = true;
                    bleedY = y;
                    break;
                }
            }
            if (bleedFound) break;
        }

        // Save annotated diagnostic
        {
            QImage diag = img.copy();
            QPainter dp(&diag);
            dp.setPen(QPen(Qt::cyan, 1));
            dp.drawLine(0, titleBarBottom, W-1, titleBarBottom);
            dp.setPen(QPen(Qt::yellow, 1));
            dp.drawLine(0, btnBottom, W-1, btnBottom);
            dp.end();
            diag.save("titlebar_hover_annotated.png");
        }

        QVERIFY2(!bleedFound,
            qPrintable(QString("Red hover bleeds below title bar at y=%1 "
                "(titleBar bottom=%2, button bottom=%3). "
                "See titlebar_hover_annotated.png")
                .arg(bleedY).arg(titleBarBottom).arg(btnBottom)));

        qDebug() << "PASS: No red bleed below title bar";
    }
};

QTEST_MAIN(TestTitleBarBorder)
#include "test_titlebar_border.moc"
