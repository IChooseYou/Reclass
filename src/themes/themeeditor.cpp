#include "themeeditor.h"
#include "thememanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QComboBox>
#include <cmath>

namespace rcx {

// ── Color utilities ──

namespace {

double srgbLinear(double c) {
    return (c <= 0.03928) ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& c) {
    return 0.2126 * srgbLinear(c.redF())
         + 0.7152 * srgbLinear(c.greenF())
         + 0.0722 * srgbLinear(c.blueF());
}

double contrastRatio(const QColor& fg, const QColor& bg) {
    double l1 = relativeLuminance(fg);
    double l2 = relativeLuminance(bg);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05) / (l2 + 0.05);
}

QString wcagLevel(double ratio) {
    if (ratio >= 7.0) return QStringLiteral("AAA");
    if (ratio >= 4.5) return QStringLiteral("AA");
    return QStringLiteral("FAIL");
}

// Compute the minimum fg lightness (HSL L) to reach targetRatio against bg
QColor autoFixFg(const QColor& fg, const QColor& bg, double targetRatio) {
    double lBg = relativeLuminance(bg);

    // Determine if fg should be lighter or darker than bg
    bool fgLighter = relativeLuminance(fg) >= relativeLuminance(bg);

    double targetLum;
    if (fgLighter)
        targetLum = targetRatio * (lBg + 0.05) - 0.05;
    else
        targetLum = (lBg + 0.05) / targetRatio - 0.05;

    targetLum = qBound(0.0, targetLum, 1.0);

    // Binary search for HSL lightness that yields the target luminance
    int h, s, l, a;
    fg.getHsl(&h, &s, &l, &a);

    int lo = fgLighter ? l : 0;
    int hi = fgLighter ? 255 : l;

    for (int iter = 0; iter < 20; iter++) {
        int mid = (lo + hi) / 2;
        QColor test;
        test.setHsl(h, s, mid, a);
        double testLum = relativeLuminance(test);
        if (fgLighter) {
            if (testLum < targetLum) lo = mid + 1;
            else hi = mid;
        } else {
            if (testLum > targetLum) hi = mid - 1;
            else lo = mid;
        }
    }

    QColor result;
    result.setHsl(h, s, fgLighter ? hi : lo, a);
    return result;
}

} // anon

// ── Section header label ──

static QLabel* makeSectionLabel(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setStyleSheet(QStringLiteral(
        "font-weight: bold; font-size: 11px; color: #888;"
        "padding: 6px 0 2px 0; border-bottom: 1px solid #444;"));
    return lbl;
}

// ── Constructor ──

