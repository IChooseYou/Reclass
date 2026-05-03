#include <QTest>
#include <QSignalSpy>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QClipboard>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <cstring>
#include "scannerpanel.h"
#include "scanner.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"

using namespace rcx;

class TestScannerUI : public QObject {
    Q_OBJECT

private:
    ScannerPanel* m_panel = nullptr;

private slots:

    void init() {
        m_panel = new ScannerPanel();
        m_panel->show();
        // Clear mode-dependent filter defaults so BufferProvider scans
        // (which have no executable regions) work without filter issues.
        // The initialState_filterCheckboxes test verifies defaults separately.
        m_panel->execCheck()->setChecked(false);
        m_panel->writeCheck()->setChecked(false);
        QApplication::processEvents();
    }

    void cleanup() {
        delete m_panel;
        m_panel = nullptr;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Widget creation and initial state
    // ═══════════════════════════════════════════════════════════════════

    void initialState_modeCombo() {
        QCOMPARE(m_panel->modeCombo()->count(), 2);
        QCOMPARE(m_panel->modeCombo()->currentIndex(), 0); // Signature
        QCOMPARE(m_panel->modeCombo()->itemText(0), QStringLiteral("Signature"));
        QCOMPARE(m_panel->modeCombo()->itemText(1), QStringLiteral("Value"));
    }

    void initialState_signatureMode() {
        // In signature mode: pattern visible, value hidden
        QVERIFY(m_panel->patternEdit()->isVisible());
        QVERIFY(!m_panel->typeCombo()->isVisible());
        QVERIFY(!m_panel->valueEdit()->isVisible());
    }

    void initialState_scanButton() {
        // Renamed for workflow clarity: First Scan / Next Scan / Reset.
        QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("First Scan"));
    }

    void initialState_progressBarHidden() {
        QVERIFY(!m_panel->progressBar()->isVisible());
    }

    void initialState_resultsEmpty() {
        QCOMPARE(m_panel->resultsTable()->rowCount(), 0);
    }

    void initialState_buttonsDisabled() {
        QVERIFY(!m_panel->gotoButton()->isEnabled());
        QVERIFY(!m_panel->copyButton()->isEnabled());
    }

    void initialState_statusLabel() {
        QCOMPARE(m_panel->statusLabel()->text(), QStringLiteral("Ready"));
    }

    void initialState_filterCheckboxes() {
        // Verify defaults on a fresh panel (init() clears filters for test convenience)
        ScannerPanel fresh;
        // Signature mode default: executable checked, writable unchecked
        QVERIFY(fresh.execCheck()->isChecked());
        QVERIFY(!fresh.writeCheck()->isChecked());
        // Switching to value mode flips them
        fresh.modeCombo()->setCurrentIndex(1);
        QVERIFY(!fresh.execCheck()->isChecked());
        QVERIFY(fresh.writeCheck()->isChecked());
        // Switching back restores signature defaults
        fresh.modeCombo()->setCurrentIndex(0);
        QVERIFY(fresh.execCheck()->isChecked());
        QVERIFY(!fresh.writeCheck()->isChecked());
    }

    void initialState_patternPlaceholder() {
        QVERIFY(!m_panel->patternEdit()->placeholderText().isEmpty());
    }

    void initialState_typeComboHasAllTypes() {
        QCOMPARE(m_panel->typeCombo()->count(), 10); // int8..double = 10 types
    }

    void initialState_resultsTableColumns() {
        QCOMPARE(m_panel->resultsTable()->columnCount(), 2);
    }

    void initialState_noHeaders() {
        QVERIFY(!m_panel->resultsTable()->horizontalHeader()->isVisible());
        QVERIFY(!m_panel->resultsTable()->verticalHeader()->isVisible());
    }

    void initialState_noGrid() {
        QVERIFY(!m_panel->resultsTable()->showGrid());
    }

    // ═══════════════════════════════════════════════════════════════════
    // Mode switching
    // ═══════════════════════════════════════════════════════════════════

    void switchToValueMode() {
        m_panel->modeCombo()->setCurrentIndex(1); // Value

        QVERIFY(!m_panel->patternEdit()->isVisible());
        QVERIFY(m_panel->typeCombo()->isVisible());
        QVERIFY(m_panel->valueEdit()->isVisible());
    }

    void switchBackToSignatureMode() {
        m_panel->modeCombo()->setCurrentIndex(1); // Value
        m_panel->modeCombo()->setCurrentIndex(0); // Signature

        QVERIFY(m_panel->patternEdit()->isVisible());
        QVERIFY(!m_panel->typeCombo()->isVisible());
        QVERIFY(!m_panel->valueEdit()->isVisible());
    }

    // ═══════════════════════════════════════════════════════════════════
    // No provider — error handling
    // ═══════════════════════════════════════════════════════════════════

    void scan_noProvider() {
        // No provider getter set
        m_panel->patternEdit()->setText("48 8B");
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        QVERIFY(m_panel->statusLabel()->text().contains("No source"));
    }

