#include "sourcechooserpopup.h"
#include "themes/thememanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QApplication>
#include <QScreen>
#include <QScrollBar>
#include <QIcon>
#include <QToolButton>
#include <cstring>

namespace rcx {

// ── Fuzzy scoring ──

static constexpr int kMaxFuzzyLen = 64;

static int fuzzyScore(const QString& pattern, const QString& text,
                      QVector<int>* outPositions = nullptr) {
    int pLen = pattern.size(), tLen = text.size();
    if (pLen == 0) return 1;
    if (pLen > tLen) return 0;
    if (pLen > kMaxFuzzyLen || tLen > 256) {
        if (text.startsWith(pattern, Qt::CaseInsensitive)) return 1;
        return 0;
    }
    QChar pLow[kMaxFuzzyLen];
    for (int i = 0; i < pLen; i++) pLow[i] = pattern[i].toLower();
    QChar tLow[256];
    for (int i = 0; i < tLen; i++) tLow[i] = text[i].toLower();
    { int pi = 0;
      for (int ti = 0; ti < tLen && pi < pLen; ti++)
          if (pLow[pi] == tLow[ti]) pi++;
      if (pi < pLen) return 0;
    }
    int bestPos[kMaxFuzzyLen], curPos[kMaxFuzzyLen];
    int best = 0, bestLen = 0;
    auto solve = [&](auto& self, int pi, int ti, int curLen, int score) -> void {
        if (pi == pLen) {
            if (score > best) { best = score; bestLen = curLen; memcpy(bestPos, curPos, curLen * sizeof(int)); }
            return;
        }
        int maxTi = tLen - (pLen - pi), branches = 0;
        for (int i = ti; i <= maxTi && branches < 4; i++) {
            if (pLow[pi] != tLow[i]) continue;
            int bonus = 1;
            if (i == 0)                                          bonus = 10;
            else if (text[i - 1] == '_' || text[i - 1] == ' ') bonus = 8;
            else if (text[i].isUpper() && text[i - 1].isLower()) bonus = 8;
            if (curLen > 0 && i == curPos[curLen - 1] + 1)      bonus += 5;
            curPos[curLen] = i;
            self(self, pi + 1, i + 1, curLen + 1, score + bonus);
            branches++;
        }
    };
    solve(solve, 0, 0, 0, 0);
    if (best > 0) {
        best += qMax(0, 20 - (tLen - pLen));
        if (pLen == tLen) best += 20;
        if (outPositions) { outPositions->resize(bestLen); memcpy(outPositions->data(), bestPos, bestLen * sizeof(int)); }
    }
    return best;
}

// ── Icon cache ──

static QPixmap cachedIcon(const QString& path, int size) {
    static QHash<QString, QPixmap> s_cache;
    QString key = path + QChar(':') + QString::number(size);
    auto it = s_cache.constFind(key);
    if (it != s_cache.constEnd()) return *it;
    QPixmap pm = QIcon(path).pixmap(size, size);
    s_cache.insert(key, pm);
    return pm;
}

// ── Custom delegate (two-line card layout) ──

class SourceChooserDelegate : public QStyledItemDelegate {
public:
    const QVector<SourceEntry>* entries = nullptr;
    const QVector<QVector<int>>* matchPositions = nullptr;
    QFont baseFont;

