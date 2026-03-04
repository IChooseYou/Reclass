#include "optionsdialog.h"
#include "themes/thememanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QGroupBox>
#include <QLabel>
#include <QTreeWidgetItem>
#include <functional>

namespace rcx {

OptionsDialog::OptionsDialog(const OptionsResult& current, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Options");
    setFixedSize(700, 450);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // -- Middle: left column (search + tree) | right column (pages) --
    auto* middleLayout = new QHBoxLayout;
    middleLayout->setSpacing(8);

    // Left column: search bar + tree
    auto* leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(4);

    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search Options (Ctrl+E)");
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &OptionsDialog::filterTree);
    leftColumn->addWidget(m_search);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setFixedWidth(200);
    m_tree->setMouseTracking(true);
    m_tree->setIconSize(QSize(16, 16));
    {
        const auto& t = ThemeManager::instance().current();
        QPalette tp = m_tree->palette();
        tp.setColor(QPalette::Text, t.textDim);
        tp.setColor(QPalette::Highlight, t.hover);
        tp.setColor(QPalette::HighlightedText, t.text);
        m_tree->setPalette(tp);
    }

    auto* envItem = new QTreeWidgetItem(m_tree, {"Environment"});
    envItem->setIcon(0, QIcon(":/vsicons/folder.svg"));
    auto* generalItem = new QTreeWidgetItem(envItem, {"General"});
    generalItem->setIcon(0, QIcon(":/vsicons/settings-gear.svg"));
    m_tree->expandAll();
    m_tree->setCurrentItem(generalItem);
    leftColumn->addWidget(m_tree, 1);

    middleLayout->addLayout(leftColumn);

    // Right column: stacked pages with group boxes
    m_pages = new QStackedWidget;

    // -- General page --
    auto* generalPage = new QWidget;
    auto* generalLayout = new QVBoxLayout(generalPage);
    generalLayout->setContentsMargins(0, 0, 0, 0);
    generalLayout->setSpacing(8);

    // Refresh Rate group box
    auto* refreshGroup = new QGroupBox("Refresh Rate");
    auto* refreshLayout = new QFormLayout(refreshGroup);
    refreshLayout->setSpacing(8);
    refreshLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_refreshSpin = new QSpinBox;
    m_refreshSpin->setRange(1, 60000);
    m_refreshSpin->setSingleStep(50);
    m_refreshSpin->setValue(current.refreshMs);
    m_refreshSpin->setSuffix(" ms");
    m_refreshSpin->setObjectName("refreshSpin");
    refreshLayout->addRow("Interval:", m_refreshSpin);

    auto* refreshDesc = new QLabel(
        "How often live memory is re-read and the view is updated, in milliseconds. "
        "Lower values give faster updates but use more CPU. Default: 660 ms.");
    refreshDesc->setWordWrap(true);
    refreshDesc->setContentsMargins(0, 0, 0, 0);
    refreshLayout->addRow(refreshDesc);

    generalLayout->addWidget(refreshGroup);

    // Visual Experience group box
    auto* visualGroup = new QGroupBox("Visual Experience");
    auto* visualLayout = new QFormLayout(visualGroup);
    visualLayout->setSpacing(8);
    visualLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_themeCombo = new QComboBox;
    auto& tm = ThemeManager::instance();
    for (const auto& theme : tm.themes())
        m_themeCombo->addItem(theme.name);
    m_themeCombo->setCurrentIndex(current.themeIndex);
    m_themeCombo->setObjectName("themeCombo");
    visualLayout->addRow("Color theme:", m_themeCombo);

    m_fontCombo = new QComboBox;
    m_fontCombo->addItem("JetBrains Mono");
    m_fontCombo->addItem("Consolas");
    m_fontCombo->setCurrentText(current.fontName);
    m_fontCombo->setObjectName("fontCombo");
    visualLayout->addRow("Editor Font:", m_fontCombo);

    m_titleCaseCheck = new QCheckBox("Uppercase menu items");
    m_titleCaseCheck->setChecked(current.menuBarTitleCase);
    visualLayout->addRow(m_titleCaseCheck);

    m_showIconCheck = new QCheckBox("Show icon in title bar");
    m_showIconCheck->setChecked(current.showIcon);
    visualLayout->addRow(m_showIconCheck);

    generalLayout->addWidget(visualGroup);
    generalLayout->addStretch();

    m_pages->addWidget(generalPage);                     // index 0
    m_pageKeywords[generalItem] = collectPageKeywords(generalPage);

    // -- AI Features page --
    auto* aiItem = new QTreeWidgetItem(envItem, {"AI Features"});
    aiItem->setIcon(0, QIcon(":/vsicons/remote.svg"));

