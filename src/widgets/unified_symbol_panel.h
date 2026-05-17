#pragma once

#include "themes/thememanager.h"
#include "widgets/category_chip.h"
#include "widgets/fuzzy_match.h"
#include "names/name_registry.h"
#include "names/name_provider.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QPainter>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QFontMetrics>
#include <QSet>
#include <QSettings>
#include <QKeyEvent>
#include <QHelpEvent>
#include <QToolTip>
#include <QMimeData>
#include <QDrag>
#include <QStringList>
#include <functional>

namespace rcx {

class Provider;

// ── Layout constants (mirror src/typeselectorpopup.cpp:135-139) ──────────
// Adopting the type chooser's exact rhythm so the two panels look like the
// same family of UI. Iteration [10] of the 30-pass UI parity sweep.
static constexpr int kSymAccent = 2;
static constexpr int kSymPadL   = 4;
static constexpr int kSymPipSz  = 4;     // section pip / source pip size
static constexpr int kSymBadgeW = 14;
static constexpr int kSymKindW  = 12;
static constexpr int kSymSzCol  = 38;    // right size column for type rows

// Model: a flat list of NamedAddress rows + per-row fuzzy match positions.
// Implements mimeData() so rows can be dragged out as "source!name" text
// (iteration [25]).
class UnifiedSymbolModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit UnifiedSymbolModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    int rowCount(const QModelIndex& parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : m_rows.size();
    }
    QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override {
        if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_rows.size()) return {};
        if (role == Qt::DisplayRole) return m_rows.at(idx.row()).name;
        return {};
    }
    Qt::ItemFlags flags(const QModelIndex& idx) const override {
        Qt::ItemFlags f = QAbstractListModel::flags(idx);
        if (idx.isValid()) f |= Qt::ItemIsDragEnabled;
        return f;
    }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QStringList mimeTypes() const override {
        return { QStringLiteral("text/plain") };
    }
    // [25] Drag-out: pack selected rows as text/plain so the editor's
    // expression bar (or any other text sink) can consume them.
    QMimeData* mimeData(const QModelIndexList& indexes) const override {
        QStringList lines;
        for (const auto& mi : indexes) {
            if (!mi.isValid()) continue;
            const auto& e = m_rows.at(mi.row());
            QString qualified = e.source.isEmpty()
                ? e.name
                : (e.source + QStringLiteral("!") + e.name);
            lines.append(qualified);
        }
        auto* md = new QMimeData;
        md->setText(lines.join(QLatin1Char('\n')));
        return md;
    }

    const NamedAddress& rowAt(int row) const { return m_rows.at(row); }
    const QVector<int>& matchPositionsAt(int row) const { return m_matchPositions.at(row); }
    void setRows(QVector<NamedAddress> rows, QVector<QVector<int>> mp) {
        beginResetModel();
        m_rows = std::move(rows);
        m_matchPositions = std::move(mp);
        endResetModel();
    }

private:
    QVector<NamedAddress>  m_rows;
    QVector<QVector<int>>  m_matchPositions;
};

