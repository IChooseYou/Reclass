#include "scannerpanel.h"
#include "addressparser.h"
#include "themes/thememanager.h"
#include <cstring>
#include <QElapsedTimer>
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QInputDialog>
#include <QPainter>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QShortcut>
#include <QDrag>
#include <QMimeData>
#include <QTimer>
#include <QLocale>
#include <QStyle>
#include <QFrame>

namespace rcx {

// Forward declaration: defined later — used by lambdas in the constructor
// that translate sortable-table rows back to m_results indices.
static int rowToResultIdx(QTableWidget* tbl, int row);

// ── FilterChip ──
// Drop-in replacement for QCheckBox that paints in the same flat-pip style
// as the type-chooser CategoryChip. Derives from QCheckBox so the panel's
// public API (execCheck() / writeCheck() / etc. returning QCheckBox*) and
// every existing test against those getters stay intact — we just override
// the painting to look like a chip rather than a native check square.
class FilterChip : public QCheckBox {
public:
    explicit FilterChip(const QString& label, QWidget* parent = nullptr)
        : QCheckBox(label, parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setMouseTracking(true);
    }
    QSize sizeHint() const override {
        QFontMetrics fm(font());
        return QSize(5 + 4 + fm.horizontalAdvance(text()) + 16, fm.height() + 4);
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto& t = ThemeManager::instance().current();
        bool hov = underMouse();
        bool chk = isChecked();
        // Mirrors CategoryChip: pip uses indHoverSpan when checked
        // (matches the editor's accent color across the app).
        QColor onColor = t.indHoverSpan;

        if (hov) p.fillRect(rect(), t.hover);

        const int pipSz = 5;
        const int gap   = 4;
        QFontMetrics fm(font());
        int textW  = fm.horizontalAdvance(text());
        int blockW = pipSz + gap + textW;
        int x      = (width() - blockW) / 2;
        int baseline = (height() + fm.ascent() - fm.descent()) / 2;

        p.fillRect(x, (height() - pipSz) / 2, pipSz, pipSz,
                   chk ? onColor : t.textFaint);
        x += pipSz + gap;

        p.setPen(chk ? t.text : t.textMuted);
        p.setFont(font());
        p.drawText(x, baseline, text());
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

// ── EmptyResultsTable ──
// QTableWidget with a centered placeholder painted when row count is 0.
// Clearer than a blank gray rectangle which the user might misread as
// "the panel is broken".
class EmptyResultsTable : public QTableWidget {
public:
    using QTableWidget::QTableWidget;
    QString placeholder = QStringLiteral(
        "Set scan criteria above and click First Scan to begin.");
    // [iter 48] Optional secondary line that names the keyboard shortcut.
    // Painted dimmer than the primary line so the eye reads it as a hint.
    QString hint = QStringLiteral(
        "Tip — press Enter inside the value field, or Ctrl+Return anywhere.");
protected:
    void paintEvent(QPaintEvent* e) override {
        QTableWidget::paintEvent(e);
        if (rowCount() != 0) return;
        QPainter p(viewport());
        p.setRenderHint(QPainter::TextAntialiasing);
        const auto& t = ThemeManager::instance().current();
        p.setFont(font());
        // Two-line layout: primary call-to-action centered, smaller hint
        // beneath. Both clipped to viewport rect with word-wrap so the
        // placeholder doesn't overflow on narrow docks.
        QRect r = viewport()->rect();
        QFontMetrics fm(font());
        int totalH = fm.height() * 2 + 6;
        QRect primary  = r;
        primary.setHeight(r.height()/2 + totalH/2 - fm.height());
        QRect secondary = r;
        secondary.setTop(primary.bottom() + 6);
        p.setPen(t.textMuted);
        p.drawText(primary,   Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, placeholder);
        p.setPen(t.textFaint);
        p.drawText(secondary, Qt::AlignHCenter | Qt::AlignTop    | Qt::TextWordWrap, hint);
    }
};

// ── ScanButton ──
// A single uniform button class for every scanner action so all five
// (First Scan / Next Scan / Reset / Go to Address / Copy Address) end up
// with identical height, padding, font, and theme behaviour. Previously
// each button was a raw QPushButton + per-button stylesheet — the result
// was a row where Reset looked taller than First Scan and Copy Address
// had different padding from Go to Address. Subclass so each button still
// pulls its theme colors from the live theme manager.
class ScanButton : public QPushButton {
public:
    enum Style { Primary, Secondary, Subtle };
    explicit ScanButton(const QIcon& ic, const QString& label,
                        Style s = Secondary, QWidget* parent = nullptr)
        : QPushButton(ic, label, parent), m_style(s) {
        setIconSize(QSize(14, 14));
        setCursor(Qt::PointingHandCursor);
        // [iter 10] All ScanButton instances share a fixed 28 px height
        // (vs minimumHeight 26) so the action row and the selection row
        // are exactly the same height regardless of font size.
        setFixedHeight(28);
        applyChrome();
    }
    void setVariant(Style s) { m_style = s; applyChrome(); }
    // [iter 47] Allow the panel to ask for a re-style after toggling the
    // scanCancel dynamic property — without this the cancel-state tint is
    // invisible until the next theme switch.
    void refreshChrome() { applyChrome(); }
private:
    Style m_style;
    void applyChrome() {
        const auto& t = ThemeManager::instance().current();
        // Match the type-chooser visual language: flat, hairline-bordered,
        // hover background = theme.hover, NEVER a solid accent fill. The
        // "primary" affordance is a thin 2 px accent border on top — a
        // subtle pulse, not a glaring purple slab.
        QString fg, accent;
        switch (m_style) {
        case Primary:
            fg     = t.text.name();
            accent = t.indHoverSpan.name();   // 2 px top stripe only
            break;
        case Secondary:
            fg     = t.text.name();
            accent = QStringLiteral("transparent");
            break;
        case Subtle:
            fg     = t.textDim.name();
            accent = QStringLiteral("transparent");
            break;
        }
        // [iter 47] When the button is mid-cancel (scan in flight), use the
        // theme's `focusGlow` (documented as warm amber, used by the MCP
        // focus pulse) for the top stripe — reads as "click to abort"
        // instead of "click to start a fresh scan." Pulled from the theme
        // so light/dark/custom themes stay consistent.
        if (property("scanCancel").toBool()) {
            accent = t.focusGlow.name();
        }
        setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color:%1;"
            "  border: 1px solid %2; border-top: 2px solid %3;"
            "  border-radius: 2px; padding: 2px 12px; }"
            "QPushButton:hover { background:%4; }"
            "QPushButton:pressed { background:%5; }"
            "QPushButton:disabled { color:%6; border-top-color: %2; }")
            .arg(fg, t.border.name(), accent,
                 t.hover.name(), t.hover.darker(115).name(), t.textMuted.name()));
    }
};

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
    // [iter 1] Slightly larger right margin so Reset button doesn't sit
    // flush against the dock edge / resize grip.
    mainLayout->setContentsMargins(10, 8, 12, 8);
    // [iter 2] Looser vertical rhythm between sections (8 → 7 inside row,
    // sections add their own header padding for breathing).
    mainLayout->setSpacing(7);

    // Helper: small all-caps section header. Mirrors the workspace's
    // "ALL TYPES" / "PINNED" label style — proper visual grouping
    // instead of inline "Filters:" labels jammed into the controls.
    auto makeSectionHeader = [this](const QString& text) -> QLabel* {
        auto* lbl = new QLabel(text, this);
        lbl->setProperty("scannerHeader", true);
        QFont f = lbl->font();
        f.setPointSize(qMax(8, f.pointSize() - 2));
        f.setCapitalization(QFont::AllUppercase);
        // [iter 3] Brighter section headers — bumped letter-spacing from 1
        // to 1.5 px so they read as proper category dividers, not muted
        // labels easy to miss.
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
        // [iter 4] Bolder weight for the section header so it competes
        // for attention with the inputs that follow it.
        f.setWeight(QFont::DemiBold);
        lbl->setFont(f);
        // [iter 5] Top margin gives breathing room above each section,
        // bottom margin keeps the input cluster snug to its header.
        lbl->setContentsMargins(0, 4, 0, 1);
        return lbl;
    };

    // ═══════════════════════════════════════════════════════════════════
    // ROW 1 — Action buttons (Cheat Engine layout)
    // ═══════════════════════════════════════════════════════════════════
    // First Scan / Next Scan / Undo Scan live at the very top so the
    // user's eye lands on the workflow controls before reading the inputs.
    // Reset (subtle) is pushed all the way right so it can't be misclicked.
    auto* actionRow = new QHBoxLayout;
    // [iter 6] Slightly tighter button cluster — 4 px between siblings
    // makes the button group read as one unit.
    actionRow->setSpacing(4);
    actionRow->setContentsMargins(0, 0, 0, 0);