    explicit SourceChooserDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    bool hasSubline(const SourceEntry& e) const {
        return e.entryKind == SourceEntry::SavedSource
            || (e.entryKind == SourceEntry::ProviderAction && !e.dllFileName.isEmpty());
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        int row = idx.row();
        QFontMetrics fm(baseFont);
        if (entries && row >= 0 && row < entries->size()) {
            const auto& e = (*entries)[row];
            if (e.entryKind == SourceEntry::SectionHeader)
                return QSize(opt.rect.width(), fm.height() + 8);
            if (hasSubline(e)) {
                QFont small = baseFont;
                small.setPointSize(qMax(7, baseFont.pointSize() - 2));
                return QSize(opt.rect.width(), fm.height() + QFontMetrics(small).height() + 14);
            }
        }
        return QSize(opt.rect.width(), fm.height() + 12);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        int row = idx.row();
        if (!entries || row < 0 || row >= entries->size()) return;
        const auto& e = (*entries)[row];
        const auto& theme = ThemeManager::instance().current();

        p->setRenderHint(QPainter::Antialiasing, false);
        p->setFont(baseFont);
        QFontMetrics fm(baseFont);
        QRect r = opt.rect;

        QFont smallFont = baseFont;
        smallFont.setPointSize(qMax(7, baseFont.pointSize() - 2));
        QFontMetrics sfm(smallFont);

        // ── Section header ──
        if (e.entryKind == SourceEntry::SectionHeader) {
            p->fillRect(r, theme.background);
            if (r.top() > 0)
                p->fillRect(r.left() + 10, r.top(), r.width() - 20, 1, theme.border);
            smallFont.setBold(true);
            p->setFont(smallFont);
            p->setPen(theme.textFaint);
            p->drawText(r.adjusted(10, 2, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                        e.displayName.toUpper());
            return;
        }

        // ── Background ──
        bool selected = opt.state & QStyle::State_Selected;
        bool hovered  = opt.state & QStyle::State_MouseOver;
        QColor bg = theme.background;
        if (selected)     bg = theme.selected;
        else if (hovered) bg = theme.hover;
        if (e.isStale && !selected)
            bg = QColor(qMin(255, bg.red() + 12), qMax(0, bg.green() - 4), qMax(0, bg.blue() - 4));
        p->fillRect(r, bg);

        // Active accent bar
        if (e.isActive)
            p->fillRect(r.left(), r.top(), 3, r.height(), theme.indHoverSpan);

        bool twoLine = hasSubline(e);
        int x = r.left() + 10;

        // ── Layout: row1 and row2 baselines ──
        int row1Y, row2Y = 0;
        if (twoLine) {
            int totalTextH = fm.height() + sfm.height() + 3;
            int topPad = (r.height() - totalTextH) / 2;
            row1Y = r.top() + topPad + fm.ascent();
            row2Y = row1Y + fm.descent() + 3 + sfm.ascent();
        } else {
            row1Y = r.top() + (r.height() - fm.height()) / 2 + fm.ascent();
        }

        // ── Icon (centered on full card height) ──
        int iconSz = twoLine ? fm.height() + 4 : fm.height();
        int iconY = r.top() + (r.height() - iconSz) / 2;
        if (!e.iconPath.isEmpty()) {
            QPixmap pm = cachedIcon(e.iconPath, iconSz);
            if (!pm.isNull()) {
                p->setOpacity(e.isStale ? 0.3 : 0.8);
                p->drawPixmap(x, iconY, pm);
                p->setOpacity(1.0);
            }
        }
        x += iconSz + 8;
        int textX = x;

        // ── Row 1: display name ──
        QColor nameColor = e.isStale ? theme.textMuted
                         : e.entryKind == SourceEntry::ClearAction ? theme.textDim
                         : theme.text;

        const QVector<int>* positions = (matchPositions && row < matchPositions->size())
            ? &(*matchPositions)[row] : nullptr;
        QSet<int> hlSet;
        if (positions) for (int pi : *positions) hlSet.insert(pi);

        QFont nameFont = baseFont;
        if (e.isActive) nameFont.setBold(true);

        for (int ci = 0; ci < e.displayName.size(); ci++) {
            QChar ch = e.displayName[ci];
            bool hl = hlSet.contains(ci);
            p->setPen(hl ? theme.indHoverSpan : nameColor);
            QFont charFont = nameFont;
            if (hl) charFont.setBold(true);
            p->setFont(charFont);
            p->drawText(x, row1Y, QString(ch));
            x += QFontMetrics(charFont).horizontalAdvance(ch);
        }
        p->setFont(baseFont);

        // Stale suffix
        if (e.isStale) {
            x += 6;
            p->setFont(smallFont);
            p->setPen(QColor(200, 80, 80));
            p->drawText(x, row1Y, QStringLiteral("(exited)"));
            p->setFont(baseFont);
        }

        // Provider chevron (right side, row 1)
        if (e.entryKind == SourceEntry::ProviderAction) {
            p->setPen(theme.textFaint);
            p->drawText(r.right() - 18, row1Y, QStringLiteral("\u203A"));
        }

        if (!twoLine) return;

        // ── Row 2: metadata ──
        x = textX;
        p->setFont(smallFont);

        // Helper: draw a pill badge and advance x
        auto drawBadge = [&](const QString& text, QColor bgColor, QColor fgColor) {
            int bw = sfm.horizontalAdvance(text) + 8;
            int bh = sfm.height() + 1;
            int by = row2Y - sfm.ascent();
            QRect br(x, by, bw, bh);
            p->setPen(Qt::NoPen);
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setBrush(bgColor);
            p->drawRoundedRect(br, 3, 3);
            p->setRenderHint(QPainter::Antialiasing, false);
            p->setPen(fgColor);
            p->drawText(br, Qt::AlignCenter, text);
            x += bw + 5;
        };

        if (e.entryKind == SourceEntry::SavedSource) {
            // Kind label as text
            if (!e.kindLabel.isEmpty()) {
                p->setPen(theme.textMuted);
                p->drawText(x, row2Y, e.kindLabel);
                x += sfm.horizontalAdvance(e.kindLabel) + 6;
            }

            // PID badge
            if (!e.pid.isEmpty()) {
                drawBadge(QStringLiteral("PID ") + e.pid,
                          QColor(theme.textDim.red(), theme.textDim.green(), theme.textDim.blue(), 25),
                          theme.textDim);
            }

            // Architecture badge
            if (!e.arch.isEmpty()) {
                QColor ac = (e.arch == QStringLiteral("x64"))
                    ? theme.syntaxKeyword : QColor(140, 100, 180);
                drawBadge(e.arch,
                          QColor(ac.red(), ac.green(), ac.blue(), 35),
                          ac);
            }

            // Active label
            if (e.isActive) {
                p->setPen(theme.indHoverSpan);
                p->drawText(x, row2Y, QStringLiteral("active"));
                x += sfm.horizontalAdvance(QStringLiteral("active")) + 6;
            }

            // Base address or file path (right-aligned on row 2)
            QString rightText;
            if (!e.filePath.isEmpty())
                rightText = e.filePath;
            else if (!e.baseAddress.isEmpty())
                rightText = e.baseAddress;

            if (!rightText.isEmpty()) {
                int maxW = r.right() - 10 - x - 4;
                if (maxW > 0) {
                    QString elided = sfm.elidedText(rightText, Qt::ElideMiddle, maxW);
                    int tw = sfm.horizontalAdvance(elided);
                    p->setPen(theme.textFaint);
                    p->drawText(r.right() - 10 - tw, row2Y, elided);
                }
            }
        }

        // Provider: kind + DLL name on row 2
        if (e.entryKind == SourceEntry::ProviderAction) {
            if (!e.kindLabel.isEmpty()) {
                p->setPen(theme.textMuted);
                p->drawText(x, row2Y, e.kindLabel);
                x += sfm.horizontalAdvance(e.kindLabel) + 6;
            }
            if (!e.dllFileName.isEmpty()) {
                p->setPen(theme.textFaint);
                p->drawText(x, row2Y, e.dllFileName);
            }
        }

        p->setFont(baseFont);
    }
};

// ── SourceChooserPopup ──

SourceChooserPopup::SourceChooserPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    // Title row
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(10, 6, 6, 2);
        row->setSpacing(4);
        m_titleLabel = new QLabel(QStringLiteral("Data Source"));
        QFont bold = font();
        bold.setBold(true);
        m_titleLabel->setFont(bold);
        row->addWidget(m_titleLabel);
        row->addStretch();