// Custom delegate. Anatomy of one data row (matches type chooser conventions):
//   [accent stripe | pad | kind icon | pip | name (fuzzy hl) | T badge |
//    size col (types) | address col]
class UnifiedSymbolDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using AccentFn = std::function<QColor(const QString&)>;

    UnifiedSymbolDelegate(UnifiedSymbolModel* model, AccentFn fn, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), m_model(model), m_accent(std::move(fn)) {}

    void setSearchActive(bool on) { m_searchActive = on; }
    // [9] Density modes — compact for dense lists, normal for default.
    void setCompact(bool on) {
        if (m_compact == on) return;
        m_compact = on;
        recomputeMetrics();
    }
    bool compact() const { return m_compact; }
    // [20] Mark a single absolute address as "currently navigated" — that
    // row gets a brighter left stripe + subtle background.
    void setActiveAddress(uint64_t addr) { m_activeAddr = addr; }
    // [26-27] Padding the address column to a uniform width so digits
    // line up across rows (mirrors how the type chooser's size column
    // is reserved to kSzCol pixels).
    void setAddressColWidth(int w) { m_addressColW = qMax(w, 16); }

    void setSizeMetrics(const QFont& base) {
        m_font = base;
        m_smallFont = base;
        if (m_smallFont.pixelSize() > 0)
            m_smallFont.setPixelSize(qMax(8, m_smallFont.pixelSize() - 2));
        else
            m_smallFont.setPointSize(qMax(7, m_smallFont.pointSize() - 1));
        recomputeMetrics();
    }
    int rowHeight() const { return m_rowH; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, m_rowH);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        if (!m_model || !idx.isValid()) return;
        const auto& t = ThemeManager::instance().current();
        const auto& e = m_model->rowAt(idx.row());
        QRect r = opt.rect;

        const bool selected = (opt.state & QStyle::State_Selected);
        const bool hovered  = (opt.state & QStyle::State_MouseOver);
        const bool active   = (m_activeAddr != 0 && e.address == m_activeAddr); // [20]

        // [19] Subtle hover (instead of full t.hover fill) — match type
        // chooser which uses solid t.hover; we keep solid but ensure the
        // selection state wins. Active address gets a soft tint.
        if (selected)      p->fillRect(r, t.selected);
        else if (hovered)  p->fillRect(r, t.hover);
        else if (active)   p->fillRect(r, QColor(t.hover.red(), t.hover.green(),
                                                  t.hover.blue(), 96));

        const QColor accent = m_accent ? m_accent(e.source) : t.text;

        // Left accent stripe (selected/active rows get brighter accent).
        QColor stripeCol = (selected || active) ? accent : QColor(accent.red(), accent.green(),
                                                                   accent.blue(), 180);
        p->fillRect(r.x(), r.y(), kSymAccent, r.height(), stripeCol);

        QFontMetrics fm(m_font);
        const int baseline = r.y() + (r.height() + fm.ascent() - fm.descent()) / 2;
        int x = r.x() + kSymAccent + kSymPadL;

        // [11] Per-row kind icon — small SVG by source/kind. Symbols look
        // like symbol-method, structs/classes like symbol-class, enums
        // like symbol-enum, unions get the class glyph (no dedicated
        // VS icon), bookmarks like a bookmark, RTTI as symbol-class
        // (it's effectively a class name).
        static const QHash<QString, QString> kKindIcon = {
            {QStringLiteral("symbol"),   QStringLiteral(":/vsicons/symbol-method.svg")},
            {QStringLiteral("type"),     QStringLiteral(":/vsicons/symbol-class.svg")},
            {QStringLiteral("struct"),   QStringLiteral(":/vsicons/symbol-class.svg")},
            {QStringLiteral("class"),    QStringLiteral(":/vsicons/symbol-class.svg")},
            {QStringLiteral("union"),    QStringLiteral(":/vsicons/symbol-class.svg")},
            {QStringLiteral("enum"),     QStringLiteral(":/vsicons/symbol-enum.svg")},
            {QStringLiteral("bookmark"), QStringLiteral(":/vsicons/bookmark.svg")},
            {QStringLiteral("rtti"),     QStringLiteral(":/vsicons/symbol-class.svg")},
        };
        QString iconPath = kKindIcon.value(e.kind);
        if (iconPath.isEmpty()) iconPath = QStringLiteral(":/vsicons/symbol-method.svg");
        const int iconY = r.y() + (r.height() - kSymKindW) / 2;
        QIcon(iconPath).paint(p, x, iconY, kSymKindW, kSymKindW);
        x += kSymKindW + 4;

        // Source pip (4×4, matches type chooser section pip dimensions).
        p->fillRect(x, r.y() + (r.height() - kSymPipSz) / 2,
                    kSymPipSz, kSymPipSz, accent);
        x += kSymPipSz + 5;

        // Right-side: address column (right-aligned, padded to uniform width).
        QString rvaText;
        if (e.address != 0) {
            // [27] Digit-grouped hex for readability: 0xFFFF_0040_1230.
            QString hex = QString::number(e.address, 16).toUpper();
            // pad to 8 or 16 chars (pointer size dependent — use 8 if fits)
            int pad = (e.address > 0xFFFFFFFFull) ? 16 : 8;
            while (hex.size() < pad) hex.prepend(QLatin1Char('0'));
            QString grouped;
            for (int i = 0; i < hex.size(); i++) {
                if (i > 0 && (hex.size() - i) % 4 == 0)
                    grouped.append(QLatin1Char('_'));
                grouped.append(hex.at(i));
            }
            rvaText = QStringLiteral("0x") + grouped;
        } else {
            rvaText = QStringLiteral("—");
        }
        const int rvaW = fm.horizontalAdvance(rvaText);
        const int rvaX = r.right() - qMax(rvaW, m_addressColW) - 6 + (qMax(rvaW, m_addressColW) - rvaW);

        // [14] Size column for type rows (between name and address).
        int sizeColRight = rvaX - 8;
        if ((e.kind == QLatin1String("type") || e.kind == QLatin1String("enum"))
            && e.size > 0) {
            QFontMetrics smf(m_smallFont);
            QString szText = QStringLiteral("%1B").arg(e.size);
            int szW = smf.horizontalAdvance(szText);
            p->setFont(m_smallFont);
            p->setPen(t.textFaint);
            p->drawText(sizeColRight - szW, baseline - 1, szText);
            sizeColRight = sizeColRight - kSymSzCol;
        }

        // Kind-aware import badge: shows "S" for struct/class, "U" for
        // union, "E" for enum, "T" for symbol-with-attached-type. The
        // letter is the user's cue that double-clicking the row imports
        // that flavour of definition into the active document.
        int badgeRight = sizeColRight - 4;
        if (e.typeIndex != 0) {
            QString badgeText = QStringLiteral("T");
            if (e.kind == QLatin1String("enum"))        badgeText = QStringLiteral("E");
            else if (e.kind == QLatin1String("union"))  badgeText = QStringLiteral("U");
            else if (e.kind == QLatin1String("struct")
                  || e.kind == QLatin1String("class")
                  || e.kind == QLatin1String("type"))   badgeText = QStringLiteral("S");
            const int bw = kSymBadgeW, bh = kSymBadgeW;
            int by = r.y() + (r.height() - bh) / 2;
            int bx = badgeRight - bw;
            p->setPen(QPen(accent, 1));
            p->setBrush(Qt::NoBrush);
            p->drawRect(bx, by, bw - 1, bh - 1);
            p->setPen(accent);
            QFontMetrics smf(m_smallFont);
            p->setFont(m_smallFont);
            int tBaseline = by + (bh + smf.ascent() - smf.descent()) / 2 - 1;
            p->drawText(bx + (bw - smf.horizontalAdvance(badgeText)) / 2,
                        tBaseline, badgeText);
            badgeRight = bx - 6;
        }

        // Reserve room for an inline kind-word suffix (e.g. " struct",
        // " enum") rendered in a smaller dim font right after the name,
        // so the user can tell at a glance what each row IS without
        // squinting at the icon. Empty when the kind isn't a type-flavor
        // (regular symbols, bookmarks, etc.).
        QString kindWord;
        if (e.kind == QLatin1String("struct") || e.kind == QLatin1String("type")
            || e.kind == QLatin1String("class") || e.kind == QLatin1String("union")
            || e.kind == QLatin1String("enum")) {
            kindWord = e.kind == QLatin1String("type") ? QStringLiteral("struct") : e.kind;
        }
        QFontMetrics smfNameTail(m_smallFont);
        int kindTailW = kindWord.isEmpty() ? 0
            : (smfNameTail.horizontalAdvance(kindWord) + 8);

        // Name area — prefer the humanised display form when the provider
        // supplied one (e.g. demangled C++ name). Falls back to raw name.
        const QString& shownName = e.displayName.isEmpty() ? e.name : e.displayName;
        int nameMaxX = badgeRight - 4 - kindTailW;
        int nameW = qMax(0, nameMaxX - x);
        QFontMetrics nfm(m_font);
        QString elided = nfm.elidedText(shownName, Qt::ElideRight, nameW);
        p->setFont(m_font);

        const QVector<int>& mp = m_model->matchPositionsAt(idx.row());
        if (m_searchActive && !mp.isEmpty()) {
            // [13] Fuzzy highlight uses a colored background span across
            // matched chars (type chooser line 253) — much more readable
            // than the previous bold-pen-only treatment.
            int cx = x;
            for (int i = 0; i < elided.size(); i++) {
                bool hit = std::find(mp.constBegin(), mp.constEnd(), i) != mp.constEnd();
                QString ch(elided.at(i));
                int chW = nfm.horizontalAdvance(ch);
                if (hit) {
                    p->fillRect(cx, r.y() + 1, chW, r.height() - 2, t.selection);
                }
                p->setPen(t.text);
                p->drawText(cx, baseline, ch);
                cx += chW;
            }
        } else {
            p->setPen(t.text);
            p->drawText(x, baseline, elided);
        }

        // Kind word ("struct" / "union" / "enum") right after the name —
        // small + dim so it reads as a tag without competing visually
        // with the identifier itself.
        if (!kindWord.isEmpty()) {
            int nameDrawnW = nfm.horizontalAdvance(elided);
            int kx = x + nameDrawnW + 6;
            p->setFont(m_smallFont);
            p->setPen(t.textFaint);
            p->drawText(kx, baseline - 1, kindWord);
        }

        // Address (monospace baked into the panel font; right-aligned).
        p->setFont(m_font);
        p->setPen(e.address != 0 ? t.textDim : t.textFaint);
        p->drawText(rvaX, baseline, rvaText);
    }