    // [iter 26] Alt-key accelerators on every action button so power
    // users can drive the workflow from keyboard alone. The leading "&"
    // marks the underlined letter; matches Windows convention.
    m_scanBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/search.svg")),
                                QStringLiteral("&First Scan"),
                                ScanButton::Primary, this);
    m_scanBtn->setToolTip(QStringLiteral(
        "Search memory and capture matching addresses.  (Alt+F)"));
    actionRow->addWidget(m_scanBtn);

    m_updateBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/refresh.svg")),
                                  QStringLiteral("&Next Scan"),
                                  ScanButton::Primary, this);
    m_updateBtn->setToolTip(QStringLiteral(
        "Narrow existing results with the current condition.  (Alt+N)"));
    m_updateBtn->setEnabled(false);
    actionRow->addWidget(m_updateBtn);

    m_undoBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/arrow-left.svg")),
                                QStringLiteral("&Undo Scan"),
                                ScanButton::Secondary, this);
    m_undoBtn->setToolTip(QStringLiteral(
        "Revert to the previous result list (use after a Next Scan that "
        "narrowed too aggressively).  (Alt+U / Ctrl+Z)"));
    m_undoBtn->setEnabled(false);
    actionRow->addWidget(m_undoBtn);

    // [moved] m_stageLabel previously lived in the action row next to the
    // buttons. It now sits above the results table (where the redundant
    // "N results found" label used to be) — same information, one home.
    // Constructed without a parent layout here; added to mainLayout below.
    m_stageLabel = new QLabel(this);
    m_stageLabel->setContentsMargins(0, 0, 0, 0);
    m_stageLabel->setTextFormat(Qt::RichText);
    m_stageLabel->setWordWrap(true);
    m_stageLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_stageLabel->setMinimumWidth(0);

    m_newScanBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/clear-all.svg")),
                                   QStringLiteral("&Reset"),
                                   ScanButton::Subtle, this);
    m_newScanBtn->setToolTip(QStringLiteral(
        "Discard current results and start over.  (Alt+R)"));
    m_newScanBtn->setVisible(false);
    // Reset gets a fixed natural width so the breadcrumb can never crowd
    // it off-screen; sizePolicy(Fixed) plus the explicit ordering
    // guarantees Reset always reserves its slot first.
    m_newScanBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    actionRow->addWidget(m_newScanBtn, 0, Qt::AlignRight);

    mainLayout->addLayout(actionRow);
    updateStageLabel();

    // Skinny progress strip — invisible until a scan is in flight.
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(2);
    m_progressBar->hide();
    mainLayout->addWidget(m_progressBar);

    // ═══════════════════════════════════════════════════════════════════
    // FORM GRID — Value / Scan Type / Value Type, aligned in two columns
    // ═══════════════════════════════════════════════════════════════════
    // QGridLayout guarantees the labels share a fixed-width column and
    // the input widgets line up vertically — three rows of inputs that
    // read like a tidy form, not a randomly-spaced toolbar.
    auto* form = new QGridLayout;
    // [iter 7] Bumped horizontal spacing 6 → 8 so labels breathe from
    // their inputs; vertical spacing 4 → 5 so rows don't feel cramped.
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(5);
    form->setContentsMargins(0, 4, 0, 0);
    // [iter 8] Wider label column — 84 → 92 px — so "Value Type:" never
    // truncates to "Value Type" without the colon at small font sizes.
    form->setColumnMinimumWidth(0, 92);
    form->setColumnStretch(0, 0);
    form->setColumnStretch(1, 1);        // input column absorbs slack

    // ── Row A: Value / Pattern ──
    m_valueLabel = new QLabel(QStringLiteral("Value:"), this);
    m_valueLabel->setProperty("scannerSection", true);
    m_valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->addWidget(m_valueLabel, 0, 0);

    auto* valueWrap = new QHBoxLayout;
    valueWrap->setSpacing(6);
    valueWrap->setContentsMargins(0, 0, 0, 0);

    m_valueEdit = new QLineEdit(this);
    m_valueEdit->setPlaceholderText(QStringLiteral(
        "Decimal value, or 0xABCD for hex"));
    m_valueEdit->setToolTip(QStringLiteral(
        "Decimal by default. Prefix with 0x for hexadecimal: 0xABCD."));
    // [iter 18] Native clear-X button on every input field — saves a
    // user from having to select-all + delete to start over.
    m_valueEdit->setClearButtonEnabled(true);
    valueWrap->addWidget(m_valueEdit, 2);

    // Pattern input shares the value row — only one is visible at a time.
    m_patternLabel = new QLabel(QString(), this);
    m_patternLabel->setVisible(false);  // legacy API stub
    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setPlaceholderText(QStringLiteral(
        "Hex pattern, e.g. 48 8B ?? 05 ?? ?? CC   (?? = wildcard)"));
    m_patternEdit->setToolTip(QStringLiteral(
        "Space-separated hex bytes. Use ?? to skip a byte. "
        "Also accepts \\xAB\\xCD or packed 488B05CC."));
    m_patternEdit->setClearButtonEnabled(true);  // [iter 19]
    valueWrap->addWidget(m_patternEdit, 2);

    m_value2Label = new QLabel(QStringLiteral("…to:"), this);
    m_value2Label->setProperty("scannerSection", true);
    m_value2Label->hide();
    valueWrap->addWidget(m_value2Label);
    m_value2Edit = new QLineEdit(this);
    m_value2Edit->setPlaceholderText(QStringLiteral("upper bound"));
    m_value2Edit->setClearButtonEnabled(true);  // [iter 20]
    m_value2Edit->hide();
    valueWrap->addWidget(m_value2Edit, 1);

    form->addLayout(valueWrap, 0, 1);

    // ── Row B: Scan Type ──
    m_condLabel = new QLabel(QStringLiteral("Scan Type:"), this);
    m_condLabel->setProperty("scannerSection", true);
    m_condLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->addWidget(m_condLabel, 1, 0);

    auto* condRow = new QHBoxLayout;
    condRow->setSpacing(6);
    condRow->setContentsMargins(0, 0, 0, 0);

    m_condCombo = new QComboBox(this);
    // Coloured icons earn a few of the entries to make the combo easier to
    // skim. We have 3 VS palette colours available:
    //   blue   #75BEFF  (symbol-variable) — exact-value lookups
    //   orange #EE9D28  (symbol-event)    — numeric range comparisons
    //   gray   default  (regex)           — signature / pattern scan
    // Other entries (delta family, unknown) stay icon-less so the colour
    // signal stays meaningful, not noise.
    QIcon icoValue (QStringLiteral(":/vsicons/symbol-variable.svg"));
    QIcon icoRange (QStringLiteral(":/vsicons/symbol-event.svg"));
    QIcon icoSig   (QStringLiteral(":/vsicons/regex.svg"));

    m_condCombo->addItem(icoValue, QStringLiteral("Exact Value"),  (int)ScanCondition::ExactValue);
    m_condCombo->addItem(icoSig,   QStringLiteral("Exact Sig"),    -1);   // -1 sentinel = Signature mode
    m_condCombo->insertSeparator(m_condCombo->count());
    m_condCombo->addItem(QIcon(),  QStringLiteral("Unknown"),      (int)ScanCondition::UnknownValue);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Changed"),      (int)ScanCondition::Changed);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Unchanged"),    (int)ScanCondition::Unchanged);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Increased"),    (int)ScanCondition::Increased);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Decreased"),    (int)ScanCondition::Decreased);
    m_condCombo->insertSeparator(m_condCombo->count());
    m_condCombo->addItem(icoRange, QStringLiteral("Bigger Than"),  (int)ScanCondition::BiggerThan);
    m_condCombo->addItem(icoRange, QStringLiteral("Smaller Than"), (int)ScanCondition::SmallerThan);
    m_condCombo->addItem(icoRange, QStringLiteral("Between"),      (int)ScanCondition::Between);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Increased By"), (int)ScanCondition::IncreasedBy);
    m_condCombo->addItem(QIcon(),  QStringLiteral("Decreased By"), (int)ScanCondition::DecreasedBy);
    m_condCombo->setToolTip(QStringLiteral(
        "Cheat-Engine scan condition. Exact Value matches a literal number; "
        "Exact Sig switches to byte-pattern (signature) scanning; "
        "Changed/Unchanged/Increased/Decreased compare against the "
        "previous scan's values."));
    // [iter 34] Per-entry tooltips on the dropdown so a user hovering a
    // condition name in the open menu can see exactly what it does without
    // committing to it. Qt exposes this via Qt::ToolTipRole on each item.
    auto setCondTip = [this](const QString& label, const QString& tip) {
        int i = m_condCombo->findText(label);
        if (i >= 0) m_condCombo->setItemData(i, tip, Qt::ToolTipRole);
    };
    setCondTip(QStringLiteral("Exact Value"),  QStringLiteral("Find addresses whose value equals the number you type."));
    setCondTip(QStringLiteral("Exact Sig"),    QStringLiteral("Search for a byte pattern (signature). Use ?? for wildcards."));
    setCondTip(QStringLiteral("Unknown"),      QStringLiteral("Capture every address — use this first when you don't know the value yet, then narrow with Next Scan."));
    setCondTip(QStringLiteral("Changed"),      QStringLiteral("Keep addresses whose value is different from the last scan."));
    setCondTip(QStringLiteral("Unchanged"),    QStringLiteral("Keep addresses whose value is the same as the last scan."));
    setCondTip(QStringLiteral("Increased"),    QStringLiteral("Keep addresses whose value is larger than the last scan."));
    setCondTip(QStringLiteral("Decreased"),    QStringLiteral("Keep addresses whose value is smaller than the last scan."));
    setCondTip(QStringLiteral("Bigger Than"),  QStringLiteral("Keep addresses whose value is greater than the number you type."));
    setCondTip(QStringLiteral("Smaller Than"), QStringLiteral("Keep addresses whose value is less than the number you type."));
    setCondTip(QStringLiteral("Between"),      QStringLiteral("Keep addresses whose value falls within the two bounds you supply."));
    setCondTip(QStringLiteral("Increased By"), QStringLiteral("Keep addresses where new = old + delta. Type the delta in the value field."));
    setCondTip(QStringLiteral("Decreased By"), QStringLiteral("Keep addresses where new = old − delta. Type the delta in the value field."));
    condRow->addWidget(m_condCombo, 1);

    // Mode combo is no longer a separate widget — Signature / Value are
    // now folded into the Scan Type combo above ("Exact Sig" sentinel).
    // Kept as a hidden zombie so existing test/API hooks that read
    // modeCombo()->currentIndex() / setCurrentIndex still work; toggled
    // automatically by the cond-combo handler.
    m_modeCombo = new QComboBox(this);
    m_modeCombo->addItem(QStringLiteral("Signature"));
    m_modeCombo->addItem(QStringLiteral("Value"));
    m_modeCombo->setCurrentIndex(1);
    m_modeCombo->hide();

    form->addLayout(condRow, 1, 1);

    // ── Row C: Value Type ──
    m_typeLabel = new QLabel(QStringLiteral("Value Type:"), this);
    m_typeLabel->setProperty("scannerSection", true);
    m_typeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->addWidget(m_typeLabel, 2, 0);

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
    m_typeCombo->setToolTip(QStringLiteral("Bit width / signedness"));
    form->addWidget(m_typeCombo, 2, 1);

    mainLayout->addLayout(form);

    // ═══════════════════════════════════════════════════════════════════
    // SECTION — WHERE TO SCAN
    // ═══════════════════════════════════════════════════════════════════
    mainLayout->addWidget(makeSectionHeader(QStringLiteral("Where to scan")));

    auto* filterRow = new QHBoxLayout;
    // [iter 9] Chip row gets 4 px sibling spacing matching action row.
    filterRow->setSpacing(4);
    filterRow->setContentsMargins(0, 0, 0, 0);

    m_execCheck = new FilterChip(QStringLiteral("Executable"), this);
    m_execCheck->setToolTip(QStringLiteral(
        "Scan executable code regions (.text). Default for Signature mode."));
    filterRow->addWidget(m_execCheck);

    m_writeCheck = new FilterChip(QStringLiteral("Writable"), this);
    m_writeCheck->setToolTip(QStringLiteral(
        "Scan writable regions (heap, stack, .data). Default for Value mode."));
    filterRow->addWidget(m_writeCheck);

    m_userModeOnlyCheck = new FilterChip(QStringLiteral("User-mode VA"), this);
    m_userModeOnlyCheck->setToolTip(QStringLiteral(
        "Cap scan to user-mode virtual address range. Skips kernel and "
        "shadow ranges entirely."));
    filterRow->addWidget(m_userModeOnlyCheck);

    m_structOnlyCheck = new FilterChip(QStringLiteral("Current struct"), this);
    m_structOnlyCheck->setToolTip(QStringLiteral(
        "Only scan inside the bounds of the currently-viewed struct."));
    filterRow->addWidget(m_structOnlyCheck);

    // [iter 59] Spacer between chips and Fast-Scan dropdown so the dropdown
    // doesn't touch the last chip — visually separates "what to scan" from
    // "how aggressively".
    filterRow->addSpacing(8);

    // Fast Scan alignment dropdown — Cheat-Engine convention. Higher
    // alignment = faster scan but skips values stored at unusual offsets.
    // 4 is the dword default that matches typical game state layout.
    m_fastScanLabel = new QLabel(QStringLiteral("Fast Scan:"), this);
    m_fastScanLabel->setProperty("scannerSection", true);
    // [iter 60] Right-align label to match its sibling form labels — looks
    // like a cohesive form row, not a stray label dropped into a chip strip.
    m_fastScanLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    filterRow->addWidget(m_fastScanLabel);
    m_fastScanCombo = new QComboBox(this);
    for (int a : {1, 4, 8, 16, 32, 64})
        m_fastScanCombo->addItem(QString::number(a), a);
    m_fastScanCombo->setCurrentIndex(1);  // 4 = CE default
    m_fastScanCombo->setToolTip(QStringLiteral(
        "Address alignment for Value scans. 4 (dword) is the Cheat-Engine "
        "default — fastest, finds 99% of game values. Drop to 1 to scan "
        "every byte position. Higher values are even faster but only find "
        "values aligned to that boundary."));
    // [iter 61] Compact width — the dropdown only ever holds 1-2 digit
    // numbers, so there's no point letting it stretch and steal pixels
    // from the chips on its left.
    m_fastScanCombo->setMaximumWidth(64);
    // Font is set later in setEditorFont() — using the same `font` value
    // that's applied to the chip widgets, the Scan-Type combo, and the
    // Value-Type combo so everything in this row renders with one font.
    filterRow->addWidget(m_fastScanCombo);

    // Stubs kept for API/test compatibility — hidden, never displayed.
    m_fastScanCheck = new FilterChip(QString(), this);
    m_fastScanCheck->setChecked(true);
    m_fastScanCheck->hide();
    m_privateOnlyCheck = new FilterChip(QString(), this);
    m_privateOnlyCheck->hide();
    m_skipSystemCheck = new FilterChip(QString(), this);
    m_skipSystemCheck->hide();

    filterRow->addStretch();
    mainLayout->addLayout(filterRow);

    // ═══════════════════════════════════════════════════════════════════
    // SECTION 4 — RESULTS
    // ═══════════════════════════════════════════════════════════════════
    mainLayout->addWidget(makeSectionHeader(QStringLiteral("Results")));

    // Stage label (the breadcrumb with the colored circle dot, e.g.
    // "● First scan: 47 results — change condition and click Next Scan")
    // lives directly under the RESULTS header. This is the SOLE place that
    // surfaces result counts and workflow guidance — no duplicate label.
    {
        QFontMetrics fm(m_stageLabel->font());
        m_stageLabel->setMinimumHeight(fm.height() + 4);
    }
    m_stageLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mainLayout->addWidget(m_stageLabel);

    // m_statusLabel kept as a hidden sibling so existing call sites that
    // write transient status (errors, copy/write feedback, live filter
    // "X of Y match") don't crash. Mirrored into the stage label by
    // updateScanStatusLine() so the user actually sees them.
    m_statusLabel = new QLabel(QString(), this);
    m_statusLabel->setProperty("scannerStatus", true);
    m_statusLabel->setTextFormat(Qt::AutoText);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->hide();

    m_resultFilter = new QLineEdit(this);
    m_resultFilter->setPlaceholderText(QStringLiteral(
        "Filter shown results — type any address, value, or module substring"));
    m_resultFilter->setClearButtonEnabled(true);
    // [iter 42] Disable the filter when there's nothing to filter — an
    // editable textbox above an empty table reads as "the tool is broken,
    // why won't anything show up here". Re-enabled in populateTable().
    m_resultFilter->setEnabled(false);
    // [iter 43] Tooltip mentions Ctrl+L to focus and Esc to clear so the
    // shortcut isn't a hidden affordance.
    m_resultFilter->setToolTip(QStringLiteral(
        "Filter the rows currently shown.  Ctrl+L to focus, Esc to clear."));
    mainLayout->addWidget(m_resultFilter);

    // Truncation banner — only visible when display is capped.
    m_truncBanner = new QLabel(this);
    m_truncBanner->setVisible(false);
    m_truncBanner->setContentsMargins(6, 3, 6, 3);
    mainLayout->addWidget(m_truncBanner);

    // ── Results table ──
    m_resultTable = new QTableWidget(this);
    m_resultTable->setColumnCount(2);
    m_resultTable->horizontalHeader()->hide();
    m_resultTable->verticalHeader()->hide();
    m_resultTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_resultTable->horizontalHeader()->setStretchLastSection(true);
    // Highlight-tracking on hover gives the user clear feedback about which
    // row a click would target — table rows look more interactive.
    m_resultTable->horizontalHeader()->setHighlightSections(false);
    m_resultTable->horizontalHeader()->setSectionsClickable(true);
    // [iter 70] Show the sort indicator arrow in the header so users see
    // which column they're sorted by — invisible-by-default sort is one of
    // the most common "is this even sortable?" usability traps.
    m_resultTable->horizontalHeader()->setSortIndicatorShown(true);
    m_resultTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Multi-select so users can batch Delete / Copy / Add-as-nodes against
    // groups of hits. Single-select mode forced one-row-at-a-time everything.
    m_resultTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_resultTable->setEditTriggers(QAbstractItemView::DoubleClicked);
    m_resultTable->setShowGrid(false);
    m_resultTable->setMouseTracking(true);
    m_resultTable->setFocusPolicy(Qt::StrongFocus);
    m_resultTable->setContextMenuPolicy(Qt::CustomContextMenu);
    // Drag-out support — dropping onto the editor adds the address as a node.
    m_resultTable->setDragEnabled(true);
    m_resultTable->setDragDropMode(QAbstractItemView::DragOnly);
    // [iter 69] Tooltip on the table itself advertises the (otherwise
    // hidden) drag-to-editor affordance. Shown when the user hovers any
    // empty area of the table — Qt routes table-level tooltips here.
    m_resultTable->setToolTip(QStringLiteral(
        "Double-click to edit • drag a row into the editor to add a node • "
        "right-click for batch actions"));
    // Alternating row colors disabled — the banding fights the address
    // delegate's dim/bright two-tone scheme and makes the table look noisy.
    m_resultTable->setAlternatingRowColors(false);
    // [iter 35] Smooth pixel-by-pixel scroll for the results table — feels
    // dramatically less janky on huge lists than the default per-row jumps,
    // particularly when flicking with a trackpad.
    m_resultTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // [iter 36] Word-wrap off — addresses + values are fixed-width and we
    // want consistent row heights for predictable navigation. (Qt default
    // is true, which silently grows row height on long preview values.)
    m_resultTable->setWordWrap(false);
    // [iter 37] Slightly tighter row height — Qt's default leaves too much
    // air around monospaced rows. 4px in each padding direction lines up
    // visually with the editor pane.
    m_resultTable->verticalHeader()->setDefaultSectionSize(
        m_resultTable->fontMetrics().height() + 8);

    // Address column delegate for dimmed leading zeros
    m_addrDelegate = new AddressDelegate(this);
    m_resultTable->setItemDelegateForColumn(0, m_addrDelegate);
    mainLayout->addWidget(m_resultTable, 1);

    // ── Bottom strip: per-row actions ──
    // Status line moved up under the Results header — this row only carries
    // the per-selection actions now.
    auto* selectionRow = new QHBoxLayout;
    selectionRow->setSpacing(6);
    selectionRow->setContentsMargins(0, 2, 0, 0);
    selectionRow->addStretch(1);

    m_gotoBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/arrow-right.svg")),
                                QStringLiteral("Go to"),
                                ScanButton::Secondary, this);
    m_gotoBtn->setToolTip(QStringLiteral(
        "Jump the editor to the selected address (Enter / double-click)."));
    m_gotoBtn->setEnabled(false);
    selectionRow->addWidget(m_gotoBtn);

    m_copyBtn = new ScanButton(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                                QStringLiteral("Copy"),
                                ScanButton::Secondary, this);
    m_copyBtn->setToolTip(QStringLiteral(
        "Copy the selected address to the clipboard (Ctrl+C)."));
    m_copyBtn->setEnabled(false);
    selectionRow->addWidget(m_copyBtn);
    selectionRow->addSpacing(20);  // room for resize grip when floating

    mainLayout->addLayout(selectionRow);

    // ── Initial state: VALUE mode + Exact Value ──
    // Value scans are the more common starting point (find a number in
    // memory, mutate it, verify). The user opts into Signature scanning
    // by picking "Exact Sig" from the Scan Type combo.
    m_patternLabel->hide();
    m_patternEdit->hide();
    m_writeCheck->setChecked(true);
    onConditionChanged(0);  // sync field visibility for the default cond

    // ── Connections ──
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScannerPanel::onModeChanged);
    connect(m_condCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ScannerPanel::onConditionChanged);
    connect(m_scanBtn, &QPushButton::clicked,
            this, &ScannerPanel::onScanClicked);
    // Live input validation — disable First Scan when there's nothing to
    // scan FOR, so the button never fires an empty request.
    auto syncScanEnabled = [this]() {
        if (m_engine->isRunning()) return;  // running scan owns its own state
        bool isSig = m_modeCombo->currentIndex() == 0;
        bool needsValue = false;
        if (!isSig) {
            auto cond = (ScanCondition)m_condCombo->currentData().toInt();
            needsValue = (cond == ScanCondition::ExactValue
                       || cond == ScanCondition::BiggerThan
                       || cond == ScanCondition::SmallerThan
                       || cond == ScanCondition::Between
                       || cond == ScanCondition::IncreasedBy
                       || cond == ScanCondition::DecreasedBy);
        }
        bool hasInput = isSig
            ? !m_patternEdit->text().trimmed().isEmpty()
            : (!needsValue || !m_valueEdit->text().trimmed().isEmpty());
        m_scanBtn->setEnabled(hasInput);
    };
    connect(m_patternEdit, &QLineEdit::textChanged, this, syncScanEnabled);
    connect(m_valueEdit,   &QLineEdit::textChanged, this, syncScanEnabled);
    connect(m_modeCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, syncScanEnabled);
    connect(m_condCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, syncScanEnabled);
    syncScanEnabled();

    // Pressing Enter in any input field runs the appropriate scan action
    // (First Scan if no results yet, Next Scan otherwise) — saves a click
    // for keyboard-driven workflows.
    auto onReturn = [this]() {
        if (m_engine->isRunning()) return;
        if (m_updateBtn->isEnabled()) onUpdateClicked();
        else if (m_scanBtn->isEnabled()) onScanClicked();
    };
    connect(m_patternEdit, &QLineEdit::returnPressed, this, onReturn);
    connect(m_valueEdit,   &QLineEdit::returnPressed, this, onReturn);
    connect(m_value2Edit,  &QLineEdit::returnPressed, this, onReturn);

    // Live filter summary — hover any chip to see a one-line summary of
    // every filter currently active. Helps when the user can't tell why
    // a scan is returning fewer hits than expected.
    auto buildSummary = [this]() {
        QStringList parts;
        if (m_execCheck->isChecked())        parts << "Executable";
        if (m_writeCheck->isChecked())       parts << "Writable";
        if (m_privateOnlyCheck->isChecked()) parts << "Private only";
        if (m_skipSystemCheck->isChecked())  parts << "Skip system DLLs";
        if (m_userModeOnlyCheck->isChecked())parts << "User-mode VA";
        if (m_structOnlyCheck->isChecked())  parts << "Current Struct";
        QString s = parts.isEmpty() ? QStringLiteral("All readable memory")
                                     : parts.join(QStringLiteral(" + "));
        QString tip = QStringLiteral("Active filters:\n  %1").arg(s);
        for (auto* c : {m_execCheck, m_writeCheck, m_privateOnlyCheck,
                         m_skipSystemCheck, m_userModeOnlyCheck, m_structOnlyCheck}) {
            // Preserve each chip's own tooltip when set, just append the summary.
            QString own = c->property("base_tooltip").toString();
            if (own.isEmpty() && !c->toolTip().isEmpty()) {
                own = c->toolTip();
                c->setProperty("base_tooltip", own);
            }
            c->setToolTip(own.isEmpty() ? tip : own + QStringLiteral("\n\n") + tip);
        }
    };
    for (auto* c : {m_execCheck, m_writeCheck, m_privateOnlyCheck,
                     m_skipSystemCheck, m_userModeOnlyCheck, m_structOnlyCheck}) {
        connect(c, &QCheckBox::toggled, this, buildSummary);
    }
    buildSummary();
    connect(m_updateBtn, &QPushButton::clicked,
            this, &ScannerPanel::onUpdateClicked);
    connect(m_undoBtn, &QPushButton::clicked, this, [this]() {
        popUndoSnapshot();
    });
    connect(m_newScanBtn, &QPushButton::clicked,
            this, &ScannerPanel::onNewScanClicked);
    // [iter 68] Debounce filter input by 80 ms — at 10K rows the loop
    // through every cell on every keystroke noticeably stalls typing.
    // 80 ms is below human-perceptible delay but coalesces fast typing
    // bursts into a single re-filter pass.
    {
        auto* filterTimer = new QTimer(this);
        filterTimer->setSingleShot(true);
        filterTimer->setInterval(80);
        connect(filterTimer, &QTimer::timeout, this, [this]() {
            onResultFilterChanged(m_resultFilter->text());
        });
        connect(m_resultFilter, &QLineEdit::textChanged, this,
                [filterTimer](const QString&) { filterTimer->start(); });
    }
    connect(m_gotoBtn, &QPushButton::clicked,
            this, &ScannerPanel::onGoToAddress);
    connect(m_copyBtn, &QPushButton::clicked,
            this, &ScannerPanel::onCopyAddress);
    connect(m_resultTable, &QTableWidget::cellDoubleClicked,
            this, &ScannerPanel::onResultDoubleClicked);
    connect(m_resultTable, &QTableWidget::cellChanged,
            this, &ScannerPanel::onCellEdited);

    // ── Keyboard shortcuts (panel-scoped) ──
    auto* scanShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    scanShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(scanShortcut, &QShortcut::activated, this, [this]() {
        if (m_updateBtn->isEnabled() && !m_engine->isRunning())
            onUpdateClicked();
        else
            onScanClicked();
    });
    auto* rescanShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    rescanShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(rescanShortcut, &QShortcut::activated, this, [this]() {
        if (m_updateBtn->isEnabled()) onUpdateClicked();
    });
    // [iter 21] Esc inside the result-filter clears it (matches the
    // editor's find bar behaviour).
    auto* clearFilterShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), m_resultFilter);
    clearFilterShortcut->setContext(Qt::WidgetShortcut);
    connect(clearFilterShortcut, &QShortcut::activated, m_resultFilter, &QLineEdit::clear);
    // [iter 22] Ctrl+L focuses the result filter — common "jump to
    // search" muscle memory from terminals/browsers.
    auto* focusFilter = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this);
    focusFilter->setContext(Qt::WidgetWithChildrenShortcut);
    connect(focusFilter, &QShortcut::activated, this, [this]() {
        m_resultFilter->setFocus();
        m_resultFilter->selectAll();
    });
    // [iter 23] Esc on the panel cancels a running scan instead of
    // forcing the user to find the Cancel button.
    auto* cancelShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    cancelShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(cancelShortcut, &QShortcut::activated, this, [this]() {
        if (m_engine->isRunning()) m_engine->abort();
    });
    // [iter 24] Ctrl+Z = Undo Scan; Ctrl+Shift+Z disabled (CE doesn't
    // have a redo concept). Activates only when Undo Scan is enabled.
    auto* undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this);
    undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoShortcut, &QShortcut::activated, this, [this]() {
        if (m_undoBtn->isEnabled()) popUndoSnapshot();
    });
    // [iter 25] Tab order: Value → Scan Type → Value Type → Fast Scan
    // → First Scan → Next Scan → Undo. Walks the workflow in order.
    // [iter 55] Focus proxy: when the dock receives focus (e.g. by Ctrl+Shift+S
    // or clicking the dock title), Qt forwards into the value field rather
    // than the panel widget itself. Cuts the click to start typing.
    setFocusProxy(m_valueEdit);
    QWidget::setTabOrder(m_valueEdit, m_value2Edit);  // Between's upper bound
    QWidget::setTabOrder(m_value2Edit, m_condCombo);
    QWidget::setTabOrder(m_condCombo, m_typeCombo);
    QWidget::setTabOrder(m_typeCombo, m_fastScanCombo);
    QWidget::setTabOrder(m_fastScanCombo, m_scanBtn);
    QWidget::setTabOrder(m_scanBtn, m_updateBtn);
    QWidget::setTabOrder(m_updateBtn, m_undoBtn);
    QWidget::setTabOrder(m_undoBtn, m_newScanBtn);
    QWidget::setTabOrder(m_newScanBtn, m_resultTable);
    QWidget::setTabOrder(m_resultTable, m_resultFilter);
    connect(m_resultTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        bool hasSel = !m_resultTable->selectedItems().isEmpty();
        // [iter 32] Hide Go to / Copy entirely when no selection — empty
        // disabled buttons looked like dead UI; hidden makes the row
        // visually empty until there's something to act on.
        m_gotoBtn->setVisible(hasSel);
        m_copyBtn->setVisible(hasSel);
    });
    // [iter 33] Initial state: no selection ⇒ buttons hidden.
    m_gotoBtn->setVisible(false);
    m_copyBtn->setVisible(false);

    connect(m_resultTable, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        int row = m_resultTable->rowAt(pos.y());
        int idxResolved = rowToResultIdx(m_resultTable, row);
        if (idxResolved < 0 || idxResolved >= m_results.size()) return;
        const int row_actual_index = idxResolved;
        QMenu menu;
        auto* copyAddr = menu.addAction(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                                        QStringLiteral("Copy Address"));
        auto* copyVal = menu.addAction(QIcon(QStringLiteral(":/vsicons/clippy.svg")),
                                       QStringLiteral("Copy Value"));
        auto* goTo = menu.addAction(QIcon(QStringLiteral(":/vsicons/arrow-right.svg")),
                                    QStringLiteral("Go to Address"));
        menu.addSeparator();
        auto* changeAll = menu.addAction(QStringLiteral("Change All Values (%1)").arg(m_results.size()));
        auto* chosen = menu.exec(m_resultTable->viewport()->mapToGlobal(pos));
        if (chosen == copyAddr) {
            QString addr = QStringLiteral("0x%1")
                .arg(m_results[row_actual_index].address, 0, 16, QLatin1Char('0')).toUpper();
            QApplication::clipboard()->setText(addr);
            m_statusLabel->setText(QStringLiteral("Copied: %1").arg(addr));
        } else if (chosen == copyVal) {
            QApplication::clipboard()->setText(formatValue(m_results[row_actual_index].scanValue));
            m_statusLabel->setText(QStringLiteral("Copied value"));
        } else if (chosen == goTo) {
            emit goToAddress(m_results[row_actual_index].address);
        } else if (chosen == changeAll) {
            QString hint = m_lastScanMode == 0
                ? QStringLiteral("hex bytes (e.g. 90 90 90)")
                : QStringLiteral("value (e.g. 999)");
            bool ok;
            QString text = QInputDialog::getText(this, QStringLiteral("Change All Values"),
                QStringLiteral("New %1:").arg(hint), QLineEdit::Normal, QString(), &ok);
            if (!ok || text.isEmpty()) return;

            std::shared_ptr<Provider> prov;
            if (m_providerGetter) prov = m_providerGetter();
            if (!prov || !prov->isWritable()) {
                m_statusLabel->setText(QStringLiteral("Provider is read-only"));
                return;
            }

            // Parse value using same logic as single-cell edit
            QByteArray bytes;
            if (m_lastScanMode == 0) {
                QStringList tokens = text.split(' ', Qt::SkipEmptyParts);
                for (const QString& tok : tokens) {
                    bool tokOk;
                    uint val = tok.toUInt(&tokOk, 16);
                    if (!tokOk || val > 0xFF) {
                        m_statusLabel->setText(QStringLiteral("Invalid hex byte: %1").arg(tok));
                        return;
                    }
                    bytes.append(char(val));
                }
            } else {
                bool valOk = false;
                bytes.resize(valueSize());
                char* d = bytes.data();
                switch (m_lastValueType) {
                case ValueType::Int8:   { auto v = (int8_t)text.toInt(&valOk);     if (valOk) memcpy(d, &v, 1); break; }
                case ValueType::UInt8:  { auto v = (uint8_t)text.toUInt(&valOk);   if (valOk) memcpy(d, &v, 1); break; }
                case ValueType::Int16:  { auto v = (int16_t)text.toShort(&valOk);  if (valOk) memcpy(d, &v, 2); break; }
                case ValueType::UInt16: { auto v = text.toUShort(&valOk);          if (valOk) memcpy(d, &v, 2); break; }
                case ValueType::Int32:  { auto v = text.toInt(&valOk);             if (valOk) memcpy(d, &v, 4); break; }
                case ValueType::UInt32: { auto v = text.toUInt(&valOk);            if (valOk) memcpy(d, &v, 4); break; }
                case ValueType::Int64:  { auto v = text.toLongLong(&valOk);        if (valOk) memcpy(d, &v, 8); break; }
                case ValueType::UInt64: { auto v = text.toULongLong(&valOk);       if (valOk) memcpy(d, &v, 8); break; }
                case ValueType::Float:  { auto v = text.toFloat(&valOk);           if (valOk) memcpy(d, &v, 4); break; }
                case ValueType::Double: { auto v = text.toDouble(&valOk);          if (valOk) memcpy(d, &v, 8); break; }
                default: break;
                }
                if (!valOk) {
                    m_statusLabel->setText(QStringLiteral("Invalid value"));
                    return;
                }
            }
            if (bytes.isEmpty()) return;

            int wrote = 0;
            int readSize = (m_lastScanMode == 1) ? valueSize() : 16;
            for (auto& r : m_results) {
                if (prov->writeBytes(r.address, bytes)) {
                    r.scanValue = prov->readBytes(r.address, readSize);
                    ++wrote;
                }
            }
            populateTable(m_resultTable->columnCount() > 2);
            m_statusLabel->setText(QStringLiteral("Wrote to %1/%2 addresses")
                .arg(wrote).arg(m_results.size()));
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
        // [iter 56] Prefix engine errors with a unicode warning glyph so
        // the eye picks them out of the regular status feed without us
        // needing a second status widget or a color-by-message-type rule.
        // Keep the literal word "Error" too — existing tests assert on it.
        m_statusLabel->setText(QStringLiteral("⚠  Error: %1").arg(msg));
        m_scanBtn->setText(QStringLiteral("&First Scan"));
        m_scanBtn->setProperty("scanCancel", false);
        if (auto* sb = dynamic_cast<ScanButton*>(m_scanBtn)) sb->refreshChrome();
        // [iter 45] Re-enable the OTHER action button after error so the
        // user isn't left in the disabled-Update state when a typo in the
        // value field aborts before the engine starts.
        m_updateBtn->setEnabled(!m_results.isEmpty());
        m_progressBar->hide();
    });
    // Surface region count + scanned MB to the user as soon as the engine
    // has resolved the scan scope. Big perceived-performance win — the user
    // sees "247 MB across 89 regions" instead of an opaque progress bar.
    connect(m_engine, &ScanEngine::regionsResolved, this,
            [this](int count, qulonglong totalBytes) {
        if (count <= 0) {
            m_statusLabel->setText(QStringLiteral("No regions match filters"));
            return;
        }
        double mb = (double)totalBytes / (1024.0 * 1024.0);
        m_statusLabel->setText(QStringLiteral("Scanning %1 region%2 (%3 MB)…")
            .arg(count).arg(count == 1 ? "" : "s")
            .arg(mb, 0, 'f', mb < 10 ? 2 : 1));
    });
    connect(m_engine, &ScanEngine::scanStats, this,
            [this](rcx::ScanStats s) {
        if (s.bytesFailed > 0) {
            // Suffix the next status update with the unreadable byte count
            // so the user knows page-protection / unmap denied N KB.
            QString cur = m_statusLabel->text();
            cur += QStringLiteral("  (%1 KB unreadable)")
                .arg(s.bytesFailed / 1024);
            m_statusLabel->setText(cur);
        }
    });
}

