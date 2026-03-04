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
    QCheckBox*    execCheck()    const { return m_execCheck; }
    QCheckBox*    writeCheck()   const { return m_writeCheck; }
    QPushButton*  scanButton()   const { return m_scanBtn; }
    QPushButton*  updateButton() const { return m_updateBtn; }
    QProgressBar* progressBar()  const { return m_progressBar; }
    QTableWidget* resultsTable() const { return m_resultTable; }
    QLabel*       statusLabel()  const { return m_statusLabel; }
    QPushButton*  gotoButton()   const { return m_gotoBtn; }
    QPushButton*  copyButton()   const { return m_copyBtn; }
    ScanEngine*   engine()       const { return m_engine; }
    QComboBox*    condCombo()    const { return m_condCombo; }
    QLabel*       condLabel()    const { return m_condLabel; }
    QCheckBox*    structOnlyCheck() const { return m_structOnlyCheck; }

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

private:
    ScanRequest buildRequest();
    void populateTable(bool showPrevious);
    void updateComboWidth();

    void onConditionChanged(int index);

    // Input widgets
    QComboBox*    m_modeCombo;      // Signature / Value
    QLineEdit*    m_patternEdit;    // Signature pattern input
    QComboBox*    m_typeCombo;      // Value type dropdown
    QComboBox*    m_condCombo;      // Scan condition (Exact/Unknown/Changed/...)
    QLineEdit*    m_valueEdit;      // Value input
    QLabel*       m_patternLabel;
    QLabel*       m_typeLabel;
    QLabel*       m_condLabel;
    QLabel*       m_valueLabel;

    // Filters
    QCheckBox*    m_execCheck;
    QCheckBox*    m_writeCheck;
    QCheckBox*    m_structOnlyCheck;

    // Actions
    QPushButton*  m_scanBtn;
    QPushButton*  m_updateBtn;
    QProgressBar* m_progressBar;

    // Results
    QTableWidget*    m_resultTable;
    AddressDelegate* m_addrDelegate;
    QLabel*          m_statusLabel;
    QPushButton*     m_gotoBtn;
    QPushButton*     m_copyBtn;

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