private:
    void recomputeMetrics() {
        QFontMetrics fm(m_font);
        // [9] Compact vs Normal — same constants as type chooser line 322:
        // fm.height() + 3 vs fm.height() + 6.
        m_rowH = m_compact ? (fm.height() + 3) : (fm.height() + 6);
        if (auto* v = qobject_cast<QListView*>(parent())) {
            v->doItemsLayout();
        }
    }

    UnifiedSymbolModel* m_model;
    AccentFn m_accent;
    bool m_searchActive = false;
    bool m_compact = false;
    int  m_rowH = 22;
    int  m_addressColW = 0;
    uint64_t m_activeAddr = 0;
    QFont m_font;
    QFont m_smallFont;
};

// One unified, search-driven Symbols panel.
class UnifiedSymbolPanel : public QWidget {
    Q_OBJECT
public:
    using ActiveProviderFn = std::function<const Provider*()>;

    explicit UnifiedSymbolPanel(QWidget* parent = nullptr) : QWidget(parent) {
        // Anchor our own sizeHint so QDockWidget doesn't expand to fit the
        // footer's long single-line text or the view's content width when
        // a row is selected. Without this, picking a row whose footer
        // crumb has a wide "name · source · 0x... · N B · type" string
        // would cause the dock to balloon out.
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::MinimumExpanding);

        // [1] Outer layout margins 6,5,6,5 + 3px spacing (type chooser
        // line 391 conventions). This is the foundational visual rhythm.
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(6, 5, 6, 5);
        outer->setSpacing(3);

        buildSearchRow(outer);
        buildChipRow(outer);
        buildSortRow(outer);
        buildList(outer);
        buildFooter(outer);

        // Refresh on provider changes
        connect(&NameRegistry::instance(), &NameRegistry::providersChanged, this,
                &UnifiedSymbolPanel::rebuild);

        loadPersistedState(); // [22]
        applyTheme(ThemeManager::instance().current());
        refreshSortLabels();
        installEventFilter(this);
        m_view->installEventFilter(this);
        m_search->installEventFilter(this);
    }

    ~UnifiedSymbolPanel() override { savePersistedState(); }

    void setActiveProviderFn(ActiveProviderFn fn) { m_activeFn = std::move(fn); }
    // [20] Caller passes the editor's current absolute address so the
    // matching row (if any) gets a soft active-row marker.
    void setActiveAddress(uint64_t addr) {
        m_delegate->setActiveAddress(addr);
        m_view->viewport()->update();
    }

    void applyTheme(const Theme& t) {
        QSettings s("Reclass", "Reclass");
        QFont base(s.value("font", "JetBrains Mono").toString(), 10);
        base.setFixedPitch(true);
        m_view->setFont(base);
        m_search->setFont(base);
        m_footer->setFont(scaledFont(base, -2));
        m_statusLabel->setFont(scaledFont(base, -2));
        for (auto* b : m_sortBtns) b->setFont(scaledFont(base, -1));
        for (auto* c : m_providerChips) c->setFont(scaledFont(base, -1));
        for (auto* b : m_densityBtns) b->setFont(scaledFont(base, -1));

        m_delegate->setSizeMetrics(base);
        QFontMetrics fm(base);
        // [26] Pre-compute uniform address column width — pad to widest
        // possible 16-digit grouped hex string (3 underscores).
        m_delegate->setAddressColWidth(fm.horizontalAdvance(
            QStringLiteral("0xFFFF_FFFF_FFFF_FFFF")));

        m_search->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: 1px solid %4;"
            " border-radius: 2px; padding: 2px 4px; }"
            "QLineEdit:focus { border: 1px solid %5; }"
            "QLineEdit QToolButton { padding: 0px 4px; }"
            "QLineEdit QToolButton:hover { background: %3; }")
            .arg(t.background.name(), t.textDim.name(), t.hover.name(),
                 t.border.name(), t.borderFocused.name()));

        if (m_chipRowHost) {
            m_chipRowHost->setAutoFillBackground(true);
            QPalette pp = m_chipRowHost->palette();
            pp.setColor(QPalette::Window, t.backgroundAlt);
            m_chipRowHost->setPalette(pp);
        }
        if (m_sortRow) {
            m_sortRow->setAutoFillBackground(true);
            QPalette pp = m_sortRow->palette();
            pp.setColor(QPalette::Window, t.backgroundAlt);
            m_sortRow->setPalette(pp);
        }
        // [5,6,7] Sort buttons styled with type-chooser palette: muted
        // off, t.text+selected-bg on, border-right between buttons.
        const QString sortSheet = QStringLiteral(
            "QToolButton { color: %1; background: transparent;"
            " border: none; border-right: 1px solid %4;"
            " padding: 0 8px; }"
            "QToolButton:last-child { border-right: none; }"
            "QToolButton:checked { color: %2; background: %3; }"
            "QToolButton:hover   { color: %2; }")
            .arg(t.textMuted.name(), t.syntaxKeyword.name(),
                 t.selected.name(), t.border.name());
        for (auto* b : m_sortBtns) b->setStyleSheet(sortSheet);
        for (auto* b : m_densityBtns) b->setStyleSheet(sortSheet);

        QPalette vp = m_view->palette();
        vp.setColor(QPalette::Base, t.background);
        vp.setColor(QPalette::Text, t.text);
        vp.setColor(QPalette::Highlight, t.selected);
        vp.setColor(QPalette::HighlightedText, t.text);
        m_view->setPalette(vp);
        m_view->setStyleSheet(QStringLiteral(
            "QListView { background: %1; border: none; }"
            "QAbstractScrollArea::corner { background: %1; border: none; }")
            .arg(t.background.name()));

        m_footer->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; background: %2; border-top: 1px solid %3;"
            " padding: 0 6px; }")
            .arg(t.textFaint.name(), t.backgroundAlt.name(), t.border.name()));
        m_statusLabel->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; padding: 0 6px; }").arg(t.textFaint.name()));

        for (auto* c : m_providerChips)
            c->setGroupColor(accentFor(c->property("provId").toString()));
        m_view->viewport()->update();
    }

    void rebuild() {
        m_allEntries.clear();
        const Provider* active = m_activeFn ? m_activeFn() : nullptr;
        for (const auto& p : NameRegistry::instance().providers()) {
            auto rows = p->entries(active);
            const QString src = p->id();
            for (auto& r : rows) r.source = src;
            m_allEntries.append(rows);
        }
        // [28] Pinned rows (provider-agnostic, stored in QSettings) bubble
        // to the top of the listing.
        applyPinning();
        rebuildProviderChips();
        refilter();
    }