void ScannerPanel::setProviderGetter(ProviderGetter getter) {
    m_providerGetter = std::move(getter);
    // [iter 49] When no provider is wired up the panel is decorative — say
    // so explicitly so the user knows to attach to a process or open a
    // memory file from the start page first.
    if (auto* empty = dynamic_cast<EmptyResultsTable*>(m_resultTable)) {
        if (!m_providerGetter) {
            empty->placeholder = QStringLiteral(
                "No memory source attached.\n"
                "Open a process or memory file from the start page to begin scanning.");
            empty->hint.clear();
        } else {
            empty->placeholder = QStringLiteral(
                "Set scan criteria above and click First Scan to begin.");
            empty->hint = QStringLiteral(
                "Tip — press Enter inside the value field, or Ctrl+Return anywhere.");
        }
        empty->viewport()->update();
    }
}

void ScannerPanel::setBoundsGetter(BoundsGetter getter) {
    m_boundsGetter = std::move(getter);
    // [iter 62] Disable the "Current struct" chip when no boundsGetter is
    // attached — checking it would silently fall back to a full scan, which
    // is confusing if the user assumes the constraint took effect.
    if (m_structOnlyCheck) {
        m_structOnlyCheck->setEnabled((bool)m_boundsGetter);
        if (!m_boundsGetter) m_structOnlyCheck->setChecked(false);
    }
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
    // Fast Scan combo gets the SAME editor font as the chip widgets next to
    // it (m_execCheck/m_writeCheck below also setFont(font)). Reading
    // m_execCheck->font() before line ~1103 returns the stale application
    // font; since every sibling in the row uses `font` directly, do the
    // same here so they always match regardless of evaluation order.
    m_fastScanCombo->setFont(font);
    if (m_fastScanCombo->view()) m_fastScanCombo->view()->setFont(font);
    if (m_fastScanLabel) m_fastScanLabel->setFont(font);
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
    m_privateOnlyCheck->setFont(font);
    m_skipSystemCheck->setFont(font);
    m_userModeOnlyCheck->setFont(font);
    m_updateBtn->setFont(font);
    m_newScanBtn->setFont(font);
    m_undoBtn->setFont(font);
    m_value2Edit->setFont(font);
    m_value2Label->setFont(font);
    m_resultFilter->setFont(font);
    m_truncBanner->setFont(font);
    m_stageLabel->setFont(font);
    // Header row uses its own QFont — propagate so column titles match the
    // monospaced editor font instead of the system Sans default.
    m_resultTable->horizontalHeader()->setFont(font);
    // Re-render the breadcrumb so its embedded font-family CSS picks up
    // the new font (QLabel's RichText path ignores QFont — see
    // updateStageLabel for the fontCss embedding).
    updateStageLabel();
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
    // Mode combo is hidden now; "Exact Sig" inside the cond combo flips
    // the mode programmatically. This handler keeps the filter defaults
    // sensible per mode and defers field-visibility to onConditionChanged.
    bool isSig = (index == 0);

    // Smart filter defaults per mode.
    m_execCheck->setChecked(isSig);
    m_writeCheck->setChecked(!isSig);
    m_privateOnlyCheck->setChecked(false);
    m_skipSystemCheck->setChecked(false);
    m_userModeOnlyCheck->setChecked(false);

    onConditionChanged(0);
}

