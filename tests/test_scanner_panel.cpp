// UI regression tests for the Memory Scanner panel (ScannerPanel). The panel
// constructs standalone (no provider needed) and loadResultsFrom() populates
// the results table, so these run headless without a live scan. First enabled
// CI coverage of the panel widget — the old test_scanner_ui is disabled (hangs)
// and stale. Keep these construct-only + assertion-only (no modal/wait) so they
// don't reintroduce that hang.
#include <QtTest/QTest>
#include <QApplication>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryFile>
#include <memory>
#include "scannerpanel.h"
#include "themes/thememanager.h"
#include "providers/provider.h"
#include "providers/buffer_provider.h"

using namespace rcx;

class TestScannerPanel : public QObject {
    Q_OBJECT

    // Helper: a panel populated with one numeric (value-mode) result row.
    static bool loadOneRow(ScannerPanel& panel, QTemporaryFile& f) {
        if (!f.open()) return false;
        // scanMode 1 = value mode → numeric → value column is right-aligned.
        f.write("{\"version\":1,\"scanMode\":1,\"valueType\":2,\"results\":["
                "{\"address\":\"7ff600001000\",\"value\":\"2a000000\","
                "\"module\":\"game.exe\"}]}");
        f.flush();
        return panel.loadResultsFrom(f.fileName());
    }

private slots:

    // Regression: the in-place value-cell editor must inherit the cell's display
    // alignment (right, for numerics). Qt's default editor is left-aligned, so
    // without AlignedEditorDelegate the value jumped left the instant you began
    // editing. Verified by opening the editor and reading its alignment.
    void testValueEditorInheritsRightAlignment() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());
        QTemporaryFile f;
        QVERIFY(loadOneRow(panel, f));

        auto* tbl = panel.resultsTable();
        QVERIFY(tbl->rowCount() >= 1);
        QTableWidgetItem* valItem = tbl->item(0, 1);
        QVERIFY(valItem);
        // Sanity: the cell DISPLAY is right-aligned for a value-mode scan.
        QVERIFY(valItem->textAlignment() & Qt::AlignRight);

        // The editor must match — this is what AlignedEditorDelegate guarantees.
        tbl->openPersistentEditor(valItem);
        QApplication::processEvents();
        auto* le = tbl->findChild<QLineEdit*>();
        QVERIFY2(le, "no value-cell editor was created");
        QVERIFY2((le->alignment() & Qt::AlignRight) != 0,
            qPrintable(QString("editor alignment 0x%1 lacks AlignRight "
                "(AlignedEditorDelegate regression)")
                .arg((int)le->alignment(), 0, 16)));
        tbl->closePersistentEditor(valItem);
    }

    // Regression: once results exist, Reset must be VISIBLE and ENABLED (and
    // Next Scan enabled). The reported bug was Reset rendering greyed-but-
    // clickable after a first scan; loadResultsFrom() / onScanFinished() now
    // enable it. Guards that path.
    void testResetEnabledWhenResultsExist() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());
        // Before any results: Reset is explicitly hidden. (isHidden() reflects
        // the explicit visibility flag; isVisible() would be false for every
        // child here simply because the panel itself is never shown.)
        QVERIFY2(panel.newScanButton()->isHidden(), "Reset should start hidden");

        QTemporaryFile f;
        QVERIFY(loadOneRow(panel, f));

        QVERIFY2(!panel.newScanButton()->isHidden(),
                 "Reset must be shown once results exist");
        QVERIFY2(panel.newScanButton()->isEnabled(),
                 "Reset must be enabled (not greyed) once results exist");
        QVERIFY2(panel.updateButton()->isEnabled(),
                 "Next Scan must be enabled once results exist");
    }

    // Functional (end-to-end): the panel's blocking scan path drives a value
    // through serialize → engine → results. Plants int32 42 in a buffer and
    // asserts the scan finds it. Covers the panel→engine wiring (the engine
    // itself is tested separately in test_scanner). BufferProvider maps at base
    // 0, so the hit address is the buffer offset.
    void testValueScanFindsPlantedInt() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());

        QByteArray data(64, '\0');
        data[8] = 0x2A;  // little-endian int32 == 42, at a 4-aligned offset
        auto prov = std::make_shared<BufferProvider>(data);
        panel.setProviderGetter(
            [prov]() -> std::shared_ptr<Provider> { return prov; });

        const auto results =
            panel.runValueScanAndWait(ValueType::Int32, QStringLiteral("42"));
        QVERIFY2(!results.isEmpty(), "scan should find the planted value");
        bool foundAt8 = false;
        for (const auto& r : results)
            if (r.address == 8) foundAt8 = true;
        QVERIFY2(foundAt8, "int32 42 should be found at offset 8");
    }

    // Functional (end-to-end): the iterative-narrowing workflow — the heart of a
    // memory scanner. First scan finds all values > 0, then a rescan tightens to
    // ExactValue 42, which must narrow the in-place result set (here 2 → 1).
    void testRescanNarrowsResults() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());

        QByteArray data(64, '\0');
        data[8]  = 0x2A;  // int32 42 at offset 8
        data[16] = 0x64;  // int32 100 at offset 16
        auto prov = std::make_shared<BufferProvider>(data);
        panel.setProviderGetter(
            [prov]() -> std::shared_ptr<Provider> { return prov; });

        // First scan: every aligned int32 > 0 → offsets 8 and 16.
        const auto first = panel.runValueScanAndWait(
            ValueType::Int32, ScanCondition::BiggerThan, QStringLiteral("0"));
        QCOMPARE(first.size(), 2);

        // Rescan tightens to == 42 → only offset 8 survives.
        const auto narrowed =
            panel.runRescanAndWait(ScanCondition::ExactValue, QStringLiteral("42"));
        QCOMPARE(narrowed.size(), 1);
        QCOMPARE(narrowed[0].address, (uint64_t)8);
    }

    // Functional (end-to-end): completes the scan→narrow→UNDO loop. After a
    // rescan over-narrows, Undo must restore the pre-rescan result set (the
    // Cheat-Engine "I narrowed too far" recovery). runRescanAndWait snapshots
    // before narrowing; the Undo button pops it.
    void testUndoRestoresPreNarrowResults() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());

        QByteArray data(64, '\0');
        data[8]  = 0x2A;  // int32 42
        data[16] = 0x64;  // int32 100
        auto prov = std::make_shared<BufferProvider>(data);
        panel.setProviderGetter(
            [prov]() -> std::shared_ptr<Provider> { return prov; });

        panel.runValueScanAndWait(ValueType::Int32, ScanCondition::BiggerThan,
                                  QStringLiteral("0"));
        QCOMPARE(panel.results().size(), 2);
        panel.runRescanAndWait(ScanCondition::ExactValue, QStringLiteral("42"));
        QCOMPARE(panel.results().size(), 1);

        QVERIFY2(panel.undoButton()->isEnabled(), "Undo should be available after a rescan");
        panel.undoButton()->click();  // pops the snapshot
        QCOMPARE(panel.results().size(), 2);  // restored
    }

    // Functional: saveResultsTo() → loadResultsFrom() must round-trip a result
    // set losslessly (address, raw value bytes, module). Guards against the save
    // and load JSON formats drifting apart and silently corrupting saved scans.
    void testResultsSaveLoadRoundTrip() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());
        QTemporaryFile src;
        QVERIFY(loadOneRow(panel, src));
        QCOMPARE(panel.results().size(), 1);

        QTemporaryFile dst;
        QVERIFY(dst.open());
        const QString dstPath = dst.fileName();
        dst.close();
        QVERIFY2(panel.saveResultsTo(dstPath), "saveResultsTo failed");

        ScannerPanel panel2;
        panel2.applyTheme(ThemeManager::instance().current());
        QVERIFY2(panel2.loadResultsFrom(dstPath), "loadResultsFrom failed");

        QCOMPARE(panel2.results().size(), panel.results().size());
        const auto& a = panel.results()[0];
        const auto& b = panel2.results()[0];
        QCOMPARE(b.address, a.address);
        QCOMPARE(b.scanValue, a.scanValue);
        QCOMPARE(b.regionModule, a.regionModule);
    }

    // Functional: the "Filter shown results" box hides rows that don't match in
    // any column. The live filter is debounced (80ms timer), so we set the text
    // and invoke the slot directly to keep the test deterministic (no qWait /
    // timing race). QVERIFY on invokeMethod catches a future slot rename.
    void testResultFilterHidesNonMatchingRows() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());
        QTemporaryFile f;
        QVERIFY(f.open());
        f.write("{\"version\":1,\"scanMode\":1,\"valueType\":2,\"results\":["
                "{\"address\":\"1000\",\"value\":\"2a000000\",\"module\":\"game.exe\"},"
                "{\"address\":\"2000\",\"value\":\"64000000\",\"module\":\"engine.dll\"}]}");
        f.flush();
        QVERIFY(panel.loadResultsFrom(f.fileName()));
        auto* tbl = panel.resultsTable();
        QCOMPARE(tbl->rowCount(), 2);

        auto applyFilter = [&](const QString& text) {
            panel.resultFilter()->setText(text);  // applyResultFilter reads this
            QVERIFY(QMetaObject::invokeMethod(&panel, "onResultFilterChanged",
                        Qt::DirectConnection, Q_ARG(QString, text)));
        };
        auto visibleRows = [&]() {
            int n = 0;
            for (int i = 0; i < tbl->rowCount(); ++i)
                if (!tbl->isRowHidden(i)) ++n;
            return n;
        };

        applyFilter(QStringLiteral("engine"));   // matches engine.dll only
        QCOMPARE(visibleRows(), 1);
        applyFilter(QString());                  // cleared → all rows back
        QCOMPARE(visibleRows(), 2);
    }

    // Regression: clicking the SCAN FOR header toggles the collapse state, which
    // is reflected in the header's chevron (▾ expanded / ▸ collapsed).
    void testScanForHeaderTogglesChevron() {
        ScannerPanel panel;
        panel.applyTheme(ThemeManager::instance().current());

        QPushButton* header = nullptr;
        for (auto* b : panel.findChildren<QPushButton*>())
            if (b->property("scanForHeader").toBool()) { header = b; break; }
        QVERIFY2(header, "SCAN FOR header button not found");

        const QString expanded = header->text();
        QVERIFY2(expanded.contains(QString::fromUtf8("\xE2\x96\xBE")),  // ▾
                 "expanded header should show the down chevron");
        header->click();
        QVERIFY2(header->text().contains(QString::fromUtf8("\xE2\x96\xB8")),  // ▸
                 "collapsed header should show the right chevron");
        header->click();
        QCOMPARE(header->text(), expanded);  // back to expanded
    }
};

QTEST_MAIN(TestScannerPanel)
#include "test_scanner_panel.moc"
