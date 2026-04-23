#pragma once
#include "controller.h"
#include "titlebar.h"
#include "pluginmanager.h"
#include "scannerpanel.h"
#include "imports/import_pdb.h"
#include "startpage.h"
#include "generator.h"
#include "workspace_model.h"
namespace rcx { class SymbolDownloader; class DockOverlay; class DockDragDetector; }
#include <QMainWindow>
#include <QLabel>
#include <QSplitter>
#include <QTabWidget>
#include <QDockWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPointer>
#include <QButtonGroup>
#include <QJsonArray>
#include <QJsonObject>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
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
    void exportToFile(CodeFormat fmt);
    void exportCpp();
    void exportRust();
    void exportDefines();
    void exportCSharp();
    void exportPython();
    void exportReclassXmlAction();
    void importFromSource();
    void importReclassXml();
    void importPdb();
    void showTypeAliasesDialog();
    void editTheme();
    void showOptionsDialog();
    void showOptionsDialog(int initialPage);

public:
    // Status bar helpers — separate app / MCP channels
    void setAppStatus(const QString& text);
    void setAppStatus(const QString& text, const QString& dimSuffix);
    void setMcpStatus(const QString& text);
    void clearMcpStatus();

    bool presentationMode() const { return m_presentationMode; }

    // Project Lifecycle API
    QDockWidget* project_new(const QString& classKeyword = QString());
    QDockWidget* project_open(const QString& path = {});
    bool project_save(QDockWidget* dock = nullptr, bool saveAs = false);
    void project_close(QDockWidget* dock = nullptr);

    // Layout presets — wired from TitleBarWidget::layoutPresetSelected.
    // Sets visibility of workspace/symbols/scanner docks per preset.
    // Values match rcx::LayoutPreset enum in titlebar.h.
    void applyLayoutPreset(int preset);

    // Find-references — scan all open documents for any Node.refId ==
    // targetId (or Node.structTypeName matching the target name for unlinked
    // types imported from another project), then present a modal list.
    // Double-click an entry jumps to that tab + scrolls to the reference.
    void showFindReferences(const QString& targetTypeName, uint64_t targetStructId);

    // Data form for the same query, exposed for MCP tool reuse.
    struct ReferenceHit {
        QDockWidget*    ownerDock;   // which doc tab contains the hit
        uint64_t        nodeId;      // node holding the reference
        QString         ownerType;   // root struct containing nodeId
        QString         fieldName;   // nodeId's name
        int             fieldOffset; // for display
    };
    QVector<ReferenceHit> findReferences(const QString& targetTypeName,
                                          uint64_t targetStructId) const;

