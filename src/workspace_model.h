#pragma once
#include "core.h"
#include "themes/theme.h"
#include <QIcon>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStyledItemDelegate>
#include <QSortFilterProxyModel>
#include <QPainter>
#include <QApplication>
#include <algorithm>

namespace rcx {

// ── Data roles ──
// Qt::UserRole + 0  → void* subPtr (QDockWidget*)
// Qt::UserRole + 1  → uint64_t id (node ID)
// Qt::UserRole + 2  → bool isEnum
// Qt::UserRole + 3  → bool isViewed (currently in a tab)
// Qt::UserRole + 4  → bool isPinned
// Qt::UserRole + 5  → QString sectionLabel (non-empty = section header item)
// Qt::UserRole + 6  → bool isDirty (document modified)

static constexpr int RoleSectionHeader = Qt::UserRole + 5;
static constexpr int RoleDirty         = Qt::UserRole + 6;

struct TabInfo {
    const NodeTree* tree;
    QString         name;
    void*           subPtr;   // QDockWidget* as void*
};

// Helper: is a Hex padding node
inline bool isHexPad(NodeKind k) {
    return k == NodeKind::Hex8 || k == NodeKind::Hex16
        || k == NodeKind::Hex32 || k == NodeKind::Hex64;
}

// Build child rows for a struct item.
inline void buildStructChildren(QStandardItem* item,
                                const NodeTree* tree, uint64_t structId,
                                void* subPtr) {
    item->removeRows(0, item->rowCount());

    QVector<int> members = tree->childrenOf(structId);
    std::sort(members.begin(), members.end(), [&](int a, int b) {
        return tree->nodes[a].offset < tree->nodes[b].offset;
    });

    auto memberTypeName = [](const Node& m) -> QString {
        if (m.kind == NodeKind::Struct) {
            return m.structTypeName.isEmpty() ? m.resolvedClassKeyword()
                                              : m.structTypeName;
        }
        return QString::fromLatin1(kindToString(m.kind));
    };

    for (int mi : members) {
        if (mi < 0 || mi >= tree->nodes.size()) continue;
        const Node& m = tree->nodes[mi];
        if (isHexPad(m.kind)) continue;
        QString childDisplay = QStringLiteral("%1 %2")
            .arg(memberTypeName(m), m.name);
        auto* childItem = new QStandardItem(childDisplay);
        childItem->setData(QVariant::fromValue(subPtr), Qt::UserRole);
        childItem->setData(QVariant::fromValue(m.id), Qt::UserRole + 1);
        item->appendRow(childItem);
    }
}

// Helper to build display string for a type entry.
inline QString typeDisplayString(const Node* node, const NodeTree* tree) {
    auto nameOf = [](const Node* n) {
        return n->structTypeName.isEmpty() ? n->name : n->structTypeName;
    };
    if (node->resolvedClassKeyword() == QStringLiteral("enum")) {
        return QStringLiteral("%1 \u2014 %2")
            .arg(nameOf(node),
                 QString::number(node->enumMembers.size()));
    }
    QVector<int> members = tree->childrenOf(node->id);
    int vc = 0;
    for (int mi : members)
        if (!isHexPad(tree->nodes[mi].kind)) ++vc;
    return QStringLiteral("%1 \u2014 %2")
        .arg(nameOf(node), QString::number(vc));
}

// Build a new item for a type entry.
inline QStandardItem* makeTypeItem(const Node* node, const NodeTree* tree,
                                   void* subPtr) {
    static const QIcon enumIcon(":/vsicons/symbol-enum.svg");
    static const QIcon structIcon(":/vsicons/symbol-structure.svg");
    bool isEnum = node->resolvedClassKeyword() == QStringLiteral("enum");
    auto* item = new QStandardItem(
        isEnum ? enumIcon : structIcon,
        typeDisplayString(node, tree));
    item->setData(QVariant::fromValue(subPtr), Qt::UserRole);
    item->setData(QVariant::fromValue(node->id), Qt::UserRole + 1);
    item->setData(isEnum, Qt::UserRole + 2);

    if (!isEnum)
        buildStructChildren(item, tree, node->id, subPtr);

    return item;
}

// Create a non-interactive section header item (PINNED, RECENT, ALL TYPES).
inline QStandardItem* makeSectionItem(const QString& label) {
    auto* item = new QStandardItem(label);
    item->setData(label, RoleSectionHeader);
    item->setFlags(Qt::ItemIsEnabled);
    return item;
}

// Full rebuild with two sections: PINNED (if any) + ALL TYPES.
inline void buildProjectExplorer(QStandardItemModel* model,
                                 const QVector<TabInfo>& tabs,
                                 const QSet<uint64_t>& pinnedIds = {}) {
    model->clear();
    model->setHorizontalHeaderLabels({QStringLiteral("Name")});

    struct Entry { const Node* node; void* subPtr; const NodeTree* tree; };
    QVector<Entry> allEntries;

    for (const auto& tab : tabs) {
        QVector<int> topLevel = tab.tree->childrenOf(0);
        for (int idx : topLevel) {
            const Node& n = tab.tree->nodes[idx];
            if (n.kind != NodeKind::Struct) continue;
            allEntries.push_back(Entry{&n, tab.subPtr, tab.tree});
        }
    }

    // ── PINNED section (only if any pinned) ──
    QVector<const Entry*> pinned;
    for (const auto& e : allEntries)
        if (pinnedIds.contains(e.node->id))
            pinned.append(&e);
    if (!pinned.isEmpty()) {
        model->appendRow(makeSectionItem(QStringLiteral("PINNED")));
        for (const auto* e : pinned)
            model->appendRow(makeTypeItem(e->node, e->tree, e->subPtr));
    }

    // ── ALL TYPES section ──
    QVector<Entry> types, enums;
    for (const auto& e : allEntries) {
        if (e.node->resolvedClassKeyword() == QStringLiteral("enum"))
            enums.push_back(e);
        else
            types.push_back(e);
    }
    model->appendRow(makeSectionItem(QStringLiteral("ALL TYPES")));
    for (const auto& e : types)
        model->appendRow(makeTypeItem(e.node, e.tree, e.subPtr));
    for (const auto& e : enums)
        model->appendRow(makeTypeItem(e.node, e.tree, e.subPtr));
}

// Full rebuild (debounced at 50ms).
inline void syncProjectExplorer(QStandardItemModel* model,
                                const QVector<TabInfo>& tabs,
                                const QSet<uint64_t>& pinnedIds = {}) {
    buildProjectExplorer(model, tabs, pinnedIds);
}

// ── Proxy model that hides section headers when a filter is active ──

class WorkspaceProxyModel : public QSortFilterProxyModel {
    bool m_hasFilter = false;
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setHasFilter(bool v) {
        if (m_hasFilter != v) {
            m_hasFilter = v;
            invalidateFilter();
        }
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        bool isSection = !idx.data(RoleSectionHeader).toString().isEmpty();
        if (m_hasFilter) {
            if (isSection) return false;
            return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
        }
        if (isSection) return true;
        return QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent);
    }
};

