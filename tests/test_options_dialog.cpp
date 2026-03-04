#include <QtTest/QTest>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QGroupBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include "optionsdialog.h"
#include "themes/thememanager.h"

using namespace rcx;

// Helper: apply the global palette the same way main.cpp does
static void applyGlobalTheme(const Theme& theme) {
    QPalette pal;
    pal.setColor(QPalette::Window,          theme.background);
    pal.setColor(QPalette::WindowText,      theme.text);
    pal.setColor(QPalette::Base,            theme.background);
    pal.setColor(QPalette::AlternateBase,   theme.surface);
    pal.setColor(QPalette::Text,            theme.text);
    pal.setColor(QPalette::Button,          theme.button);
    pal.setColor(QPalette::ButtonText,      theme.text);
    pal.setColor(QPalette::Highlight,       theme.selection);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::ToolTipBase,     theme.backgroundAlt);
    pal.setColor(QPalette::ToolTipText,     theme.text);
    pal.setColor(QPalette::Mid,             theme.border);
    pal.setColor(QPalette::Dark,            theme.background);
    pal.setColor(QPalette::Light,           theme.textFaint);
    pal.setColor(QPalette::Link,            theme.indHoverSpan);

    pal.setColor(QPalette::Disabled, QPalette::WindowText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Text,            theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText,      theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, theme.textMuted);
    pal.setColor(QPalette::Disabled, QPalette::Light,           theme.background);

    qApp->setPalette(pal);
    qApp->setStyleSheet(QString());
}

class TestOptionsDialog : public QObject {
    Q_OBJECT
private slots:

    void initTestCase() {
        // Apply theme palette so dialog inherits real colors
        auto& tm = ThemeManager::instance();
        applyGlobalTheme(tm.current());
    }

    void dialogCreatesAllWidgets() {
        OptionsResult defaults;
        defaults.themeIndex = 0;
        defaults.fontName = "JetBrains Mono";
        defaults.menuBarTitleCase = true;
        defaults.autoStartMcp = false;

        OptionsDialog dlg(defaults);

        // Core widgets exist
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        auto* pages = dlg.findChild<QStackedWidget*>();
        QVERIFY(pages);
        QCOMPARE(pages->count(), 3);

        auto* themeCombo = dlg.findChild<QComboBox*>("themeCombo");
        QVERIFY(themeCombo);
        QVERIFY(themeCombo->count() >= 3);

        auto* fontCombo = dlg.findChild<QComboBox*>("fontCombo");
        QVERIFY(fontCombo);
        QCOMPARE(fontCombo->count(), 2);

        auto* showIconCheck = dlg.findChild<QCheckBox*>();
        QVERIFY(showIconCheck);

        auto* buttons = dlg.findChild<QDialogButtonBox*>();
        QVERIFY(buttons);
        QVERIFY(buttons->button(QDialogButtonBox::Ok));
        QVERIFY(buttons->button(QDialogButtonBox::Cancel));
    }

    void resultReflectsInput() {
        OptionsResult input;
        input.themeIndex = 1;
        input.fontName = "Consolas";
        input.menuBarTitleCase = false;
        input.autoStartMcp = true;

        OptionsDialog dlg(input);
        auto r = dlg.result();

        QCOMPARE(r.themeIndex, 1);
        QCOMPARE(r.fontName, QString("Consolas"));
        QCOMPARE(r.menuBarTitleCase, false);
        QCOMPARE(r.autoStartMcp, true);
    }

    void noStyleSheetOnDialog() {
        OptionsResult defaults;
        OptionsDialog dlg(defaults);

        // Dialog itself must have no stylesheet override
        QVERIFY(dlg.styleSheet().isEmpty());

        // Combo boxes must have no stylesheet override
        auto* themeCombo = dlg.findChild<QComboBox*>("themeCombo");
        QVERIFY(themeCombo->styleSheet().isEmpty());
        auto* fontCombo = dlg.findChild<QComboBox*>("fontCombo");
        QVERIFY(fontCombo->styleSheet().isEmpty());

        // No child widget should have a stylesheet set
        for (auto* child : dlg.findChildren<QWidget*>()) {
            QVERIFY2(child->styleSheet().isEmpty(),
                      qPrintable(QString("Widget %1 (%2) has unexpected stylesheet: %3")
                          .arg(child->objectName(),
                               child->metaObject()->className(),
                               child->styleSheet())));
        }
    }

    void highlightColorDiffersFromBackground() {
        // Verify the palette Highlight is distinguishable from Window background
        // This is the root cause of broken hover: if they're the same, hover is invisible
        auto& tm = ThemeManager::instance();
        const auto themes = tm.themes();
        for (const auto& theme : themes) {
            QVERIFY2(theme.selection != theme.background,
                      qPrintable(QString("Theme '%1': selection == background (%2)")
                          .arg(theme.name, theme.background.name())));
        }
    }

    void paletteHighlightIsSelection() {
        // After applying theme, QPalette::Highlight must be theme.selection (not theme.hover)
        auto& tm = ThemeManager::instance();
        const auto& theme = tm.current();
        applyGlobalTheme(theme);

        QPalette pal = qApp->palette();
        QCOMPARE(pal.color(QPalette::Highlight), theme.selection);
    }

