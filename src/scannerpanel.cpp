#include "scannerpanel.h"
#include "addressparser.h"
#include <cstring>
#include <QElapsedTimer>
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QEventLoop>

namespace rcx {

void AddressDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    // Draw background (selection/hover handled by style)
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

    QString text = index.data(Qt::DisplayRole).toString();
    if (text.isEmpty()) return;

    // Find first non-zero hex digit (skip backtick)
    int dimEnd = 0;
    for (int i = 0; i < text.size(); i++) {
        QChar c = text[i];
        if (c == '`') { dimEnd = i + 1; continue; }
        if (c != '0') break;
        dimEnd = i + 1;
    }

    QRect textRect = opt.rect.adjusted(7, 0, -4, 0); // match item padding
    painter->setFont(opt.font);

    if (dimEnd > 0) {
        painter->setPen(dimColor);
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text.left(dimEnd));
        // Advance past dim prefix
        int dimWidth = painter->fontMetrics().horizontalAdvance(text.left(dimEnd));
        textRect.setLeft(textRect.left() + dimWidth);
    }
    painter->setPen(brightColor);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text.mid(dimEnd));
}

ScannerPanel::ScannerPanel(QWidget* parent)
    : QWidget(parent)
    , m_engine(new ScanEngine(this))
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(4);

    // ── Row 1: Mode + pattern/value input ──
    auto* inputRow = new QHBoxLayout;
    inputRow->setSpacing(6);

    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(QIcon(QStringLiteral(":/vsicons/regex.svg")),
                         QStringLiteral("Signature"));
    m_modeCombo->addItem(QIcon(QStringLiteral(":/vsicons/symbol-variable.svg")),
                         QStringLiteral("Value"));
    updateComboWidth();
    inputRow->addWidget(m_modeCombo);

    // Signature input
    m_patternLabel = new QLabel(QStringLiteral("Pattern:"), this);
    inputRow->addWidget(m_patternLabel);

    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setPlaceholderText(QStringLiteral("48 8B ?? 05 ?? ?? ?? ?? CC"));
    inputRow->addWidget(m_patternEdit, 1);

    // Value input (hidden initially)
    m_typeLabel = new QLabel(QStringLiteral("Type:"), this);
    inputRow->addWidget(m_typeLabel);

    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(QStringLiteral("int8"),    (int)ValueType::Int8);
    m_typeCombo->addItem(QStringLiteral("int16"),   (int)ValueType::Int16);
    m_typeCombo->addItem(QStringLiteral("int32"),   (int)ValueType::Int32);
    m_typeCombo->addItem(QStringLiteral("int64"),   (int)ValueType::Int64);
    m_typeCombo->addItem(QStringLiteral("uint8"),   (int)ValueType::UInt8);
    m_typeCombo->addItem(QStringLiteral("uint16"),  (int)ValueType::UInt16);
    m_typeCombo->addItem(QStringLiteral("uint32"),  (int)ValueType::UInt32);
    m_typeCombo->addItem(QStringLiteral("uint64"),  (int)ValueType::UInt64);
    m_typeCombo->addItem(QStringLiteral("float"),   (int)ValueType::Float);
    m_typeCombo->addItem(QStringLiteral("double"),  (int)ValueType::Double);
    m_typeCombo->setCurrentIndex(2); // default: int32
    inputRow->addWidget(m_typeCombo);

    m_condLabel = new QLabel(QStringLiteral("Scan:"), this);
    inputRow->addWidget(m_condLabel);

    m_condCombo = new QComboBox(this);
    m_condCombo->addItem(QStringLiteral("Exact Value"),    (int)ScanCondition::ExactValue);
    m_condCombo->addItem(QStringLiteral("Unknown Value"),  (int)ScanCondition::UnknownValue);
    m_condCombo->addItem(QStringLiteral("Changed"),        (int)ScanCondition::Changed);
    m_condCombo->addItem(QStringLiteral("Unchanged"),      (int)ScanCondition::Unchanged);
    m_condCombo->addItem(QStringLiteral("Increased"),      (int)ScanCondition::Increased);
    m_condCombo->addItem(QStringLiteral("Decreased"),      (int)ScanCondition::Decreased);
    inputRow->addWidget(m_condCombo);

    m_valueLabel = new QLabel(QStringLiteral("Value:"), this);
    inputRow->addWidget(m_valueLabel);

    m_valueEdit = new QLineEdit(this);
    m_valueEdit->setPlaceholderText(QStringLiteral("12345"));
    inputRow->addWidget(m_valueEdit, 1);

    mainLayout->addLayout(inputRow);

    // ── Row 2: Filters + scan button + progress ──
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);

    m_execCheck = new QCheckBox(QStringLiteral("Executable"), this);
    filterRow->addWidget(m_execCheck);

    m_writeCheck = new QCheckBox(QStringLiteral("Writable"), this);
    filterRow->addWidget(m_writeCheck);

    m_structOnlyCheck = new QCheckBox(QStringLiteral("Current Struct"), this);
    filterRow->addWidget(m_structOnlyCheck);

    filterRow->addStretch();

    m_scanBtn = new QPushButton(QIcon(QStringLiteral(":/vsicons/search.svg")),
                               QStringLiteral("Scan"), this);
    filterRow->addWidget(m_scanBtn);

    m_updateBtn = new QPushButton(QIcon(QStringLiteral(":/vsicons/refresh.svg")),
                                  QStringLiteral("Re-scan"), this);
    m_updateBtn->setEnabled(false);
    filterRow->addWidget(m_updateBtn);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFixedWidth(150);
    m_progressBar->hide();
    filterRow->addWidget(m_progressBar);

    mainLayout->addLayout(filterRow);

    // ── Results table ──
    m_resultTable = new QTableWidget(this);
    m_resultTable->setColumnCount(2);
    m_resultTable->horizontalHeader()->hide();
    m_resultTable->verticalHeader()->hide();
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::DoubleClicked);
    m_resultTable->setShowGrid(false);
    m_resultTable->setMouseTracking(true);
    m_resultTable->setFocusPolicy(Qt::StrongFocus);
    m_resultTable->setContextMenuPolicy(Qt::CustomContextMenu);

    // Address column delegate for dimmed leading zeros
    m_addrDelegate = new AddressDelegate(this);
    m_resultTable->setItemDelegateForColumn(0, m_addrDelegate);
    mainLayout->addWidget(m_resultTable, 1);

    // ── Row 3: Status + action buttons ──
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(6);

    m_statusLabel = new QLabel(QStringLiteral("Ready"), this);
    actionRow->addWidget(m_statusLabel, 1);

    m_gotoBtn = new QPushButton(QIcon(QStringLiteral(":/vsicons/arrow-right.svg")),
                               QStringLiteral("Go to Address"), this);
    m_gotoBtn->setEnabled(false);
    actionRow->addWidget(m_gotoBtn);

    m_copyBtn = new QPushButton(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                               QStringLiteral("Copy Address"), this);
    m_copyBtn->setEnabled(false);
    actionRow->addWidget(m_copyBtn);
    actionRow->addSpacing(20);  // room for resize grip when floating

    mainLayout->addLayout(actionRow);

    // ── Initial state: signature mode ──
    m_typeLabel->hide();
    m_typeCombo->hide();
    m_condLabel->hide();
    m_condCombo->hide();
    m_valueLabel->hide();
    m_valueEdit->hide();
    m_execCheck->setChecked(true);

    // ── Connections ──
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScannerPanel::onModeChanged);
    connect(m_condCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScannerPanel::onConditionChanged);
    connect(m_scanBtn, &QPushButton::clicked,
            this, &ScannerPanel::onScanClicked);
    connect(m_updateBtn, &QPushButton::clicked,
            this, &ScannerPanel::onUpdateClicked);
    connect(m_gotoBtn, &QPushButton::clicked,
            this, &ScannerPanel::onGoToAddress);
    connect(m_copyBtn, &QPushButton::clicked,
            this, &ScannerPanel::onCopyAddress);
    connect(m_resultTable, &QTableWidget::cellDoubleClicked,
            this, &ScannerPanel::onResultDoubleClicked);
    connect(m_resultTable, &QTableWidget::cellChanged,
            this, &ScannerPanel::onCellEdited);
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        bool hasSel = !m_resultTable->selectedItems().isEmpty();
        m_gotoBtn->setEnabled(hasSel);
        m_copyBtn->setEnabled(hasSel);
    });

    connect(m_resultTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        int row = m_resultTable->rowAt(pos.y());
        if (row < 0 || row >= m_results.size()) return;
        QMenu menu;
        auto* copyAddr = menu.addAction(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                                        QStringLiteral("Copy Address"));
        auto* copyVal = menu.addAction(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                                       QStringLiteral("Copy Value"));
        auto* goTo = menu.addAction(QIcon(QStringLiteral(":/vsicons/arrow-right.svg")),
                                    QStringLiteral("Go to Address"));
        auto* chosen = menu.exec(m_resultTable->viewport()->mapToGlobal(pos));
        if (chosen == copyAddr) {
            QString addr = QStringLiteral("0x%1")
                .arg(m_results[row].address, 0, 16, QLatin1Char('0')).toUpper();
            QApplication::clipboard()->setText(addr);
            m_statusLabel->setText(QStringLiteral("Copied: %1").arg(addr));
        } else if (chosen == copyVal) {
            QApplication::clipboard()->setText(formatValue(m_results[row].scanValue));
            m_statusLabel->setText(QStringLiteral("Copied value"));
        } else if (chosen == goTo) {
            emit goToAddress(m_results[row].address);
        }
    });

    connect(m_engine, &ScanEngine::progress, this, [this](int pct) {
        m_progressBar->setValue(pct);
    });
    connect(m_engine, &ScanEngine::finished,
            this, &ScannerPanel::onScanFinished);
    connect(m_engine, &ScanEngine::rescanFinished,
            this, &ScannerPanel::onRescanFinished);
    connect(m_engine, &ScanEngine::error, this, [this](const QString& msg) {
        m_statusLabel->setText(QStringLiteral("Error: %1").arg(msg));
        m_scanBtn->setText(QStringLiteral("Scan"));
        m_progressBar->hide();
    });
}