// ── Custom delegate for rich workspace tree rendering ──

class WorkspaceDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setThemeColors(const Theme& t) {
        m_text      = t.text;
        m_textDim   = t.textDim;
        m_textMuted = t.textMuted;
        m_syntaxType = t.syntaxType;
        m_hover     = t.hover;
        m_selected  = t.selected;
        m_accent    = t.borderFocused; // left accent bar
        m_bg        = t.background;
        m_badgeBg   = t.backgroundAlt;
        m_badgeText = t.textDim;
        m_surface   = t.surface;       // count pill bg (darker than badgeBg)
        m_border    = t.border;
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        // Section headers get extra vertical space
        if (!index.data(RoleSectionHeader).toString().isEmpty()) {
            QSize s = QStyledItemDelegate::sizeHint(option, index);
            s.setHeight(option.fontMetrics.height() + 16);
            return s;
        }
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        int pad = index.parent().isValid() ? 6 : 10;
        s.setHeight(option.fontMetrics.height() + pad);
        return s;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        // ── Section header rendering ──
        QString sectionLabel = index.data(RoleSectionHeader).toString();
        if (!sectionLabel.isEmpty()) {
            painter->fillRect(option.rect, m_bg);

            QFont sf = option.font;
            sf.setPointSizeF(sf.pointSizeF() * 0.67);
            sf.setBold(false);
            sf.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
            painter->setFont(sf);
            QFontMetrics sfm(sf);

            QRect textRect = option.rect.adjusted(4, 0, -4, 0);
            int textW = sfm.horizontalAdvance(sectionLabel);
            int textY = textRect.y() + (textRect.height() + sfm.ascent() - sfm.descent()) / 2;

            painter->setPen(m_textMuted);
            painter->drawText(textRect.x(), textY, sectionLabel);

            // Hairline extending right from label
            int lineY = textRect.y() + textRect.height() / 2;
            int lineStart = textRect.x() + textW + 8;
            if (lineStart < textRect.right()) {
                QPen hp(m_border);
                hp.setWidthF(0.5);
                painter->setPen(hp);
                painter->drawLine(lineStart, lineY, textRect.right(), lineY);
            }

            painter->restore();
            return;
        }