ThemeEditor::ThemeEditor(int themeIndex, QWidget* parent)
    : QDialog(parent), m_themeIndex(themeIndex)
{
    auto& tm = ThemeManager::instance();
    auto all = tm.themes();
    m_theme = (themeIndex >= 0 && themeIndex < all.size()) ? all[themeIndex] : tm.current();

    setWindowTitle(QStringLiteral("Theme Editor"));
    setMinimumSize(420, 480);
    resize(440, 640);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(6);

    // ── Theme selector combo ──
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Theme:")));
        m_themeCombo = new QComboBox;
        for (const auto& t : all)
            m_themeCombo->addItem(t.name);
        m_themeCombo->setCurrentIndex(themeIndex);
        connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int idx) { loadTheme(idx); });
        row->addWidget(m_themeCombo, 1);
        mainLayout->addLayout(row);
    }

    // ── Name field ──
    {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Name:")));
        m_nameEdit = new QLineEdit(m_theme.name);
        connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
            m_theme.name = t;
        });
        row->addWidget(m_nameEdit, 1);
        mainLayout->addLayout(row);
    }

    // ── File info ──
    m_fileInfoLabel = new QLabel;
    m_fileInfoLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 10px; padding: 0 0 4px 0;"));
    QString path = tm.themeFilePath(themeIndex);
    m_fileInfoLabel->setText(path.isEmpty()
        ? QStringLiteral("Built-in theme (edits save as user copy)")
        : QStringLiteral("File: %1").arg(path));
    mainLayout->addWidget(m_fileInfoLabel);

    // ── Scrollable area for swatches + contrast ──
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* scrollWidget = new QWidget;
    auto* scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setContentsMargins(0, 0, 6, 0);  // right margin for scrollbar
    scrollLayout->setSpacing(2);

    // ── Color swatches ──
    struct FieldDef { const char* label; QColor Theme::*ptr; };

    auto addGroup = [&](const QString& title, std::initializer_list<FieldDef> fields) {
        scrollLayout->addWidget(makeSectionLabel(title));
        for (const auto& f : fields) {
            int idx = m_swatches.size();

            auto* row = new QHBoxLayout;
            row->setSpacing(6);
            row->setContentsMargins(8, 1, 0, 1);

            auto* lbl = new QLabel(QString::fromLatin1(f.label));
            lbl->setFixedWidth(120);
            row->addWidget(lbl);

            auto* swatchBtn = new QPushButton;
            swatchBtn->setFixedSize(32, 18);
            swatchBtn->setCursor(Qt::PointingHandCursor);
            connect(swatchBtn, &QPushButton::clicked, this, [this, idx]() { pickColor(idx); });
            row->addWidget(swatchBtn);

            auto* hexLbl = new QLabel;
            hexLbl->setFixedWidth(60);
            hexLbl->setStyleSheet(QStringLiteral("color: #aaa; font-size: 10px;"));
            row->addWidget(hexLbl);

            row->addStretch();

            SwatchEntry se;
            se.label = f.label;
            se.field = f.ptr;
            se.swatchBtn = swatchBtn;
            se.hexLabel = hexLbl;
            m_swatches.append(se);

            scrollLayout->addLayout(row);
        }
    };

    addGroup("Chrome", {
        {"Background",     &Theme::background},
        {"Background Alt", &Theme::backgroundAlt},
        {"Surface",        &Theme::surface},
        {"Border",         &Theme::border},
        {"Button",         &Theme::button},
    });
    addGroup("Text", {
        {"Text",        &Theme::text},
        {"Text Dim",    &Theme::textDim},
        {"Text Muted",  &Theme::textMuted},
        {"Text Faint",  &Theme::textFaint},
    });
    addGroup("Interactive", {
        {"Hover",       &Theme::hover},
        {"Selected",    &Theme::selected},
        {"Selection",   &Theme::selection},
    });
    addGroup("Syntax", {
        {"Keyword",      &Theme::syntaxKeyword},
        {"Number",       &Theme::syntaxNumber},
        {"String",       &Theme::syntaxString},
        {"Comment",      &Theme::syntaxComment},
        {"Preprocessor", &Theme::syntaxPreproc},
        {"Type",         &Theme::syntaxType},
    });
    addGroup("Indicators", {
        {"Hover Span",    &Theme::indHoverSpan},
        {"Cmd Pill",      &Theme::indCmdPill},
        {"Data Changed",  &Theme::indDataChanged},
        {"Hint Green",    &Theme::indHintGreen},
    });
    addGroup("Markers", {
        {"Pointer",  &Theme::markerPtr},
        {"Cycle",    &Theme::markerCycle},
        {"Error",    &Theme::markerError},
    });

    // ── Contrast pairs ──
    scrollLayout->addWidget(makeSectionLabel(QStringLiteral("Contrast")));

    struct PairDef {
        const char* fgLabel; const char* bgLabel;
        QColor Theme::*fg; QColor Theme::*bg;
    };
    const PairDef pairs[] = {
        {"text",        "background",    &Theme::text,          &Theme::background},
        {"textDim",     "background",    &Theme::textDim,       &Theme::background},
        {"textMuted",   "background",    &Theme::textMuted,     &Theme::background},
        {"textFaint",   "background",    &Theme::textFaint,     &Theme::background},
        {"text",        "backgroundAlt", &Theme::text,          &Theme::backgroundAlt},
        {"text",        "surface",       &Theme::text,          &Theme::surface},
        {"keyword",     "background",    &Theme::syntaxKeyword, &Theme::background},
        {"type",        "background",    &Theme::syntaxType,    &Theme::background},
        {"number",      "background",    &Theme::syntaxNumber,  &Theme::background},
        {"string",      "background",    &Theme::syntaxString,  &Theme::background},
        {"comment",     "background",    &Theme::syntaxComment, &Theme::background},
        {"preproc",     "background",    &Theme::syntaxPreproc, &Theme::background},
        {"hoverSpan",   "background",    &Theme::indHoverSpan,  &Theme::background},
        {"hintGreen",   "background",    &Theme::indHintGreen,  &Theme::background},
    };

    for (int pi = 0; pi < (int)(sizeof(pairs) / sizeof(pairs[0])); pi++) {
        const auto& p = pairs[pi];
        int idx = m_contrastPairs.size();

        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        row->setContentsMargins(8, 1, 0, 1);

        auto* pairLabel = new QLabel(QStringLiteral("%1 / %2")
            .arg(QString::fromLatin1(p.fgLabel), QString::fromLatin1(p.bgLabel)));
        pairLabel->setFixedWidth(150);
        pairLabel->setStyleSheet(QStringLiteral("font-size: 10px;"));
        row->addWidget(pairLabel);

        auto* ratioLbl = new QLabel;
        ratioLbl->setFixedWidth(44);
        ratioLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        ratioLbl->setStyleSheet(QStringLiteral("font-size: 10px;"));
        row->addWidget(ratioLbl);

        auto* levelLbl = new QLabel;
        levelLbl->setFixedWidth(34);
        levelLbl->setAlignment(Qt::AlignCenter);
        row->addWidget(levelLbl);

        auto* fixBtn = new QPushButton(QStringLiteral("Fix"));
        fixBtn->setFixedSize(36, 18);
        fixBtn->setCursor(Qt::PointingHandCursor);
        fixBtn->setStyleSheet(QStringLiteral(
            "QPushButton { font-size: 9px; padding: 0; border: 1px solid #555; border-radius: 2px; }"
            "QPushButton:hover { background: #444; }"));
        fixBtn->hide();
        connect(fixBtn, &QPushButton::clicked, this, [this, idx]() { autoFixContrast(idx); });
        row->addWidget(fixBtn);

        row->addStretch();

        ContrastEntry ce;
        ce.fgLabel    = p.fgLabel;
        ce.bgLabel    = p.bgLabel;
        ce.fgField    = p.fg;
        ce.bgField    = p.bg;
        ce.ratioLabel = ratioLbl;
        ce.levelLabel = levelLbl;
        ce.fixBtn     = fixBtn;
        m_contrastPairs.append(ce);

        scrollLayout->addLayout(row);
    }

    scrollLayout->addStretch();
    scroll->setWidget(scrollWidget);
    mainLayout->addWidget(scroll, 1);

    // ── Bottom bar ──
    auto* bottomRow = new QHBoxLayout;
    m_previewBtn = new QPushButton(QStringLiteral("Live Preview"));
    m_previewBtn->setCheckable(true);
    connect(m_previewBtn, &QPushButton::toggled, this, [this](bool) { togglePreview(); });
    bottomRow->addWidget(m_previewBtn);

    bottomRow->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
        if (m_previewing) {
            ThemeManager::instance().revertPreview();
            m_previewing = false;
        }
        reject();
    });
    bottomRow->addWidget(buttons);
    mainLayout->addLayout(bottomRow);

    // Initial update
    for (int i = 0; i < m_swatches.size(); i++)
        updateSwatch(i);
    updateAllContrast();
}

