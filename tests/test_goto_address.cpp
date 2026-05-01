#include "gotoaddressdialog.h"
#include "addressparser.h"
#include "themes/thememanager.h"
#include <QtTest/QTest>
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QSettings>

using namespace rcx;

// GotoAddressDialog is a thin wrapper around AddressParser. We mostly verify
// it constructs cleanly, live-validates input, persists recent entries via
// QSettings, and accepts only when the input parses.

static AddressParserCallbacks moduleCbs() {
    AddressParserCallbacks cbs;
    cbs.resolveModule = [](const QString& name, bool* ok) -> uint64_t {
        if (name == QStringLiteral("game.exe")) { *ok = true; return 0x140000000ULL; }
        *ok = false;
        return 0;
    };
    return cbs;
}

class TestGotoAddress : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // Use a dedicated organisation/app scope so we don't trash a real
        // user's Reclass settings while running tests.
        QCoreApplication::setOrganizationName(QStringLiteral("Reclass"));
        QCoreApplication::setApplicationName(QStringLiteral("Reclass"));
        GotoAddressDialog::clearRecent();
    }
    void cleanupTestCase() {
        GotoAddressDialog::clearRecent();
    }

    void constructsAndDisablesOkInitially() {
        GotoAddressDialog dlg(moduleCbs(), 8);
        // Find the OK button via the dialog button box
        auto* ok = dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(ok);
        QVERIFY(!ok->isEnabled());  // empty input → cannot proceed
    }

    void enablesOkOnValidInput() {
        GotoAddressDialog dlg(moduleCbs(), 8);
        auto* input = dlg.findChild<QLineEdit*>();
        QVERIFY(input);
        input->setText(QStringLiteral("0x1234"));
        QCoreApplication::processEvents();
        auto* ok = dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(ok->isEnabled());
        QCOMPARE(dlg.resolvedAddress(), 0x1234ULL);
    }

    void rejectsInvalidInput() {
        GotoAddressDialog dlg(moduleCbs(), 8);
        auto* input = dlg.findChild<QLineEdit*>();
        input->setText(QStringLiteral("xyz nonsense"));
        QCoreApplication::processEvents();
        auto* ok = dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        QVERIFY(!ok->isEnabled());
    }

    void evaluatesModuleExpressions() {
        GotoAddressDialog dlg(moduleCbs(), 8);
        auto* input = dlg.findChild<QLineEdit*>();
        input->setText(QStringLiteral("<game.exe> + 0x40"));
        QCoreApplication::processEvents();
        QCOMPARE(dlg.resolvedAddress(), 0x140000040ULL);
    }

    void persistsRecentEntries() {
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0xAA"));
        GotoAddressDialog::pushRecent(QStringLiteral("0xBB"));
        GotoAddressDialog::pushRecent(QStringLiteral("0xCC"));
        auto recent = GotoAddressDialog::loadRecent();
        QCOMPARE(recent.size(), 3);
        QCOMPARE(recent[0], QStringLiteral("0xCC"));  // most recent first
        QCOMPARE(recent[1], QStringLiteral("0xBB"));
        QCOMPARE(recent[2], QStringLiteral("0xAA"));
    }

    void recentDeduplicates() {
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0xAA"));
        GotoAddressDialog::pushRecent(QStringLiteral("0xBB"));
        GotoAddressDialog::pushRecent(QStringLiteral("0xAA"));  // dup → moves to top
        auto recent = GotoAddressDialog::loadRecent();
        QCOMPARE(recent.size(), 2);
        QCOMPARE(recent[0], QStringLiteral("0xAA"));
        QCOMPARE(recent[1], QStringLiteral("0xBB"));
    }

    void recentIsCapped() {
        GotoAddressDialog::clearRecent();
        for (int i = 0; i < 30; i++)
            GotoAddressDialog::pushRecent(QStringLiteral("0x%1").arg(i, 0, 16));
        auto recent = GotoAddressDialog::loadRecent();
        QCOMPARE(recent.size(), GotoAddressDialog::kMaxRecent);
    }

    void recentListShowsExistingEntries() {
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0xDEAD"));
        GotoAddressDialog::pushRecent(QStringLiteral("<game.exe>+0x10"));
        GotoAddressDialog dlg(moduleCbs(), 8);
        auto* list = dlg.findChild<QListWidget*>();
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->text(), QStringLiteral("<game.exe>+0x10"));
    }
};

QTEST_MAIN(TestGotoAddress)
#include "test_goto_address.moc"
