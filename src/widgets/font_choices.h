#pragma once

// ── What the View ▸ Font menu may offer ──
//
// The menu used to be three hard-coded names: JetBrains Mono, IBM Plex Mono
// and Consolas. Two of those ship in resources.qrc and are therefore present
// on every platform; the third is a Windows font. On macOS or Linux that
// entry did not fail — it silently resolved to whatever the font matcher
// picked instead, so the menu offered a face the user could not identify and
// could not have asked for. And there was no way to reach anything else,
// however many good monospace faces were installed.
//
// So the list is DISCOVERED, not written down: the bundled pair first (a
// guarantee, on any platform, even with no fonts installed at all), then
// every fixed-pitch family the font database actually reports. That is the
// whole cross-platform story — nothing here names an operating system.
//
// Four filters stand between "the font database says so" and "worth
// offering", because the raw list on a normal Windows box is 32 rows of
// which about a dozen are real choices:
//
//   monospace   — the editor lays every column out on the advance of one
//                 character (offsets, type, name, hex bytes). A proportional
//                 face does not degrade that view, it destroys it. And the
//                 font's own claim is not enough: some lie, so we measure.
//   scalable    — 8514oem, Fixedsys, Terminal and friends are bitmap fonts
//                 with a couple of baked sizes. At the editor's size they
//                 render as something between wrong and unreadable.
//   one weight  — Qt reports every weight as its own family ("Cascadia Code",
//                 "Cascadia Code Light", "Cascadia Code SemiBold", …), which
//                 turns twelve faces into thirty-odd near-identical rows.
//   not ours    — Departure Mono ships for the ribbon's type glyphs. It is a
//                 PIXEL font: exactly one good size, integer multiples only.
//                 It has no business being offered as body text.
//
// Header-only and free of the widget tree so a test target can compile it —
// nothing that lives in src/main.cpp is testable (see docs), and "which
// families does this machine offer" is exactly the kind of thing worth
// pinning.

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QString>
#include <QStringList>

namespace rcx {

// The faces that ship inside the binary (src/resources.qrc → loaded by
// addApplicationFont in main()). They are the floor the menu can always
// offer: no system font, no network, no install step.
inline QStringList bundledMonoFamilies() {
    return { QStringLiteral("JetBrains Mono"), QStringLiteral("IBM Plex Mono") };
}

// Bundled for the ribbon's glyphs, not for reading code in. Kept out of the
// menu rather than out of the qrc — pixelglyphs.h needs it.
inline QStringList nonBodyBundledFamilies() {
    return { QStringLiteral("Departure Mono") };
}

// Does `family` actually advance every glyph by the same width?
//
// QFontDatabase::isFixedPitch reads the font's own claim, and fonts lie —
// a handful of decorative and icon families flag themselves fixed-pitch and
// are not. Measuring three glyphs of very different natural widths costs one
// QFontMetrics and settles it, which matters here because a face that slips
// through misaligns every column in the editor.
inline bool isTrulyMonospaced(const QString& family) {
    QFont f(family, 12);
    const QFontMetrics fm(f);
    const int w = fm.horizontalAdvance(QLatin1Char('i'));
    return w > 0
        && fm.horizontalAdvance(QLatin1Char('W')) == w
        && fm.horizontalAdvance(QLatin1Char('0')) == w;
}

// The weight words a font vendor tacks onto a family name. Ordered longest
// first so "ExtraLight" is stripped before "Light" would match inside it.
inline QStringList fontWeightWords() {
    return { QStringLiteral("ExtraLight"), QStringLiteral("UltraLight"),
             QStringLiteral("SemiLight"),  QStringLiteral("DemiLight"),
             QStringLiteral("ExtraBold"),  QStringLiteral("UltraBold"),
             QStringLiteral("SemiBold"),   QStringLiteral("DemiBold"),
             QStringLiteral("Medium"),     QStringLiteral("Light"),
             QStringLiteral("Black"),      QStringLiteral("Heavy"),
             QStringLiteral("Thin"),       QStringLiteral("Bold"),
             QStringLiteral("Book"),       QStringLiteral("Retina") };
}

// "Cascadia Code SemiBold" → "Cascadia Code"; anything else unchanged.
inline QString familyWithoutWeight(const QString& family) {
    for (const QString& w : fontWeightWords()) {
        const QString tail = QLatin1Char(' ') + w;
        if (family.size() > tail.size() && family.endsWith(tail, Qt::CaseInsensitive))
            return family.left(family.size() - tail.size());
    }
    return family;
}

// Every monospace family installed on THIS machine that is worth offering,
// minus the bundled ones (the menu pins those itself) — sorted, so the order
// does not depend on the font database's enumeration order, which differs
// per platform.
//
// Private families are skipped: macOS reports faces like ".AppleSystemUIFont"
// that are not the user's to pick.
inline QStringList systemMonoFamilies() {
    const QStringList bundled = bundledMonoFamilies();
    const QStringList notBody = nonBodyBundledFamilies();
    QStringList kept;
    for (const QString& family : QFontDatabase::families()) {
        if (family.startsWith(QLatin1Char('.'))) continue;      // private/system face
        if (QFontDatabase::isPrivateFamily(family)) continue;
        if (bundled.contains(family, Qt::CaseInsensitive)) continue;
        if (notBody.contains(family, Qt::CaseInsensitive)) continue;
        if (!QFontDatabase::isFixedPitch(family)) continue;
        if (!QFontDatabase::isSmoothlyScalable(family)) continue;   // bitmap relics
        if (!isTrulyMonospaced(family)) continue;                   // and liars
        kept << family;
    }
    // Weight spellings go only when their base family survived, so a face
    // whose base is missing keeps its full name and stays reachable.
    QStringList out;
    for (const QString& family : kept) {
        const QString base = familyWithoutWeight(family);
        if (base != family && kept.contains(base, Qt::CaseInsensitive)) continue;
        out << family;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// The family currently in force needs a row of its own when it is neither
// bundled nor installed — a settings file carried over from another machine,
// or a font uninstalled since. Without this the menu would show nothing
// checked and quietly disagree with what is on screen.
inline bool fontMenuNeedsInForceRow(const QString& current,
                                    const QStringList& bundled,
                                    const QStringList& system) {
    return !current.isEmpty()
        && !bundled.contains(current, Qt::CaseInsensitive)
        && !system.contains(current, Qt::CaseInsensitive);
}

}  // namespace rcx
