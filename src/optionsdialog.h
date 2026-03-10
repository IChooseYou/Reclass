#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QHash>
#include <QSpinBox>

namespace rcx {

struct OptionsResult {
    int     themeIndex = 0;
    QString fontName;
    bool    menuBarTitleCase = true;
    bool    showIcon = false;
    bool    autoStartMcp = true;
    int     refreshMs = 660;
    bool    generatorAsserts = false;
    bool    braceWrap = false;
};

class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(const OptionsResult& current, QWidget* parent = nullptr);

    OptionsResult result() const;
    void selectPage(int index);

private:
    void filterTree(const QString& text);
    static QStringList collectPageKeywords(QWidget* page);

    QLineEdit*      m_search         = nullptr;
    QTreeWidget*    m_tree           = nullptr;
    QStackedWidget* m_pages          = nullptr;
    QComboBox*      m_themeCombo     = nullptr;
    QComboBox*      m_fontCombo      = nullptr;
    QCheckBox*      m_titleCaseCheck = nullptr;
    QCheckBox*      m_showIconCheck  = nullptr;
    QCheckBox*      m_autoMcpCheck   = nullptr;
    QSpinBox*       m_refreshSpin    = nullptr;
    QCheckBox*      m_assertCheck    = nullptr;
    QCheckBox*      m_braceWrapCheck = nullptr;

    // searchable keywords per leaf tree item
    QHash<QTreeWidgetItem*, QStringList> m_pageKeywords;
    // tree item → stacked widget page index
    QHash<QTreeWidgetItem*, int> m_itemPageIndex;
};

} // namespace rcx