void ScannerPanel::setProviderGetter(ProviderGetter getter) {
    m_providerGetter = std::move(getter);
}

void ScannerPanel::setBoundsGetter(BoundsGetter getter) {
    m_boundsGetter = std::move(getter);
}

void ScannerPanel::setEditorFont(const QFont& font) {
    m_resultTable->setFont(font);
    QFontMetrics fm(font);
    m_resultTable->verticalHeader()->setDefaultSectionSize(fm.height() + 6);
    // Address column width: "00000000`00000000" + padding
    m_resultTable->setColumnWidth(0, fm.horizontalAdvance(QStringLiteral("00000000`00000000")) + 20);
    m_patternEdit->setFont(font);
    m_valueEdit->setFont(font);
    m_modeCombo->setFont(font);
    m_typeCombo->setFont(font);
    m_condCombo->setFont(font);
    m_statusLabel->setFont(font);
    m_scanBtn->setFont(font);
    m_gotoBtn->setFont(font);
    m_copyBtn->setFont(font);
    m_patternLabel->setFont(font);
    m_typeLabel->setFont(font);
    m_condLabel->setFont(font);
    m_valueLabel->setFont(font);
    m_execCheck->setFont(font);
    m_writeCheck->setFont(font);
    m_structOnlyCheck->setFont(font);
    m_updateBtn->setFont(font);
    updateComboWidth();
}