        auto* escBtn = new QToolButton;
        escBtn->setText(QStringLiteral("Esc"));
        escBtn->setAutoRaise(true);
        escBtn->setCursor(Qt::PointingHandCursor);
        escBtn->setFixedSize(32, 20);
        connect(escBtn, &QToolButton::clicked, this, &QFrame::hide);
        m_escBtn = escBtn;
        row->addWidget(escBtn);

        layout->addLayout(row);
    }

    // Filter edit
    m_filterEdit = new QLineEdit;
    m_filterEdit->setPlaceholderText(QStringLiteral("Filter sources..."));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFrame(false);
    m_filterEdit->setFixedHeight(30);
    layout->addWidget(m_filterEdit);

    // Separator
    m_separator = new QFrame;
    m_separator->setFrameShape(QFrame::HLine);
    m_separator->setFixedHeight(1);
    layout->addWidget(m_separator);

    // List view
    m_listView = new QListView;
    m_model = new QStringListModel(this);
    m_listView->setModel(m_model);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_listView->setMouseTracking(true);
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setUniformItemSizes(false);

    auto* delegate = new SourceChooserDelegate(this);
    m_listView->setItemDelegate(delegate);
    layout->addWidget(m_listView, 1);

    // Footer
    m_footerLabel = new QLabel;
    m_footerLabel->setAlignment(Qt::AlignCenter);
    m_footerLabel->setContentsMargins(0, 3, 0, 5);
    layout->addWidget(m_footerLabel);