signals:
    void navigateRequested(const rcx::NamedAddress& entry);
    void importTypeRequested(const rcx::NamedAddress& entry);
    void importSelectedRequested(const QVector<rcx::NamedAddress>& entries);
    void removeRequested(const rcx::NamedAddress& entry);
    // [24] Quick-actions from the right-click-empty-area menu.
    void loadPdbRequested();
    void addBookmarkRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* ev) override {
        // [16,17,18] Keyboard parity with type chooser navigation rules.
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            // [16] Ctrl+F focuses search from anywhere in the panel.
            if (ke->modifiers().testFlag(Qt::ControlModifier)
                && ke->key() == Qt::Key_F) {
                m_search->setFocus();
                m_search->selectAll();
                return true;
            }
            if (watched == m_search) {
                // [16] Esc clears search if any, else lets parent close.
                if (ke->key() == Qt::Key_Escape) {
                    if (!m_search->text().isEmpty()) {
                        m_search->clear();
                        return true;
                    }
                }
                // [18] Down arrow / Enter from search → first selectable row.
                if (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Return
                    || ke->key() == Qt::Key_Enter) {
                    if (m_model->rowCount() > 0) {
                        QModelIndex idx = m_model->index(0, 0);
                        m_view->setCurrentIndex(idx);
                        m_view->setFocus();
                        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                            activateRow(idx);
                        }
                        return true;
                    }
                }
                // [19] Number keys 1-9 toggle provider chips while in search
                // only when Alt is held (so they don't conflict with typing).
                if (ke->modifiers().testFlag(Qt::AltModifier)
                    && ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9) {
                    int n = ke->key() - Qt::Key_1;
                    if (n < m_providerChips.size())
                        m_providerChips[n]->toggle();
                    return true;
                }
            }
            if (watched == m_view) {
                // [17] Enter on a row → navigate / import based on kind.
                if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                    activateRow(m_view->currentIndex());
                    return true;
                }
                // [19] Number keys 1-9 toggle chips while focused on the list.
                if (ke->key() >= Qt::Key_1 && ke->key() <= Qt::Key_9
                    && !ke->modifiers().testFlag(Qt::ControlModifier)) {
                    int n = ke->key() - Qt::Key_1;
                    if (n < m_providerChips.size()) {
                        m_providerChips[n]->toggle();
                        return true;
                    }
                }
                // [18] Backspace pops back to search (type chooser line 1803).
                if (ke->key() == Qt::Key_Backspace) {
                    QString t = m_search->text();
                    if (!t.isEmpty()) m_search->setText(t.chopped(1));
                    m_search->setFocus();
                    return true;
                }
                // [18] Printable keys forward to search (type-to-filter).
                QString s = ke->text();
                if (!s.isEmpty() && s.at(0).isPrint()
                    && !ke->modifiers().testFlag(Qt::ControlModifier)
                    && !ke->modifiers().testFlag(Qt::AltModifier)) {
                    m_search->setText(m_search->text() + s);
                    m_search->setFocus();
                    return true;
                }
            }
        }
        // [29] Tooltip on hover — show full info when name is elided
        // or for richer context (kind + size + source).
        if (ev->type() == QEvent::ToolTip && watched == m_view) {
            auto* he = static_cast<QHelpEvent*>(ev);
            QModelIndex idx = m_view->indexAt(he->pos());
            if (idx.isValid()) {
                const auto& e = m_model->rowAt(idx.row());
                QString tip = QStringLiteral("<b>%1</b>").arg(e.name.toHtmlEscaped());
                if (!e.kind.isEmpty())
                    tip += QStringLiteral("<br>kind: %1").arg(e.kind);
                tip += QStringLiteral("<br>source: %1").arg(e.source);
                if (e.address != 0)
                    tip += QStringLiteral("<br>address: 0x%1").arg(e.address, 0, 16);
                if (e.size != 0)
                    tip += QStringLiteral("<br>size: %1 B").arg(e.size);
                if (e.typeIndex != 0)
                    tip += QStringLiteral("<br>typeIndex: %1").arg(e.typeIndex);
                QToolTip::showText(he->globalPos(), tip, m_view);
                return true;
            }
            QToolTip::hideText();
        }
        return QWidget::eventFilter(watched, ev);
    }

