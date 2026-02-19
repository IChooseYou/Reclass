#pragma once
#include "controller.h"
#include "titlebar.h"
#include "pluginmanager.h"
#include <QMainWindow>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QDockWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QMap>
#include <Qsci/qsciscintilla.h>

namespace rcx {

class McpBridge;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class McpBridge;
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void newClass();
    void newStruct();
    void newEnum();
    void selfTest();
    void openFile();
    void saveFile();
    void saveFileAs();
    void closeFile();

    void addNode();
    void removeNode();
    void changeNodeType();
    void renameNodeAction();
    void duplicateNodeAction();
    void splitView();
    void unsplitView();

    void undo();
    void redo();
    void about();
    void toggleMcp();
    void setEditorFont(const QString& fontName);
    void exportCpp();
    void exportReclassXmlAction();
    void importFromSource();
    void importReclassXml();
    void showTypeAliasesDialog();
    void editTheme();
    void showOptionsDialog();

public:
    // Project Lifecycle API
    QMdiSubWindow* project_new(const QString& classKeyword = QString());
    QMdiSubWindow* project_open(const QString& path = {});
    bool project_save(QMdiSubWindow* sub = nullptr, bool saveAs = false);
    void project_close(QMdiSubWindow* sub = nullptr);

private:
    enum ViewMode { VM_Reclass, VM_Rendered };

    QMdiArea*       m_mdiArea;
    QLabel*         m_statusLabel;
    TitleBarWidget* m_titleBar = nullptr;
    QWidget*        m_borderOverlay = nullptr;
    PluginManager   m_pluginManager;
    McpBridge*      m_mcp       = nullptr;
    QAction*        m_mcpAction = nullptr;
    QMenu*          m_sourceMenu = nullptr;

    struct SplitPane {
        QTabWidget*    tabWidget = nullptr;
        RcxEditor*     editor    = nullptr;
        QsciScintilla* rendered  = nullptr;
        ViewMode       viewMode  = VM_Reclass;
        uint64_t       lastRenderedRootId = 0;
    };

    struct TabState {
        RcxDocument*       doc;
        RcxController*     ctrl;
        QSplitter*         splitter;
        QVector<SplitPane> panes;
        int                activePaneIdx = 0;
    };
    QMap<QMdiSubWindow*, TabState> m_tabs;
    QVector<RcxDocument*> m_allDocs;  // all open docs, shared with controllers
    void rebuildAllDocs();

    void createMenus();
    void createStatusBar();
    void showPluginsDialog();
    void populateSourceMenu();
    QIcon makeIcon(const QString& svgPath);

    RcxController* activeController() const;
    TabState* activeTab();
    TabState* tabByIndex(int index);
    int tabCount() const { return m_tabs.size(); }
    QMdiSubWindow* createTab(RcxDocument* doc);
    void updateWindowTitle();

    void setViewMode(ViewMode mode);
    void updateRenderedView(TabState& tab, SplitPane& pane);
    void updateAllRenderedPanes(TabState& tab);
    uint64_t findRootStructForNode(const NodeTree& tree, uint64_t nodeId) const;
    void setupRenderedSci(QsciScintilla* sci);

    SplitPane createSplitPane(TabState& tab);
    void applyTheme(const Theme& theme);
    void applyTabWidgetStyle(QTabWidget* tw);
    void styleTabCloseButtons();
    SplitPane* findPaneByTabWidget(QTabWidget* tw);
    SplitPane* findActiveSplitPane();
    RcxEditor* activePaneEditor();

    // Workspace dock
    QDockWidget*        m_workspaceDock  = nullptr;
    QTreeView*          m_workspaceTree  = nullptr;
    QStandardItemModel* m_workspaceModel = nullptr;
    void createWorkspaceDock();
    void rebuildWorkspaceModel();
    void updateBorderColor(const QColor& color);

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
};

} // namespace rcx