private:
    enum ViewMode { VM_Reclass, VM_Rendered, VM_Debug };

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

    // ── UI Inspection (Ctrl+Click) ──
    QWidget*        m_inspectionOverlay = nullptr;
    struct InspectionResult {
        bool        selected = false;
        QString     widgetName;
        QString     region;
        QString     description;
        QRect       globalRect;
        QJsonArray  themeColors;   // [{key, value, label, group}]
        QJsonObject properties;    // {fontSize, fontFamily, width, height, ...}
    };
    InspectionResult m_inspectedRegion;
    InspectionResult inspectAt(QWidget* widget, QPoint localPos);
    void clearInspection();
    PluginManager   m_pluginManager;
    McpBridge*      m_mcp       = nullptr;
    QAction*        m_mcpAction = nullptr;
    QAction*        m_actRelOfs = nullptr;
    bool            m_presentationMode = false;
    QAction*        m_actPresentationMode = nullptr;
    QMenu*          m_sourceMenu = nullptr;
    QMenu*          m_recentFilesMenu = nullptr;

    struct SplitPane {
        QTabWidget*    tabWidget = nullptr;
        RcxEditor*     editor    = nullptr;
        QsciScintilla* rendered  = nullptr;
        QsciScintilla* debugView = nullptr;
        QLineEdit*     findBar   = nullptr;
        QWidget*       findContainer = nullptr;
        QWidget*       renderedContainer = nullptr;
        QComboBox*     fmtCombo   = nullptr;
        QComboBox*     scopeCombo = nullptr;
        QToolButton*   fmtGear    = nullptr;
        ViewMode       viewMode  = VM_Reclass;
        uint64_t       lastRenderedRootId = 0;
        // Minimap: narrow read-only Scintilla mirror to the right of the
        // main editor. Synced via RcxEditor::documentApplied. Created but
        // hidden; visibility toggled via the View menu "Minimap" action.
        QsciScintilla* minimap         = nullptr;
        QWidget*       editorContainer = nullptr;
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
    // QPointer so stale-pointer access after dock destruction fails safely
    // (null-compare, null-deref fires Q_ASSERT) rather than silently running
    // over freed memory. Automatically nulls when the dock is destroyed.
    QPointer<QDockWidget> m_activeDocDock;  // tracks active document dock
    QVector<QDockWidget*> m_sentinelDocks;    // permanent sentinels for always-visible tab bars
    QVector<RcxDocument*> m_allDocs;  // all open docs, shared with controllers
    bool m_closingAll = false;        // guards spurious project_new during batch close
    struct ClosingGuard {
        bool& flag;
        ClosingGuard(bool& f) : flag(f) { flag = true; }
        ~ClosingGuard() { flag = false; }
    };
    void rebuildAllDocs();

    void createMenus();
    void applyMenuBarTitleCase(bool titleCase);

    // ── Sidebar dock placement ──
    // Single entry point for positioning a sidebar dock (workspace, bookmarks,
    // symbols, scanner). If another sidebar dock is already visible+docked in
    // the target area, tabifies with it — new panel becomes a tab instead of
    // subdividing the area. Otherwise positions next to the first doc dock
    // (horizontal side-dock) or in the requested area, then resizes to the
    // remembered size from QSettings (falling back to preferredSize).
    void placeSidebarDock(QDockWidget* dock, Qt::DockWidgetArea area,
                          int preferredSize = -1);
    // Persist / restore dock size under ui/dock.<objectName>.size.
    void saveDockSize(QDockWidget* dock);
    int  loadDockSize(QDockWidget* dock, int fallback) const;
    // Guard to prevent visibilityChanged → placeSidebarDock → show() →
    // visibilityChanged recursion when a dock is tabified on becoming visible.
    bool m_placingSidebar = false;
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
    QDockWidget* createSentinelDock();
    QDockWidget* createTab(RcxDocument* doc);
    QString tabTitle(const TabState& tab) const;
    void setupDockTabBars();
    void updateWindowTitle();
    void closeAllDocDocks();

    void setViewMode(ViewMode mode);
    void updateRenderedView(TabState& tab, SplitPane& pane);
    void updateAllRenderedPanes(TabState& tab);
    void updateDebugView(TabState& tab, SplitPane& pane);
    void updateAllDebugPanes(TabState& tab);
    QString generateDebugText(RcxEditor* editor) const;
    uint64_t findRootStructForNode(const NodeTree& tree, uint64_t nodeId) const;
    void setupRenderedSci(QsciScintilla* sci);
    void setupDebugSci(QsciScintilla* sci);
    void applyDebugStyles(QsciScintilla* sci);
    void styleDebugText(QsciScintilla* sci, const QString& text);

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
    int  computeWorkspaceDockWidth() const;  // fit to longest type name
    QTimer*               m_workspaceRebuildTimer = nullptr;
    QTimer*               m_workspaceSearchTimer  = nullptr;
    void updateBorderColor(const QColor& color);

    // Dock overlay drag system
    DockOverlay*       m_dockOverlay      = nullptr;
    DockDragDetector*  m_dockDragDetector = nullptr;
    Qt::DockWidgetArea m_dragOrigArea     = Qt::NoDockWidgetArea;
    // QPointer because a peer dock can be closed or destroyed mid-drag
    // (e.g. MCP-driven project close). Null-guard in the cancel handler
    // below turns a potential UAF into a benign no-op.
    QPointer<QDockWidget> m_dragOrigPeer;
    void setupDockOverlay();
    void onDockDragStarted(QDockWidget* dock, QPoint globalPos);
    void onDockDropRequested(QDockWidget* source, QDockWidget* target, int zone);

    // Scanner dock
    QDockWidget*          m_scannerDock      = nullptr;
    ScannerPanel*         m_scannerPanel     = nullptr;
    QLabel*               m_scanDockTitle    = nullptr;
    QToolButton*          m_scanDockCloseBtn = nullptr;
    DockGripWidget*       m_scanDockGrip     = nullptr;
    void createScannerDock();

    // Modules/Symbols dock
    QDockWidget*           m_symbolsDock      = nullptr;
    QTabWidget*            m_symTabWidget     = nullptr;
    // Modules tab
    QTreeView*             m_modulesTree      = nullptr;
    QStandardItemModel*    m_modulesModel     = nullptr;
    // Symbols tab
    QTreeView*             m_symbolsTree      = nullptr;
    QStandardItemModel*    m_symbolsModel     = nullptr;
    QSortFilterProxyModel* m_symbolsProxy     = nullptr;
    QLineEdit*             m_symbolsSearch    = nullptr;
    // Title bar
    QLabel*                m_symDockTitle     = nullptr;
    QToolButton*           m_symDockCloseBtn  = nullptr;
    QToolButton*           m_symDownloadBtn   = nullptr;
    DockGripWidget*        m_symDockGrip      = nullptr;
    rcx::SymbolDownloader* m_symDownloader    = nullptr;
    // Types tab
    QTreeView*             m_typesTree      = nullptr;
    QStandardItemModel*    m_typesModel     = nullptr;
    QSortFilterProxyModel* m_typesProxy     = nullptr;
    QLineEdit*             m_typesSearch    = nullptr;
    QPushButton*           m_typesImportBtn = nullptr;
    struct CachedModuleTypes {
        QString pdbPath;
        QVector<rcx::PdbTypeInfo> types;
    };
    QHash<QString, CachedModuleTypes> m_cachedModuleTypes;

    void createSymbolsDock();

    // Bookmarks dock
    QDockWidget* m_bookmarksDock   = nullptr;
    QListWidget* m_bookmarksList   = nullptr;
    QLineEdit*   m_bookmarksFilter = nullptr;
    void createBookmarksDock();
    void refreshBookmarksDock();
    void promptAddBookmark();
    void navigateBookmark(int idx);
    void rebuildSymbolsModel();
    void rebuildTypesModel();
    void populateTypesModuleItem(QStandardItem* moduleItem);
    void rebuildModulesModel();
    void importSelectedTypes();
    void downloadSymbolsForProcess();
    // Load PDB symbols + typeIndices into SymbolStore, cache types. Returns symbol count.
    int loadPdbAndCacheTypes(const QString& pdbPath);

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