        // ── Normal item — keep existing drawControl flow (safe, no global side effects) ──
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();  // we draw icon manually
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

        // Custom background for selection/hover
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, m_selected);
            // Left accent bar (2px, inset 4px top/bottom)
            painter->fillRect(QRect(opt.rect.x(), opt.rect.y() + 4, 2, opt.rect.height() - 8), m_accent);
        } else if (opt.state & QStyle::State_MouseOver) {
            painter->fillRect(opt.rect, m_hover);
        }

        bool isChild = index.parent().isValid();
        QString fullText = index.data(Qt::DisplayRole).toString();
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);

        // Letter badge (S/E for top-level, F for children)
        {
            QChar letter = 'F';
            if (!isChild) {
                bool isEnum = index.data(Qt::UserRole + 2).toBool();
                letter = isEnum ? 'E' : 'S';
            }
            int sz = opt.fontMetrics.height();
            int y = textRect.y() + (textRect.height() - sz) / 2;
            QRect badge(textRect.x(), y, sz, sz);
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setRenderHint(QPainter::TextAntialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_badgeBg);
            painter->drawRoundedRect(badge, 3, 3);
            QColor letterCol = m_badgeText;
            if (!isChild && !index.data(Qt::UserRole + 3).toBool())
                letterCol.setAlpha(100);
            painter->setPen(letterCol);
            QFont bf = opt.font;
            bf.setBold(true);
            painter->setFont(bf);
            painter->drawText(badge, Qt::AlignCenter, letter);
            painter->setRenderHint(QPainter::Antialiasing, false);
            textRect.setLeft(textRect.left() + sz + 4);
        }

        painter->setFont(opt.font);

        if (!isChild) {
            // Top-level: "StructName — 3" → name left, count pill right
            int dashPos = fullText.indexOf(QChar(0x2014));
            QString name = (dashPos > 1) ? fullText.left(dashPos - 1) : fullText;
            QString count = (dashPos > 1) ? fullText.mid(dashPos + 2).trimmed() : QString();

            bool pinned = index.data(Qt::UserRole + 4).toBool();

            // Reserve right side for pin icon + count pill
            int rightEdge = textRect.right();
            if (!count.isEmpty()) {
                int cw = opt.fontMetrics.horizontalAdvance(count) + 10;
                int ch = opt.fontMetrics.height();
                int cy = textRect.y() + (textRect.height() - ch) / 2;
                QRect pill(rightEdge - cw, cy, cw, ch);
                rightEdge = pill.left() - 2;

                painter->setPen(Qt::NoPen);
                painter->setBrush(m_surface);
                painter->drawRect(pill);
                painter->setPen(m_textMuted);
                painter->drawText(pill, Qt::AlignCenter, count);
            }
            if (pinned) {
                static const QIcon pinIcon(":/vsicons/pin.svg");
                int isz = opt.fontMetrics.height() - 2;
                int iy = textRect.y() + (textRect.height() - isz) / 2;
                QRect pinRect(rightEdge - isz, iy, isz, isz);
                pinIcon.paint(painter, pinRect);
                rightEdge = pinRect.left() - 2;
            }

            // Draw name clipped before right-side elements
            if (rightEdge > textRect.left() + 4) {
                QRect nameRect = textRect;
                nameRect.setRight(rightEdge);
                QString elided = opt.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width());
                painter->setPen(m_text);
                painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elided);
            }
        } else {
            // Child: "TypeName fieldName"
            int spacePos = fullText.indexOf(' ');
            if (spacePos > 0) {
                QString typeName = fullText.left(spacePos);
                QString fieldName = fullText.mid(spacePos);

                painter->setPen(m_syntaxType);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, typeName);
                int typeW = opt.fontMetrics.horizontalAdvance(typeName);

                QRect fieldRect = textRect;
                fieldRect.setLeft(textRect.left() + typeW);
                painter->setPen(m_textDim);
                painter->drawText(fieldRect, Qt::AlignLeft | Qt::AlignVCenter, fieldName);
            } else {
                painter->setPen(m_textDim);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, fullText);
            }
        }

        painter->restore();
    }

private:
    QColor m_text, m_textDim, m_textMuted, m_syntaxType;
    QColor m_hover, m_selected, m_accent, m_bg;
    QColor m_badgeBg, m_badgeText, m_surface;
    QColor m_border;
};

} // namespace rcx
