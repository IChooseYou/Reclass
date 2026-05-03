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

signals:
    void goToAddress(uint64_t address);

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