void ScannerPanel::onConditionChanged(int /*index*/) {
    int rawData = m_condCombo->currentData().toInt();

    // -1 sentinel = "Exact Sig" — flip the (hidden) mode combo to
    // Signature so the rest of the buildRequest path stays unchanged.
    bool isSig = (rawData == -1);
    if (m_modeCombo->currentIndex() != (isSig ? 0 : 1))
        m_modeCombo->setCurrentIndex(isSig ? 0 : 1);

    auto cond = isSig ? ScanCondition::ExactValue
                      : (ScanCondition)rawData;

    // Conditions that need an input value (or pair of bounds for Between).
    bool needsValue = isSig
                   || cond == ScanCondition::ExactValue
                   || cond == ScanCondition::BiggerThan
                   || cond == ScanCondition::SmallerThan
                   || cond == ScanCondition::Between
                   || cond == ScanCondition::IncreasedBy
                   || cond == ScanCondition::DecreasedBy;
    bool needsRange = (cond == ScanCondition::Between) && !isSig;

    // Toggle which input field is shown — pattern field for sig, value
    // field for everything else. Both share the value row so only one
    // is ever visible.
    m_patternEdit->setVisible(isSig);
    m_valueEdit->setVisible(!isSig);
    m_valueLabel->setText(isSig ? QStringLiteral("Pattern:") : QStringLiteral("Value:"));
    m_valueEdit->setEnabled(needsValue);
    m_valueLabel->setEnabled(needsValue);
    m_value2Edit->setVisible(needsRange);
    m_value2Label->setVisible(needsRange);

    // The value-type combo only applies to value scans.
    m_typeLabel->setEnabled(!isSig);
    m_typeCombo->setEnabled(!isSig);

    // [iter 38] Auto-focus the now-visible input field. Picking a condition
    // is almost always followed by typing a value — saving the click into
    // the textbox makes the loop noticeably tighter for keyboard users.
    if (needsValue) {
        QLineEdit* target = isSig ? m_patternEdit : m_valueEdit;
        if (target->isVisible() && target->isEnabled()) {
            // Defer focus to the next event-loop tick so the visibility
            // change has actually settled in the layout — focusing a widget
            // that's still hidden in the current tick is a no-op.
            QMetaObject::invokeMethod(target, [target]() {
                target->setFocus(Qt::OtherFocusReason);
                target->selectAll();
            }, Qt::QueuedConnection);
        }
    }
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

    // First-scan path always resets the workflow stage to 1.
    m_scanGeneration = 1;
    m_lastResultCount = 0;
    updateStageLabel(QStringLiteral("scanning"));

    // Cancel lives on the same button the user pressed. Next Scan is
    // disabled while First Scan is in flight (and vice versa) so the
    // workflow can't get into an "I clicked First Scan but it cancelled
    // my re-scan" stuck state.
    m_scanBtn->setText(QStringLiteral("Cancel"));
    // [iter 44] Mark the button so style polish can paint Cancel in a
    // distinct (warmer) color than First Scan.
    m_scanBtn->setProperty("scanCancel", true);
    if (auto* sb = dynamic_cast<ScanButton*>(m_scanBtn)) sb->refreshChrome();
    m_updateBtn->setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->show();

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

        // Compare-against-previous conditions need a baseline. On first scan
        // we capture every aligned address; the user's actual comparison
        // semantics get applied on the next Re-scan.
        if (cond == ScanCondition::Changed || cond == ScanCondition::Unchanged ||
            cond == ScanCondition::Increased || cond == ScanCondition::Decreased ||
            cond == ScanCondition::IncreasedBy || cond == ScanCondition::DecreasedBy) {
            cond = ScanCondition::UnknownValue;
        }

        req.condition = cond;
        // Fast Scan dropdown sets the stride. Floor at the type's natural
        // alignment so a uint64 can't be scanned at 4-byte stride (would
        // skip the high half of every other value).
        int natAlign  = naturalAlignment(vt);
        int chosen    = qMax(1, m_fastScanCombo->currentData().toInt());
        req.alignment = qMax(chosen, natAlign);
        req.valueSize = valueSizeForType(vt);
        req.valueType = vt;

        if (cond == ScanCondition::UnknownValue) {
            // No pattern needed — capture all aligned addresses
            req.maxResults = 10000000;
        } else if (cond == ScanCondition::BiggerThan
                || cond == ScanCondition::SmallerThan
                || cond == ScanCondition::Between) {
            // Inline typed compare against constant(s). pattern = lower bound,
            // pattern2 = upper bound (Between only).
            QByteArray dummyMask;
            if (!serializeValue(vt, m_valueEdit->text(), req.pattern, dummyMask, &err)) {
                m_statusLabel->setText(QStringLiteral("Value error: %1").arg(err));
                return {};
            }
            if (cond == ScanCondition::Between) {
                if (!serializeValue(vt, m_value2Edit->text(), req.pattern2, dummyMask, &err)) {
                    m_statusLabel->setText(QStringLiteral("Upper bound error: %1").arg(err));
                    return {};
                }
            }
            req.mask.fill('\xFF', req.pattern.size());
        } else {
            // Exact value mode
            if (!serializeValue(vt, m_valueEdit->text(), req.pattern, req.mask, &err)) {
                m_statusLabel->setText(QStringLiteral("Value error: %1").arg(err));
                return {};
            }
        }
    }

    req.filterExecutable   = m_execCheck->isChecked();
    req.filterWritable     = m_writeCheck->isChecked();
    req.privateOnly        = m_privateOnlyCheck->isChecked();
    req.skipSystemModules  = m_skipSystemCheck->isChecked();

    // User-mode VA cap. Pointer size is taken from the live provider so a
    // 32-bit target gets a 32-bit cap automatically.
    if (m_userModeOnlyCheck->isChecked()) {
        std::shared_ptr<Provider> prov;
        if (m_providerGetter) prov = m_providerGetter();
        int psz = prov ? prov->pointerSize() : 8;
        uint64_t cap = (psz >= 8) ? 0x00007FFFFFFFFFFFULL : 0x7FFFFFFFULL;
        if (req.endAddress == 0 || req.endAddress > cap)
            req.endAddress = cap;
    }

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
    m_scanBtn->setText(QStringLiteral("&First Scan"));
    // [iter 44] Drop the cancel-style mark so the button paints as a
    // primary action again.
    m_scanBtn->setProperty("scanCancel", false);
    if (auto* sb = dynamic_cast<ScanButton*>(m_scanBtn)) sb->refreshChrome();
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
    m_newScanBtn->setVisible(!m_results.isEmpty());
    {
        QElapsedTimer pt;
        pt.start();
        populateTable(false);
        qDebug() << "[panel] populateTable(initial):" << m_results.size()
                 << "results," << pt.elapsed() << "ms";
    }
    updateModuleColumnVisibility();

    int n = m_results.size();
    // [iter 30] Format result counts with thousands separators using the
    // current locale — "47,832" reads instantly versus "47832".
    QLocale loc;
    QString nFmt = loc.toString(n);
    if (n == 0) {
        m_statusLabel->setText(QStringLiteral(
            "0 results — try widening the filters above or checking the value/pattern."));
    } else {
        m_statusLabel->setText(QStringLiteral("%1 result%2 found")
            .arg(nFmt).arg(n == 1 ? "" : "s"));
    }
    updateStageLabel();

    // Truncation banner — populateTable caps display at 10K rows; tell the
    // user explicitly so they don't think a large scan completed empty.
    constexpr int kMaxRows = 10000;
    if (n > kMaxRows) {
        m_truncBanner->setText(QStringLiteral(
            "Displaying %1 of %2 — narrow results with Next Scan or filter.")
            .arg(loc.toString(kMaxRows)).arg(nFmt));
        m_truncBanner->setVisible(true);
    } else {
        m_truncBanner->setVisible(false);
    }

    // Apply any active result-filter text to the freshly populated rows.
    if (!m_resultFilter->text().isEmpty()) applyResultFilter();
}

