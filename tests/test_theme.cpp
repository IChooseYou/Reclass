#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QJsonDocument>
#include <QJsonObject>
#include "themes/theme.h"
#include "themes/thememanager.h"

using namespace rcx;

class TestTheme : public QObject {
    Q_OBJECT
private slots:
    void builtInThemes() {
        auto& tm = ThemeManager::instance();
        auto all = tm.themes();
        QVERIFY(all.size() >= 2);

        // Find themes by name
        const Theme* dark = nullptr;
        const Theme* warm = nullptr;
        for (const auto& t : all) {
            if (t.name == "Reclass Dark") dark = &t;
            if (t.name == "Warm") warm = &t;
        }
        QVERIFY(dark);
        QCOMPARE(dark->name, QString("Reclass Dark"));
        QVERIFY(dark->background.isValid());
        QVERIFY(dark->text.isValid());
        QVERIFY(dark->syntaxKeyword.isValid());
        QVERIFY(dark->markerError.isValid());

        QVERIFY(warm);
        QCOMPARE(warm->name, QString("Warm"));
        QVERIFY(warm->background.isValid());
        QVERIFY(warm->text.isValid());
        QCOMPARE(warm->background, QColor("#212121"));
        QCOMPARE(warm->selection, QColor("#21213A"));
        QCOMPARE(warm->syntaxKeyword, QColor("#AA9565"));
        QCOMPARE(warm->syntaxType, QColor("#6B959F"));
    }

    void jsonRoundTrip() {
        auto& tm = ThemeManager::instance();
        Theme orig = tm.themes()[0];
        QJsonObject json = orig.toJson();
        Theme loaded = Theme::fromJson(json);

        QCOMPARE(loaded.name, orig.name);
        QCOMPARE(loaded.background, orig.background);
        QCOMPARE(loaded.text, orig.text);
        QCOMPARE(loaded.selection, orig.selection);
        QCOMPARE(loaded.syntaxKeyword, orig.syntaxKeyword);
        QCOMPARE(loaded.syntaxNumber, orig.syntaxNumber);
        QCOMPARE(loaded.syntaxString, orig.syntaxString);
        QCOMPARE(loaded.syntaxComment, orig.syntaxComment);
        QCOMPARE(loaded.syntaxType, orig.syntaxType);
        QCOMPARE(loaded.markerPtr, orig.markerPtr);
        QCOMPARE(loaded.markerError, orig.markerError);
        QCOMPARE(loaded.indHoverSpan, orig.indHoverSpan);
    }

    void jsonRoundTripWarm() {
        auto& tm = ThemeManager::instance();
        auto all = tm.themes();
        Theme orig;
        for (const auto& t : all)
            if (t.name == "Warm") { orig = t; break; }

        QJsonObject json = orig.toJson();
        Theme loaded = Theme::fromJson(json);

        QCOMPARE(loaded.name, orig.name);
        QCOMPARE(loaded.background, orig.background);
        QCOMPARE(loaded.selection, orig.selection);
        QCOMPARE(loaded.syntaxKeyword, orig.syntaxKeyword);
    }

    void fromJsonMissingFields() {
        QJsonObject sparse;
        sparse["name"] = "Sparse";
        sparse["background"] = "#ff0000";
        Theme t = Theme::fromJson(sparse);

        QCOMPARE(t.name, QString("Sparse"));
        QCOMPARE(t.background, QColor("#ff0000"));
        // Missing fields are default (invalid) QColor
        QVERIFY(!t.text.isValid());
        QVERIFY(!t.syntaxKeyword.isValid());
        QVERIFY(!t.markerError.isValid());
    }

    void themeManagerHasBuiltIns() {
        auto& tm = ThemeManager::instance();
        auto all = tm.themes();
        QVERIFY(all.size() >= 3);
        QCOMPARE(all[0].name, QString("Reclass Dark"));
        // VS2022 Dark and Warm are also loaded (order depends on filename sort)
        bool hasVs = false, hasWarm = false;
        for (const auto& t : all) {
            if (t.name == "VS2022 Dark") hasVs = true;
            if (t.name == "Warm") hasWarm = true;
        }
        QVERIFY(hasVs);
        QVERIFY(hasWarm);
    }

    void themeManagerSwitch() {
        auto& tm = ThemeManager::instance();
        QSignalSpy spy(&tm, &ThemeManager::themeChanged);

        int startIdx = tm.currentIndex();
        int target = (startIdx == 0) ? 1 : 0;
        tm.setCurrent(target);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(tm.currentIndex(), target);
        QCOMPARE(tm.current().name, tm.themes()[target].name);

        // Restore
        tm.setCurrent(startIdx);
    }

    void themeManagerCRUD() {
        auto& tm = ThemeManager::instance();
        int initialCount = tm.themes().size();

        // Add
        Theme custom = tm.themes()[0];
        custom.name = "Test Custom";
        custom.background = QColor("#ff0000");
        tm.addTheme(custom);
        QCOMPARE(tm.themes().size(), initialCount + 1);
        QCOMPARE(tm.themes().last().name, QString("Test Custom"));

        // Update
        int idx = tm.themes().size() - 1;
        Theme updated = custom;
        updated.background = QColor("#00ff00");
        tm.updateTheme(idx, updated);
        QCOMPARE(tm.themes()[idx].background, QColor("#00ff00"));

        // Remove
        tm.removeTheme(idx);
        QCOMPARE(tm.themes().size(), initialCount);
    }
};

QTEST_MAIN(TestTheme)
#include "test_theme.moc"