    // Connections
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, &SourceChooserPopup::applyFilter);
    connect(m_listView, &QListView::clicked,
            this, [this](const QModelIndex& idx) { acceptIndex(idx.row()); });

    m_filterEdit->installEventFilter(this);
    m_listView->installEventFilter(this);
    m_listView->viewport()->installEventFilter(this);
}

void SourceChooserPopup::setFont(const QFont& font) {
    m_font = font;
    m_filterEdit->setFont(font);
    m_listView->setFont(font);

    QFont bold = font;
    bold.setBold(true);
    m_titleLabel->setFont(bold);

    QFont small = font;
    small.setPointSize(qMax(7, font.pointSize() - 2));
    m_footerLabel->setFont(small);
    m_escBtn->setFont(small);

    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    delegate->baseFont = font;
}

void SourceChooserPopup::applyTheme(const Theme& theme) {
    const QColor bg = theme.background;

    QPalette pal;
    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::WindowText, theme.text);
    pal.setColor(QPalette::Base, bg);
    pal.setColor(QPalette::Text, theme.text);
    pal.setColor(QPalette::Highlight, theme.selected);
    pal.setColor(QPalette::HighlightedText, theme.text);
    pal.setColor(QPalette::Dark, theme.border);
    setPalette(pal);

    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme.text.name()));
    m_filterEdit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: none; padding: 4px 10px; }")
        .arg(theme.backgroundAlt.name(), theme.text.name()));
    m_separator->setStyleSheet(
        QStringLiteral("background: %1; border: none;").arg(theme.border.name()));
    m_listView->setStyleSheet(QStringLiteral(
        "QListView { background: %1; border: none; }"
        "QListView::item { border: none; background: transparent; }")
        .arg(bg.name()));
    m_footerLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme.textFaint.name()));
    m_escBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: 3px;"
        " padding: 1px 4px; font-size: 9px; }"
        "QToolButton:hover { background: %3; color: %4; }")
        .arg(theme.textFaint.name(), theme.border.name(),
             theme.hover.name(), theme.text.name()));
}