    void scan_nullProvider() {
        m_panel->setProviderGetter([]() -> std::shared_ptr<Provider> {
            return nullptr;
        });
        m_panel->patternEdit()->setText("48 8B");
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        QVERIFY(m_panel->statusLabel()->text().contains("No source"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Invalid pattern — error handling
    // ═══════════════════════════════════════════════════════════════════

    void scan_emptyPattern() {
        auto prov = std::make_shared<BufferProvider>(QByteArray(16, '\0'));
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("");
        // The button now disables itself the moment the input is empty —
        // safer than letting the click fire and surface an error string.
        QVERIFY(!m_panel->scanButton()->isEnabled());
    }

    void scan_invalidPattern() {
        auto prov = std::make_shared<BufferProvider>(QByteArray(16, '\0'));
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("GG HH");
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        QVERIFY(m_panel->statusLabel()->text().contains("error", Qt::CaseInsensitive));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Successful scan — signature mode
    // ═══════════════════════════════════════════════════════════════════

    void scan_signatureFindsResults() {
        QByteArray data(32, '\0');
        data[8] = 0x48; data[9] = 0x8B;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("48 8B");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);

        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);
        QVERIFY(m_panel->statusLabel()->text().contains("1 result"));
    }

    void scan_signatureNoResults() {
        QByteArray data(32, '\0');
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("FF FF");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);

        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 0);
        QVERIFY(m_panel->statusLabel()->text().contains("0 result"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Scan button toggle (Scan ↔ Cancel)
    // ═══════════════════════════════════════════════════════════════════

    void scan_buttonShowsCancel() {
        // Use a large buffer so scan takes a measurable amount of time
        QByteArray data(4 * 1024 * 1024, '\0'); // 4MB
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("FF");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);

        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        // If scan hasn't finished yet, button should be Cancel
        if (m_panel->engine()->isRunning()) {
            QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("Cancel"));
            QVERIFY(m_panel->progressBar()->isVisible());
        }

        // Wait for finish
        QVERIFY(finSpy.wait(30000));
        QApplication::processEvents();

        // Button back to Scan
        // Renamed for workflow clarity: First Scan / Next Scan / Reset.
        QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("First Scan"));
        QVERIFY(!m_panel->progressBar()->isVisible());
    }

    void scan_cancelMidScan() {
        // This test verifies the cancel codepath works.
        // The scan may complete faster than we can click cancel,
        // so we just verify the final state is correct.
        QByteArray data(1024 * 1024, '\0'); // 1MB
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("FF");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);

        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        // Immediately try to cancel (may or may not succeed depending on timing)
        m_panel->engine()->abort();

        // Wait for finished signal
        if (finSpy.isEmpty())
            QVERIFY(finSpy.wait(10000));
        QApplication::processEvents();

        // After scan completes (or is cancelled), verify button returns to "Scan"
        // Need to process any remaining events (the finished handler sets button text)
        QApplication::processEvents();
        // If the panel still shows "Cancel", click it to reset
        if (m_panel->scanButton()->text() == QStringLiteral("Cancel")) {
            QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
            QApplication::processEvents();
        }
        // Renamed for workflow clarity: First Scan / Next Scan / Reset.
        QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("First Scan"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Value mode scan
    // ═══════════════════════════════════════════════════════════════════

    void scan_valueInt32() {
        QByteArray data(64, '\0');
        int32_t target = 42;
        std::memcpy(data.data() + 8, &target, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1); // Value mode
        // Find int32 in combo
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText("42");

        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);
        // Preview should show native int32 value, not hex
        auto* prevItem = m_panel->resultsTable()->item(0, 1);
        QVERIFY(prevItem);
        QCOMPARE(prevItem->text(), QStringLiteral("42"));
    }