private:
    // ── Builders ─────────────────────────────────────────────────────────
    void buildSearchRow(QVBoxLayout* outer) {
        // [2] Search field with type-chooser-style compact padding.
        m_search = new QLineEdit(this);
        m_search->setPlaceholderText(QStringLiteral("Search names..."));
        auto* searchIcon = m_search->addAction(
            QIcon(QStringLiteral(":/vsicons/search.svg")), QLineEdit::LeadingPosition);
        for (auto* btn : m_search->findChildren<QToolButton*>())
            if (btn->defaultAction() == searchIcon) { btn->setIconSize(QSize(14, 14)); break; }
        auto* clearAct = m_search->addAction(
            QIcon(QStringLiteral(":/vsicons/close.svg")), QLineEdit::TrailingPosition);
        clearAct->setVisible(false);
        connect(clearAct, &QAction::triggered, m_search, &QLineEdit::clear);
        connect(m_search, &QLineEdit::textChanged, clearAct,
                [clearAct](const QString& s) { clearAct->setVisible(!s.isEmpty()); });
        for (auto* btn : m_search->findChildren<QToolButton*>())
            if (btn->defaultAction() == clearAct) { btn->setIconSize(QSize(14, 14)); break; }
        outer->addWidget(m_search);

        m_searchTimer = new QTimer(this);
        m_searchTimer->setSingleShot(true);
        m_searchTimer->setInterval(120);
        connect(m_search, &QLineEdit::textChanged, this, [this] {
            // [21] If the search text starts with "0x" / "0X" treat it as
            // an address query — short-circuit the fuzzy filter and jump
            // to the matching entry.
            const QString t = m_search->text().trimmed();
            if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                && t.size() > 2) {
                bool ok = false;
                uint64_t a = t.mid(2).toULongLong(&ok, 16);
                if (ok) {
                    jumpToAddress(a);
                    return;
                }
            }
            m_searchTimer->start();
        });
        connect(m_searchTimer, &QTimer::timeout, this, &UnifiedSymbolPanel::refilter);
    }

    void buildChipRow(QVBoxLayout* outer) {
        // [3,4] Chip row with equalized widths + right-aligned status label.
        m_chipRowHost = new QWidget(this);
        m_chipRowHost->setFixedHeight(24);
        m_chipRow = new QHBoxLayout(m_chipRowHost);
        m_chipRow->setContentsMargins(2, 2, 2, 2);
        m_chipRow->setSpacing(2);
        m_chipRow->addStretch();
        m_statusLabel = new QLabel(m_chipRowHost);
        m_statusLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_chipRow->addWidget(m_statusLabel);
        outer->addWidget(m_chipRowHost);
    }

    void buildSortRow(QVBoxLayout* outer) {
        // [5,7,8] Sort row mirrors type chooser line 487-594:
        // sort buttons LEFT (anchored to name column), density toggles RIGHT.
        auto* sortRow = new QWidget(this);
        sortRow->setFixedHeight(22);
        m_sortRow = sortRow;
        auto* slay = new QHBoxLayout(sortRow);
        slay->setContentsMargins(0, 0, 0, 0);
        slay->setSpacing(0);
        const char* sortLabels[] = {"name", "address", "kind"};
        for (int i = 0; i < 3; i++) {
            auto* b = new QToolButton(sortRow);
            b->setText(QString::fromLatin1(sortLabels[i]));
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setCursor(Qt::PointingHandCursor);
            if (i == 0) b->setChecked(true);
            connect(b, &QToolButton::clicked, this, [this, i] {
                if (m_sort == i) m_sortDir = -m_sortDir;
                else { m_sort = i; m_sortDir = 1; }
                for (int k = 0; k < m_sortBtns.size(); k++)
                    m_sortBtns[k]->setChecked(k == m_sort);
                refreshSortLabels();
                refilter();
                savePersistedState();
            });
            m_sortBtns.append(b);
            slay->addWidget(b);
        }
        slay->addStretch();
        // [8] Density toggles on the right side of the sort row. The icon
        // that visually reads "looser" maps to Normal density (taller
        // rows); the icon that reads "tighter" maps to Compact (shorter
        // rows). Earlier rev had these swapped — users intuitively read
        // ≡ as the dense one and ☰ as the loose one in most fonts.
        auto* normal = new QToolButton(sortRow);
        normal->setText(QStringLiteral("☰"));
        normal->setCheckable(true); normal->setChecked(true);
        normal->setToolTip(QStringLiteral("Normal density"));
        normal->setCursor(Qt::PointingHandCursor);
        auto* compact = new QToolButton(sortRow);
        compact->setText(QStringLiteral("≡"));
        compact->setCheckable(true);
        compact->setToolTip(QStringLiteral("Compact density"));
        compact->setCursor(Qt::PointingHandCursor);
        m_densityBtns << normal << compact;
        slay->addWidget(normal);
        slay->addWidget(compact);
        auto applyDensity = [this, normal, compact](bool wantCompact) {
            m_delegate->setCompact(wantCompact);
            normal->setChecked(!wantCompact);
            compact->setChecked(wantCompact);
            savePersistedState();
        };
        connect(normal, &QToolButton::clicked, this, [applyDensity] { applyDensity(false); });
        connect(compact, &QToolButton::clicked, this, [applyDensity] { applyDensity(true); });
        outer->addWidget(sortRow);
    }

    void buildList(QVBoxLayout* outer) {
        m_model = new UnifiedSymbolModel(this);
        m_view  = new QListView(this);
        m_view->setModel(m_model);
        m_view->setMouseTracking(true);
        m_view->setUniformItemSizes(true);
        m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_view->setContextMenuPolicy(Qt::CustomContextMenu);
        m_view->setFrameShape(QFrame::NoFrame);
        // [25] Enable drag so users can drag rows into the editor.
        m_view->setDragEnabled(true);
        m_view->setDragDropMode(QAbstractItemView::DragOnly);
        m_view->setAlternatingRowColors(false); // we paint our own striping
        m_delegate = new UnifiedSymbolDelegate(m_model,
            [this](const QString& src) { return accentFor(src); }, m_view);
        m_view->setItemDelegate(m_delegate);
        outer->addWidget(m_view, 1);

        connect(m_view, &QAbstractItemView::doubleClicked, this,
                [this](const QModelIndex& idx) {
            if (!idx.isValid()) return;
            activateRow(idx);
        });
        connect(m_view, &QWidget::customContextMenuRequested, this,
                &UnifiedSymbolPanel::onContextMenu);
        // [21] Clickable "T" badge — single-click in badge rect imports.
        m_view->viewport()->installEventFilter(this);
        // Selection drives footer update (iteration [30]).
        connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this] { updateFooter(); });
    }

    void buildFooter(QVBoxLayout* outer) {
        // [11] Footer crumb — fixed 20px, border-top, dim text. Mirrors
        // type chooser line 811-822. Width-Ignored so a long selection
        // crumb (e.g. "name · source · 0x... · N B · type") never
        // propagates up the layout chain and balloons the dock.
        m_footer = new QLabel(this);
        m_footer->setFixedHeight(20);
        m_footer->setTextFormat(Qt::RichText);
        m_footer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_footer->setText(QStringLiteral("↑↓ navigate · Enter activate · Ctrl+F filter · drag to editor"));
        outer->addWidget(m_footer);
    }

public:
    // Anchor our preferred size so the dock keeps whatever width the user
    // sized it to. Returning a small constant width means QListView's own
    // content-width hint (which can grow when a wide row is selected) is
    // effectively ignored.
    QSize sizeHint() const override { return QSize(280, 400); }
    QSize minimumSizeHint() const override { return QSize(200, 120); }

