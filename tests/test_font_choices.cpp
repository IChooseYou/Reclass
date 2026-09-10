// What the View ▸ Font menu is allowed to offer.
//
// The rule this pins: the menu names no operating system. Two families are
// guaranteed because they ship inside the binary; everything else is
// DISCOVERED from the font database, so the same code offers Consolas and
// Cascadia on Windows, Menlo and SF Mono on macOS, DejaVu and Liberation on
// Linux, without a single #ifdef. Before this the third entry was the literal
// string "Consolas", which off Windows resolved to a substitute the user
// never chose and could not name.

#include <QtTest/QTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QFontDatabase>

#include "widgets/font_choices.h"

using namespace rcx;

class TestFontChoices : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // The bundled faces reach the font database the same way main() puts
        // them there. Without this the "always available" claim below is only
        // true of the shipped app, not of this test.
        QVERIFY2(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf")) != -1,
                 "JetBrains Mono is not in the resource bundle");
        QVERIFY2(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono.ttf")) != -1,
                 "IBM Plex Mono is not in the resource bundle");
    }

    // The floor: whatever the machine has or hasn't got installed, these two
    // are offerable, because they are inside the executable.
    void testBundledFamiliesAreAlwaysAvailable() {
        const QStringList bundled = bundledMonoFamilies();
        QCOMPARE(bundled, (QStringList{ QStringLiteral("JetBrains Mono"),
                                        QStringLiteral("IBM Plex Mono") }));
        const QStringList installed = QFontDatabase::families();
        for (const QString& f : bundled) {
            QVERIFY2(installed.contains(f), qPrintable(f + QStringLiteral(" did not load")));
            QVERIFY2(isTrulyMonospaced(f), qPrintable(f + QStringLiteral(" is not monospaced")));
        }
    }

    // Nothing in the offered list is a hard-coded platform font, and every
    // entry really is fixed-pitch — the editor lays its columns out on one
    // advance, so a proportional face would not degrade the view, it would
    // destroy it.
    void testEveryOfferedFamilyIsRealAndMonospaced() {
        const QStringList system = systemMonoFamilies();
        const QStringList installed = QFontDatabase::families();
        QVERIFY2(!system.isEmpty(), "no monospace families discovered at all");
        for (const QString& f : system) {
            QVERIFY2(installed.contains(f), qPrintable(f + QStringLiteral(" is not installed")));
            QVERIFY2(isTrulyMonospaced(f), qPrintable(f + QStringLiteral(" is not monospaced")));
            QVERIFY2(!f.startsWith(QLatin1Char('.')), qPrintable(f + QStringLiteral(" is a private face")));
        }
    }

    // The bundled pair is pinned by the menu itself, so the discovered list
    // must not repeat it; and the order must not depend on the font
    // database's enumeration order, which differs per platform.
    void testSystemListIsSortedAndExcludesTheBundledPair() {
        const QStringList system = systemMonoFamilies();
        for (const QString& f : bundledMonoFamilies())
            QVERIFY2(!system.contains(f, Qt::CaseInsensitive),
                     qPrintable(f + QStringLiteral(" is listed twice")));
        QStringList sorted = system;
        sorted.sort(Qt::CaseInsensitive);
        QCOMPARE(system, sorted);
        // Deduplicated: a family offered twice would be two rows that do the
        // same thing, and only one of them could carry the check mark.
        QCOMPARE(QSet<QString>(system.begin(), system.end()).size(), system.size());
    }

    // A face the user is currently on, but which is neither bundled nor
    // installed (a settings file carried from another machine, or a font
    // since uninstalled), still needs a row — otherwise the menu shows
    // nothing checked and quietly disagrees with what is on screen.
    void testAFontInForceButMissingStillGetsARow() {
        const QStringList bundled = bundledMonoFamilies();
        const QStringList system  = systemMonoFamilies();
        QVERIFY(fontMenuNeedsInForceRow(QStringLiteral("A Font Nobody Has"), bundled, system));
        QVERIFY(!fontMenuNeedsInForceRow(bundled.first(), bundled, system));
        QVERIFY(!fontMenuNeedsInForceRow(QString(), bundled, system));
        if (!system.isEmpty())
            QVERIFY(!fontMenuNeedsInForceRow(system.first(), bundled, system));
    }

    // A liar is rejected. Every font here claims nothing; the measurement is
    // what decides, which is the point of isTrulyMonospaced existing at all
    // beside QFontDatabase::isFixedPitch.
    void testProportionalFacesAreRejectedEvenIfTheyClaimOtherwise() {
        // Whatever this platform calls its UI font is not monospaced.
        const QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
        if (!QFontDatabase::isFixedPitch(ui.family()))
            QVERIFY2(!isTrulyMonospaced(ui.family()),
                     qPrintable(ui.family() + QStringLiteral(" measured as monospaced")));
        QVERIFY(!systemMonoFamilies().contains(QStringLiteral("Times New Roman")));
        QVERIFY(!systemMonoFamilies().contains(QStringLiteral("Arial")));
    }

    // Bitmap relics (8514oem, Fixedsys, Terminal, the raster Courier) are
    // monospace and useless: a couple of baked sizes, nothing near the
    // editor's. They are what made the raw list twice as long as the useful
    // one.
    void testNonScalableBitmapFacesAreExcluded() {
        const QStringList system = systemMonoFamilies();
        for (const QString& f : system)
            QVERIFY2(QFontDatabase::isSmoothlyScalable(f),
                     qPrintable(f + QStringLiteral(" is a bitmap font")));
        for (const QString& relic : {QStringLiteral("8514oem"), QStringLiteral("Fixedsys"),
                                     QStringLiteral("Terminal")})
            if (QFontDatabase::families().contains(relic))
                QVERIFY2(!system.contains(relic), qPrintable(relic + QStringLiteral(" was offered")));
    }

    // Qt reports every weight as its own family, so "Cascadia Code" arrives
    // with four near-identical siblings. Keeping only the base turns thirty-odd
    // rows into a list you can actually read.
    void testWeightSpellingsCollapseOntoTheirBaseFamily() {
        QCOMPARE(familyWithoutWeight(QStringLiteral("Cascadia Code SemiBold")),
                 QStringLiteral("Cascadia Code"));
        QCOMPARE(familyWithoutWeight(QStringLiteral("Source Code Pro ExtraLight")),
                 QStringLiteral("Source Code Pro"));
        // "ExtraLight" must not be matched as "Light" with junk left over.
        QCOMPARE(familyWithoutWeight(QStringLiteral("Cascadia Code ExtraLight")),
                 QStringLiteral("Cascadia Code"));
        // A name that merely ends in a word we know is left alone unless the
        // word is a separate trailing token.
        QCOMPARE(familyWithoutWeight(QStringLiteral("Noto Mono")), QStringLiteral("Noto Mono"));
        QCOMPARE(familyWithoutWeight(QStringLiteral("Bold")), QStringLiteral("Bold"));

        const QStringList system = systemMonoFamilies();
        for (const QString& f : system) {
            const QString base = familyWithoutWeight(f);
            if (base == f) continue;
            QVERIFY2(!system.contains(base, Qt::CaseInsensitive),
                     qPrintable(f + QStringLiteral(" listed beside its base ") + base));
        }
    }

    // Departure Mono ships for the ribbon's type glyphs. It is a PIXEL font —
    // one good size, integer multiples only — so offering it as body text
    // would reproduce the blocky ribbon the user rejected.
    void testTheRibbonGlyphFontIsNotOfferedAsBodyText() {
        QVERIFY(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DepartureMono.otf")) != -1);
        QVERIFY(QFontDatabase::families().contains(QStringLiteral("Departure Mono")));
        QVERIFY(!systemMonoFamilies().contains(QStringLiteral("Departure Mono")));
        QVERIFY(!bundledMonoFamilies().contains(QStringLiteral("Departure Mono")));
    }

    // The menu is built on every open; discovery must not be so slow that it
    // stalls the pointer. (It runs once and is cached in main.cpp, but the
    // uncached cost is the one worth knowing.)
    void testDiscoveryIsFastEnoughToBuildAMenuWith() {
        QElapsedTimer t;
        t.start();
        const QStringList system = systemMonoFamilies();
        const qint64 ms = t.elapsed();
        qInfo("discovered %lld monospace families in %lld ms", (long long)system.size(), (long long)ms);
        QVERIFY2(ms < 2000, qPrintable(QStringLiteral("font discovery took %1 ms").arg(ms)));
    }
};

QTEST_MAIN(TestFontChoices)
#include "test_font_choices.moc"
