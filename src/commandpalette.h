#pragma once
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QListView>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QSettings>
#include <QFontMetrics>
#include "themes/thememanager.h"

namespace rcx {

// Frameless popup that walks every QAction reachable from the menu bar
// and lets the user fuzzy-search by joined "menu > submenu > item" path.
// Triggering the selected entry calls QAction::trigger() — we don't
// reimplement any behavior, just surface what's already wired up.
//
// Cross-platform: the menu bar exists on every supported platform
// (we use setNativeMenuBar(false) on Linux + custom title bar on Win),
// so QAction enumeration is the same everywhere. The popup itself is
// pure Qt — no platform-specific code paths.
class CommandPalette : public QDialog {
public:
    struct Entry {
        QAction* action;
        QString  path;       // "File > Recent Files > foo.rcx"
        QString  shortcut;   // "Ctrl+O"
        bool     enabled;
    };

    explicit CommandPalette(QMenuBar* menuBar, QWidget* parent = nullptr)
        : QDialog(parent) {
        setWindowFlags(Qt::Popup | Qt::FramelessWindowHint
                       | Qt::NoDropShadowWindowHint);
        setAttribute(Qt::WA_DeleteOnClose, false);
        resize(560, 380);

        const auto& t = ThemeManager::instance().current();
        QSettings s("Reclass", "Reclass");
        QFont font(s.value("font", "JetBrains Mono").toString(), 10);
        font.setFixedPitch(true);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(0);

        m_input = new QLineEdit;
        m_input->setFont(font);
        m_input->setPlaceholderText(QStringLiteral("Type a command..."));
        m_input->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: none;"
            " padding: 8px 12px; font-size: 11pt; }")
            .arg(t.backgroundAlt.name(), t.text.name()));
        layout->addWidget(m_input);

        m_list = new QListView;
        m_list->setFont(font);
        m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setUniformItemSizes(true);
        m_list->setStyleSheet(QStringLiteral(
            "QListView { background: %1; color: %2; border: none;"
            " padding: 0px; outline: 0; }"
            "QListView::item { padding: 4px 12px; }"
            "QListView::item:hover { background: %3; }"
            "QListView::item:selected { background: %4; color: %5; }")
            .arg(t.background.name(), t.text.name(),
                 t.hover.name(), t.selected.name(), t.text.name()));
        layout->addWidget(m_list, /*stretch=*/1);

        m_model = new QStandardItemModel(this);
        m_proxy = new QSortFilterProxyModel(this);
        m_proxy->setSourceModel(m_model);
        m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_list->setModel(m_proxy);

        // Background frame so the 1px border is consistent
        setStyleSheet(QStringLiteral(
            "QDialog { background: %1; }").arg(t.border.name()));

        if (menuBar) populateFromMenuBar(menuBar);
        applyFilter(QString());

        connect(m_input, &QLineEdit::textChanged,
                this, &CommandPalette::applyFilter);
        connect(m_input, &QLineEdit::returnPressed,
                this, &CommandPalette::activateCurrent);
        connect(m_list, &QAbstractItemView::activated,
                this, &CommandPalette::activateIndex);
        connect(m_list, &QAbstractItemView::clicked,
                this, &CommandPalette::activateIndex);