void ScannerPanel::updateComboWidth() {
    QFontMetrics fm(m_modeCombo->font());
    int maxW = 0;
    for (int i = 0; i < m_modeCombo->count(); i++)
        maxW = qMax(maxW, fm.horizontalAdvance(m_modeCombo->itemText(i)));
    m_modeCombo->setFixedWidth(maxW + 50); // icon + dropdown arrow + padding
}

void ScannerPanel::onModeChanged(int index) {
    bool isSig = (index == 0);

    m_patternLabel->setVisible(isSig);
    m_patternEdit->setVisible(isSig);

    m_typeLabel->setVisible(!isSig);
    m_typeCombo->setVisible(!isSig);
    m_condLabel->setVisible(!isSig);
    m_condCombo->setVisible(!isSig);

    // Enable/disable value input based on condition
    auto cond = (ScanCondition)m_condCombo->currentData().toInt();
    bool needsValue = !isSig && (cond == ScanCondition::ExactValue);
    m_valueLabel->setVisible(!isSig);
    m_valueEdit->setVisible(!isSig);
    m_valueEdit->setEnabled(needsValue);
    m_valueLabel->setEnabled(needsValue);

    // Auto-toggle filters: signatures → executable code, values → writable data
    m_execCheck->setChecked(isSig);
    m_writeCheck->setChecked(!isSig);
}

void ScannerPanel::onConditionChanged(int /*index*/) {
    auto cond = (ScanCondition)m_condCombo->currentData().toInt();
    bool needsValue = (cond == ScanCondition::ExactValue);
    m_valueEdit->setEnabled(needsValue);
    m_valueLabel->setEnabled(needsValue);
}

void ScannerPanel::onScanClicked() {
    if (m_engine->isRunning()) {
        m_engine->abort();
        return; // finished/rescanFinished handler resets UI
    }

    // Get provider
    std::shared_ptr<Provider> provider;
    if (m_providerGetter)
        provider = m_providerGetter();

    if (!provider) {
        m_statusLabel->setText(QStringLiteral("No source attached"));
        return;
    }

    // Build request
    ScanRequest req = buildRequest();
    if (req.condition != ScanCondition::UnknownValue && req.pattern.isEmpty())
        return; // error already shown by buildRequest

    m_lastScanMode = m_modeCombo->currentIndex();
    if (m_lastScanMode == 1) {
        m_lastValueType = (ValueType)m_typeCombo->currentData().toInt();
        m_lastCondition = req.condition;
    }
    m_lastPattern = req.pattern;

    m_scanBtn->setText(QStringLiteral("Cancel"));
    m_progressBar->setValue(0);
    m_progressBar->show();
    m_statusLabel->setText(QStringLiteral("Scanning..."));

    m_engine->start(provider, req);
}