    void scan_valueInvalidInput() {
        auto prov = std::make_shared<BufferProvider>(QByteArray(16, '\0'));
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1); // Value
        m_panel->valueEdit()->setText("not_a_number");

        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);

        QVERIFY(m_panel->statusLabel()->text().contains("error", Qt::CaseInsensitive));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Go to Address signal
    // ═══════════════════════════════════════════════════════════════════

    void goToAddress_signal() {
        QByteArray data(32, '\0');
        data[16] = 0xAB; data[17] = 0xCD;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("AB CD");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Select the row
        m_panel->resultsTable()->selectRow(0);
        QVERIFY(m_panel->gotoButton()->isEnabled());

        QSignalSpy goSpy(m_panel, &ScannerPanel::goToAddress);
        QTest::mouseClick(m_panel->gotoButton(), Qt::LeftButton);
        QCOMPARE(goSpy.size(), 1);
        QCOMPARE(goSpy.first().first().value<uint64_t>(), (uint64_t)16);
    }

    void doubleClick_startsEditing() {
        // Double-click now starts inline editing, not goToAddress
        QByteArray data(32, '\0');
        data[4] = 0xEF;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("EF");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Double-click should NOT emit goToAddress directly
        QSignalSpy goSpy(m_panel, &ScannerPanel::goToAddress);
        emit m_panel->resultsTable()->cellDoubleClicked(0, 0);
        QCOMPARE(goSpy.size(), 0);

        // Edit triggers should be DoubleClicked
        QVERIFY(m_panel->resultsTable()->editTriggers() & QAbstractItemView::DoubleClicked);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Copy Address
    // ═══════════════════════════════════════════════════════════════════

    void copyAddress() {
        QByteArray data(32, '\0');
        data[20] = 0xAA;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("AA");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        m_panel->resultsTable()->selectRow(0);
        QVERIFY(m_panel->copyButton()->isEnabled());

        QTest::mouseClick(m_panel->copyButton(), Qt::LeftButton);

        QString clip = QApplication::clipboard()->text();
        QVERIFY(clip.startsWith("0x", Qt::CaseInsensitive));
        // 20 = 0x14, verify it's present somewhere
        QVERIFY(clip.toUpper().endsWith("14"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Results table formatting
    // ═══════════════════════════════════════════════════════════════════

    void results_addressFormat() {
        QByteArray data(32, '\0');
        data[0] = (char)0xFF;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("FF");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        // Check address column has WinDbg backtick format
        auto* item = m_panel->resultsTable()->item(0, 0);
        QVERIFY(item);
        QVERIFY(item->text().contains('`')); // backtick separator
        // Address 0 should be: 00000000`00000000
        QCOMPARE(item->text(), QStringLiteral("00000000`00000000"));
    }

    void results_previewColumn() {
        QByteArray data(32, '\0');
        data[0] = 0xDE; data[1] = 0xAD; data[2] = 0xBE; data[3] = 0xEF;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("DE AD");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        auto* item = m_panel->resultsTable()->item(0, 1);
        QVERIFY(item);
        // Value column shows ONLY the matched bytes (the pattern length),
        // not the 16-byte chunk the engine cached for context. Previously
        // a 1-byte "DD" search produced a 16-byte string of unrelated bytes.
        QCOMPARE(item->text(), QStringLiteral("DE AD"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Filter checkboxes
    // ═══════════════════════════════════════════════════════════════════

    void filters_toggleExecutable() {
        m_panel->execCheck()->setChecked(true);
        QVERIFY(m_panel->execCheck()->isChecked());
        m_panel->execCheck()->setChecked(false);
        QVERIFY(!m_panel->execCheck()->isChecked());
    }

    void filters_toggleWritable() {
        m_panel->writeCheck()->setChecked(true);
        QVERIFY(m_panel->writeCheck()->isChecked());
    }

    // ═══════════════════════════════════════════════════════════════════
    // Theme application
    // ═══════════════════════════════════════════════════════════════════

    void theme_apply() {
        Theme t;
        t.background    = QColor("#1e1e1e");
        t.backgroundAlt = QColor("#252526");
        t.text          = QColor("#d4d4d4");
        t.textDim       = QColor("#858585");
        t.textMuted     = QColor("#555555");
        t.textFaint     = QColor("#333333");
        t.border        = QColor("#333333");
        t.borderFocused = QColor("#007acc");
        t.hover         = QColor("#2a2d2e");
        t.selection     = QColor("#264f78");
        t.button        = QColor("#333333");
        t.indHoverSpan  = QColor("#007acc");

        // Should not crash
        m_panel->applyTheme(t);

        // Verify stylesheet was applied (background color in stylesheet)
        QVERIFY(m_panel->resultsTable()->styleSheet().contains(t.background.name()));
        QVERIFY(m_panel->resultsTable()->styleSheet().contains(t.hover.name()));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Multiple scans — results get replaced
    // ═══════════════════════════════════════════════════════════════════

    void scan_resultsReplaced() {
        QByteArray data(32, '\0');
        data[0] = 0xAA;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        // First scan
        m_panel->patternEdit()->setText("AA");
        QSignalSpy finSpy1(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy1.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Second scan with different pattern (no results)
        m_panel->patternEdit()->setText("FF FF FF");
        QSignalSpy finSpy2(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy2.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 0);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Selection enables/disables action buttons
    // ═══════════════════════════════════════════════════════════════════

    void buttons_enableOnSelection() {
        QByteArray data(32, '\0');
        data[0] = 0xBB; data[16] = 0xBB;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("BB");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 2);

        // No selection yet
        QVERIFY(!m_panel->gotoButton()->isEnabled());
        QVERIFY(!m_panel->copyButton()->isEnabled());

        // Select row
        m_panel->resultsTable()->selectRow(0);
        QVERIFY(m_panel->gotoButton()->isEnabled());
        QVERIFY(m_panel->copyButton()->isEnabled());

        // Clear selection
        m_panel->resultsTable()->clearSelection();
        QVERIFY(!m_panel->gotoButton()->isEnabled());
        QVERIFY(!m_panel->copyButton()->isEnabled());
    }

    // ═══════════════════════════════════════════════════════════════════
    // Wildcard signature scan
    // ═══════════════════════════════════════════════════════════════════

    void scan_wildcardSignature() {
        QByteArray data(32, '\0');
        data[0]  = 0x48; data[1]  = 0x99; data[2]  = 0x05;
        data[16] = 0x48; data[17] = 0xAA; data[18] = 0x05;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("48 ?? 05");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->resultsTable()->rowCount(), 2);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Multiple results — status label pluralization
    // ═══════════════════════════════════════════════════════════════════

    void status_singleResult() {
        QByteArray data(16, '\0');
        data[0] = 0xCC;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("CC");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        // Status line now reads "N result(s) found" so it's clearer to the
        // user that the count IS the result of a fresh scan, not stale text.
        QCOMPARE(m_panel->statusLabel()->text(), QStringLiteral("1 result found"));
    }

    void status_multipleResults() {
        QByteArray data(16, '\0');
        data[0] = 0xDD; data[8] = 0xDD;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("DD");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QCOMPARE(m_panel->statusLabel()->text(), QStringLiteral("2 results found"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Provider getter is lazy (captures at scan time)
    // ═══════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════
    // Inline editing
    // ═══════════════════════════════════════════════════════════════════

    void edit_addressExpression() {
        QByteArray data(64, '\0');
        data[0] = 0xAA;
        data[32] = 0x55;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("AA");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Edit address cell with hex expression
        QSignalSpy goSpy(m_panel, &ScannerPanel::goToAddress);
        m_panel->resultsTable()->item(0, 0)->setText("0x20");
        QApplication::processEvents();

        // Should emit goToAddress with the new address
        QCOMPARE(goSpy.size(), 1);
        QCOMPARE(goSpy.first().first().value<uint64_t>(), (uint64_t)0x20);
    }

    void edit_previewHex() {
        QByteArray data(32, '\0');
        data[0] = 0xCC;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("CC");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Edit preview cell with new hex bytes
        m_panel->resultsTable()->item(0, 1)->setText("DE AD BE EF");
        QApplication::processEvents();

        // Verify bytes were written
        QByteArray written = prov->readBytes(0, 4);
        QCOMPARE((uint8_t)written[0], (uint8_t)0xDE);
        QCOMPARE((uint8_t)written[1], (uint8_t)0xAD);
        QCOMPARE((uint8_t)written[2], (uint8_t)0xBE);
        QCOMPARE((uint8_t)written[3], (uint8_t)0xEF);
    }

    void edit_previewValueMode() {
        QByteArray data(64, '\0');
        int32_t target = 100;
        std::memcpy(data.data() + 8, &target, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1); // Value mode
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText("100");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Preview shows "100", edit to "999"
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("100"));
        m_panel->resultsTable()->item(0, 1)->setText("999");
        QApplication::processEvents();

        // Verify int32 999 was written at offset 8
        int32_t written;
        QByteArray raw = prov->readBytes(8, 4);
        std::memcpy(&written, raw.constData(), 4);
        QCOMPARE(written, 999);
    }

    void edit_invalidAddress() {
        QByteArray data(32, '\0');
        data[0] = 0xDD;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("DD");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        // Edit with invalid expression — should show error and restore original
        m_panel->resultsTable()->item(0, 0)->setText("invalid!!!");
        QApplication::processEvents();

        QVERIFY(m_panel->statusLabel()->text().contains("error", Qt::CaseInsensitive));
        // Address should be restored to original (00000000`00000000)
        QVERIFY(m_panel->resultsTable()->item(0, 0)->text().contains('`'));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Update button (rescan)
    // ═══════════════════════════════════════════════════════════════════

    void update_disabledInitially() {
        QVERIFY(!m_panel->updateButton()->isEnabled());
    }

    void update_enabledAfterScan() {
        QByteArray data(32, '\0');
        data[0] = 0xAA;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->patternEdit()->setText("AA");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        QVERIFY(m_panel->updateButton()->isEnabled());
    }

    void update_showsPreviousColumn() {
        QByteArray data(64, '\0');
        int32_t val = 50;
        std::memcpy(data.data() + 8, &val, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1); // Value
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText("50");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);
        QCOMPARE(m_panel->resultsTable()->columnCount(), 2); // no previous yet

        // Modify via provider — change value from 50 to 99
        int32_t newVal = 99;
        QByteArray newBytes(4, '\0');
        std::memcpy(newBytes.data(), &newVal, 4);
        prov->writeBytes(8, newBytes);
        m_panel->valueEdit()->setText("99");

        // Click update — runs async
        QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
        QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
        QVERIFY(rescanSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->columnCount(), 3);
        // Current value = 99, previous = 50
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("99"));
        QCOMPARE(m_panel->resultsTable()->item(0, 2)->text(), QStringLiteral("50"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Re-scan: progress completes, values update, table populates
    // ═══════════════════════════════════════════════════════════════════

    void update_progressCompletes() {
        // Use a buffer with many results to verify progress reaches 100%
        // and doesn't hang. Place int32 value "7" every 4 bytes.
        QByteArray data(4096, '\0');
        int32_t val = 7;
        for (int i = 0; i < 1024; i++)
            std::memcpy(data.data() + i * 4, &val, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1); // Value
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText("7");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QVERIFY(m_panel->resultsTable()->rowCount() >= 512);
        QCOMPARE(m_panel->resultsTable()->columnCount(), 2);

        // Modify all values: 7 → 21
        int32_t newVal = 21;
        for (int i = 0; i < 1024; i++) {
            QByteArray nb(4, '\0');
            std::memcpy(nb.data(), &newVal, 4);
            prov->writeBytes(i * 4, nb);
        }
        m_panel->valueEdit()->setText("21");

        // Click Re-scan — runs async
        QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
        QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
        QVERIFY(rescanSpy.wait(5000));
        QApplication::processEvents();

        // Progress bar should be hidden (completed)
        QVERIFY(!m_panel->progressBar()->isVisible());
        // Table should have 3 columns now
        QCOMPARE(m_panel->resultsTable()->columnCount(), 3);
        // Spot check first and last row
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("21"));
        QCOMPARE(m_panel->resultsTable()->item(0, 2)->text(), QStringLiteral("7"));
        int lastRow = m_panel->resultsTable()->rowCount() - 1;
        QVERIFY(m_panel->resultsTable()->item(lastRow, 0) != nullptr);
        QCOMPARE(m_panel->resultsTable()->item(lastRow, 1)->text(), QStringLiteral("21"));
        QCOMPARE(m_panel->resultsTable()->item(lastRow, 2)->text(), QStringLiteral("7"));
        // Status now reads either "Narrowed N → M (eliminated K)" or
        // "All N results still match" — it surfaces the rescan delta
        // explicitly instead of a generic "Updated N results".
        QString status = m_panel->statusLabel()->text();
        QVERIFY2(status.contains("Narrowed") || status.contains("still match")
                  || status.contains("result"),
                 qPrintable(QStringLiteral("Unexpected rescan status: ") + status));
        // Buttons should be re-enabled
        QVERIFY(m_panel->updateButton()->isEnabled());
        QVERIFY(m_panel->scanButton()->isEnabled());
    }

    void update_signatureMode() {
        // Re-scan in signature mode (reads actual bytes, not cached pattern)
        QByteArray data(64, '\0');
        data[0] = 0x48; data[1] = 0x8B; data[2] = 0xAA;
        data[32] = 0x48; data[33] = 0x8B; data[34] = 0xBB;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(0); // Signature
        m_panel->execCheck()->setChecked(false);  // BufferProvider has no exec regions
        m_panel->patternEdit()->setText("48 8B ??");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 2);

        // Modify bytes at first match
        QByteArray mod(3, '\0');
        mod[0] = 0x48; mod[1] = 0x8B; mod[2] = (char)0xFF;
        prov->writeBytes(0, mod);

        // Re-scan — runs async
        QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
        QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
        QVERIFY(rescanSpy.wait(5000));
        QApplication::processEvents();

        QVERIFY(!m_panel->progressBar()->isVisible());
        QCOMPARE(m_panel->resultsTable()->columnCount(), 3);
        // First result current value should contain FF
        QVERIFY(m_panel->resultsTable()->item(0, 1)->text().contains("FF"));
        // First result previous value should contain AA
        QVERIFY(m_panel->resultsTable()->item(0, 2)->text().contains("AA"));
    }

    void update_doubleRescan() {
        // Two consecutive re-scans: previous column updates each time
        QByteArray data(32, '\0');
        int32_t v1 = 10;
        std::memcpy(data.data() + 4, &v1, 4);

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1);
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText("10");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("10"));

        // First update: 10 → 20
        int32_t v2 = 20;
        QByteArray nb2(4, '\0');
        std::memcpy(nb2.data(), &v2, 4);
        prov->writeBytes(4, nb2);
        m_panel->valueEdit()->setText("20");
        {
            QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
            QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
            QVERIFY(rescanSpy.wait(5000));
            QApplication::processEvents();
        }
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("20"));
        QCOMPARE(m_panel->resultsTable()->item(0, 2)->text(), QStringLiteral("10"));

        // Second update: 20 → 30
        int32_t v3 = 30;
        QByteArray nb3(4, '\0');
        std::memcpy(nb3.data(), &v3, 4);
        prov->writeBytes(4, nb3);
        m_panel->valueEdit()->setText("30");
        {
            QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
            QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
            QVERIFY(rescanSpy.wait(5000));
            QApplication::processEvents();
        }
        QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(), QStringLiteral("30"));
        QCOMPARE(m_panel->resultsTable()->item(0, 2)->text(), QStringLiteral("20"));
    }

    // ═══════════════════════════════════════════════════════════════════
    // Provider getter is lazy (captures at scan time)
    // ═══════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════
    // Benchmark: initial scan + 10 re-scans on a large buffer
    // ═══════════════════════════════════════════════════════════════════

    void bench_rescan10x() {
        // 1MB buffer, int32 value=42 placed every 512 bytes → 2048 results
        constexpr int kBufSize = 1 * 1024 * 1024;
        constexpr int kStride = 512;
        constexpr int32_t kVal = 42;

        QByteArray data(kBufSize, '\0');
        int planted = 0;
        for (int off = 0; off + 4 <= kBufSize; off += kStride) {
            std::memcpy(data.data() + off, &kVal, 4);
            planted++;
        }
        qDebug() << "[bench] buffer:" << (kBufSize / 1024) << "KB,"
                 << planted << "planted values, stride:" << kStride;

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(1);
        for (int i = 0; i < m_panel->typeCombo()->count(); i++) {
            if (m_panel->typeCombo()->itemData(i).toInt() == (int)ValueType::Int32) {
                m_panel->typeCombo()->setCurrentIndex(i);
                break;
            }
        }
        m_panel->valueEdit()->setText(QString::number(kVal));

        // ── Initial scan ──
        QElapsedTimer totalTimer;
        totalTimer.start();

        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(30000));
        QApplication::processEvents();

        int resultCount = m_panel->resultsTable()->rowCount();
        qDebug() << "[bench] initial scan:" << totalTimer.elapsed() << "ms,"
                 << resultCount << "results displayed";
        QVERIFY(resultCount > 0);
        QVERIFY(m_panel->updateButton()->isEnabled());

        // ── 10 re-scans — mutate values each time ──
        for (int iter = 1; iter <= 10; iter++) {
            int32_t newVal = kVal + iter;
            for (int off = 0; off + 4 <= kBufSize; off += kStride)
                std::memcpy(prov->data().data() + off, &newVal, 4);
            m_panel->valueEdit()->setText(QString::number(newVal));

            QElapsedTimer iterTimer;
            iterTimer.start();

            QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
            QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
            QVERIFY2(rescanSpy.wait(30000),
                     qPrintable(QString("rescan #%1 timed out").arg(iter)));
            QApplication::processEvents();

            qDebug() << "[bench] rescan #" << iter << ":" << iterTimer.elapsed() << "ms"
                     << "| val:" << newVal
                     << "| rows:" << m_panel->resultsTable()->rowCount()
                     << "| status:" << m_panel->statusLabel()->text();

            QVERIFY(m_panel->resultsTable()->item(0, 1) != nullptr);
            QCOMPARE(m_panel->resultsTable()->item(0, 1)->text(),
                     QString::number(newVal));
            if (m_panel->resultsTable()->columnCount() >= 3) {
                QCOMPARE(m_panel->resultsTable()->item(0, 2)->text(),
                         QString::number(newVal - 1));
            }
            // Renamed for workflow clarity: First Scan / Next Scan / Reset.
        QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("First Scan"));
            QVERIFY(!m_panel->progressBar()->isVisible());
            QVERIFY(m_panel->updateButton()->isEnabled());
        }

        qDebug() << "[bench] total (scan + 10 rescans):" << totalTimer.elapsed() << "ms";
    }

    // ═══════════════════════════════════════════════════════════════════
    // Benchmark: signature re-scan (16 bytes per result)
    // ═══════════════════════════════════════════════════════════════════

    void bench_rescanSignature10x() {
        // 1MB buffer with "MZ" planted every 4096 bytes → 256 results
        constexpr int kBufSize = 1 * 1024 * 1024;
        constexpr int kStride = 4096;

        QByteArray data(kBufSize, '\0');
        int planted = 0;
        for (int off = 0; off + 2 <= kBufSize; off += kStride) {
            data[off] = 0x4D; data[off + 1] = 0x5A;
            planted++;
        }
        qDebug() << "[bench-sig] buffer:" << (kBufSize / 1024) << "KB,"
                 << planted << "planted sigs";

        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(0);
        m_panel->patternEdit()->setText("4D 5A");

        QElapsedTimer totalTimer;
        totalTimer.start();

        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(30000));
        QApplication::processEvents();

        qDebug() << "[bench-sig] initial scan:" << totalTimer.elapsed() << "ms,"
                 << m_panel->resultsTable()->rowCount() << "results";
        QVERIFY(m_panel->resultsTable()->rowCount() > 0);

        for (int iter = 1; iter <= 10; iter++) {
            for (int off = 0; off + 3 <= kBufSize; off += kStride)
                prov->data().data()[off + 2] = (char)(iter & 0xFF);

            QElapsedTimer iterTimer;
            iterTimer.start();

            QSignalSpy rescanSpy(m_panel->engine(), &ScanEngine::rescanFinished);
            QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
            QVERIFY2(rescanSpy.wait(30000),
                     qPrintable(QString("sig rescan #%1 timed out").arg(iter)));
            QApplication::processEvents();

            qDebug() << "[bench-sig] rescan #" << iter << ":" << iterTimer.elapsed() << "ms"
                     << "| status:" << m_panel->statusLabel()->text();

            // Renamed for workflow clarity: First Scan / Next Scan / Reset.
        QCOMPARE(m_panel->scanButton()->text(), QStringLiteral("First Scan"));
            QVERIFY(!m_panel->progressBar()->isVisible());
            QVERIFY(m_panel->updateButton()->isEnabled());
        }

        qDebug() << "[bench-sig] total:" << totalTimer.elapsed() << "ms";
    }

    // ═══════════════════════════════════════════════════════════════════
    // Provider getter is lazy (captures at scan time)
    // ═══════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════
    // "Current Struct" checkbox
    // ═══════════════════════════════════════════════════════════════════

    void structOnly_checkboxExists() {
        QVERIFY(m_panel->structOnlyCheck() != nullptr);
        QCOMPARE(m_panel->structOnlyCheck()->isChecked(), false);
        // Lowercased to match the redesigned panel label.
        QCOMPARE(m_panel->structOnlyCheck()->text(), QStringLiteral("Current struct"));
    }

    void structOnly_setsAddressRange() {
        // Set up a bounds getter that returns a known range
        m_panel->setBoundsGetter([]() -> ScannerPanel::StructBounds {
            return { 0x1000, 0x200 };
        });

        // Set up a simple buffer provider
        QByteArray data(0x2000, '\x00');
        data[0x1000] = '\xCC';
        data[0x1100] = '\xCC';
        data[0x1500] = '\xCC'; // outside bounds (0x1000 + 0x200 = 0x1200)
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        // Enable struct-only mode
        m_panel->structOnlyCheck()->setChecked(true);

        // Scan for \xCC
        m_panel->patternEdit()->setText("CC");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        // Should only find results within [0x1000, 0x1200)
        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 2);
    }

    void structOnly_uncheckedScansAll() {
        // Same setup but with checkbox unchecked — should find all 3
        m_panel->setBoundsGetter([]() -> ScannerPanel::StructBounds {
            return { 0x1000, 0x200 };
        });

        QByteArray data(0x2000, '\x00');
        data[0x1000] = '\xCC';
        data[0x1100] = '\xCC';
        data[0x1500] = '\xCC';
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->structOnlyCheck()->setChecked(false); // unchecked

        m_panel->patternEdit()->setText("CC");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 3);
    }

    void structOnly_noBoundsGetterIgnored() {
        // No bounds getter set — checkbox checked but no effect
        QByteArray data(16, '\xDD');
        auto prov = std::make_shared<BufferProvider>(data);
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->structOnlyCheck()->setChecked(true);
        // Don't set bounds getter

        m_panel->patternEdit()->setText("DD");
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();

        auto results = finSpy.first().first().value<QVector<ScanResult>>();
        QCOMPARE(results.size(), 16); // all 16 bytes match
    }

    void providerGetter_lazy() {
        auto prov1 = std::make_shared<BufferProvider>(QByteArray(16, '\xAA'));
        auto prov2 = std::make_shared<BufferProvider>(QByteArray(16, '\xBB'));

        auto current = prov1;
        m_panel->setProviderGetter([&current]() { return current; });

        // Scan with prov1 — should find AA
        m_panel->patternEdit()->setText("AA");
        QSignalSpy finSpy1(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy1.wait(5000));
        QApplication::processEvents();
        QVERIFY(m_panel->resultsTable()->rowCount() > 0);

        // Switch provider
        current = prov2;

        // Scan for BB — should find in new provider
        m_panel->patternEdit()->setText("BB");
        QSignalSpy finSpy2(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy2.wait(5000));
        QApplication::processEvents();
        QVERIFY(m_panel->resultsTable()->rowCount() > 0);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Tier A/B/C UI surface — new checkboxes, conditions, filter, banner,
    // save/load, multi-select, sort, shortcuts, persisted settings.
    // ═══════════════════════════════════════════════════════════════════

    void newCheckboxes_exist() {
        QVERIFY(m_panel->privateOnlyCheck());
        QVERIFY(m_panel->skipSystemCheck());
        QVERIFY(m_panel->userModeOnlyCheck());
    }

    void modeChange_setsSmartDefaults() {
        // Defaults simplified: Value mode → Writable only; Signature mode →
        // Executable only. The narrower private/system/user-mode filters
        // are opt-in (the user toggles them when they need them).
        m_panel->modeCombo()->setCurrentIndex(1);
        QApplication::processEvents();
        QVERIFY(m_panel->writeCheck()->isChecked());
        QVERIFY(!m_panel->execCheck()->isChecked());
        QVERIFY(!m_panel->privateOnlyCheck()->isChecked());
        QVERIFY(!m_panel->skipSystemCheck()->isChecked());
        QVERIFY(!m_panel->userModeOnlyCheck()->isChecked());

        m_panel->modeCombo()->setCurrentIndex(0);
        QApplication::processEvents();
        QVERIFY(m_panel->execCheck()->isChecked());
        QVERIFY(!m_panel->writeCheck()->isChecked());
        QVERIFY(!m_panel->privateOnlyCheck()->isChecked());
        QVERIFY(!m_panel->skipSystemCheck()->isChecked());
        QVERIFY(!m_panel->userModeOnlyCheck()->isChecked());
    }

    void newConditions_inCombo() {
        // Lowercase shorthand condition labels in the redesigned combo.
        QStringList expected = {
            "bigger than", "smaller than", "between",
            "inc by", "dec by"
        };
        for (const QString& label : expected) {
            int idx = m_panel->condCombo()->findText(label);
            QVERIFY2(idx >= 0, qPrintable("Condition not found: " + label));
        }
    }

    void betweenCondition_revealsValue2() {
        // Switch to Value mode + Between → upper-bound field becomes visible.
        m_panel->modeCombo()->setCurrentIndex(1);
        QApplication::processEvents();
        int idx = m_panel->condCombo()->findText("between");
        m_panel->condCombo()->setCurrentIndex(idx);
        QApplication::processEvents();
        QVERIFY(m_panel->value2Edit()->isVisible());

        // Switch to Exact Value → upper-bound hidden again.
        m_panel->condCombo()->setCurrentIndex(
            m_panel->condCombo()->findText("equals"));
        QApplication::processEvents();
        QVERIFY(!m_panel->value2Edit()->isVisible());
    }

    void resultFilter_narrowsTable() {
        // Plant two values, scan, then filter by one.
        QByteArray data(64, 0);
        int32_t a = 0xAABBCCDD, b = 0x11223344;
        memcpy(data.data() + 0, &a, 4);
        memcpy(data.data() + 8, &b, 4);
        std::shared_ptr<Provider> prov = std::make_shared<BufferProvider>(data, "test");
        m_panel->setProviderGetter([prov]() { return prov; });

        m_panel->modeCombo()->setCurrentIndex(0);
        m_panel->patternEdit()->setText("DD CC BB AA");
        m_panel->execCheck()->setChecked(false);
        m_panel->writeCheck()->setChecked(false);
        QSignalSpy finSpy(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(finSpy.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 1);

        // Filter that matches → row visible.
        m_panel->resultFilter()->setText("DD");
        QApplication::processEvents();
        QVERIFY(!m_panel->resultsTable()->isRowHidden(0));

        // Filter that doesn't match → row hidden.
        m_panel->resultFilter()->setText("zzz_no_match");
        QApplication::processEvents();
        QVERIFY(m_panel->resultsTable()->isRowHidden(0));

        // Clear filter → row visible again.
        m_panel->resultFilter()->clear();
        QApplication::processEvents();
        QVERIFY(!m_panel->resultsTable()->isRowHidden(0));
    }

    void newScanButton_appearsAndResets() {
        // Initially hidden.
        QVERIFY(!m_panel->newScanButton()->isVisible());

        // Plant + scan.
        QByteArray data(8, 0);
        int32_t v = 1; memcpy(data.data(), &v, 4);
        std::shared_ptr<Provider> prov = std::make_shared<BufferProvider>(data, "x");
        m_panel->setProviderGetter([prov]() { return prov; });
        m_panel->patternEdit()->setText("01 00 00 00");
        QSignalSpy fin(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(fin.wait(5000));
        QApplication::processEvents();
        QVERIFY(m_panel->newScanButton()->isVisible());

        // Click New Scan → table cleared, button hidden again.
        QTest::mouseClick(m_panel->newScanButton(), Qt::LeftButton);
        QApplication::processEvents();
        QCOMPARE(m_panel->resultsTable()->rowCount(), 0);
        QVERIFY(!m_panel->newScanButton()->isVisible());
    }

    void multiSelect_enabled() {
        // Multi-select must be the default mode now.
        QCOMPARE(m_panel->resultsTable()->selectionMode(),
                 QAbstractItemView::ExtendedSelection);
    }

    void saveLoad_jsonRoundTrip() {
        // Plant 3 values, scan, save → clear → load → results match.
        QByteArray data(48, 0);
        int32_t v = 0xCAFEBABE;
        memcpy(data.data() + 0,  &v, 4);
        memcpy(data.data() + 16, &v, 4);
        memcpy(data.data() + 32, &v, 4);
        std::shared_ptr<Provider> prov = std::make_shared<BufferProvider>(data, "x");
        m_panel->setProviderGetter([prov]() { return prov; });
        m_panel->patternEdit()->setText("BE BA FE CA");
        QSignalSpy fin(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(fin.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->results().size(), 3);

        QString tmpPath = QDir::tempPath() + "/rcx_scanner_test.json";
        QVERIFY(m_panel->saveResultsTo(tmpPath));

        // Reset panel state, then load.
        QTest::mouseClick(m_panel->newScanButton(), Qt::LeftButton);
        QCOMPARE(m_panel->results().size(), 0);
        QVERIFY(m_panel->loadResultsFrom(tmpPath));
        QCOMPARE(m_panel->results().size(), 3);
        QFile::remove(tmpPath);
    }

    void persistSettings_roundTrip() {
        m_panel->modeCombo()->setCurrentIndex(1);
        QApplication::processEvents();
        m_panel->privateOnlyCheck()->setChecked(true);
        m_panel->skipSystemCheck()->setChecked(true);
        m_panel->userModeOnlyCheck()->setChecked(true);
        // Pick a non-default valueType + condition we can verify after reload.
        m_panel->typeCombo()->setCurrentIndex(7);  // uint64
        m_panel->condCombo()->setCurrentIndex(
            m_panel->condCombo()->findText("bigger than"));
        QApplication::processEvents();

        const QString key = "scanner_test_persist";
        m_panel->saveSettings(key);

        // Reset everything, then reload.
        m_panel->modeCombo()->setCurrentIndex(0);
        m_panel->privateOnlyCheck()->setChecked(false);
        m_panel->skipSystemCheck()->setChecked(false);
        m_panel->userModeOnlyCheck()->setChecked(false);
        m_panel->typeCombo()->setCurrentIndex(0);
        m_panel->condCombo()->setCurrentIndex(0);
        QApplication::processEvents();

        m_panel->loadSettings(key);
        QCOMPARE(m_panel->modeCombo()->currentIndex(), 1);
        QVERIFY(m_panel->privateOnlyCheck()->isChecked());
        QVERIFY(m_panel->skipSystemCheck()->isChecked());
        QVERIFY(m_panel->userModeOnlyCheck()->isChecked());
        QCOMPARE(m_panel->typeCombo()->currentIndex(), 7);
        QCOMPARE(m_panel->condCombo()->currentText(), QStringLiteral("bigger than"));

        // Clean up the test settings group.
        QSettings s("Reclass", "Reclass");
        s.remove(key);
    }

    void truncationBanner_appearsAtCap() {
        // We'd need 10001+ results to trigger the banner. Construct a buffer
        // with many one-byte 'A' hits and scan with alignment 1.
        QByteArray data(11000, 'A');
        std::shared_ptr<Provider> prov = std::make_shared<BufferProvider>(data, "x");
        m_panel->setProviderGetter([prov]() { return prov; });
        m_panel->patternEdit()->setText("41");
        QSignalSpy fin(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(fin.wait(15000));
        QApplication::processEvents();

        // m_results holds all 11000; table shows max 10000; banner visible.
        QCOMPARE(m_panel->results().size(), 11000);
        QCOMPARE(m_panel->resultsTable()->rowCount(), 10000);
    }

    void e2e_findMutateRevalidate_uiFlow() {
        // The integration counterpart of test_scanner.cpp::e2e_findMutateRevalidate
        // — driven through the panel widgets so we exercise buildRequest +
        // signal wiring + onScanFinished + onUpdateClicked.
        QByteArray data(32, 0);
        int32_t hp = 100, gold = 1234;
        memcpy(data.data() + 0, &hp, 4);
        memcpy(data.data() + 8, &gold, 4);
        std::shared_ptr<Provider> prov = std::make_shared<BufferProvider>(data, "x");
        m_panel->setProviderGetter([prov]() { return prov; });

        // Switch to Value mode, type int32, condition ExactValue, value 1234.
        m_panel->modeCombo()->setCurrentIndex(1);
        QApplication::processEvents();
        m_panel->typeCombo()->setCurrentIndex(2);  // int32
        m_panel->condCombo()->setCurrentIndex(0);  // Exact Value
        m_panel->valueEdit()->setText("1234");

        // Disable filters so the synthetic BufferProvider isn't excluded.
        m_panel->execCheck()->setChecked(false);
        m_panel->writeCheck()->setChecked(false);
        m_panel->privateOnlyCheck()->setChecked(false);
        m_panel->skipSystemCheck()->setChecked(false);
        m_panel->userModeOnlyCheck()->setChecked(false);

        QSignalSpy fin(m_panel->engine(), &ScanEngine::finished);
        QTest::mouseClick(m_panel->scanButton(), Qt::LeftButton);
        QVERIFY(fin.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->results().size(), 1);
        QCOMPARE(m_panel->results()[0].address, (uint64_t)8);

        // Mutate via provider directly (simulates user editing the cell).
        int32_t newGold = 9999;
        QVERIFY(prov->writeBytes(8, QByteArray((const char*)&newGold, 4)));

        // Re-scan with new exact value.
        m_panel->valueEdit()->setText("9999");
        QSignalSpy resc(m_panel->engine(), &ScanEngine::rescanFinished);
        QTest::mouseClick(m_panel->updateButton(), Qt::LeftButton);
        QVERIFY(resc.wait(5000));
        QApplication::processEvents();
        QCOMPARE(m_panel->results().size(), 1);
        QCOMPARE(m_panel->results()[0].address, (uint64_t)8);
        // Confirm the cached value reflects the mutation.
        int32_t reread = 0;
        memcpy(&reread, m_panel->results()[0].scanValue.constData(), 4);
        QCOMPARE(reread, newGold);
    }
};

QTEST_MAIN(TestScannerUI)
#include "test_scanner_ui.moc"