private:

    // ── Helpers ──────────────────────────────────────────────────────────
    QFont scaledFont(const QFont& base, int dPx) const {
        QFont f = base;
        if (f.pixelSize() > 0) f.setPixelSize(qMax(7, f.pixelSize() + dPx));
        else f.setPointSize(qMax(6, f.pointSize() + dPx));
        return f;
    }

    QColor accentFor(const QString& providerId) const {
        for (const auto& p : NameRegistry::instance().providers()) {
            if (p->id() == providerId) {
                uint32_t packed = p->accent();
                if (packed != 0) return QColor::fromRgba(packed);
                break;
            }
        }
        const auto& t = ThemeManager::instance().current();
        static const QColor palette[] = {
            t.indHoverSpan, t.syntaxKeyword, t.markerCycle, t.markerPtr,
            t.syntaxType,   t.syntaxString,  t.indDataChanged, t.syntaxPreproc
        };
        uint h = 0;
        for (QChar c : providerId.toLower()) h = h * 131 + c.unicode();
        return palette[h % (sizeof(palette) / sizeof(palette[0]))];
    }

    void rebuildProviderChips() {
        const auto provs = NameRegistry::instance().providers();
        QSet<QString> wanted;
        for (const auto& p : provs) wanted.insert(p->id());

        for (int i = m_providerChips.size() - 1; i >= 0; i--) {
            QString id = m_providerChips[i]->property("provId").toString();
            if (!wanted.contains(id)) {
                m_chipRow->removeWidget(m_providerChips[i]);
                m_providerChips[i]->deleteLater();
                m_providerChips.removeAt(i);
            }
        }
        int insertAt = 0;
        for (const auto& p : provs) {
            bool exists = false;
            for (auto* c : m_providerChips)
                if (c->property("provId").toString() == p->id()) { exists = true; break; }
            if (!exists) {
                auto* chip = new CategoryChip(p->displayName(), m_chipRowHost);
                chip->setProperty("provId", p->id());
                chip->setGroupColor(accentFor(p->id()));
                chip->setChecked(!m_hiddenProviders.contains(p->id()));
                chip->setCursor(Qt::PointingHandCursor);
                chip->setContextMenuPolicy(Qt::CustomContextMenu);
                // [23] Right-click chip → Show only / Hide all / Show all.
                connect(chip, &QWidget::customContextMenuRequested, this,
                        [this, chip](const QPoint& pos) { onChipContextMenu(chip, pos); });
                connect(chip, &QAbstractButton::toggled, this, [this, chip](bool on) {
                    QString id = chip->property("provId").toString();
                    if (on) m_hiddenProviders.remove(id);
                    else    m_hiddenProviders.insert(id);
                    savePersistedState();
                    refilter();
                });
                m_chipRow->insertWidget(insertAt, chip);
                m_providerChips.append(chip);
            }
            insertAt++;
        }
        // [3] Equalize chip widths so the row reads as a single strip
        // (type chooser line 1671-1682 technique).
        int maxW = 0;
        for (auto* c : m_providerChips)
            maxW = qMax(maxW, c->sizeHint().width());
        for (auto* c : m_providerChips)
            c->setFixedWidth(maxW);

        QHash<QString, int> perProv;
        for (const auto& e : m_allEntries) perProv[e.source]++;
        for (auto* c : m_providerChips)
            c->setCount(perProv.value(c->property("provId").toString()));
    }

    void applyPinning() {
        // [28] Pinned rows bubble to top. m_pinned holds "providerId|name".
        if (m_pinned.isEmpty()) return;
        QVector<NamedAddress> pinned, rest;
        for (auto& e : m_allEntries) {
            QString key = e.source + QLatin1Char('|') + e.name;
            if (m_pinned.contains(key)) pinned.append(std::move(e));
            else                         rest.append(std::move(e));
        }
        m_allEntries = pinned;
        m_allEntries += rest;
    }

    void refilter() {
        QString pat = m_search->text().trimmed();
        if (pat.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) pat.clear();

        QSet<QString> activeProvs;
        bool anyOff = false;
        for (auto* c : m_providerChips) {
            if (c->isChecked()) activeProvs.insert(c->property("provId").toString());
            else anyOff = true;
        }

        QVector<NamedAddress> visible;
        QVector<QVector<int>> positions;
        QVector<int> scores;
        visible.reserve(m_allEntries.size());

        for (const auto& e : m_allEntries) {
            if (anyOff && !activeProvs.contains(e.source)) continue;
            QVector<int> mp;
            int sc = 1;
            if (!pat.isEmpty()) {
                // Match against the humanised name when present so the user
                // can search "Foo::Bar" instead of "?Bar@Foo@@..." and get
                // highlight positions that line up with what's painted.
                const QString& target = e.displayName.isEmpty() ? e.name : e.displayName;
                sc = fuzzyScore(pat, target, &mp);
                if (sc == 0) continue;
            }
            visible.append(e);
            positions.append(std::move(mp));
            scores.append(sc);
        }

        QVector<int> order(visible.size());
        for (int i = 0; i < order.size(); i++) order[i] = i;
        int sortMode = m_sort;
        int dir = m_sortDir;
        bool searchActive = !pat.isEmpty();
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (searchActive && scores[a] != scores[b]) return scores[a] > scores[b];
            const auto& A = visible[a]; const auto& B = visible[b];
            int c = 0;
            switch (sortMode) {
                case 0: c = A.name.compare(B.name, Qt::CaseInsensitive); break;
                case 1: c = (A.address < B.address) ? -1 : (A.address > B.address) ? 1 : 0; break;
                case 2: { // kind grouping
                    c = A.kind.compare(B.kind, Qt::CaseInsensitive);
                    if (c == 0) c = A.source.compare(B.source, Qt::CaseInsensitive);
                    break;
                }
            }
            if (c == 0) c = A.name.compare(B.name, Qt::CaseInsensitive);
            return dir * c < 0;
        });

        QVector<NamedAddress> sortedVis(order.size());
        QVector<QVector<int>> sortedPos(order.size());
        for (int i = 0; i < order.size(); i++) {
            sortedVis[i] = std::move(visible[order[i]]);
            sortedPos[i] = std::move(positions[order[i]]);
        }

        QHash<QString, int> perVis, perTot;
        for (const auto& e : sortedVis) perVis[e.source]++;
        for (const auto& e : m_allEntries) perTot[e.source]++;
        for (auto* c : m_providerChips) {
            QString id = c->property("provId").toString();
            int vis = perVis.value(id);
            int tot = perTot.value(id);
            if (searchActive) c->setCount(vis, tot);
            else              c->setCount(tot);
        }

        m_delegate->setSearchActive(searchActive);
        m_model->setRows(std::move(sortedVis), std::move(sortedPos));
        updateFooter();
        updateStatus(perVis, perTot, searchActive);
    }

    void updateStatus(const QHash<QString, int>& perVis,
                      const QHash<QString, int>& perTot, bool searchActive) {
        int vis = 0, tot = 0;
        for (auto it = perVis.constBegin(); it != perVis.constEnd(); ++it) vis += it.value();
        for (auto it = perTot.constBegin(); it != perTot.constEnd(); ++it) tot += it.value();
        // [4] Status label right-aligned in chip row: "N of M" or "M total".
        if (searchActive || vis != tot)
            m_statusLabel->setText(QStringLiteral("%1 of %2").arg(vis).arg(tot));
        else if (tot > 0)
            m_statusLabel->setText(QStringLiteral("%1 total").arg(tot));
        else
            m_statusLabel->setText(QString());
    }

    void updateFooter() {
        // [11,30] Footer crumb: selection-aware. Shows hot row info or,
        // when multiple type rows are selected, sum of sizes.
        auto* sel = m_view->selectionModel();
        QModelIndexList rows = sel ? sel->selectedIndexes() : QModelIndexList();
        if (rows.size() >= 2) {
            int typesSel = 0;
            uint64_t totalSize = 0;
            for (const auto& mi : rows) {
                if (!mi.isValid()) continue;
                const auto& e = m_model->rowAt(mi.row());
                if (e.kind == QLatin1String("type") || e.kind == QLatin1String("enum")) {
                    typesSel++;
                    totalSize += e.size;
                }
            }
            if (typesSel > 0)
                m_footer->setText(QStringLiteral("%1 types selected · Σ %2 B")
                    .arg(typesSel).arg(totalSize));
            else
                m_footer->setText(QStringLiteral("%1 rows selected").arg(rows.size()));
            return;
        }
        if (rows.size() == 1 && rows.first().isValid()) {
            const auto& e = m_model->rowAt(rows.first().row());
            const QString shown = e.displayName.isEmpty() ? e.name : e.displayName;
            QStringList bits;
            bits << QStringLiteral("<b>%1</b>").arg(shown.toHtmlEscaped());
            bits << e.source;
            if (!e.kind.isEmpty()) bits << e.kind;
            if (e.address != 0)
                bits << QStringLiteral("0x%1").arg(e.address, 0, 16);
            if (e.size != 0) bits << QStringLiteral("%1 B").arg(e.size);
            // Discoverability cue: if the row carries a type definition,
            // tell the user double-click imports it.
            if (e.typeIndex != 0 && e.address == 0)
                bits << QStringLiteral("<i>double-click to import</i>");
            else if (e.typeIndex != 0)
                bits << QStringLiteral("<i>right-click → Import type</i>");
            else if (e.address != 0)
                bits << QStringLiteral("<i>double-click to navigate</i>");
            m_footer->setText(bits.join(QStringLiteral(" · ")));
            return;
        }
        // [15] Empty-state-ish hint when no rows or hint about workflow.
        if (m_model->rowCount() == 0) {
            if (m_allEntries.isEmpty())
                m_footer->setText(QStringLiteral("No names yet — open a PDB or scan RTTI"));
            else
                m_footer->setText(QStringLiteral("No matches"));
            return;
        }
        m_footer->setText(QStringLiteral(
            "↑↓ navigate · Enter activate · Ctrl+F filter · drag to editor"));
    }

    void refreshSortLabels() {
        static const char* labels[] = {"name", "address", "kind"};
        for (int i = 0; i < m_sortBtns.size(); i++) {
            QString s = QString::fromLatin1(labels[i]);
            if (i == m_sort)
                s += (m_sortDir == 1 ? QStringLiteral(" ↑") : QStringLiteral(" ↓"));
            m_sortBtns[i]->setText(s);
        }
    }

    void activateRow(const QModelIndex& idx) {
        if (!idx.isValid()) return;
        const auto& e = m_model->rowAt(idx.row());
        if (e.address != 0) {
            emit navigateRequested(e);
        } else if (e.typeIndex != 0) {
            emit importTypeRequested(e);
        }
    }

    void jumpToAddress(uint64_t addr) {
        // [21] Address search — find an entry exactly at addr.
        for (int i = 0; i < m_allEntries.size(); i++) {
            if (m_allEntries[i].address == addr) {
                m_model->setRows({ m_allEntries[i] }, { {} });
                m_delegate->setSearchActive(false);
                if (m_model->rowCount() > 0) {
                    m_view->setCurrentIndex(m_model->index(0, 0));
                }
                m_statusLabel->setText(QStringLiteral("addr lookup"));
                m_footer->setText(QStringLiteral("Address 0x%1 → %2")
                    .arg(addr, 0, 16).arg(m_allEntries[i].name));
                return;
            }
        }
        m_model->setRows({}, {});
        m_footer->setText(QStringLiteral("No entry at 0x%1").arg(addr, 0, 16));
    }

    void onContextMenu(const QPoint& pos) {
        QModelIndex idx = m_view->indexAt(pos);
        if (!idx.isValid()) {
            // [24] Right-click empty area → quick actions.
            QMenu menu(this);
            menu.addAction(QStringLiteral("Load PDB..."), this,
                           [this] { emit loadPdbRequested(); });
            menu.addAction(QStringLiteral("Add bookmark..."), this,
                           [this] { emit addBookmarkRequested(); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Refresh"), this,
                           [this] { rebuild(); });
            menu.exec(m_view->viewport()->mapToGlobal(pos));
            return;
        }
        const NamedAddress e = m_model->rowAt(idx.row());

        QVector<NamedAddress> selectedRows;
        for (const auto& mi : m_view->selectionModel()->selectedIndexes()) {
            if (mi.isValid()) selectedRows.append(m_model->rowAt(mi.row()));
        }
        bool clickedInSel = false;
        for (const auto& r : selectedRows)
            if (r.name == e.name && r.address == e.address) { clickedInSel = true; break; }
        if (!clickedInSel) selectedRows = { e };

        QMenu menu(this);

        if (e.address != 0) {
            auto* a = menu.addAction(QStringLiteral("Go to address\tEnter"));
            connect(a, &QAction::triggered, this, [this, e] { emit navigateRequested(e); });
        }

        int withType = 0;
        for (const auto& r : selectedRows) if (r.typeIndex != 0) withType++;
        if (withType == 1) {
            auto* a = menu.addAction(QStringLiteral("Import type"));
            const NamedAddress target = selectedRows.size() == 1 ? selectedRows.first() : e;
            connect(a, &QAction::triggered, this, [this, target] {
                emit importTypeRequested(target);
            });
        } else if (withType >= 2) {
            auto* a = menu.addAction(QStringLiteral("Import %1 selected types").arg(withType));
            QVector<NamedAddress> filtered;
            for (const auto& r : selectedRows) if (r.typeIndex != 0) filtered.append(r);
            connect(a, &QAction::triggered, this, [this, filtered] {
                emit importSelectedRequested(filtered);
            });
        }

        menu.addSeparator();
        auto* actName = menu.addAction(QStringLiteral("Copy name"));
        connect(actName, &QAction::triggered, this, [e] {
            QApplication::clipboard()->setText(e.name);
        });
        if (e.address != 0) {
            auto* actAddr = menu.addAction(QStringLiteral("Copy address"));
            connect(actAddr, &QAction::triggered, this, [e] {
                QApplication::clipboard()->setText(
                    QStringLiteral("0x%1").arg(e.address, 0, 16));
            });
        }
        // [18 bonus] Copy multi-row selection as TSV.
        if (selectedRows.size() >= 2) {
            auto* actTsv = menu.addAction(QStringLiteral("Copy %1 rows as TSV")
                .arg(selectedRows.size()));
            connect(actTsv, &QAction::triggered, this, [selectedRows] {
                QStringList lines;
                lines << QStringLiteral("name\taddress\tsource\tkind");
                for (const auto& r : selectedRows) {
                    lines << QStringLiteral("%1\t0x%2\t%3\t%4")
                        .arg(r.name).arg(r.address, 0, 16).arg(r.source).arg(r.kind);
                }
                QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
            });
        }

        // [28] Pin / unpin
        menu.addSeparator();
        QString pinKey = e.source + QLatin1Char('|') + e.name;
        bool pinned = m_pinned.contains(pinKey);
        auto* actPin = menu.addAction(pinned
            ? QStringLiteral("Unpin from top")
            : QStringLiteral("Pin to top"));
        connect(actPin, &QAction::triggered, this, [this, pinKey, pinned] {
            if (pinned) m_pinned.remove(pinKey);
            else        m_pinned.insert(pinKey);
            savePersistedState();
            rebuild();
        });

        // [Remove] when provider supports it.
        for (const auto& p : NameRegistry::instance().providers()) {
            if (p->id() == e.source && p->supportsRemove()) {
                menu.addSeparator();
                auto* a = menu.addAction(QStringLiteral("Remove"));
                connect(a, &QAction::triggered, this, [this, e] { emit removeRequested(e); });
                break;
            }
        }

        menu.exec(m_view->viewport()->mapToGlobal(pos));
    }

    // [23] Provider chip right-click context: Show only this / Hide all / Show all.
    void onChipContextMenu(CategoryChip* chip, const QPoint& pos) {
        QString id = chip->property("provId").toString();
        QMenu menu(this);
        menu.addAction(QStringLiteral("Show only this"), this, [this, id] {
            m_hiddenProviders.clear();
            for (auto* c : m_providerChips) {
                QString cid = c->property("provId").toString();
                bool keep = (cid == id);
                QSignalBlocker block(c);
                c->setChecked(keep);
                if (!keep) m_hiddenProviders.insert(cid);
            }
            savePersistedState();
            refilter();
        });
        menu.addAction(QStringLiteral("Show all"), this, [this] {
            m_hiddenProviders.clear();
            for (auto* c : m_providerChips) {
                QSignalBlocker block(c);
                c->setChecked(true);
            }
            savePersistedState();
            refilter();
        });
        menu.addAction(QStringLiteral("Hide all others"), this, [this, id] {
            m_hiddenProviders.clear();
            for (auto* c : m_providerChips) {
                QString cid = c->property("provId").toString();
                bool keep = (cid == id);
                QSignalBlocker block(c);
                c->setChecked(keep);
                if (!keep) m_hiddenProviders.insert(cid);
            }
            savePersistedState();
            refilter();
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Refresh"), this, [this] { rebuild(); });
        menu.exec(chip->mapToGlobal(pos));
    }

    // ── Persistence ([22], [28]) ─────────────────────────────────────────
    void loadPersistedState() {
        QSettings s("Reclass", "Reclass");
        s.beginGroup("SymbolPanel");
        m_sort = s.value("sort", 0).toInt();
        m_sortDir = s.value("sortDir", 1).toInt();
        bool compact = s.value("compact", false).toBool();
        QStringList hidden = s.value("hiddenProviders").toStringList();
        m_hiddenProviders = QSet<QString>(hidden.begin(), hidden.end());
        QStringList pinned = s.value("pinned").toStringList();
        m_pinned = QSet<QString>(pinned.begin(), pinned.end());
        QString lastSearch = s.value("lastSearch").toString();
        s.endGroup();
        if (m_sort < 0 || m_sort >= 3) m_sort = 0;
        for (int i = 0; i < m_sortBtns.size(); i++)
            m_sortBtns[i]->setChecked(i == m_sort);
        m_delegate->setCompact(compact);
        if (m_densityBtns.size() == 2) {
            m_densityBtns[0]->setChecked(!compact);
            m_densityBtns[1]->setChecked(compact);
        }
        if (!lastSearch.isEmpty()) m_search->setText(lastSearch);
    }

    void savePersistedState() {
        QSettings s("Reclass", "Reclass");
        s.beginGroup("SymbolPanel");
        s.setValue("sort", m_sort);
        s.setValue("sortDir", m_sortDir);
        s.setValue("compact", m_delegate ? m_delegate->compact() : false);
        s.setValue("hiddenProviders",
                   QStringList(m_hiddenProviders.begin(), m_hiddenProviders.end()));
        s.setValue("pinned",
                   QStringList(m_pinned.begin(), m_pinned.end()));
        s.setValue("lastSearch", m_search ? m_search->text() : QString());
        s.endGroup();
    }

    // ── Members ──────────────────────────────────────────────────────────
    QLineEdit*                  m_search = nullptr;
    QTimer*                     m_searchTimer = nullptr;
    QWidget*                    m_chipRowHost = nullptr;
    QHBoxLayout*                m_chipRow = nullptr;
    QLabel*                     m_statusLabel = nullptr;
    QWidget*                    m_sortRow = nullptr;
    QList<CategoryChip*>        m_providerChips;
    QList<QToolButton*>         m_sortBtns;
    QList<QToolButton*>         m_densityBtns;
    UnifiedSymbolModel*         m_model = nullptr;
    UnifiedSymbolDelegate*      m_delegate = nullptr;
    QListView*                  m_view = nullptr;
    QLabel*                     m_footer = nullptr;
    QVector<NamedAddress>       m_allEntries;
    QSet<QString>               m_hiddenProviders;
    QSet<QString>               m_pinned;
    int                         m_sort = 0;    // 0=name, 1=address, 2=kind
    int                         m_sortDir = 1;
    ActiveProviderFn            m_activeFn;
};

} // namespace rcx