ScanRequest ScannerPanel::buildRequest() {
    ScanRequest req;
    QString err;

    if (m_modeCombo->currentIndex() == 0) {
        // Signature mode
        if (!parseSignature(m_patternEdit->text(), req.pattern, req.mask, &err)) {
            m_statusLabel->setText(QStringLiteral("Pattern error: %1").arg(err));
            return {};
        }
        req.alignment = 1;
    } else {
        // Value mode
        auto vt = (ValueType)m_typeCombo->currentData().toInt();
        auto cond = (ScanCondition)m_condCombo->currentData().toInt();

        // Comparison conditions on fresh scan → treat as unknown
        if (cond == ScanCondition::Changed || cond == ScanCondition::Unchanged ||
            cond == ScanCondition::Increased || cond == ScanCondition::Decreased) {
            cond = ScanCondition::UnknownValue;
        }

        req.condition = cond;
        req.alignment = naturalAlignment(vt);
        req.valueSize = valueSizeForType(vt);

        if (cond == ScanCondition::UnknownValue) {
            // No pattern needed — capture all aligned addresses
            req.maxResults = 10000000;
        } else {
            // Exact value mode
            if (!serializeValue(vt, m_valueEdit->text(), req.pattern, req.mask, &err)) {
                m_statusLabel->setText(QStringLiteral("Value error: %1").arg(err));
                return {};
            }
        }
    }

    req.filterExecutable = m_execCheck->isChecked();
    req.filterWritable = m_writeCheck->isChecked();

    if (m_structOnlyCheck->isChecked() && m_boundsGetter) {
        auto bounds = m_boundsGetter();
        if (bounds.size > 0) {
            req.startAddress = bounds.start;
            req.endAddress   = bounds.start + bounds.size;
        }
    }

    return req;
}

QVector<ScanResult> ScannerPanel::runValueScanAndWait(ValueType valueType, const QString& value,
                                                      bool filterExecutable, bool filterWritable,
                                                      const QVector<AddressRange>& constrainRegions) {
    QVector<ScanResult> results;
    QString err;
    ScanRequest req;
    if (!serializeValue(valueType, value, req.pattern, req.mask, &err)) {
        m_statusLabel->setText(QStringLiteral("Value error: %1").arg(err));
        return results;
    }
    req.alignment = naturalAlignment(valueType);
    req.filterExecutable = filterExecutable;
    req.filterWritable = filterWritable;
    req.constrainRegions = constrainRegions;

    auto provider = m_providerGetter ? m_providerGetter() : nullptr;
    if (!provider) {
        m_statusLabel->setText(QStringLiteral("No provider (attach to a process or open a file first)"));
        return results;
    }
    if (m_engine->isRunning()) {
        m_statusLabel->setText(QStringLiteral("Scan already in progress"));
        return results;
    }

    m_lastScanMode = 1;
    m_lastValueType = valueType;
    m_lastPattern = req.pattern;
    m_progressBar->setValue(0);
    m_progressBar->show();
    m_statusLabel->setText(QStringLiteral("Scanning..."));

    QEventLoop loop;
    connect(m_engine, &ScanEngine::finished, this, [&results, &loop](const QVector<ScanResult>& r) {
        results = r;
        loop.quit();
    }, Qt::SingleShotConnection);
    m_engine->start(provider, req);
    loop.exec();

    return results;
}

QVector<ScanResult> ScannerPanel::runPatternScanAndWait(const QString& pattern,
                                                        bool filterExecutable, bool filterWritable,
                                                        const QVector<AddressRange>& constrainRegions) {
    auto provider = m_providerGetter ? m_providerGetter() : nullptr;
    return runPatternScanAndWait(provider, pattern, filterExecutable, filterWritable, constrainRegions);
}

