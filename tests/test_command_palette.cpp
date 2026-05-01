#include "commandpalette.h"
#include "themes/thememanager.h"
#include <QtTest/QTest>
#include <QApplication>
#include <QMenuBar>
#include <QAction>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>

using namespace rcx;

class TestCommandPalette : public QObject {
    Q_OBJECT
private slots:

    // ── Pure fuzzy-match tests (no UI needed) ──

    void fuzzyEmptyNeedleMatchesEverything() {
        QVERIFY(CommandPalette::fuzzyScore(QString(), QStringLiteral("File > Open")) > 0);
    }

    void fuzzyExactMatchScoresHigh() {
        int s1 = CommandPalette::fuzzyScore(QStringLiteral("open"),
                                            QStringLiteral("File > Open"));
        int s2 = CommandPalette::fuzzyScore(QStringLiteral("open"),
                                            QStringLiteral("Reclass > Reopen Last"));
        QVERIFY(s1 > 0);
        QVERIFY(s2 > 0);
        // Word-start "Open" should outscore middle-of-word "open" in "Reopen"
        QVERIFY(s1 > s2);
    }

    void fuzzyMissReturnsZero() {
        QCOMPARE(CommandPalette::fuzzyScore(QStringLiteral("xyz"),
                                             QStringLiteral("File > Open")), 0);
    }

    void fuzzyAcronymMatch() {
        // "fs" should match "File > Save" (word-start hits)
        int s = CommandPalette::fuzzyScore(QStringLiteral("fs"),
                                            QStringLiteral("File > Save"));
        QVERIFY(s > 0);
    }

    void fuzzyCaseInsensitive() {
        int hi = CommandPalette::fuzzyScore(QStringLiteral("OPEN"),
                                             QStringLiteral("File > Open"));
        int lo = CommandPalette::fuzzyScore(QStringLiteral("open"),
                                             QStringLiteral("File > Open"));
        QCOMPARE(hi, lo);
    }

    // ── Menu enumeration tests ──

    void enumeratesActions() {
        auto* bar = makeFakeMenuBar();
        CommandPalette palette(bar);
        const auto& entries = palette.entries();
        // We added 4 leaf actions: Open, Save, Recent>foo.rcx, Edit>Undo
        QCOMPARE(entries.size(), 4);

        QStringList paths;
        for (const auto& e : entries) paths << e.path;
        QVERIFY(paths.contains(QStringLiteral("File > Open")));
        QVERIFY(paths.contains(QStringLiteral("File > Save")));
        QVERIFY(paths.contains(QStringLiteral("File > Recent > foo.rcx")));
        QVERIFY(paths.contains(QStringLiteral("Edit > Undo")));
        delete bar;
    }

    void skipsSeparators() {
        auto* bar = makeFakeMenuBar();
        // Inject a separator in the File menu
        for (auto* a : bar->actions()) {
            if (a->text() == QStringLiteral("&File")) {
                a->menu()->addSeparator();
                a->menu()->addSeparator();
            }
        }
        CommandPalette palette(bar);
        // Same entry count — separators skipped
        QCOMPARE(palette.entries().size(), 4);
        delete bar;
    }

    void disabledActionsAreIncludedButMarked() {
        auto* bar = makeFakeMenuBar();
        // Disable Save
        for (auto* a : bar->actions()) {
            if (a->text() == QStringLiteral("&File")) {
                for (auto* sub : a->menu()->actions()) {
                    if (sub->text() == QStringLiteral("&Save"))
                        sub->setEnabled(false);
                }
            }
        }
        CommandPalette palette(bar);
        bool foundDisabled = false;
        for (const auto& e : palette.entries()) {
            if (e.path == QStringLiteral("File > Save")) {
                QVERIFY(!e.enabled);
                foundDisabled = true;
            }
        }
        QVERIFY(foundDisabled);
        delete bar;
    }

    void activateTriggersAction() {
        auto* bar = makeFakeMenuBar();
        int triggered = 0;
        for (auto* a : bar->actions()) {
            if (a->text() == QStringLiteral("&File")) {
                for (auto* sub : a->menu()->actions()) {
                    if (sub->text() == QStringLiteral("&Open"))
                        QObject::connect(sub, &QAction::triggered,
                                         [&triggered]() { triggered++; });
                }
            }
        }
        CommandPalette palette(bar);
        // Find the index of "File > Open"
        int openIdx = -1;
        for (int i = 0; i < palette.entries().size(); i++) {
            if (palette.entries()[i].path == QStringLiteral("File > Open"))
                openIdx = i;
        }
        QVERIFY(openIdx >= 0);
        QVERIFY(palette.activateEntry(openIdx));
        QCOMPARE(triggered, 1);
        delete bar;
    }

    void shortcutsCaptured() {
        auto* bar = new QMenuBar;
        auto* file = bar->addMenu(QStringLiteral("&File"));
        auto* save = file->addAction(QStringLiteral("&Save"));
        save->setShortcut(QKeySequence::Save);
        CommandPalette palette(bar);
        QCOMPARE(palette.entries().size(), 1);
        QVERIFY(!palette.entries()[0].shortcut.isEmpty());
        delete bar;
    }

private:
    QMenuBar* makeFakeMenuBar() {
        auto* bar = new QMenuBar;
        auto* file = bar->addMenu(QStringLiteral("&File"));
        file->addAction(QStringLiteral("&Open"));
        file->addAction(QStringLiteral("&Save"));
        auto* recent = file->addMenu(QStringLiteral("&Recent"));
        recent->addAction(QStringLiteral("foo.rcx"));
        auto* edit = bar->addMenu(QStringLiteral("&Edit"));
        edit->addAction(QStringLiteral("&Undo"));
        return bar;
    }
};

QTEST_MAIN(TestCommandPalette)
#include "test_command_palette.moc"