void SourceChooserPopup::setSources(const QVector<SourceEntry>& entries) {
    m_allEntries = entries;
    m_cachedMaxNameLen = 0;
    for (const auto& e : entries) {
        if (e.entryKind != SourceEntry::SectionHeader)
            m_cachedMaxNameLen = qMax(m_cachedMaxNameLen, (int)e.displayName.size());
    }

    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    delegate->entries = &m_filteredEntries;
    delegate->matchPositions = &m_matchPositions;

    m_filterEdit->clear();
    applyFilter(QString());
}

void SourceChooserPopup::setLivenessResults(const QVector<bool>& alive) {
    bool changed = false;
    for (auto& e : m_allEntries) {
        if (e.savedIndex < 0 || e.savedIndex >= alive.size()) continue;
        bool stale = !alive[e.savedIndex];
        if (e.isStale != stale) { e.isStale = stale; changed = true; }
    }
    if (changed)
        applyFilter(m_filterEdit->text());
}

void SourceChooserPopup::applyFilter(const QString& text) {
    m_filteredEntries.clear();
    m_matchPositions.clear();

    if (text.isEmpty()) {
        m_filteredEntries = m_allEntries;
        m_matchPositions.resize(m_allEntries.size());
    } else {
        struct Scored { int srcIdx; int score; QVector<int> positions; };
        QVector<Scored> scored;
        for (int i = 0; i < m_allEntries.size(); i++) {
            const auto& e = m_allEntries[i];
            if (e.entryKind == SourceEntry::SectionHeader) continue;
            QString searchable = e.displayName;
            if (!e.kindLabel.isEmpty()) searchable += QChar(' ') + e.kindLabel;
            if (!e.pid.isEmpty()) searchable += QChar(' ') + e.pid;
            if (!e.dllFileName.isEmpty()) searchable += QChar(' ') + e.dllFileName;
            if (!e.filePath.isEmpty()) searchable += QChar(' ') + e.filePath;
            QVector<int> pos;
            int sc = fuzzyScore(text, searchable, &pos);
            if (sc > 0)
                scored.append({i, sc, pos});
        }
        std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            return a.score > b.score;
        });
        for (const auto& s : scored) {
            m_filteredEntries.append(m_allEntries[s.srcIdx]);
            m_matchPositions.append(s.positions);
        }
    }

    QStringList displayStrings;
    displayStrings.reserve(m_filteredEntries.size());
    for (const auto& e : m_filteredEntries)
        displayStrings.append(e.displayName);
    m_model->setStringList(displayStrings);

    // Footer
    int total = 0;
    for (const auto& e : m_allEntries)
        if (e.entryKind != SourceEntry::SectionHeader) total++;

    if (text.isEmpty()) {
        m_footerLabel->setText(
            QStringLiteral("\u2191\u2193 navigate  \u21B5 select  Esc close"));
    } else if (m_filteredEntries.isEmpty()) {
        m_footerLabel->setText(
            QStringLiteral("No matches for \"%1\"").arg(text));
    } else {
        m_footerLabel->setText(
            QStringLiteral("%1 of %2 sources").arg(m_filteredEntries.size()).arg(total));
    }

    int first = nextSelectableRow(0, 1);
    if (first >= 0)
        m_listView->setCurrentIndex(m_model->index(first));
}