        m_input->installEventFilter(this);
        m_input->setFocus();
    }

    // Visible to tests: re-runs the enumeration.
    void populateFromMenuBar(QMenuBar* menuBar) {
        m_entries.clear();
        m_model->clear();
        QSet<QMenu*> seen;
        for (QAction* topAct : menuBar->actions()) {
            if (auto* sub = topAct->menu())
                walkMenu(sub, topAct->text().remove('&'), seen);
        }
        rebuildModel();
    }

    // Visible to tests: list entries currently in the palette.
    const QVector<Entry>& entries() const { return m_entries; }

    // Visible to tests: trigger entry N as if user pressed Enter on it.
    bool activateEntry(int idx) {
        if (idx < 0 || idx >= m_entries.size()) return false;
        if (!m_entries[idx].enabled) return false;
        QAction* a = m_entries[idx].action;
        if (!a) return false;
        a->trigger();
        accept();
        return true;
    }

    // Static fuzzy match — exposed for unit testing without UI.
    // Returns >0 score on hit, 0 on miss. Higher = better.
    // Bonuses: contiguous run, word-start match, leading match.
    static int fuzzyScore(const QString& needle, const QString& haystack) {
        if (needle.isEmpty()) return 1;
        int ni = 0, hi = 0;
        int score = 0;
        int run = 0;
        bool prevSep = true;  // start of string counts as separator
        while (ni < needle.size() && hi < haystack.size()) {
            QChar n = needle[ni].toLower();
            QChar h = haystack[hi].toLower();
            if (n == h) {
                int bonus = 1;
                if (run > 0)   bonus += 2;     // contiguous match
                if (prevSep)   bonus += 3;     // word-start
                if (hi == ni)  bonus += 1;     // leading-prefix bias
                score += bonus;
                run++;
                ni++;
            } else {
                run = 0;
            }
            prevSep = (h == ' ' || h == '>' || h == '/' || h == '_' || h == '-');
            hi++;
        }
        if (ni < needle.size()) return 0;     // not all needle matched
        return score;
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj == m_input && ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            switch (ke->key()) {
            case Qt::Key_Down:
            case Qt::Key_Up:
            case Qt::Key_PageDown:
            case Qt::Key_PageUp: {
                // Forward navigation keys to the list
                QApplication::sendEvent(m_list, ke);
                return true;
            }
            case Qt::Key_Escape:
                reject();
                return true;
            default:
                break;
            }
        }
        return QDialog::eventFilter(obj, ev);
    }

private:
    void applyFilter(const QString& text) {
        m_filter = text.trimmed();
        rebuildModel();
    }

    void activateCurrent() {
        QModelIndex idx = m_list->currentIndex();
        if (!idx.isValid() && m_proxy->rowCount() > 0)
            idx = m_proxy->index(0, 0);
        if (idx.isValid()) activateIndex(idx);
    }

    void activateIndex(const QModelIndex& proxyIdx) {
        if (!proxyIdx.isValid()) return;
        QModelIndex src = m_proxy->mapToSource(proxyIdx);
        int row = src.row();
        int entryIdx = m_model->item(row)->data(Qt::UserRole).toInt();
        activateEntry(entryIdx);
    }

private:
    void walkMenu(QMenu* menu, const QString& path, QSet<QMenu*>& seen) {
        if (!menu || seen.contains(menu)) return;
        seen.insert(menu);
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) continue;
            QString label = a->text();
            // Trim shortcut hint if it was embedded with \t
            QString cleanLabel = label;
            int tabPos = cleanLabel.indexOf('\t');
            if (tabPos >= 0) cleanLabel = cleanLabel.left(tabPos);
            cleanLabel.remove('&');
            if (cleanLabel.trimmed().isEmpty()) continue;
            QString fullPath = path + QStringLiteral(" > ") + cleanLabel;
            if (auto* sub = a->menu()) {
                walkMenu(sub, fullPath, seen);
            } else {
                Entry e;
                e.action = a;
                e.path = fullPath;
                e.shortcut = a->shortcut().isEmpty()
                    ? QString() : a->shortcut().toString(QKeySequence::NativeText);
                e.enabled = a->isEnabled();
                m_entries.append(e);
            }
        }
    }

    void rebuildModel() {
        m_model->clear();
        struct Scored { int score; int idx; };
        QVector<Scored> hits;
        for (int i = 0; i < m_entries.size(); ++i) {
            int s = fuzzyScore(m_filter, m_entries[i].path);
            if (s > 0) hits.push_back({s, i});
        }
        std::stable_sort(hits.begin(), hits.end(),
            [](const Scored& a, const Scored& b) { return a.score > b.score; });

        const auto& t = ThemeManager::instance().current();
        for (const auto& h : hits) {
            const Entry& e = m_entries[h.idx];
            QString label = e.path;
            if (!e.shortcut.isEmpty())
                label += QStringLiteral("    [") + e.shortcut + QStringLiteral("]");
            auto* item = new QStandardItem(label);
            item->setData(h.idx, Qt::UserRole);
            if (!e.enabled) {
                item->setForeground(QBrush(t.textMuted));
                item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            }
            m_model->appendRow(item);
        }
        if (m_model->rowCount() > 0)
            m_list->setCurrentIndex(m_proxy->index(0, 0));
    }

    QLineEdit*             m_input  = nullptr;
    QListView*             m_list   = nullptr;
    QStandardItemModel*    m_model  = nullptr;
    QSortFilterProxyModel* m_proxy  = nullptr;
    QString                m_filter;
    QVector<Entry>         m_entries;
};

} // namespace rcx