    void treePageSwitching() {
        OptionsResult defaults;
        OptionsDialog dlg(defaults);

        auto* tree = dlg.findChild<QTreeWidget*>();
        auto* pages = dlg.findChild<QStackedWidget*>();
        QVERIFY(tree && pages);

        // General is selected by default -> page 0
        QCOMPARE(pages->currentIndex(), 0);

        // Find "AI Features" item and select it
        auto* envItem = tree->topLevelItem(0);
        QVERIFY(envItem);
        QTreeWidgetItem* aiItem = nullptr;
        for (int i = 0; i < envItem->childCount(); ++i) {
            if (envItem->child(i)->text(0) == "AI Features") {
                aiItem = envItem->child(i);
                break;
            }
        }
        QVERIFY(aiItem);
        tree->setCurrentItem(aiItem);
        QCOMPARE(pages->currentIndex(), 1);

        // Switch back to General
        QTreeWidgetItem* generalItem = nullptr;
        for (int i = 0; i < envItem->childCount(); ++i) {
            if (envItem->child(i)->text(0) == "General") {
                generalItem = envItem->child(i);
                break;
            }
        }
        QVERIFY(generalItem);
        tree->setCurrentItem(generalItem);
        QCOMPARE(pages->currentIndex(), 0);
    }

    void searchFilterHidesItems() {
        OptionsResult defaults;
        OptionsDialog dlg(defaults);

        auto* search = dlg.findChild<QLineEdit*>();
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(search && tree);

        auto* envItem = tree->topLevelItem(0);
        QVERIFY(envItem);

        // All children visible initially
        for (int i = 0; i < envItem->childCount(); ++i)
            QVERIFY(!envItem->child(i)->isHidden());

        // Search for "MCP" - should hide General, show AI Features
        search->setText("MCP");
        QTreeWidgetItem* generalItem = nullptr;
        QTreeWidgetItem* aiItem = nullptr;
        for (int i = 0; i < envItem->childCount(); ++i) {
            auto* child = envItem->child(i);
            if (child->text(0) == "General") generalItem = child;
            if (child->text(0) == "AI Features") aiItem = child;
        }
        QVERIFY(generalItem && aiItem);
        QVERIFY(generalItem->isHidden());
        QVERIFY(!aiItem->isHidden());

        // Clear search - all visible again
        search->setText("");
        QVERIFY(!generalItem->isHidden());
        QVERIFY(!aiItem->isHidden());
    }

    void refreshRateSpinBoxExists() {
        OptionsResult defaults;
        defaults.refreshMs = 660;
        OptionsDialog dlg(defaults);

        auto* spin = dlg.findChild<QSpinBox*>("refreshSpin");
        QVERIFY(spin);
        QCOMPARE(spin->value(), 660);
        QCOMPARE(spin->minimum(), 1);
        QCOMPARE(spin->maximum(), 60000);
    }

    void refreshRateResultReflectsInput() {
        OptionsResult input;
        input.refreshMs = 200;
        OptionsDialog dlg(input);

        auto r = dlg.result();
        QCOMPARE(r.refreshMs, 200);

        // Change via spin box
        auto* spin = dlg.findChild<QSpinBox*>("refreshSpin");
        QVERIFY(spin);
        spin->setValue(100);
        r = dlg.result();
        QCOMPARE(r.refreshMs, 100);
    }

    void refreshRateClampsMin() {
        OptionsResult input;
        input.refreshMs = 0; // below minimum
        OptionsDialog dlg(input);

        auto* spin = dlg.findChild<QSpinBox*>("refreshSpin");
        QVERIFY(spin);
        // QSpinBox clamps to minimum
        QCOMPARE(spin->value(), 1);
    }

    void dialogInheritsPalette() {
        auto& tm = ThemeManager::instance();
        const auto& theme = tm.current();
        applyGlobalTheme(theme);

        OptionsResult defaults;
        OptionsDialog dlg(defaults);
        dlg.show();
        QTest::qWaitForWindowExposed(&dlg);

        // Dialog's effective palette should match the app palette
        QPalette dlgPal = dlg.palette();
        QPalette appPal = qApp->palette();

        QCOMPARE(dlgPal.color(QPalette::Window), appPal.color(QPalette::Window));
        QCOMPARE(dlgPal.color(QPalette::WindowText), appPal.color(QPalette::WindowText));
        QCOMPARE(dlgPal.color(QPalette::Highlight), appPal.color(QPalette::Highlight));
        QCOMPARE(dlgPal.color(QPalette::Button), appPal.color(QPalette::Button));
        QCOMPARE(dlgPal.color(QPalette::ButtonText), appPal.color(QPalette::ButtonText));

        // Highlight must be visible against background
        QVERIFY(dlgPal.color(QPalette::Highlight) != dlgPal.color(QPalette::Window));
    }
};

QTEST_MAIN(TestOptionsDialog)
#include "test_options_dialog.moc"