void SourceChooserPopup::popup(const QPoint& globalPos) {
    QFontMetrics fm(m_font);
    int charW = fm.horizontalAdvance(QChar('M'));
    int popupW = qBound(360, (m_cachedMaxNameLen + 24) * charW, 560);

    QFont small = m_font;
    small.setPointSize(qMax(7, m_font.pointSize() - 2));
    QFontMetrics sfm(small);

    int singleRowH = fm.height() + 12;
    int cardRowH = fm.height() + sfm.height() + 14;
    int sectionH = fm.height() + 8;
    int contentH = 0;
    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    for (const auto& e : m_filteredEntries) {
        if (e.entryKind == SourceEntry::SectionHeader)
            contentH += sectionH;
        else if (delegate->hasSubline(e))
            contentH += cardRowH;
        else
            contentH += singleRowH;
    }
    int chromeH = 32 + 30 + 1 + 28;
    int popupH = qBound(140, contentH + chromeH + 4, 520);

    setFixedSize(popupW, popupH);

    QRect screen = QApplication::screenAt(globalPos)
        ? QApplication::screenAt(globalPos)->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    int x = qBound(screen.left(), globalPos.x(), screen.right() - popupW);
    int y = globalPos.y();
    if (y + popupH > screen.bottom())
        y = globalPos.y() - popupH;
    move(x, y);

    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
    m_filterEdit->selectAll();
}

void SourceChooserPopup::warmUp() {
    show();
    hide();
}

void SourceChooserPopup::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    QPainter p(this);
    QColor bd = palette().color(QPalette::Dark);
    int w = width(), h = height();
    p.fillRect(0, 0, w, 1, bd);
    p.fillRect(0, h - 1, w, 1, bd);
    p.fillRect(0, 0, 1, h, bd);
    p.fillRect(w - 1, 0, 1, h, bd);
}

void SourceChooserPopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    emit dismissed();
}

bool SourceChooserPopup::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            hide();
            return true;
        }

        if (obj == m_filterEdit) {
            if (ke->key() == Qt::Key_Down) {
                m_listView->setFocus();
                int first = nextSelectableRow(0, 1);
                if (first >= 0)
                    m_listView->setCurrentIndex(m_model->index(first));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
        }

        if (obj == m_listView) {
            if (ke->key() == Qt::Key_Up) {
                int cur = m_listView->currentIndex().row();
                int prev = nextSelectableRow(cur - 1, -1);
                if (prev >= 0)
                    m_listView->setCurrentIndex(m_model->index(prev));
                else
                    m_filterEdit->setFocus();
                return true;
            }
            if (ke->key() == Qt::Key_Down) {
                int cur = m_listView->currentIndex().row();
                int next = nextSelectableRow(cur + 1, 1);
                if (next >= 0)
                    m_listView->setCurrentIndex(m_model->index(next));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
            if (ke->key() == Qt::Key_Backspace) {
                QString t = m_filterEdit->text();
                if (!t.isEmpty()) {
                    m_filterEdit->setText(t.left(t.size() - 1));
                    m_filterEdit->setFocus();
                }
                return true;
            }
            QString text = ke->text();
            if (!text.isEmpty() && text[0].isPrint()) {
                m_filterEdit->setText(m_filterEdit->text() + text);
                m_filterEdit->setFocus();
                return true;
            }
        }
    }

    return QFrame::eventFilter(obj, event);
}

void SourceChooserPopup::acceptCurrent() {
    QModelIndex idx = m_listView->currentIndex();
    if (idx.isValid())
        acceptIndex(idx.row());
}

void SourceChooserPopup::acceptIndex(int row) {
    if (row < 0 || row >= m_filteredEntries.size()) return;
    const auto& e = m_filteredEntries[row];
    if (!e.enabled || e.entryKind == SourceEntry::SectionHeader) return;

    if (e.entryKind == SourceEntry::SavedSource && e.isActive) {
        hide();
        return;
    }

    hide();
    if (e.entryKind == SourceEntry::ClearAction)
        emit clearRequested();
    else if (e.entryKind == SourceEntry::ProviderAction)
        emit providerSelected(e.providerIdentifier);
    else if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0)
        emit sourceSelected(e.savedIndex);
}

int SourceChooserPopup::nextSelectableRow(int from, int direction) const {
    for (int i = from; i >= 0 && i < m_filteredEntries.size(); i += direction) {
        const auto& e = m_filteredEntries[i];
        if (e.entryKind != SourceEntry::SectionHeader && e.enabled)
            return i;
    }
    return -1;
}

} // namespace rcx