QVector<ScanResult> ScannerPanel::runPatternScanAndWait(std::shared_ptr<Provider> provider,
                                                        const QString& pattern,
                                                        bool filterExecutable, bool filterWritable,
                                                        const QVector<AddressRange>& constrainRegions) {
    QVector<ScanResult> results;
    QString err;
    ScanRequest req;
    if (!parseSignature(pattern, req.pattern, req.mask, &err)) {
        m_statusLabel->setText(QStringLiteral("Pattern error: %1").arg(err));
        return results;
    }
    req.alignment = 1;
    req.filterExecutable = filterExecutable;
    req.filterWritable = filterWritable;
    req.constrainRegions = constrainRegions;

    if (!provider) {
        m_statusLabel->setText(QStringLiteral("No provider (attach to a process or open a file first)"));
        return results;
    }
    if (m_engine->isRunning()) {
        m_statusLabel->setText(QStringLiteral("Scan already in progress"));
        return results;
    }

    m_lastScanMode = 0;
    m_lastPattern = req.pattern;
    m_progressBar->setValue(0);
    m_progressBar->show();
    m_statusLabel->setText(QStringLiteral("Scanning..."));

    QEventLoop loop;
    connect(m_engine, &ScanEngine::finished, this, [&results, &loop](const QVector<ScanResult>& r) {
        results = r;
        loop.quit();
    }, Qt::SingleShotConnection);
    m_engine->start(provider, req);
    loop.exec();

    return results;
}

void ScannerPanel::onScanFinished(QVector<ScanResult> results) {
    m_scanBtn->setText(QStringLiteral("Scan"));
    m_progressBar->hide();
    m_results = std::move(results);

    // Bytes are cached by the engine during scan.
    // Value mode (exact): override with exact search pattern (engine caches raw chunk bytes).
    // Unknown mode: keep engine-captured bytes as-is (they're the baseline).
    for (auto& r : m_results) {
        r.previousValue.clear();
        if (m_lastScanMode == 1 && m_lastCondition == ScanCondition::ExactValue)
            r.scanValue = m_lastPattern;
    }

    m_updateBtn->setEnabled(!m_results.isEmpty());
    {
        QElapsedTimer pt;
        pt.start();
        populateTable(false);
        qDebug() << "[panel] populateTable(initial):" << m_results.size()
                 << "results," << pt.elapsed() << "ms";
    }

    int n = m_results.size();
    if (m_lastCondition == ScanCondition::UnknownValue && n >= 10000000)
        m_statusLabel->setText(QStringLiteral("%1 results (capped — narrow with Re-scan)").arg(n));
    else
        m_statusLabel->setText(QStringLiteral("%1 result%2").arg(n).arg(n == 1 ? "" : "s"));
}

void ScannerPanel::populateTable(bool showPrevious) {
    constexpr int kMaxRows = 10000;

    m_resultTable->blockSignals(true);
    int cols = showPrevious ? 3 : 2;
    m_resultTable->setColumnCount(cols);
    int displayCount = qMin(m_results.size(), kMaxRows);
    m_resultTable->setRowCount(displayCount);

    for (int i = 0; i < displayCount; i++) {
        const auto& r = m_results[i];

        // Address column — WinDbg backtick format: 00000000`00000000
        QString hexPart = QStringLiteral("%1").arg(r.address, 16, 16, QLatin1Char('0')).toUpper();
        hexPart.insert(8, '`');
        auto* addrItem = new QTableWidgetItem(hexPart);
        addrItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
        m_resultTable->setItem(i, 0, addrItem);

        // Value column
        auto* valItem = new QTableWidgetItem(formatValue(r.scanValue));
        valItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
        m_resultTable->setItem(i, 1, valItem);

        // Previous column
        if (showPrevious) {
            auto* prevItem = new QTableWidgetItem(
                r.previousValue.isEmpty() ? QString() : formatValue(r.previousValue));
            prevItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            m_resultTable->setItem(i, 2, prevItem);
        }
    }

    m_resultTable->blockSignals(false);
}

void ScannerPanel::onUpdateClicked() {
    if (m_results.isEmpty() || m_engine->isRunning()) return;

    std::shared_ptr<Provider> prov;
    if (m_providerGetter)
        prov = m_providerGetter();
    if (!prov) {
        m_statusLabel->setText(QStringLiteral("No source attached"));
        return;
    }

    int readSize = (m_lastScanMode == 1) ? valueSize() : 16;

    // Determine rescan condition
    ScanCondition cond = ScanCondition::ExactValue;
    if (m_lastScanMode == 1)
        cond = (ScanCondition)m_condCombo->currentData().toInt();

    // For UnknownValue on rescan, just re-read all (update only, no filter)
    if (cond == ScanCondition::UnknownValue)
        cond = ScanCondition::ExactValue; // with empty filter = update only

    // Build filter from current input field (only for ExactValue condition)
    QByteArray filterPattern, filterMask;
    if (cond == ScanCondition::ExactValue) {
        if (m_lastScanMode == 0) {
            // Signature mode
            QString err;
            if (!m_patternEdit->text().trimmed().isEmpty()) {
                if (!parseSignature(m_patternEdit->text(), filterPattern, filterMask, &err)) {
                    m_statusLabel->setText(QStringLiteral("Pattern error: %1").arg(err));
                    return;
                }
            }
        } else {
            // Value mode — exact value filter
            QString err;
            if (!m_valueEdit->text().trimmed().isEmpty()) {
                auto vt = (ValueType)m_typeCombo->currentData().toInt();
                if (!serializeValue(vt, m_valueEdit->text(), filterPattern, filterMask, &err)) {
                    m_statusLabel->setText(QStringLiteral("Value error: %1").arg(err));
                    return;
                }
                m_lastValueType = vt;
            }
        }
    }
    // Comparison conditions (Changed/Unchanged/Increased/Decreased) don't need a filter pattern

    // Update last pattern so display uses the new value
    if (!filterPattern.isEmpty())
        m_lastPattern = filterPattern;

    m_preRescanCount = m_results.size();
    m_updateBtn->setEnabled(false);
    m_scanBtn->setText(QStringLiteral("Cancel"));
    m_statusLabel->setText(QStringLiteral("Re-scanning..."));
    m_progressBar->setValue(0);
    m_progressBar->show();

    m_engine->startRescan(prov, m_results, readSize, cond, m_lastValueType,
                          filterPattern, filterMask);
}

