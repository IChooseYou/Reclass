#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QTabWidget>
#include <QTextEdit>

// Replicates the real app layout: QTabWidget central widget, project dock in LeftDockWidgetArea.

class TestProjectDock : public QObject {
    Q_OBJECT
private:
    struct AppLayout {
        QMainWindow* win;
        QTabWidget* tabs;
        QDockWidget* project;
    };

    AppLayout buildApp() {
        auto* win = new QMainWindow;
        win->resize(1280, 800);

        // QTabWidget as central widget — same as real app
        auto* tabs = new QTabWidget(win);
        tabs->setTabsClosable(true);
        tabs->setMovable(true);
        tabs->setDocumentMode(true);
        tabs->addTab(new QTextEdit(tabs), "Untitled");
        win->setCentralWidget(tabs);

        // Project dock — same as real app
        auto* project = new QDockWidget("Project", win);
        project->setObjectName("WorkspaceDock");
        project->setAllowedAreas(Qt::AllDockWidgetAreas);
        project->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
        project->setWidget(new QTextEdit(project));
        win->addDockWidget(Qt::LeftDockWidgetArea, project);
        project->hide();

        return {win, tabs, project};
    }

    void showProject(AppLayout& a) {
        if (a.project->isHidden() && !a.project->isFloating()) {
            a.win->addDockWidget(Qt::LeftDockWidgetArea, a.project);
            a.project->show();
            a.win->resizeDocks({a.project}, {qMax(200, a.win->width() / 5)}, Qt::Horizontal);
        } else {
            a.project->show();
        }
    }

private slots:
    void dockStartsLeft();
    void dockWidthIsReasonable();
    void dockStaysLeftAfterHideShow();
    void dockRespectsDragAfterShow();
};

void TestProjectDock::dockStartsLeft()
{
    auto app = buildApp();
    app.win->show();
    QTest::qWaitForWindowExposed(app.win);

    showProject(app);
    QApplication::processEvents();

    // Project should be to the left of the central tab widget
    QVERIFY2(app.project->x() < app.tabs->x(),
             qPrintable(QString("Project x=%1, Tabs x=%2")
                        .arg(app.project->x()).arg(app.tabs->x())));
    delete app.win;
}

void TestProjectDock::dockWidthIsReasonable()
{
    auto app = buildApp();
    app.win->show();
    QTest::qWaitForWindowExposed(app.win);

    showProject(app);
    QApplication::processEvents();

    int dockWidth = app.project->width();
    int winWidth = app.win->width();
    double ratio = (double)dockWidth / winWidth;

    qDebug() << "Dock width:" << dockWidth << "Window width:" << winWidth
             << "Ratio:" << QString::number(ratio * 100, 'f', 1) + "%";

    QVERIFY2(ratio < 0.40,
             qPrintable(QString("Dock too wide: %1% of window").arg(ratio * 100, 0, 'f', 1)));
    QVERIFY2(ratio > 0.10,
             qPrintable(QString("Dock too narrow: %1% of window").arg(ratio * 100, 0, 'f', 1)));
    delete app.win;
}

void TestProjectDock::dockStaysLeftAfterHideShow()
{
    auto app = buildApp();
    app.win->show();
    QTest::qWaitForWindowExposed(app.win);

    showProject(app);
    QApplication::processEvents();
    QVERIFY(app.project->x() < app.tabs->x());

    app.project->hide();
    QApplication::processEvents();

    showProject(app);
    QApplication::processEvents();
    QVERIFY2(app.project->x() < app.tabs->x(),
             qPrintable(QString("After re-show: Project x=%1, Tabs x=%2")
                        .arg(app.project->x()).arg(app.tabs->x())));
    delete app.win;
}

void TestProjectDock::dockRespectsDragAfterShow()
{
    auto app = buildApp();
    app.win->show();
    QTest::qWaitForWindowExposed(app.win);

    showProject(app);
    QApplication::processEvents();
    QVERIFY(app.project->x() < app.tabs->x());

    // Simulate user dragging to right
    app.win->addDockWidget(Qt::RightDockWidgetArea, app.project);
    QApplication::processEvents();
    QCOMPARE(app.win->dockWidgetArea(app.project), Qt::RightDockWidgetArea);

    // Dock is visible — showProject should NOT force it back to left
    showProject(app);
    QApplication::processEvents();
    QCOMPARE(app.win->dockWidgetArea(app.project), Qt::RightDockWidgetArea);
    delete app.win;
}

QTEST_MAIN(TestProjectDock)
#include "test_project_dock.moc"
