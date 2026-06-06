#pragma once
#include "scanner.h"
#include "themes/theme.h"
#include <QWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QStyledItemDelegate>
#include <QLabel>
#include <functional>
#include <memory>

namespace rcx {

// Delegate that paints address with dimmed high-bytes prefix
class AddressDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QColor dimColor;
    QColor brightColor;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};

class ScannerPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScannerPanel(QWidget* parent = nullptr);

    using ProviderGetter = std::function<std::shared_ptr<Provider>()>;
    void setProviderGetter(ProviderGetter getter);

    struct StructBounds { uint64_t start = 0; uint64_t size = 0; };
    using BoundsGetter = std::function<StructBounds()>;
    void setBoundsGetter(BoundsGetter getter);

    void setEditorFont(const QFont& font);
    void applyTheme(const Theme& theme);

    // Test accessors
    QComboBox*    modeCombo()    const { return m_modeCombo; }
    QLineEdit*    patternEdit()  const { return m_patternEdit; }
    QComboBox*    typeCombo()    const { return m_typeCombo; }
    QLineEdit*    valueEdit()    const { return m_valueEdit; }
    QLineEdit*    value2Edit()   const { return m_value2Edit; }
    QCheckBox*    execCheck()    const { return m_execCheck; }
    QCheckBox*    writeCheck()   const { return m_writeCheck; }
    QCheckBox*    privateOnlyCheck()    const { return m_privateOnlyCheck; }
    QCheckBox*    skipSystemCheck()     const { return m_skipSystemCheck; }
    QCheckBox*    userModeOnlyCheck()   const { return m_userModeOnlyCheck; }
    QCheckBox*    fastScanCheck()       const { return m_fastScanCheck; }
    QComboBox*    fastScanCombo()       const { return m_fastScanCombo; }
    QPushButton*  scanButton()   const { return m_scanBtn; }
    QPushButton*  updateButton() const { return m_updateBtn; }
    QPushButton*  newScanButton() const { return m_newScanBtn; }
    QPushButton*  undoButton()    const { return m_undoBtn; }
    QProgressBar* progressBar()  const { return m_progressBar; }
    QTableWidget* resultsTable() const { return m_resultTable; }
    QLabel*       statusLabel()  const { return m_statusLabel; }
    QPushButton*  gotoButton()   const { return m_gotoBtn; }
    QPushButton*  copyButton()   const { return m_copyBtn; }
    QLineEdit*    resultFilter() const { return m_resultFilter; }
    ScanEngine*   engine()       const { return m_engine; }
    QComboBox*    condCombo()    const { return m_condCombo; }
    QLabel*       condLabel()    const { return m_condLabel; }
    QCheckBox*    structOnlyCheck() const { return m_structOnlyCheck; }
    const QVector<ScanResult>& results() const { return m_results; }

    /** Save / load the result list to a JSON file. */
    bool saveResultsTo(const QString& path) const;
    bool loadResultsFrom(const QString& path);

    /** Persist + restore last scan settings via QSettings (keyed per-tab id, optional). */
    void saveSettings(const QString& key = QStringLiteral("scanner")) const;
    void loadSettings(const QString& key = QStringLiteral("scanner"));

    /** Run a value scan and block until done. For MCP / automation. Returns results; updates panel table. */
    QVector<ScanResult> runValueScanAndWait(ValueType valueType, const QString& value,
                                            bool filterExecutable = false, bool filterWritable = false,
                                            const QVector<AddressRange>& constrainRegions = {});

    /** Run a pattern/signature scan and block until done. Pattern: space-separated hex bytes, e.g. "00 00 20 42 ?? ??". */
    QVector<ScanResult> runPatternScanAndWait(const QString& pattern,
                                              bool filterExecutable = false, bool filterWritable = false,
                                              const QVector<AddressRange>& constrainRegions = {});

    /** Run pattern scan using the given provider (for MCP: use tab's provider so scan runs on the right tab). */
    QVector<ScanResult> runPatternScanAndWait(std::shared_ptr<Provider> provider, const QString& pattern,
                                              bool filterExecutable = false, bool filterWritable = false,
                                              const QVector<AddressRange>& constrainRegions = {});

    /** Blocking value scan with an explicit condition. Drives the SAME result set the panel keeps
     *  (m_results), so a subsequent runRescanAndWait narrows it — this is the iterative-narrowing
     *  loop the UI uses, exposed for MCP/automation. UnknownValue captures every aligned address
     *  (value ignored); Between uses value=lower, value2=upper; compare-against-previous conditions
     *  (Changed/Increased/...) have no baseline on a first scan and capture-all instead. A hard
     *  timeout aborts a runaway scan and returns the partial set. */
    QVector<ScanResult> runValueScanAndWait(ValueType valueType, ScanCondition condition,
                                            const QString& value, const QString& value2 = {},
                                            bool filterExecutable = false, bool filterWritable = false,
                                            bool skipSystemModules = false,
                                            const QVector<AddressRange>& constrainRegions = {},
                                            int timeoutMs = 120000);

    /** Blocking re-scan over the current result set using the existing engine (the UI Next-Scan
     *  path). Narrows m_results in place. value/value2 used for typed conditions (Between uses both),
     *  delta for IncreasedBy/DecreasedBy. */
    QVector<ScanResult> runRescanAndWait(ScanCondition condition, const QString& value = {},
                                         const QString& value2 = {}, const QString& delta = {},
                                         int timeoutMs = 120000);

    /** Blocking structure-aware scan for 4x4 affine view-matrix candidates (see matrixscan.h).
     *  Returns candidates ranked best-first; also populates the panel result set so the agent can
     *  then runRescanAndWait(Changed) while moving the camera to confirm the live one. */
    QVector<ScanResult> runMatrixScanAndWait(const MatrixScanParams& params,
                                             bool filterExecutable = false, bool filterWritable = true,
                                             bool skipSystemModules = true, int maxCandidates = 64,
                                             const QVector<AddressRange>& constrainRegions = {},
                                             int timeoutMs = 120000);

    // Accessors for the MCP bridge to build structured responses (formatValue/valueSize are private).
    int       scanGeneration()  const { return m_scanGeneration; }
    ValueType lastValueType()   const { return m_lastValueType; }
    int       lastScanMode()    const { return m_lastScanMode; }
    QString   formatValuePublic(const QByteArray& bytes) const { return formatValue(bytes); }
    int       valueSizePublic() const { return valueSize(); }