void ScannerPanel::onRescanFinished(QVector<ScanResult> results) {
    m_scanBtn->setText(QStringLiteral("Scan"));
    m_progressBar->hide();
    m_results = std::move(results);
    m_updateBtn->setEnabled(!m_results.isEmpty());

    {
        QElapsedTimer pt;
        pt.start();
        populateTable(true);
        qDebug() << "[panel] populateTable(rescan):" << m_results.size()
                 << "results," << pt.elapsed() << "ms";
    }

    int n = m_results.size();
    if (m_preRescanCount > 0 && n < m_preRescanCount)
        m_statusLabel->setText(QStringLiteral("%1 of %2 results match")
            .arg(n).arg(m_preRescanCount));
    else
        m_statusLabel->setText(QStringLiteral("Updated %1 result%2")
            .arg(n).arg(n == 1 ? "" : "s"));
}

void ScannerPanel::onGoToAddress() {
    int row = m_resultTable->currentRow();
    if (row < 0 || row >= m_results.size()) return;
    emit goToAddress(m_results[row].address);
}

void ScannerPanel::onCopyAddress() {
    int row = m_resultTable->currentRow();
    if (row < 0 || row >= m_results.size()) return;

    QString addr = QStringLiteral("0x%1")
        .arg(m_results[row].address, 0, 16, QLatin1Char('0')).toUpper();
    QApplication::clipboard()->setText(addr);
    m_statusLabel->setText(QStringLiteral("Copied: %1").arg(addr));
}

void ScannerPanel::onResultDoubleClicked(int row, int col) {
    // Double-click on address column navigates (editing also starts via edit trigger)
    // Double-click on preview column only starts inline editing
    Q_UNUSED(col);
    Q_UNUSED(row);
    // Navigation is handled by Go to Address button or onCellEdited for address expressions
}

