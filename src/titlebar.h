#pragma once
#include "themes/theme.h"
#include <QWidget>
#include <QMenuBar>
#include <QToolButton>
#include <QLabel>
#include <QHBoxLayout>

namespace rcx {

// Two-mode layout toggle: the chrome button is a plain checkable button
// — checked = workspace dock shown, unchecked = hidden. No dropdown.
// Values kept as an enum so callers stay self-documenting and to leave
// room for future presets without changing the signal signature.
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

    // Sync the toggle button's checked state when workspace visibility
    // changes via other paths (View menu, dock close button, etc.).
    void setWorkspaceChecked(bool on);

signals:
    // Emitted when the user clicks the layout toggle button.
    // Value is Layout_Workspace or Layout_NoWorkspace.
    void layoutPresetSelected(int preset);

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
    // Side-by-side workspace mode toggles. Exactly one checked at a time.
    // Off = editor-only icon, On = editor + sidebar icon.
    QToolButton* m_btnLayoutOff = nullptr;
    QToolButton* m_btnLayoutOn  = nullptr;
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
