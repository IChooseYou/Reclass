// Pixel-level diagnostic for the "double line at top of RcxEditor border"
// complaint. Builds a minimal replica of the production widget hierarchy
// (QMainWindow → top-area QDockWidget → QTabBar → pane.QTabWidget →
// editorContainer with QSS border → QsciScintilla-like inner widget),
// renders the window to a QImage, then scans the y-axis through a
// column that crosses the editor border to locate every horizontal
// "line" (row where the pixel differs from its neighbours).
//
// Reports the y-coordinate, color, and 1-px / 2-px / etc. thickness of
// each detected line, so we can pinpoint which paint paths leak through
// even when QSS / proxy-style overrides claim to suppress them.

#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSplitter>
#include <QStyleFactory>
#include <QFile>
#include <QTextStream>

// Theme colors are hardcoded to the XP Luna palette so we don't pull
// in the full ThemeManager (which has more dependencies than this
// pixel-scan test needs).
struct Theme {
    QColor background     = QColor("#D4D0C8");
    QColor border         = QColor("#5E5C50");
    QColor textMuted      = QColor("#353535");
};

static QColor pixelAt(const QImage& img, int x, int y) {
    return img.pixelColor(x, y);
}

class TestEditorBorder : public QObject {
    Q_OBJECT
private slots:
    void scanTopEdgeOfEditorContainer() {
        // Force a light theme so the bug reproduces (gray chrome + white
        // editor + dark border == three high-contrast bands).
        Theme t;
        // Push the chrome color into the app palette so every widget
        // inherits it the way the production MainWindow does.
        QPalette pal = qApp->palette();
        pal.setColor(QPalette::Window, t.background);
        pal.setColor(QPalette::WindowText, t.textMuted);
        pal.setColor(QPalette::Dark, t.border);
        qApp->setPalette(pal);

        QMainWindow window;
        window.resize(800, 400);

        // Doc dock with a single tab — mirrors the real Reclass setup,
        // including setTitleBarWidget(empty) which production uses to
        // suppress the default Fusion dock title bar paint.
        auto* dock = new QDockWidget(QStringLiteral("UnnamedClass0"), &window);
        dock->setFeatures(QDockWidget::DockWidgetClosable
                        | QDockWidget::DockWidgetMovable);
        auto* emptyTitleBar = new QWidget(dock);
        emptyTitleBar->setFixedHeight(0);
        dock->setTitleBarWidget(emptyTitleBar);

        // Add a second dock so Qt creates a real doc-tab bar (matches
        // production where multiple docs are tabified).
        auto* dock2 = new QDockWidget(QStringLiteral("Scratch"), &window);
        auto* etb2 = new QWidget(dock2);
        etb2->setFixedHeight(0);
        dock2->setTitleBarWidget(etb2);
        dock2->setWidget(new QLabel(QStringLiteral("scratch")));
        window.addDockWidget(Qt::TopDockWidgetArea, dock);
        window.addDockWidget(Qt::TopDockWidgetArea, dock2);
        window.tabifyDockWidget(dock, dock2);
        dock->raise();

        // Splitter (vertical) → pane QTabWidget (South tabs) →
        // editorContainer (border) → inner editor (white).
        auto* splitter = new QSplitter(Qt::Vertical);
        splitter->setHandleWidth(1);

        auto* tabWidget = new QTabWidget;
        tabWidget->setTabPosition(QTabWidget::South);
        tabWidget->setStyleSheet(QStringLiteral(
            "QTabWidget::pane { border: none; }"
            "QTabBar { border: none; }"
            "QTabBar::tab { background: %1; color: %2; padding: 0 16px;"
            "              border: none; border-radius: 0; height: 26px; }")
            .arg(t.background.name(), t.textMuted.name()));

        auto* editorContainer = new QWidget;
        editorContainer->setObjectName(QStringLiteral("rcxEditorContainer"));
        editorContainer->setAttribute(Qt::WA_StyledBackground, true);
        editorContainer->setStyleSheet(QStringLiteral(
            "#rcxEditorContainer { border: 1px solid %1; }").arg(t.border.name()));
        auto* ecLay = new QHBoxLayout(editorContainer);
        ecLay->setContentsMargins(1, 1, 1, 1);
        ecLay->setSpacing(0);

        // Stand-in for the real editor: a plain white widget so border
        // contrast is maximal.
        auto* fakeEditor = new QWidget;
        QPalette ep = fakeEditor->palette();
        ep.setColor(QPalette::Window, QColor(QStringLiteral("#FFFFFF")));
        fakeEditor->setPalette(ep);
        fakeEditor->setAutoFillBackground(true);
        ecLay->addWidget(fakeEditor, 1);

        tabWidget->addTab(editorContainer, QStringLiteral("Reclass"));
        tabWidget->addTab(new QWidget, QStringLiteral("Code"));
        tabWidget->addTab(new QWidget, QStringLiteral("Debug"));

        splitter->addWidget(tabWidget);
        dock->setWidget(splitter);

        // Render
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(120);  // let style finish

        QImage img(window.size(), QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        window.render(&img);

        // Find the editorContainer's global rect (relative to window).
        QPoint ecTopLeft = editorContainer->mapTo(&window, QPoint(0, 0));
        int xProbe = ecTopLeft.x() + editorContainer->width() / 2;

        // Write findings to a file so they're inspectable (QtTest swallows
        // stdout under some configurations).
        QFile out(QStringLiteral("test_editor_border.out"));
        out.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QTextStream ts(&out);
        ts << "editorContainer top-left in window: ("
           << ecTopLeft.x() << ", " << ecTopLeft.y() << ")\n"
           << "editorContainer size: " << editorContainer->width() << " x "
           << editorContainer->height() << "\n"
           << "probe column x = " << xProbe << "\n\n";

        // Scan a vertical column through the editorContainer's top edge
        // — from 20 px above the container's top to 20 px below.
        int yStart = qMax(0, ecTopLeft.y() - 20);
        int yEnd   = qMin(img.height() - 1, ecTopLeft.y() + 20);

        QColor prev;
        for (int y = yStart; y <= yEnd; ++y) {
            QColor c = pixelAt(img, xProbe, y);
            QString tag = (y == ecTopLeft.y())
                ? QStringLiteral(" <-- editorContainer.y=0")
                : QString();
            QString diffTag;
            if (prev.isValid() && c != prev)
                diffTag = QStringLiteral(" * transition from ")
                          + prev.name(QColor::HexArgb);
            ts << QString("  y=%1  rgb=%2%3%4\n")
                  .arg(y, 3).arg(c.name(QColor::HexArgb)).arg(tag).arg(diffTag);
            prev = c;
        }
        ts.flush();
        out.close();

        // Save the rendered window so we can inspect it visually too.
        img.save(QStringLiteral("test_editor_border_capture.png"));
    }
};

QTEST_MAIN(TestEditorBorder)
#include "test_editor_border.moc"
