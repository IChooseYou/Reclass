#pragma once
#include "themes/theme.h"
#include <QWidget>
#include <QMenuBar>
#include <QToolButton>
#include <QLabel>
#include <QHBoxLayout>

namespace rcx {

// Workspace layout presets, consumed by MainWindow::applyLayoutPreset.
// (The title-bar toggle buttons that used to emit these are gone — the
// Project dock's own close button and the collapsed rail replaced them —
// but the rail click, doc-tab context menu, and --screenshot still use
// the enum.) Kept as an enum so callers stay self-documenting and to
// leave room for future presets.
enum LayoutPreset {
    Layout_NoWorkspace = 0,  // workspace hidden
    Layout_Workspace   = 1,  // workspace visible
};

class TitleBarWidget : public QWidget {
    Q_OBJECT
public:
    explicit TitleBarWidget(QWidget* parent = nullptr);

    QMenuBar* menuBar() const { return m_menuBar; }
    void applyTheme(const Theme& theme);
    void setShowIcon(bool show);
    void setMenuBarTitleCase(bool titleCase);
    bool menuBarTitleCase() const { return m_titleCase; }
    void finalizeMenuBar();

    void updateMaximizeIcon();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QLabel*      m_appLabel   = nullptr;
    QMenuBar*    m_menuBar    = nullptr;
    QHBoxLayout* m_menuBtnLayout = nullptr;
    QVector<QToolButton*> m_menuButtons;
    QToolButton* m_btnMin     = nullptr;
    QToolButton* m_btnMax     = nullptr;
    QToolButton* m_btnClose   = nullptr;

    Theme m_theme;
    bool  m_titleCase = false;
    bool  m_useToolButtons = false;

    QToolButton* makeChromeButton(const QString& iconPath);
    void toggleMaximize();
};

} // namespace rcx