void ScannerPanel::onCellEdited(int row, int col) {
    if (row < 0 || row >= m_results.size()) return;

    auto* item = m_resultTable->item(row, col);
    if (!item) return;
    QString text = item->text().trimmed();

    if (col == 0) {
        // Address column — evaluate expression via AddressParser
        AddressParserCallbacks cbs;
        std::shared_ptr<Provider> prov;
        if (m_providerGetter)
            prov = m_providerGetter();
        if (prov) {
            auto* p = prov.get();
            cbs.resolveModule = [p](const QString& name, bool* ok) -> uint64_t {
                uint64_t base = p->symbolToAddress(name);
                *ok = (base != 0);
                return base;
            };
            int ptrSz = p->pointerSize();
            cbs.readPointer = [p, ptrSz](uint64_t addr, bool* ok) -> uint64_t {
                uint64_t val = 0;
                *ok = p->read(addr, &val, ptrSz);
                return val;
            };
        }
        int evalPtrSize = prov ? prov->pointerSize() : 8;
        auto result = AddressParser::evaluate(text, evalPtrSize, &cbs);
        if (result.ok) {
            m_results[row].address = result.value;
            emit goToAddress(result.value);
            // Reformat the address cell
            m_resultTable->blockSignals(true);
            QString hexPart = QStringLiteral("%1").arg(result.value, 16, 16, QLatin1Char('0')).toUpper();
            hexPart.insert(8, '`');
            item->setText(hexPart);
            // Re-read preview at new address and update cache
            if (prov) {
                int readSize = (m_lastScanMode == 1) ? valueSize() : 16;
                m_results[row].scanValue = prov->readBytes(result.value, readSize);
                if (auto* prevItem = m_resultTable->item(row, 1))
                    prevItem->setText(formatValue(m_results[row].scanValue));
            }
            m_resultTable->blockSignals(false);
        } else {
            m_statusLabel->setText(QStringLiteral("Expression error: %1").arg(result.error));
            // Restore original address
            m_resultTable->blockSignals(true);
            QString hexPart = QStringLiteral("%1").arg(m_results[row].address, 16, 16, QLatin1Char('0')).toUpper();
            hexPart.insert(8, '`');
            item->setText(hexPart);
            m_resultTable->blockSignals(false);
        }
    } else if (col == 1) {
        // Preview column — parse hex bytes and write to provider
        std::shared_ptr<Provider> prov;
        if (m_providerGetter)
            prov = m_providerGetter();
        if (!prov || !prov->isWritable()) {
            m_statusLabel->setText(QStringLiteral("Provider is read-only"));
            return;
        }
        QByteArray bytes;
        uint64_t addr = m_results[row].address;

        if (m_lastScanMode == 0) {
            // Signature mode — parse space-separated hex bytes
            QStringList tokens = text.split(' ', Qt::SkipEmptyParts);
            for (const QString& tok : tokens) {
                bool ok;
                uint val = tok.toUInt(&ok, 16);
                if (!ok || val > 0xFF) {
                    m_statusLabel->setText(QStringLiteral("Invalid hex byte: %1").arg(tok));
                    return;
                }
                bytes.append(char(val));
            }
        } else {
            // Value mode — parse native type
            bool ok = false;
            bytes.resize(valueSize());
            char* d = bytes.data();
            switch (m_lastValueType) {
            case ValueType::Int8:   { auto v = (int8_t)text.toInt(&ok);     if (ok) memcpy(d, &v, 1); break; }
            case ValueType::UInt8:  { auto v = (uint8_t)text.toUInt(&ok);   if (ok) memcpy(d, &v, 1); break; }
            case ValueType::Int16:  { auto v = (int16_t)text.toShort(&ok);  if (ok) memcpy(d, &v, 2); break; }
            case ValueType::UInt16: { auto v = text.toUShort(&ok);          if (ok) memcpy(d, &v, 2); break; }
            case ValueType::Int32:  { auto v = text.toInt(&ok);             if (ok) memcpy(d, &v, 4); break; }
            case ValueType::UInt32: { auto v = text.toUInt(&ok);            if (ok) memcpy(d, &v, 4); break; }
            case ValueType::Int64:  { auto v = text.toLongLong(&ok);        if (ok) memcpy(d, &v, 8); break; }
            case ValueType::UInt64: { auto v = text.toULongLong(&ok);       if (ok) memcpy(d, &v, 8); break; }
            case ValueType::Float:  { auto v = text.toFloat(&ok);           if (ok) memcpy(d, &v, 4); break; }
            case ValueType::Double: { auto v = text.toDouble(&ok);          if (ok) memcpy(d, &v, 8); break; }
            default: break;
            }
            if (!ok) {
                m_statusLabel->setText(QStringLiteral("Invalid value"));
                return;
            }
        }
        if (bytes.isEmpty()) return;

        if (prov->writeBytes(addr, bytes)) {
            m_statusLabel->setText(QStringLiteral("Wrote %1 byte%2 to 0x%3")
                .arg(bytes.size())
                .arg(bytes.size() == 1 ? "" : "s")
                .arg(QString::number(addr, 16).toUpper()));
            // Re-read and update cache
            m_resultTable->blockSignals(true);
            int readSize = (m_lastScanMode == 1) ? valueSize() : 16;
            m_results[row].scanValue = prov->readBytes(addr, readSize);
            item->setText(formatValue(m_results[row].scanValue));
            m_resultTable->blockSignals(false);
        } else {
            m_statusLabel->setText(QStringLiteral("Write failed"));
        }
    }
}