void ScannerPanel::populateTable(bool showPrevious) {
    constexpr int kMaxRows = 10000;

    // [iter 42] Filter widget toggles enabled state with the result list.
    m_resultFilter->setEnabled(!m_results.isEmpty());

    m_resultTable->blockSignals(true);
    // Sorting must be off while we mutate cells, otherwise Qt re-sorts mid-fill
    // and our row-index → result-index mapping breaks.
    m_resultTable->setSortingEnabled(false);

    // Module column appears only when at least one result has a module name.
    bool anyModule = false;
    for (const auto& r : m_results) {
        if (!r.regionModule.isEmpty()) { anyModule = true; break; }
    }
    int baseCols = showPrevious ? 3 : 2;
    int cols = baseCols + (anyModule ? 1 : 0);
    m_resultTable->setColumnCount(cols);
    int displayCount = qMin(m_results.size(), kMaxRows);
    m_resultTable->setRowCount(displayCount);

    // Always re-show the headers when the column set changes — they were
    // hidden by default but become useful for sorting once the user has
    // multiple result kinds in play.
    m_resultTable->horizontalHeader()->setVisible(anyModule || showPrevious);
    // [iter 51] Header items get tooltips advertising the click-to-sort
    // affordance — sortable headers are easy to miss when their visual
    // chrome is identical to read-only label rows.
    auto makeHeader = [](const QString& title, const QString& tip) {
        auto* h = new QTableWidgetItem(title);
        h->setToolTip(tip);
        return h;
    };
    m_resultTable->setHorizontalHeaderItem(0, makeHeader(
        QStringLiteral("Address"),
        QStringLiteral("Click to sort by address. Shift+click for reverse order.")));
    m_resultTable->setHorizontalHeaderItem(1, makeHeader(
        QStringLiteral("Value"),
        QStringLiteral("Click to sort by current value. Double-click a cell to edit it.")));
    if (showPrevious)
        m_resultTable->setHorizontalHeaderItem(2, makeHeader(
            QStringLiteral("Previous"),
            QStringLiteral("Value at the previous scan, before this Next-Scan narrowing.")));
    if (anyModule)
        m_resultTable->setHorizontalHeaderItem(baseCols, makeHeader(
            QStringLiteral("Module"),
            QStringLiteral("Module (DLL/exe) the address falls inside, when known.")));

    for (int i = 0; i < displayCount; i++) {
        const auto& r = m_results[i];

        // Address column — WinDbg backtick format: 00000000`00000000.
        // Stash result index in UserRole so post-sort row→result lookup
        // doesn't depend on row order matching m_results order.
        QString hexPart = QStringLiteral("%1").arg(r.address, 16, 16, QLatin1Char('0')).toUpper();
        hexPart.insert(8, '`');
        auto* addrItem = new QTableWidgetItem(hexPart);
        addrItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
        addrItem->setData(Qt::UserRole, i);
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

        // Module column
        if (anyModule) {
            auto* modItem = new QTableWidgetItem(r.regionModule);
            modItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            m_resultTable->setItem(i, baseCols, modItem);
        }
    }

    // Re-enable click-to-sort on the headers now that fill is done.
    m_resultTable->setSortingEnabled(true);

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

    m_preRescanCount  = m_results.size();
    m_lastResultCount = m_preRescanCount;
    m_scanGeneration  = qMax(2, m_scanGeneration + 1);  // Step 2+
    // Snapshot the pre-rescan list so Undo Scan can roll back if the new
    // condition over-narrows. CE-style "oh I clicked Decreased by mistake"
    // recovery — saves the user from having to redo the whole scan chain.
    pushUndoSnapshot();
    updateStageLabel(QStringLiteral("scanning"));
    // Cancel state lives on the button the user actually pressed. The old
    // code repurposed m_scanBtn ("First Scan" → "Cancel") which left the
    // user stuck — clicking First Scan to restart instead cancelled the
    // re-scan, with no obvious way out. Now Next Scan owns its own cancel
    // and First Scan stays disabled (so it can't fire while a re-scan
    // is in flight).
    m_scanBtn->setEnabled(false);
    m_updateBtn->setText(QStringLiteral("Cancel"));
    // [iter 46] Symmetrical cancel-style mark on Next Scan during re-scan
    // so it visually matches the First Scan cancel state.
    m_updateBtn->setProperty("scanCancel", true);
    if (auto* sb = dynamic_cast<ScanButton*>(m_updateBtn)) sb->refreshChrome();
    m_progressBar->setValue(0);
    m_progressBar->show();

    m_engine->startRescan(prov, m_results, readSize, cond, m_lastValueType,
                          filterPattern, filterMask);
}