    auto* aiPage = new QWidget;
    auto* aiLayout = new QVBoxLayout(aiPage);
    aiLayout->setContentsMargins(0, 0, 0, 0);
    aiLayout->setSpacing(8);

    auto* mcpGroup = new QGroupBox("MCP Server");
    auto* mcpLayout = new QVBoxLayout(mcpGroup);
    mcpLayout->setSpacing(4);

    m_autoMcpCheck = new QCheckBox("Auto-start MCP server");
    m_autoMcpCheck->setChecked(current.autoStartMcp);
    mcpLayout->addWidget(m_autoMcpCheck);

    auto* mcpDesc = new QLabel(
        "Automatically start the MCP bridge server when the application launches, "
        "allowing external AI tools to connect and interact with the editor.");
    mcpDesc->setWordWrap(true);
    mcpDesc->setContentsMargins(20, 0, 0, 0);
    mcpLayout->addWidget(mcpDesc);

    aiLayout->addWidget(mcpGroup);
    aiLayout->addStretch();

    m_pages->addWidget(aiPage);                          // index 1
    m_pageKeywords[aiItem] = collectPageKeywords(aiPage);

    // -- Generator page --
    auto* generatorItem = new QTreeWidgetItem(envItem, {"Generator"});
    generatorItem->setIcon(0, QIcon(":/vsicons/code.svg"));

    auto* generatorPage = new QWidget;
    auto* generatorLayout = new QVBoxLayout(generatorPage);
    generatorLayout->setContentsMargins(0, 0, 0, 0);
    generatorLayout->setSpacing(8);

    auto* cppGroup = new QGroupBox("C++ Header");
    auto* cppLayout = new QVBoxLayout(cppGroup);
    m_assertCheck = new QCheckBox("Emit static_assert size checks");
    m_assertCheck->setChecked(current.generatorAsserts);
    cppLayout->addWidget(m_assertCheck);
    generatorLayout->addWidget(cppGroup);

    generatorLayout->addStretch();

    m_pages->addWidget(generatorPage);                   // index 2
    m_pageKeywords[generatorItem] = collectPageKeywords(generatorPage);

    middleLayout->addWidget(m_pages, 1);

    mainLayout->addLayout(middleLayout, 1);

    // Tree <-> page connection
    m_itemPageIndex[generalItem] = 0;
    m_itemPageIndex[aiItem] = 1;
    m_itemPageIndex[generatorItem] = 2;
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!item) return;
        auto it = m_itemPageIndex.find(item);
        if (it != m_itemPageIndex.end())
            m_pages->setCurrentIndex(it.value());
    });

    // -- Button box --
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

}

OptionsResult OptionsDialog::result() const {
    OptionsResult r;
    r.themeIndex = m_themeCombo->currentIndex();
    r.fontName = m_fontCombo->currentText();
    r.menuBarTitleCase = m_titleCaseCheck->isChecked();
    r.showIcon = m_showIconCheck->isChecked();
    r.autoStartMcp = m_autoMcpCheck->isChecked();
    r.refreshMs = m_refreshSpin->value();
    r.generatorAsserts = m_assertCheck->isChecked();
    return r;
}

QStringList OptionsDialog::collectPageKeywords(QWidget* page) {
    QStringList keywords;
    for (auto* child : page->findChildren<QWidget*>()) {
        if (auto* label = qobject_cast<QLabel*>(child))
            keywords << label->text();
        else if (auto* cb = qobject_cast<QCheckBox*>(child))
            keywords << cb->text();
        else if (auto* gb = qobject_cast<QGroupBox*>(child))
            keywords << gb->title();
        else if (auto* combo = qobject_cast<QComboBox*>(child)) {
            for (int i = 0; i < combo->count(); ++i)
                keywords << combo->itemText(i);
        }
    }
    return keywords;
}

void OptionsDialog::filterTree(const QString& text) {
    std::function<bool(QTreeWidgetItem*)> filter = [&](QTreeWidgetItem* item) -> bool {
        bool anyChildVisible = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (filter(item->child(i)))
                anyChildVisible = true;
        }

        bool selfMatch = item->text(0).contains(text, Qt::CaseInsensitive);
        if (!selfMatch) {
            for (const auto& kw : m_pageKeywords.value(item)) {
                if (kw.contains(text, Qt::CaseInsensitive)) {
                    selfMatch = true;
                    break;
                }
            }
        }
        bool visible = selfMatch || anyChildVisible;
        item->setHidden(!visible);

        if (visible && item->childCount() > 0)
            item->setExpanded(true);

        return visible;
    };

    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        filter(m_tree->topLevelItem(i));
}

} // namespace rcx