void ScannerPanel::applyTheme(const Theme& theme) {
    // Address delegate colors
    m_addrDelegate->dimColor = theme.textFaint;
    m_addrDelegate->brightColor = theme.text;

    // Results table — editor-matching style
    m_resultTable->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; color: %2; border: none; }"
        "QTableWidget::item { padding: 2px 6px; border: none; }"
        "QTableWidget::item:hover { background: %3; padding: 2px 6px; border: none; }"
        "QTableWidget::item:selected { background: %3; color: %2; padding: 2px 6px; border: none; }"
        "QTableWidget QLineEdit { background: %1; color: %2; border: 1px solid %4;"
        "  padding: 1px 4px; selection-background-color: %5; }")
        .arg(theme.background.name(), theme.text.name(), theme.hover.name(),
             theme.borderFocused.name(), theme.selection.name()));

    // Input fields
    QString lineEditStyle = QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 2px 4px; }"
        "QLineEdit:focus { border-color: %4; }")
        .arg(theme.background.name(), theme.text.name(),
             theme.border.name(), theme.borderFocused.name());
    m_patternEdit->setStyleSheet(lineEditStyle);
    m_valueEdit->setStyleSheet(lineEditStyle);

    // Combo boxes
    QString comboStyle = QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3; padding: 2px 4px 2px 4px; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;"
        "  width: 16px; border-left: 1px solid %3; }"
        "QComboBox::down-arrow { image: url(:/vsicons/chevron-down.svg); width: 10px; height: 10px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; selection-background-color: %4; }")
        .arg(theme.background.name(), theme.text.name(),
             theme.border.name(), theme.hover.name());
    m_modeCombo->setStyleSheet(comboStyle);
    m_typeCombo->setStyleSheet(comboStyle);
    m_condCombo->setStyleSheet(comboStyle);

    // Labels
    QPalette lp;
    lp.setColor(QPalette::WindowText, theme.textDim);
    m_patternLabel->setPalette(lp);
    m_typeLabel->setPalette(lp);
    m_condLabel->setPalette(lp);
    m_valueLabel->setPalette(lp);
    m_statusLabel->setPalette(lp);

    // Checkboxes
    QPalette cp;
    cp.setColor(QPalette::WindowText, theme.textDim);
    m_execCheck->setPalette(cp);
    m_writeCheck->setPalette(cp);
    m_structOnlyCheck->setPalette(cp);

    // Buttons
    QString btnStyle = QStringLiteral(
        "QPushButton { background: %1; color: %2; border: 1px solid %3; padding: 4px 12px; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }"
        "QPushButton:disabled { color: %6; }")
        .arg(theme.button.name(), theme.text.name(), theme.border.name(),
             theme.hover.name(), theme.hover.darker(130).name(),
             theme.textMuted.name());
    m_scanBtn->setStyleSheet(btnStyle);
    m_updateBtn->setStyleSheet(btnStyle);
    m_gotoBtn->setStyleSheet(btnStyle);
    m_copyBtn->setStyleSheet(btnStyle);

    // Progress bar
    m_progressBar->setStyleSheet(QStringLiteral(
        "QProgressBar { background: %1; border: 1px solid %2; text-align: center; color: %3; }"
        "QProgressBar::chunk { background: %4; }")
        .arg(theme.background.name(), theme.border.name(),
             theme.textDim.name(), theme.indHoverSpan.name()));
}

int ScannerPanel::valueSize() const {
    switch (m_lastValueType) {
    case ValueType::Int8:  case ValueType::UInt8:  return 1;
    case ValueType::Int16: case ValueType::UInt16: return 2;
    case ValueType::Int32: case ValueType::UInt32: case ValueType::Float: return 4;
    case ValueType::Int64: case ValueType::UInt64: case ValueType::Double: return 8;
    default: return 16;
    }
}

QString ScannerPanel::formatValue(const QByteArray& bytes) const {
    if (m_lastScanMode == 0) {
        // Signature mode — hex bytes
        QString s;
        for (int j = 0; j < bytes.size(); j++) {
            if (j > 0) s += ' ';
            s += QStringLiteral("%1").arg((uint8_t)bytes[j], 2, 16, QLatin1Char('0')).toUpper();
        }
        return s;
    }
    // Value mode — native type
    const char* d = bytes.constData();
    int sz = bytes.size();
    switch (m_lastValueType) {
    case ValueType::Int8:   if (sz >= 1) return QString::number((int8_t)d[0]); break;
    case ValueType::UInt8:  if (sz >= 1) return QString::number((uint8_t)d[0]); break;
    case ValueType::Int16:  if (sz >= 2) { int16_t v; memcpy(&v, d, 2); return QString::number(v); } break;
    case ValueType::UInt16: if (sz >= 2) { uint16_t v; memcpy(&v, d, 2); return QString::number(v); } break;
    case ValueType::Int32:  if (sz >= 4) { int32_t v; memcpy(&v, d, 4); return QString::number(v); } break;
    case ValueType::UInt32: if (sz >= 4) { uint32_t v; memcpy(&v, d, 4); return QString::number(v); } break;
    case ValueType::Int64:  if (sz >= 8) { int64_t v; memcpy(&v, d, 8); return QString::number(v); } break;
    case ValueType::UInt64: if (sz >= 8) { uint64_t v; memcpy(&v, d, 8); return QString::number(v); } break;
    case ValueType::Float:  if (sz >= 4) { float v; memcpy(&v, d, 4); return QString::number(v, 'g', 9); } break;
    case ValueType::Double: if (sz >= 8) { double v; memcpy(&v, d, 8); return QString::number(v, 'g', 17); } break;
    default: break;
    }
    return QStringLiteral("??");
}

} // namespace rcx