signals:
    void goToAddress(uint64_t address);

protected:
    // All tooltips in the scanner are suppressed — they flickered and fought
    // with the hover UI. Swallowing QEvent::ToolTip for the whole subtree
    // kills them at the source, so no setToolTip call site (static, dynamic,
    // or item-view) can surface one. Installed on this + every descendant
    // widget at the end of the constructor.
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onModeChanged(int index);
    void onScanClicked();
    void onScanFinished(QVector<ScanResult> results);
    void onGoToAddress();
    void onCopyAddress();
    void onResultDoubleClicked(int row, int col);
    void onCellEdited(int row, int col);
    void onUpdateClicked();
    void onRescanFinished(QVector<ScanResult> results);
    void onNewScanClicked();
    void onResultFilterChanged(const QString& text);

private:
    ScanRequest buildRequest();
    void populateTable(bool showPrevious);
    void updateComboWidth();
    void applyResultFilter();
    void updateScanStatusLine();
    void updateModuleColumnVisibility();

    void onConditionChanged(int index);

    // SCAN FOR group is collapsible: the clickable header toggles m_scanBody
    // (criteria form + fast-scan + filter chips). The action buttons + progress
    // strip sit above the body and stay visible when collapsed.
    void setScanForCollapsed(bool collapsed);
    QPushButton* m_scanForHeader = nullptr;
    QWidget*     m_scanBody = nullptr;
    bool         m_scanForCollapsed = false;

    // Input widgets
    QComboBox*    m_modeCombo;      // Signature / Value
    QLineEdit*    m_patternEdit;    // Signature pattern input
    QComboBox*    m_typeCombo;      // Value type dropdown
    QComboBox*    m_condCombo;      // Scan condition (Exact/Unknown/Changed/...)
    QLineEdit*    m_valueEdit;      // Value input (or lower bound for Between)
    QLineEdit*    m_value2Edit;     // Upper bound for Between
    QLabel*       m_patternLabel;
    QLabel*       m_typeLabel;
    QLabel*       m_condLabel;
    QLabel*       m_valueLabel;
    QLabel*       m_value2Label;

    // Filters
    QCheckBox*    m_execCheck;
    QCheckBox*    m_writeCheck;
    QCheckBox*    m_structOnlyCheck;
    QCheckBox*    m_privateOnlyCheck;     // skip Image/Mapped
    QCheckBox*    m_skipSystemCheck;      // skip kernel32/ntdll/Qt6/etc.
    QCheckBox*    m_userModeOnlyCheck;    // cap end address to user-mode VA
    QCheckBox*    m_fastScanCheck;        // hidden stub kept for API compat
    QComboBox*    m_fastScanCombo;        // alignment dropdown: 1/4/8/16/32/64
    QLabel*       m_fastScanLabel = nullptr;  // "Fast Scan:" label sibling

    // Actions
    QPushButton*  m_scanBtn;
    QPushButton*  m_updateBtn;
    QPushButton*  m_undoBtn;        // restore last result list (Cheat Engine UX)
    QPushButton*  m_newScanBtn;     // explicit reset (drops result list)
    QProgressBar* m_progressBar;

    // Undo stack — snapshots of m_results before each Next Scan so the user
    // can step back when a re-scan over-narrows. Capped at 16 entries
    // (typical CE workflow rarely needs more).
    QVector<QVector<ScanResult>> m_undoStack;
    void pushUndoSnapshot();
    void popUndoSnapshot();

    // Results
    QTableWidget*    m_resultTable;
    AddressDelegate* m_addrDelegate;
    QLabel*          m_statusLabel;
    QLabel*          m_truncBanner;       // "Displaying N of M — narrow with Re-scan"
    QLabel*          m_stageLabel;        // breadcrumb: "Step 1 — First scan: 47 results"
    QPushButton*     m_gotoBtn;
    QPushButton*     m_copyBtn;
    QLineEdit*       m_resultFilter;      // post-scan filter on displayed rows

    // Workflow tracking — drives the stage label.
    int m_scanGeneration = 0;             // 0 = no scan yet; 1 = first scan; 2+ = nth re-scan
    int m_lastResultCount = 0;            // before-rescan count (for "narrowed N → M")
    void updateStageLabel(const QString& phase = {});

    // Engine
    ScanEngine*   m_engine;
    ProviderGetter m_providerGetter;
    BoundsGetter   m_boundsGetter;
    QVector<ScanResult> m_results;
    int           m_lastScanMode = 0;   // 0=signature, 1=value
    ValueType     m_lastValueType = ValueType::Int32;
    ScanCondition m_lastCondition = ScanCondition::ExactValue;
    QByteArray    m_lastPattern;        // serialized search value
    int           m_preRescanCount = 0; // result count before last rescan

    QString formatValue(const QByteArray& bytes) const;
    int     valueSize() const;
};

} // namespace rcx