// ── Load a different theme into the editor ──

void ThemeEditor::loadTheme(int index) {
    auto& tm = ThemeManager::instance();
    auto all = tm.themes();
    if (index < 0 || index >= all.size()) return;

    m_themeIndex = index;
    m_theme = all[index];
    m_nameEdit->setText(m_theme.name);

    QString path = tm.themeFilePath(index);
    m_fileInfoLabel->setText(path.isEmpty()
        ? QStringLiteral("Built-in theme (edits save as user copy)")
        : QStringLiteral("File: %1").arg(path));

    for (int i = 0; i < m_swatches.size(); i++)
        updateSwatch(i);
    updateAllContrast();

    if (m_previewing)
        tm.previewTheme(m_theme);
}

// ── Swatch update ──

void ThemeEditor::updateSwatch(int idx) {
    auto& s = m_swatches[idx];
    QColor c = m_theme.*s.field;

    s.swatchBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid #555; border-radius: 2px; }")
        .arg(c.name()));
    s.hexLabel->setText(c.name());
}

// ── Contrast update ──

void ThemeEditor::updateAllContrast() {
    for (int i = 0; i < m_contrastPairs.size(); i++) {
        auto& cp = m_contrastPairs[i];
        QColor fg = m_theme.*cp.fgField;
        QColor bg = m_theme.*cp.bgField;
        double ratio = contrastRatio(fg, bg);
        QString level = wcagLevel(ratio);

        cp.ratioLabel->setText(QStringLiteral("%1:1").arg(ratio, 0, 'f', 1));
        cp.levelLabel->setText(level);

        if (level == "AAA")
            cp.levelLabel->setStyleSheet(QStringLiteral("color: #4ec94e; font-weight: bold; font-size: 10px;"));
        else if (level == "AA")
            cp.levelLabel->setStyleSheet(QStringLiteral("color: #c9c94e; font-weight: bold; font-size: 10px;"));
        else
            cp.levelLabel->setStyleSheet(QStringLiteral("color: #c94e4e; font-weight: bold; font-size: 10px;"));

        cp.fixBtn->setVisible(level == "FAIL");
    }
}

// ── Color picker ──

void ThemeEditor::pickColor(int idx) {
    auto& s = m_swatches[idx];
    QColor c = QColorDialog::getColor(m_theme.*s.field, this, QString::fromLatin1(s.label));
    if (c.isValid()) {
        m_theme.*s.field = c;
        updateSwatch(idx);
        updateAllContrast();
        if (m_previewing)
            ThemeManager::instance().previewTheme(m_theme);
    }
}

// ── Auto-fix contrast ──

void ThemeEditor::autoFixContrast(int idx) {
    auto& cp = m_contrastPairs[idx];
    QColor fg = m_theme.*cp.fgField;
    QColor bg = m_theme.*cp.bgField;

    QColor fixed = autoFixFg(fg, bg, 4.6);  // slightly above 4.5 for margin
    m_theme.*cp.fgField = fixed;

    // Update the swatch that owns this fg color
    for (int i = 0; i < m_swatches.size(); i++) {
        if (m_swatches[i].field == cp.fgField) {
            updateSwatch(i);
            break;
        }
    }
    updateAllContrast();
    if (m_previewing)
        ThemeManager::instance().previewTheme(m_theme);
}

// ── Live preview toggle ──

void ThemeEditor::togglePreview() {
    m_previewing = m_previewBtn->isChecked();
    if (m_previewing)
        ThemeManager::instance().previewTheme(m_theme);
    else
        ThemeManager::instance().revertPreview();
}

} // namespace rcx
