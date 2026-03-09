#pragma once
#include "controller.h"
#include "titlebar.h"
#include "pluginmanager.h"
#include "scannerpanel.h"
#include "startpage.h"
#include "workspace_model.h"
#include <QMainWindow>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QDockWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QMap>
#include <QButtonGroup>
#include <QPushButton>
#include <QTimer>
#include <Qsci/qsciscintilla.h>

namespace rcx {

class McpBridge;
class ShimmerLabel;
class DockGripWidget;
class WorkspaceDelegate;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class McpBridge;
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

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
    void importPdb();
    void showTypeAliasesDialog();
    void editTheme();
    void showOptionsDialog();

public:
    // Status bar helpers — separate app / MCP channels
    void setAppStatus(const QString& text);
    void setAppStatus(const QString& text, const QString& dimSuffix);
    void setMcpStatus(const QString& text);
    void clearMcpStatus();

    // Project Lifecycle API
    QDockWidget* project_new(const QString& classKeyword = QString());
    QDockWidget* project_open(const QString& path = {});
    bool project_save(QDockWidget* dock = nullptr, bool saveAs = false);
    void project_close(QDockWidget* dock = nullptr);

private:
    enum ViewMode { VM_Reclass, VM_Rendered };

    QWidget*        m_centralPlaceholder;
    ShimmerLabel*   m_statusLabel;
    QString         m_appStatus;
    QString         m_appStatusDim;
    bool            m_mcpBusy   = false;
    QTimer*         m_mcpClearTimer = nullptr;
    TitleBarWidget* m_titleBar = nullptr;
    QMenuBar*       m_menuBar = nullptr;
    bool            m_menuBarTitleCase = false;
    QWidget*        m_borderOverlay = nullptr;
    PluginManager   m_pluginManager;
    McpBridge*      m_mcp       = nullptr;
    QAction*        m_mcpAction = nullptr;
    QAction*        m_actRelOfs = nullptr;
    QMenu*          m_sourceMenu = nullptr;
    QMenu*          m_recentFilesMenu = nullptr;

    struct SplitPane {
        QTabWidget*    tabWidget = nullptr;
        RcxEditor*     editor    = nullptr;
        QsciScintilla* rendered  = nullptr;
        QLineEdit*     findBar   = nullptr;
        QWidget*       findContainer = nullptr;
        QWidget*       renderedContainer = nullptr;
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
    QMap<QDockWidget*, TabState> m_tabs;
    QVector<QDockWidget*> m_docDocks;       // ordered list for tabByIndex
    QDockWidget* m_activeDocDock = nullptr;  // tracks active document dock
    QDockWidget* m_sentinelDock  = nullptr;  // hidden dock to bootstrap tab bar creation
    QVector<RcxDocument*> m_allDocs;  // all open docs, shared with controllers
    bool m_closingAll = false;        // guards spurious project_new during batch close
    bool m_tabBarShowGuard = false;   // prevents recursion in event filter re-show
    struct ClosingGuard {
        bool& flag;
        ClosingGuard(bool& f) : flag(f) { flag = true; }
        ~ClosingGuard() { flag = false; }
    };
    void rebuildAllDocs();

    void createMenus();
    void applyMenuBarTitleCase(bool titleCase);
    void createStatusBar();
    void showPluginsDialog();
    void populateSourceMenu();
    void addRecentFile(const QString& path);
    void updateRecentFilesMenu();
    QIcon makeIcon(const QString& svgPath);

    RcxController* activeController() const;
    TabState* activeTab();
    TabState* tabByIndex(int index);
    int tabCount() const { return m_tabs.size(); }
    QDockWidget* createTab(RcxDocument* doc);
    QString tabTitle(const TabState& tab) const;
    void setupDockTabBars();
    void updateWindowTitle();
    void closeAllDocDocks();

    void setViewMode(ViewMode mode);
    void updateRenderedView(TabState& tab, SplitPane& pane);
    void updateAllRenderedPanes(TabState& tab);
    uint64_t findRootStructForNode(const NodeTree& tree, uint64_t nodeId) const;
    void setupRenderedSci(QsciScintilla* sci);

    SplitPane createSplitPane(TabState& tab);
    void applyTheme(const Theme& theme);
    void syncViewButtons(ViewMode mode);
    SplitPane* findPaneByTabWidget(QTabWidget* tw);
    SplitPane* findActiveSplitPane();
    RcxEditor* activePaneEditor();

    // Workspace dock
    QDockWidget*          m_workspaceDock   = nullptr;
    QTreeView*            m_workspaceTree   = nullptr;
    QStandardItemModel*   m_workspaceModel  = nullptr;
    QSortFilterProxyModel* m_workspaceProxy = nullptr;
    QLineEdit*            m_workspaceSearch = nullptr;
    WorkspaceDelegate*    m_workspaceDelegate = nullptr;
    QLabel*               m_dockTitleLabel  = nullptr;
    QToolButton*          m_dockCloseBtn    = nullptr;
    DockGripWidget*       m_dockGrip        = nullptr;
    QSet<uint64_t>        m_pinnedIds;
    void createWorkspaceDock();
    void rebuildWorkspaceModel();       // debounced — safe to call frequently
    void rebuildWorkspaceModelNow();    // immediate rebuild
    QTimer*               m_workspaceRebuildTimer = nullptr;
    QTimer*               m_workspaceSearchTimer  = nullptr;
    void updateBorderColor(const QColor& color);

    // Scanner dock
    QDockWidget*          m_scannerDock      = nullptr;
    ScannerPanel*         m_scannerPanel     = nullptr;
    QLabel*               m_scanDockTitle    = nullptr;
    QToolButton*          m_scanDockCloseBtn = nullptr;
    DockGripWidget*       m_scanDockGrip     = nullptr;
    void createScannerDock();

    // Start page
    StartPageWidget*      m_startPage        = nullptr;
    Q_INVOKABLE void showStartPage();
    void dismissStartPage();

protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
};

} // namespace rcx