void ScannerPanel::onRescanFinished(QVector<ScanResult> results) {
    m_scanBtn->setEnabled(true);
    m_scanBtn->setText(QStringLiteral("&First Scan"));
    m_updateBtn->setText(QStringLiteral("&Next Scan"));
    m_updateBtn->setProperty("scanCancel", false);
    m_updateBtn->style()->unpolish(m_updateBtn);
    m_updateBtn->style()->polish(m_updateBtn);
    m_progressBar->hide();
    m_results = std::move(results);
    m_updateBtn->setEnabled(!m_results.isEmpty());
    m_newScanBtn->setVisible(!m_results.isEmpty());

    {
        QElapsedTimer pt;
        pt.start();
        populateTable(true);
        qDebug() << "[panel] populateTable(rescan):" << m_results.size()
                 << "results," << pt.elapsed() << "ms";
    }

    int n = m_results.size();
    int before = m_lastResultCount;
    // [iter 31] Locale formatter for re-scan status counts too.
    QLocale loc;
    if (n == 0) {
        m_statusLabel->setText(QStringLiteral(
            "0 results — the condition eliminated everything. "
            "Click Reset to start over, or relax the condition and try again."));
    } else if (before > 0 && n < before) {
        m_statusLabel->setText(QStringLiteral("Narrowed %1 → %2  (eliminated %3)")
            .arg(loc.toString(before))
            .arg(loc.toString(n))
            .arg(loc.toString(before - n)));
    } else if (before > 0 && n == before) {
        m_statusLabel->setText(QStringLiteral("All %1 results still match")
            .arg(loc.toString(n)));
    } else {
        m_statusLabel->setText(QStringLiteral("%1 result%2")
            .arg(loc.toString(n)).arg(n == 1 ? "" : "s"));
    }
    updateStageLabel();
}

// Helper: row index → m_results index via the UserRole stash on column 0.
// Without this, sorting silently desyncs the result list from the table view.
static int rowToResultIdx(QTableWidget* tbl, int row) {
    if (row < 0) return -1;
    auto* it = tbl->item(row, 0);
    if (!it) return -1;
    QVariant v = it->data(Qt::UserRole);
    return v.isValid() ? v.toInt() : row;
}

void ScannerPanel::onGoToAddress() {
    int row = m_resultTable->currentRow();
    int idx = rowToResultIdx(m_resultTable, row);
    if (idx < 0 || idx >= m_results.size()) return;
    emit goToAddress(m_results[idx].address);
}

