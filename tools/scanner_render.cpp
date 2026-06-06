// Offscreen render harness for the Memory Scanner panel. Builds the panel,
// themes it, optionally loads a results JSON to populate the table (so Reset /
// Go to / Copy show), and grabs it to a PNG — works under `-platform offscreen`
// with no display, so the redesign can be eyeballed deterministically.
//
// Usage: scanner_render <out.png> [results.json]
#include <QApplication>
#include <QFont>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include "scannerpanel.h"
#include "themes/thememanager.h"

using namespace rcx;

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ScannerPanel panel;
    panel.applyTheme(ThemeManager::instance().current());
    QFont mono(QStringLiteral("Consolas"), 10);
    panel.setEditorFont(mono);
    panel.resize(460, 780);

    // Optional 2nd arg: a condition label to select (e.g. "Between",
    // "Exact Sig") so signature/between states can be rendered too.
    const QString arg2 = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QString();
    const QString arg3 = (argc > 3) ? QString::fromLocal8Bit(argv[3]) : QString();
    if (arg2.endsWith(QStringLiteral(".json"))) {
        panel.loadResultsFrom(arg2);  // populate table + Reset
        auto* tbl = panel.resultsTable();
        if (tbl->rowCount() > 0) {
            if (arg3 == QStringLiteral("edit")) {
                // Open the value-cell editor inline and report its alignment —
                // verifies AlignedEditorDelegate keeps it right-aligned
                // (AlignRight|AlignVCenter == 0x82 == 130).
                tbl->openPersistentEditor(tbl->item(0, 1));
                app.processEvents();
                if (auto* le = tbl->findChild<QLineEdit*>())
                    qInfo("VALUE EDITOR alignment = 0x%X (AlignRight=0x2)",
                          (unsigned)le->alignment());
                else
                    qInfo("VALUE EDITOR: no QLineEdit found");
            } else {
                // Select the first data row so the selection-highlight color is
                // visible (verify it matches the editor's grey, not blue).
                tbl->selectRow(0);
            }
        }
    } else if (arg2 == QStringLiteral("collapsed")) {
        // Click the SCAN FOR header to fold the criteria body (verify collapse).
        for (auto* b : panel.findChildren<QPushButton*>())
            if (b->property("scanForHeader").toBool()) { b->click(); break; }
    } else if (!arg2.isEmpty()) {
        int idx = panel.condCombo()->findText(arg2);
        if (idx >= 0) panel.condCombo()->setCurrentIndex(idx);
    }

    panel.show();
    app.processEvents();
    app.processEvents();

    const QString out = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("scanner_render.png");
    panel.grab().save(out);
    return 0;
}
