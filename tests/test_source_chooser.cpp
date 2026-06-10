// Interaction tests for the SourceChooserPopup — specifically the per-row ×
// delete button added so users can remove a single bound source instead of
// only "Clear All". The render harness (sourcechooser_render) proves the glyph
// is *drawn*; these tests prove the click/key actually *fires* removeRequested
// and — critically — that clicking elsewhere on a saved row does NOT delete it
// (guards against the paint rect and the hit-test rect drifting apart, since
// both come from the shared deleteBtnRect()).
#include <QtTest/QTest>
#include <QSignalSpy>
#include <QApplication>
#include <QListView>
#include <QAbstractItemModel>
#include <QMouseEvent>
#include <QKeyEvent>
#include "sourcechooserpopup.h"
#include "themes/thememanager.h"

using namespace rcx;

class TestSourceChooser : public QObject {
    Q_OBJECT
private:
    SourceChooserPopup* m_popup = nullptr;
    QListView* m_list = nullptr;

    static SourceEntry header(const QString& name) {
        SourceEntry h;
        h.entryKind = SourceEntry::SectionHeader;
        h.displayName = name;
        h.enabled = false;
        return h;
    }
    static SourceEntry saved(const QString& name, int savedIndex, bool active) {
        SourceEntry e;
        e.entryKind = SourceEntry::SavedSource;
        e.displayName = name;
        e.providerIdentifier = QStringLiteral("processmemory");
        e.kindLabel = QStringLiteral("Process");
        e.pid = QString::number(1000 + savedIndex);
        e.arch = QStringLiteral("x64");
        e.baseAddress = QStringLiteral("0x140000000");
        e.savedIndex = savedIndex;
        e.isActive = active;
        return e;
    }

    // The popup's entry order: [Connected hdr, saved0, saved1, Add Source hdr,
    // provider, Clear All]. Saved rows live at model rows 1 and 2.
    int savedRow(int savedIndex) const { return 1 + savedIndex; }

private slots:
    void init() {
        m_popup = new SourceChooserPopup();
        m_popup->applyTheme(ThemeManager::instance().current());
        m_popup->setFont(QFont(QStringLiteral("Consolas"), 11));

        QVector<SourceEntry> entries;
        entries.append(header(QStringLiteral("Connected")));
        entries.append(saved(QStringLiteral("notepad.exe"), 0, true));
        entries.append(saved(QStringLiteral("game.exe"),    1, false));
        entries.append(header(QStringLiteral("Add Source")));
        {
            SourceEntry p;
            p.entryKind = SourceEntry::ProviderAction;
            p.displayName = QStringLiteral("Open File");
            p.providerIdentifier = QStringLiteral("File");
            p.kindLabel = QStringLiteral("File");
            p.dllFileName = QStringLiteral("built-in");
            entries.append(p);
        }
        {
            SourceEntry c;
            c.entryKind = SourceEntry::ClearAction;
            c.displayName = QStringLiteral("Clear All");
            c.enabled = true;
            entries.append(c);
        }
        m_popup->setSources(entries);
        m_popup->popup(QPoint(100, 100));
        QVERIFY(QTest::qWaitForWindowExposed(m_popup));
        QApplication::processEvents();

        m_list = m_popup->findChild<QListView*>();
        QVERIFY(m_list != nullptr);
        QVERIFY(m_list->model() != nullptr);
    }

    void cleanup() {
        delete m_popup;  m_popup = nullptr;  m_list = nullptr;
    }

    // Clicking the × on a saved row removes exactly that source.
    void testDeleteButtonClickEmitsRemove() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);
        QSignalSpy selectSpy(m_popup, &SourceChooserPopup::sourceSelected);

        QRect vr = m_list->visualRect(m_list->model()->index(savedRow(1), 0));
        QVERIFY(vr.isValid() && vr.width() > 30);
        // deleteBtnRect = 16px wide, right edge at itemRect.right()-6, centred.
        QPoint xCentre(vr.right() - 14, vr.center().y());

        QMouseEvent rel(QEvent::MouseButtonRelease, xCentre,
                        m_list->viewport()->mapToGlobal(xCentre),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_list->viewport(), &rel);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(removeSpy.takeFirst().at(0).toInt(), 1);   // game.exe = savedIndex 1
        QCOMPARE(selectSpy.count(), 0);                     // not a row activation
    }

    // Clicking the row body (over the name, far from the ×) must NOT delete —
    // this is the regression guard for the two rects drifting apart.
    void testBodyClickDoesNotDelete() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        QRect vr = m_list->visualRect(m_list->model()->index(savedRow(0), 0));
        QPoint body(vr.left() + 30, vr.center().y());

        QMouseEvent rel(QEvent::MouseButtonRelease, body,
                        m_list->viewport()->mapToGlobal(body),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_list->viewport(), &rel);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 0);
    }

    // Delete key on a focused saved row removes that source.
    void testDeleteKeyEmitsRemove() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        m_list->setFocus();
        m_list->setCurrentIndex(m_list->model()->index(savedRow(0), 0));
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(m_list, &del);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 1);
        QCOMPARE(removeSpy.takeFirst().at(0).toInt(), 0);   // notepad.exe = savedIndex 0
    }

    // Delete key on a non-saved row (the Clear All action) is a no-op.
    void testDeleteKeyOnActionNoOp() {
        QSignalSpy removeSpy(m_popup, &SourceChooserPopup::removeRequested);

        m_list->setFocus();
        // Last row is "Clear All" (ClearAction, not a saved source).
        int lastRow = m_list->model()->rowCount() - 1;
        m_list->setCurrentIndex(m_list->model()->index(lastRow, 0));
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(m_list, &del);
        QApplication::processEvents();

        QCOMPARE(removeSpy.count(), 0);
    }
};

QTEST_MAIN(TestSourceChooser)
#include "test_source_chooser.moc"