void ScannerPanel::onCopyAddress() {
    int row = m_resultTable->currentRow();
    int idx = rowToResultIdx(m_resultTable, row);
    if (idx < 0 || idx >= m_results.size()) return;

    QString addr = QStringLiteral("0x%1")
        .arg(m_results[idx].address, 0, 16, QLatin1Char('0')).toUpper();
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
    int idxResolved = rowToResultIdx(m_resultTable, row);
    if (idxResolved < 0 || idxResolved >= m_results.size()) return;
    const int row_idx = idxResolved;

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
            m_results[row_idx].address = result.value;
            emit goToAddress(result.value);
            // Reformat the address cell
            m_resultTable->blockSignals(true);
            QString hexPart = QStringLiteral("%1").arg(result.value, 16, 16, QLatin1Char('0')).toUpper();
            hexPart.insert(8, '`');
            item->setText(hexPart);
            // Re-read preview at new address and update cache
            if (prov) {
                int readSize = (m_lastScanMode == 1) ? valueSize() : 16;
                m_results[row_idx].scanValue = prov->readBytes(result.value, readSize);
                if (auto* prevItem = m_resultTable->item(row, 1))
                    prevItem->setText(formatValue(m_results[row_idx].scanValue));
            }
            m_resultTable->blockSignals(false);
        } else {
            m_statusLabel->setText(QStringLiteral("Expression error: %1").arg(result.error));
            // Restore original address
            m_resultTable->blockSignals(true);
            QString hexPart = QStringLiteral("%1").arg(m_results[row_idx].address, 16, 16, QLatin1Char('0')).toUpper();
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
        uint64_t addr = m_results[row_idx].address;

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
            m_results[row_idx].scanValue = prov->readBytes(addr, readSize);
            item->setText(formatValue(m_results[row_idx].scanValue));
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

    // Results table — editor-matching style + alternating-row banding so
    // adjacent addresses are easier to scan visually. Also style the header
    // explicitly so the column titles match the editor's monospaced font
    // and dim color.
    // [iter 15] Selection uses theme.selection (richer than hover) so a
    // selected row stands out from a hovered one — multi-select reads
    // unambiguously now.
    // [iter 16] Selected items get bright text color, hovered items keep
    // default text color — visual hierarchy: selected > hovered > idle.
    // [iter 17] Top border on the table — separates it from the result
    // filter input above so the two read as distinct widgets.
    m_resultTable->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; color: %2;"
        "  border: none; border-top: 1px solid %8;"
        "  alternate-background-color: %6; gridline-color: transparent; }"
        "QTableWidget::item { padding: 2px 6px; border: none; }"
        "QTableWidget::item:hover { background: %3; padding: 2px 6px; border: none; }"
        "QTableWidget::item:selected { background: %5; color: %2; padding: 2px 6px; border: none; }"
        "QTableWidget QLineEdit { background: %1; color: %2; border: 1px solid %4;"
        "  padding: 1px 4px; selection-background-color: %5; }"
        "QHeaderView::section { background: %1; color: %7; border: none;"
        "  border-bottom: 1px solid %8; padding: 4px 6px; }"
        "QHeaderView::section:hover { color: %2; }")
        .arg(theme.background.name(), theme.text.name(), theme.hover.name(),
             theme.borderFocused.name(), theme.selection.name(),
             theme.backgroundAlt.name(), theme.textMuted.name(),
             theme.border.name()));

    // Input fields
    QString lineEditStyle = QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 2px 4px; }"
        "QLineEdit:focus { border-color: %4; }")
        .arg(theme.background.name(), theme.text.name(),
             theme.border.name(), theme.borderFocused.name());
    m_patternEdit->setStyleSheet(lineEditStyle);
    m_valueEdit->setStyleSheet(lineEditStyle);

    // Combo boxes
    //
    // Lessons from TypeSelectorPopup (the type chooser): Fusion's default
    // frame around a popup view is invisible-on-dark or a fat 2px sunken
    // bevel — neither reads as a clean border. The fix is to give the
    // dropdown view a flat 1px border in theme.border AND pad the items
    // so they don't sit flush against the border (which makes the border
    // look glued to the first row's text). Also outline:none kills Qt's
    // dotted focus rectangle inside the popup.
    QString comboStyle = QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3; padding: 2px 4px 2px 4px; }"
        "QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;"
        "  width: 16px; border-left: 1px solid %3; }"
        "QComboBox::down-arrow { image: url(:/vsicons/chevron-down.svg); width: 10px; height: 10px; }"
        "QComboBox QAbstractItemView {"
        "  background: %1; color: %2;"
        "  selection-background-color: %4;"
        "  border: 1px solid %3;"
        "  padding: 2px 0;"
        "  outline: none;"
        "}"
        "QComboBox QAbstractItemView::item { padding: 4px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background: %4; }")
        .arg(theme.background.name(), theme.text.name(),
             theme.border.name(), theme.hover.name());
    m_modeCombo->setStyleSheet(comboStyle);
    m_typeCombo->setStyleSheet(comboStyle);
    m_fastScanCombo->setStyleSheet(comboStyle);
    m_condCombo->setStyleSheet(comboStyle);

    // Defensive: the QFrame container Qt wraps the popup view in adds its
    // own frame shape/line that fights the QSS border. Force NoFrame on
    // each combo's popup container so only the QSS border shows. Same
    // trick TypeSelectorPopup uses (`setFrameShape(QFrame::NoFrame)`).
    for (QComboBox* c : { m_modeCombo, m_typeCombo, m_fastScanCombo, m_condCombo }) {
        if (auto* view = c->view()) {
            if (auto* container = qobject_cast<QFrame*>(view->parentWidget())) {
                container->setFrameShape(QFrame::NoFrame);
                container->setLineWidth(0);
            }
            if (auto* viewFrame = qobject_cast<QFrame*>(view)) {
                viewFrame->setFrameShape(QFrame::NoFrame);
                viewFrame->setLineWidth(0);
            }
        }
    }

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

    // Buttons — ScanButton owns its own theme-aware chrome. Re-apply by
    // round-tripping the variant so each picks up the new theme colors.
    // ScanButton isn't Q_OBJECT (no signals/slots beyond QPushButton's), so
    // we rely on a property tag instead of qobject_cast for the variant.
    auto refreshBtn = [](QPushButton* b) {
        // dynamic_cast works fine on RTTI-enabled builds; ScanButton has a
        // virtual dtor via QPushButton so RTTI is present.
        if (auto* sb = dynamic_cast<ScanButton*>(b)) {
            sb->setVariant(sb->property("variant_primary").toBool()
                ? ScanButton::Primary
                : (sb->property("variant_subtle").toBool()
                   ? ScanButton::Subtle : ScanButton::Secondary));
        }
    };
    m_scanBtn->setProperty("variant_primary", true);
    m_updateBtn->setProperty("variant_primary", true);
    m_newScanBtn->setProperty("variant_subtle", true);
    refreshBtn(m_scanBtn);
    refreshBtn(m_updateBtn);
    refreshBtn(m_newScanBtn);
    refreshBtn(m_undoBtn);
    refreshBtn(m_gotoBtn);
    refreshBtn(m_copyBtn);

    // Progress bar
    m_progressBar->setStyleSheet(QStringLiteral(
        "QProgressBar { background:%1; border:none; }"
        "QProgressBar::chunk { background:%2; }")
        .arg(theme.background.name(), theme.indHoverSpan.name()));

    // Section header pill — small all-caps label that introduces each
    // group ("WHAT TO SCAN FOR" / "WHERE TO SCAN" / "RESULTS"). Same dim
    // color as the workspace dock's "ALL TYPES" header so the panel reads
    // as part of the same visual family.
    // [iter 11] Section headers brighter — was textMuted, now textDim so
    // they're visible without screaming for attention.
    // [iter 12] 1px bottom border on section headers acts as a subtle
    // divider between groups.
    // [iter 13] Section labels (Value:/Scan Type:/Value Type:) use
    // textDim too so the label column reads as one cohesive cluster.
    // [iter 14] Result count uses default text color when populated, dim
    // padding when empty — handled in the count setter, see iter 32+.
    QString sectionStyle = QStringLiteral(
        "QLabel[scannerHeader=\"true\"] { color:%1; padding:6px 0 3px 0;"
        "   border-bottom:1px solid %2; margin-bottom:2px; }"
        "QLabel[scannerSection=\"true\"] { color:%1; padding:0 4px 0 0; }"
        "QLabel[scannerStatus=\"true\"] { color:%3; padding:2px 4px; }")
        .arg(theme.textDim.name(), theme.border.name(), theme.text.name());
    setStyleSheet((styleSheet().isEmpty() ? QString() : styleSheet() + " ")
                   + sectionStyle);

    // Re-render the breadcrumb so it picks up the new theme colors.
    if (m_stageLabel) updateStageLabel();

    // Truncation banner: warm-tinted background so it reads as a notice,
    // not an inert label.
    if (m_truncBanner) {
        m_truncBanner->setStyleSheet(QStringLiteral(
            "QLabel { background:%1; color:%2; border-left: 2px solid %3;"
            " border-radius: 2px; }")
            .arg(theme.backgroundAlt.name(),
                 theme.text.name(),
                 theme.indHeatWarm.isValid()
                   ? theme.indHeatWarm.name() : theme.indHoverSpan.name()));
    }
    // Result-filter input: hairline border, no native chrome.
    if (m_resultFilter) {
        m_resultFilter->setStyleSheet(QStringLiteral(
            "QLineEdit { background:%1; color:%2; border:1px solid %3;"
            " border-radius:2px; padding:3px 6px; }"
            "QLineEdit:focus { border-color:%4; }")
            .arg(theme.background.name(), theme.text.name(),
                 theme.border.name(), theme.borderFocused.name()));
    }
    // Progress bar — accent chunk on dim background, no border.
    if (m_progressBar) {
        m_progressBar->setStyleSheet(QStringLiteral(
            "QProgressBar { background:%1; border:none; }"
            "QProgressBar::chunk { background:%2; }")
            .arg(theme.background.name(), theme.indHoverSpan.name()));
    }
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
        // Signature mode — show only the bytes the user actually searched for.
        // The engine caches up to 16 bytes per hit (for context) but the value
        // column should reflect the match length, not the chunk length. Falls
        // back to the full cached chunk if no pattern was recorded yet.
        int showLen = m_lastPattern.isEmpty()
                      ? bytes.size()
                      : qMin(bytes.size(), (int)m_lastPattern.size());
        QString s;
        for (int j = 0; j < showLen; j++) {
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

// ── New-scan / result-filter / module-column / save-load / persistence ──

void ScannerPanel::onNewScanClicked() {
    // [iter 41] Avoid the implicit "you just nuked thousands of results"
    // surprise: when the active result list is large, the first Reset
    // click only re-labels the button and the second commits. Anything
    // under 1000 results resets immediately — a CE-like pace for typical
    // narrowing chains. The cooldown auto-resets after 4s.
    constexpr int kConfirmThreshold = 1000;
    if (m_results.size() >= kConfirmThreshold
        && m_newScanBtn->property("resetArmed").toBool() == false) {
        m_newScanBtn->setProperty("resetArmed", true);
        const QString prev = m_newScanBtn->text();
        m_newScanBtn->setText(QStringLiteral("Click again to reset"));
        m_statusLabel->setText(QStringLiteral(
            "About to discard %1 results — click Reset again to confirm.")
            .arg(QLocale().toString(m_results.size())));
        QTimer::singleShot(4000, this, [this, prev]() {
            if (m_newScanBtn->property("resetArmed").toBool()) {
                m_newScanBtn->setProperty("resetArmed", false);
                m_newScanBtn->setText(prev);
                // Restore the prior result-count text — it was overwritten
                // by the confirm prompt above.
                updateScanStatusLine();
            }
        });
        return;
    }
    m_newScanBtn->setProperty("resetArmed", false);
    m_newScanBtn->setText(QStringLiteral("&Reset"));

    // Drop the result list so the next First Scan truly starts from scratch.
    m_results.clear();
    m_undoStack.clear();
    m_undoBtn->setEnabled(false);
    m_resultTable->setRowCount(0);
    m_resultTable->horizontalHeader()->hide();
    m_updateBtn->setEnabled(false);
    m_newScanBtn->setVisible(false);
    m_truncBanner->setVisible(false);
    m_statusLabel->setText(QString());  // silent until the next scan
    m_resultFilter->clear();
    m_engine->invalidateRegionCache();
    m_scanGeneration  = 0;
    m_lastResultCount = 0;
    updateStageLabel();
}

// Snapshot the current m_results so Undo Scan can restore it. Cap the
// stack at 16 entries to keep memory bounded on workflows that do many
// re-scans (typical CE chains rarely exceed 5-6 narrowing passes).
void ScannerPanel::pushUndoSnapshot() {
    constexpr int kMaxUndo = 16;
    m_undoStack.append(m_results);
    if (m_undoStack.size() > kMaxUndo)
        m_undoStack.removeFirst();
    m_undoBtn->setEnabled(!m_undoStack.isEmpty());
}

// Restore the previous result list. Decrements the scan generation so the
// breadcrumb reads correctly ("Step 2 — Next Scan #1" → "Step 1 — First
// Scan: N results"). The current results are dropped, not pushed back to
// the stack — Undo is one-directional, mirroring CE.
void ScannerPanel::popUndoSnapshot() {
    if (m_undoStack.isEmpty()) return;
    m_results = m_undoStack.takeLast();
    m_lastResultCount = 0;  // breadcrumb skips the "narrowed N → M" math
    if (m_scanGeneration > 1) --m_scanGeneration;
    m_undoBtn->setEnabled(!m_undoStack.isEmpty());
    populateTable(false);
    int n = m_results.size();
    m_statusLabel->setText(QStringLiteral("Restored — %1 result%2")
        .arg(n).arg(n == 1 ? "" : "s"));
    updateStageLabel();
}

// Updates the breadcrumb label that lives next to the action buttons.
// Compact single-line form: an accent dot + short phrase. The button labels
// themselves carry the workflow names ("First Scan", "Next Scan", "Reset")
// so the breadcrumb doesn't need to repeat them — it tells the user the
// CURRENT STATE and what just happened, not what to click. Keeps everything
// on one row so the panel doesn't grow vertically with chrome.
void ScannerPanel::updateStageLabel(const QString& phase) {
    if (!m_stageLabel) return;
    const auto& t = ThemeManager::instance().current();

    // QLabel ignores setFont for RichText — embed font in the spans.
    QFont f = m_stageLabel->font();
    QString fontCss = QStringLiteral("font-family:'%1'; font-size:%2pt;")
        .arg(f.family()).arg(qMax(f.pointSize(), 1));

    auto dot = [&](const QColor& c) {
        return QStringLiteral(
            "<span style=\"color:%1\">●</span>").arg(c.name());
    };

    QString phrase;
    QColor dotColor = t.textFaint;

    if (phase == QStringLiteral("scanning")) {
        phrase = (m_scanGeneration <= 1)
            ? QStringLiteral("Scanning…")
            : QStringLiteral("Re-scanning %1 result%2…")
                .arg(m_lastResultCount).arg(m_lastResultCount == 1 ? "" : "s");
        dotColor = t.indHoverSpan;
    } else if (m_scanGeneration == 0) {
        // Silent on idle — the empty action row + visible First Scan button
        // already tell the user what to do; no need for a babysitting phrase.
        m_stageLabel->clear();
        return;
    } else if (m_scanGeneration == 1) {
        // [iter 65] Locale-format breadcrumb counts for consistency with the
        // status line and truncation banner.
        QLocale loc;
        int n = m_results.size();
        if (n == 0) {
            phrase = QStringLiteral("0 results — try widening filters or check the value");
            dotColor = t.indHeatWarm.isValid() ? t.indHeatWarm : t.textMuted;
        } else {
            phrase = QStringLiteral(
                "First scan: <b>%1</b> result%2 — change the condition and click <b>Next Scan</b> to narrow results")
                .arg(loc.toString(n)).arg(n == 1 ? "" : "s");
            dotColor = t.indHoverSpan;
        }
    } else {
        QLocale loc;
        int n = m_results.size();
        int before = m_lastResultCount;
        if (n == 0) {
            phrase = QStringLiteral("0 results — condition eliminated everything; click Reset to start over");
            dotColor = t.indHeatWarm.isValid() ? t.indHeatWarm : t.textMuted;
        } else if (before > 0 && n < before) {
            phrase = QStringLiteral(
                "Narrowed <b>%1</b> → <b>%2</b> (eliminated %3) — keep going or Reset")
                .arg(loc.toString(before))
                .arg(loc.toString(n))
                .arg(loc.toString(before - n));
            dotColor = t.indHoverSpan;
        } else if (before > 0 && n == before) {
            phrase = QStringLiteral(
                "All <b>%1</b> results still match — try a different condition")
                .arg(loc.toString(n));
            dotColor = t.textMuted;
        } else {
            phrase = QStringLiteral("<b>%1</b> result%2")
                .arg(loc.toString(n)).arg(n == 1 ? "" : "s");
            dotColor = t.indHoverSpan;
        }
    }

    m_stageLabel->setText(QStringLiteral(
        "<span style=\"%1\">%2 <span style=\"color:%3\">%4</span></span>")
        .arg(fontCss, dot(dotColor), t.textDim.name(), phrase));
    m_stageLabel->setTextFormat(Qt::RichText);
}

void ScannerPanel::onResultFilterChanged(const QString& /*text*/) {
    applyResultFilter();
}

void ScannerPanel::applyResultFilter() {
    QString filt = m_resultFilter->text().trimmed();
    int rows = m_resultTable->rowCount();
    if (filt.isEmpty()) {
        for (int i = 0; i < rows; i++)
            m_resultTable->setRowHidden(i, false);
        // [iter 67] When the filter clears, restore the regular result-count
        // status so the user doesn't see a stale "12 of 47 match" string.
        updateScanStatusLine();
        return;
    }
    // Match against any column text — gives the user a single search box
    // that hits address / value / module without needing per-column filters.
    int matched = 0;
    for (int i = 0; i < rows; i++) {
        bool show = false;
        int cols = m_resultTable->columnCount();
        for (int c = 0; c < cols; c++) {
            auto* it = m_resultTable->item(i, c);
            if (it && it->text().contains(filt, Qt::CaseInsensitive)) {
                show = true;
                break;
            }
        }
        m_resultTable->setRowHidden(i, !show);
        if (show) ++matched;
    }
    // [iter 67] Live "N of M match" so the user knows how aggressively
    // their filter is biting before they commit it via Enter / Next Scan.
    QLocale loc;
    m_statusLabel->setText(QStringLiteral("%1 of %2 match \"%3\"")
        .arg(loc.toString(matched))
        .arg(loc.toString(rows))
        .arg(filt));
}

void ScannerPanel::updateModuleColumnVisibility() {
    bool anyModule = false;
    for (const auto& r : m_results) {
        if (!r.regionModule.isEmpty()) { anyModule = true; break; }
    }
    // The header visibility was already toggled inside populateTable; this
    // helper keeps the logic out of onScanFinished so future paths can call
    // it cheaply (e.g. result-list edits).
    Q_UNUSED(anyModule);
}

void ScannerPanel::updateScanStatusLine() {
    // Reserved for future use — the engine's regionsResolved signal already
    // handles the live "Scanning N regions" string.
}

bool ScannerPanel::saveResultsTo(const QString& path) const {
    QJsonArray arr;
    for (const auto& r : m_results) {
        QJsonObject o;
        o["address"] = QString::number(r.address, 16);
        o["value"]   = QString::fromLatin1(r.scanValue.toHex());
        if (!r.regionModule.isEmpty()) o["module"] = r.regionModule;
        arr.append(o);
    }
    QJsonObject root;
    root["version"]  = 1;
    root["scanMode"] = m_lastScanMode;
    root["valueType"] = (int)m_lastValueType;
    root["count"]    = m_results.size();
    root["results"]  = arr;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson());
    return true;
}

