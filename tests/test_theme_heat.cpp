// The heat ramp is derived against the PAPER it will be drawn on.
//
// None of the shipped themes name their own heat tokens, so every one of them
// takes the ramp Theme::fromJson derives. One ramp for all of them meant the
// amber that glows on near-black washed out to a pale orange on white — the
// light theme showed changing values in ink barely darker than the paper.
// These checks are on the derivation, so they see what the themes actually get.

#include <QtTest/QTest>
#include <QColor>
#include <QJsonObject>

#include "themes/theme.h"

#include <algorithm>
#include <cmath>

using namespace rcx;

namespace {

// WCAG relative luminance, and the contrast ratio built from it.
double luminance(const QColor& c) {
    auto lin = [](double v) {
        v /= 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Theme themeWith(const char* bg, const char* text, const char* dim) {
    QJsonObject o;
    o[QStringLiteral("name")] = QStringLiteral("Under Test");
    o[QStringLiteral("background")] = QLatin1String(bg);
    o[QStringLiteral("text")] = QLatin1String(text);
    o[QStringLiteral("textDim")] = QLatin1String(dim);
    return Theme::fromJson(o);
}

} // namespace

class TestThemeHeat : public QObject {
    Q_OBJECT

private slots:
    void heatReadsAgainstThePaper_data() {
        QTest::addColumn<QString>("bg");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("dim");
        QTest::newRow("dark")  << "#1e1e1e" << "#d4d4d4" << "#858585";
        QTest::newRow("light") << "#D4D0C8" << "#000000" << "#4a4a4a";
    }

    void heatReadsAgainstThePaper() {
        QFETCH(QString, bg);
        QFETCH(QString, text);
        QFETCH(QString, dim);
        const Theme t = themeWith(qPrintable(bg), qPrintable(text), qPrintable(dim));
        const QColor paper(bg);

        const QPair<const char*, QColor> ramp[] = {
            {"cold", t.indHeatCold}, {"warm", t.indHeatWarm}, {"hot", t.indHeatHot}};
        for (const auto& step : ramp) {
            QVERIFY2(step.second.isValid(), step.first);
            const double c = contrast(step.second, paper);
            QVERIFY2(c >= 3.0,
                     qPrintable(QStringLiteral("%1 %2 on %3: contrast %4, unreadable")
                                    .arg(QLatin1String(step.first), step.second.name(),
                                         paper.name()).arg(c, 0, 'f', 2)));
        }

        // A value that keeps moving is the loudest of the three.
        QVERIFY2(contrast(t.indHeatHot, paper) >= contrast(t.indHeatCold, paper),
                 "hot is quieter than cold");

        // And heat is its own ink: a changed value must not read as an
        // ordinary one that happens to be a shade off.
        QVERIFY2(contrast(t.indHeatWarm, QColor(text)) > 1.2,
                 qPrintable(QStringLiteral("warm %1 is the text colour %2")
                                .arg(t.indHeatWarm.name(), text)));
    }

    // A theme that names its own heat keeps them: the derivation only fills
    // what a theme left out.
    void aThemeThatNamesItsHeatKeepsIt() {
        QJsonObject o;
        o[QStringLiteral("name")] = QStringLiteral("Opinionated");
        o[QStringLiteral("background")] = QStringLiteral("#101010");
        o[QStringLiteral("indHeatWarm")] = QStringLiteral("#00ff00");
        const Theme t = Theme::fromJson(o);
        QCOMPARE(t.indHeatWarm.name().toLower(), QStringLiteral("#00ff00"));
    }
};

QTEST_MAIN(TestThemeHeat)
#include "test_theme_heat.moc"
