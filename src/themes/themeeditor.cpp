#include "themeeditor.h"
#include "thememanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QComboBox>
#include <cstring>

namespace rcx {

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
    m_fileInfoLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 10px; padding: 0 0 4px 0;")
        .arg(tm.current().textDim.name()));
    QString path = tm.themeFilePath(themeIndex);
    m_fileInfoLabel->setText(path.isEmpty()
        ? QStringLiteral("Built-in theme (edits save as user copy)")
        : QStringLiteral("File: %1").arg(path));
    mainLayout->addWidget(m_fileInfoLabel);

    // ── Scrollable area for swatches ──
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* scrollWidget = new QWidget;
    auto* scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setContentsMargins(0, 0, 6, 0);  // right margin for scrollbar
    scrollLayout->setSpacing(2);

    // ── Color swatches (driven by kThemeFields) ──
    const char* currentGroup = nullptr;
    for (int fi = 0; fi < kThemeFieldCount; fi++) {
        const auto& f = kThemeFields[fi];

        // Section header on group change
        if (!currentGroup || std::strcmp(currentGroup, f.group) != 0) {
            scrollLayout->addWidget(makeSectionLabel(QString::fromLatin1(f.group)));
            currentGroup = f.group;
        }

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
        hexLbl->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;")
            .arg(tm.current().textMuted.name()));
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

    scrollLayout->addStretch();
    scroll->setWidget(scrollWidget);
    mainLayout->addWidget(scroll, 1);

    // ── Bottom bar ──
    auto* bottomRow = new QHBoxLayout;
    bottomRow->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, [this]() {
        ThemeManager::instance().revertPreview();
        reject();
    });
    bottomRow->addWidget(buttons);
    mainLayout->addLayout(bottomRow);

    // Initial swatch update + start live preview
    for (int i = 0; i < m_swatches.size(); i++)
        updateSwatch(i);
    tm.previewTheme(m_theme);
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

// ── Color picker ──

void ThemeEditor::pickColor(int idx) {
    auto& s = m_swatches[idx];
    QColor c = QColorDialog::getColor(m_theme.*s.field, this, QString::fromLatin1(s.label));
    if (c.isValid()) {
        m_theme.*s.field = c;
        updateSwatch(idx);
        ThemeManager::instance().previewTheme(m_theme);
    }
}

} // namespace rcx