bool ScannerPanel::loadResultsFrom(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err{};
    auto doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError) return false;
    auto root = doc.object();
    if (root["version"].toInt() != 1) return false;

    m_lastScanMode = root["scanMode"].toInt(0);
    m_lastValueType = (ValueType)root["valueType"].toInt((int)ValueType::Int32);
    auto arr = root["results"].toArray();
    m_results.clear();
    m_results.reserve(arr.size());
    for (const auto& v : arr) {
        QJsonObject o = v.toObject();
        ScanResult r;
        r.address = o["address"].toString().toULongLong(nullptr, 16);
        r.scanValue = QByteArray::fromHex(o["value"].toString().toLatin1());
        r.regionModule = o["module"].toString();
        m_results.append(r);
    }
    populateTable(false);
    m_updateBtn->setEnabled(!m_results.isEmpty());
    m_newScanBtn->setVisible(!m_results.isEmpty());
    m_statusLabel->setText(QStringLiteral("Loaded %1 result%2 from %3")
        .arg(m_results.size()).arg(m_results.size() == 1 ? "" : "s")
        .arg(QFileInfo(path).fileName()));
    return true;
}

void ScannerPanel::saveSettings(const QString& key) const {
    QSettings s("Reclass", "Reclass");
    s.beginGroup(key);
    s.setValue("mode",         m_modeCombo->currentIndex());
    s.setValue("valueType",    m_typeCombo->currentIndex());
    s.setValue("condition",    m_condCombo->currentIndex());
    s.setValue("filterExec",   m_execCheck->isChecked());
    s.setValue("filterWrite",  m_writeCheck->isChecked());
    s.setValue("privateOnly",  m_privateOnlyCheck->isChecked());
    s.setValue("skipSystem",   m_skipSystemCheck->isChecked());
    s.setValue("userMode",     m_userModeOnlyCheck->isChecked());
    s.endGroup();
}

void ScannerPanel::loadSettings(const QString& key) {
    QSettings s("Reclass", "Reclass");
    s.beginGroup(key);
    if (s.contains("mode"))         m_modeCombo->setCurrentIndex(s.value("mode").toInt());
    if (s.contains("valueType"))    m_typeCombo->setCurrentIndex(s.value("valueType").toInt());
    if (s.contains("condition"))    m_condCombo->setCurrentIndex(s.value("condition").toInt());
    if (s.contains("filterExec"))   m_execCheck->setChecked(s.value("filterExec").toBool());
    if (s.contains("filterWrite"))  m_writeCheck->setChecked(s.value("filterWrite").toBool());
    if (s.contains("privateOnly"))  m_privateOnlyCheck->setChecked(s.value("privateOnly").toBool());
    if (s.contains("skipSystem"))   m_skipSystemCheck->setChecked(s.value("skipSystem").toBool());
    if (s.contains("userMode"))     m_userModeOnlyCheck->setChecked(s.value("userMode").toBool());
    s.endGroup();
}

} // namespace rcx
