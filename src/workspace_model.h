#pragma once
#include "core.h"
#include "themes/theme.h"
#include <QIcon>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <algorithm>

namespace rcx {

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
        return QStringLiteral("%1 (%2) \u2014 %3")
            .arg(nameOf(node), node->resolvedClassKeyword(),
                 QString::number(node->enumMembers.size()));
    }
    QVector<int> members = tree->childrenOf(node->id);
    int vc = 0;
    for (int mi : members)
        if (!isHexPad(tree->nodes[mi].kind)) ++vc;
    return QStringLiteral("%1 (%2) \u2014 %3")
        .arg(nameOf(node), node->resolvedClassKeyword(),
             QString::number(vc));
}

// Build a new item for a type entry.
inline QStandardItem* makeTypeItem(const Node* node, const NodeTree* tree,
                                   void* subPtr) {
    bool isEnum = node->resolvedClassKeyword() == QStringLiteral("enum");
    auto* item = new QStandardItem(
        QIcon(isEnum ? ":/vsicons/symbol-enum.svg"
                     : ":/vsicons/symbol-structure.svg"),
        typeDisplayString(node, tree));
    item->setData(QVariant::fromValue(subPtr), Qt::UserRole);
    item->setData(QVariant::fromValue(node->id), Qt::UserRole + 1);

    if (!isEnum)
        buildStructChildren(item, tree, node->id, subPtr);

    return item;
}

// Full rebuild — used by benchmarks and first build.
inline void buildProjectExplorer(QStandardItemModel* model,
                                 const QVector<TabInfo>& tabs) {
    model->clear();
    model->setHorizontalHeaderLabels({QStringLiteral("Name")});

    struct Entry { const Node* node; void* subPtr; const NodeTree* tree; };
    QVector<Entry> types, enums;
    for (const auto& tab : tabs) {
        QVector<int> topLevel = tab.tree->childrenOf(0);
        for (int idx : topLevel) {
            const Node& n = tab.tree->nodes[idx];
            if (n.kind != NodeKind::Struct) continue;
            if (n.resolvedClassKeyword() == QStringLiteral("enum"))
                enums.append({&n, tab.subPtr, tab.tree});
            else
                types.append({&n, tab.subPtr, tab.tree});
        }
    }

    for (const auto& e : types)
        model->appendRow(makeTypeItem(e.node, e.tree, e.subPtr));
    for (const auto& e : enums)
        model->appendRow(makeTypeItem(e.node, e.tree, e.subPtr));
}

// Incremental sync — preserves tree expansion/scroll state.
inline void syncProjectExplorer(QStandardItemModel* model,
                                const QVector<TabInfo>& tabs) {
    // First call — full build
    if (model->rowCount() == 0 && !tabs.isEmpty()) {
        buildProjectExplorer(model, tabs);
        return;
    }

    // Collect desired entries
    struct Entry { uint64_t id; const Node* node; void* subPtr; const NodeTree* tree; bool isEnum; };
    QVector<Entry> desired;
    for (const auto& tab : tabs) {
        QVector<int> topLevel = tab.tree->childrenOf(0);
        for (int idx : topLevel) {
            const Node& n = tab.tree->nodes[idx];
            if (n.kind != NodeKind::Struct) continue;
            bool ie = n.resolvedClassKeyword() == QStringLiteral("enum");
            desired.append({n.id, &n, tab.subPtr, tab.tree, ie});
        }
    }

    QHash<uint64_t, int> desiredMap;
    desiredMap.reserve(desired.size());
    for (int i = 0; i < desired.size(); ++i)
        desiredMap[desired[i].id] = i;

    // Remove stale items (backwards)
    for (int i = model->rowCount() - 1; i >= 0; --i) {
        uint64_t id = model->item(i)->data(Qt::UserRole + 1).toULongLong();
        if (!desiredMap.contains(id))
            model->removeRow(i);
    }

    // Update existing items
    QSet<uint64_t> existing;
    for (int i = 0; i < model->rowCount(); ++i) {
        auto* item = model->item(i);
        uint64_t id = item->data(Qt::UserRole + 1).toULongLong();
        existing.insert(id);
        auto dit = desiredMap.find(id);
        if (dit == desiredMap.end()) continue;
        const Entry& e = desired[*dit];

        QString display = typeDisplayString(e.node, e.tree);
        if (item->text() != display)
            item->setText(display);
        item->setData(QVariant::fromValue(e.subPtr), Qt::UserRole);

        // Refresh children for structs
        if (!e.isEnum)
            buildStructChildren(item, e.tree, id, e.subPtr);
    }

    // Add new items
    for (const auto& e : desired) {
        if (existing.contains(e.id)) continue;
        model->appendRow(makeTypeItem(e.node, e.tree, e.subPtr));
    }

    if (model->horizontalHeaderItem(0) == nullptr)
        model->setHorizontalHeaderLabels({QStringLiteral("Name")});
}

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
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        int pad = index.parent().isValid() ? 6 : 10;
        s.setHeight(option.fontMetrics.height() + pad);
        return s;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();  // we draw icon manually
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

        // Custom background for selection/hover
        if (opt.state & QStyle::State_Selected) {
            painter->fillRect(opt.rect, m_selected);
            // Left accent bar
            painter->fillRect(QRect(opt.rect.x(), opt.rect.y(), 1, opt.rect.height()), m_accent);
        } else if (opt.state & QStyle::State_MouseOver) {
            painter->fillRect(opt.rect, m_hover);
        }

        bool isChild = index.parent().isValid();
        QString fullText = index.data(Qt::DisplayRole).toString();
        QRect textRect = opt.rect.adjusted(4, 0, -4, 0);

        // Draw icon for top-level items
        if (!isChild) {
            QVariant iconVar = index.data(Qt::DecorationRole);
            if (iconVar.isValid()) {
                QIcon icon = iconVar.value<QIcon>();
                int iconSz = opt.fontMetrics.height();
                int iconY = textRect.y() + (textRect.height() - iconSz) / 2;
                icon.paint(painter, textRect.x(), iconY, iconSz, iconSz);
                textRect.setLeft(textRect.left() + iconSz + 4);
            }
        }

        painter->setFont(opt.font);

        if (!isChild) {
            // Top-level: "StructName (class) — 3"
            int dashPos = fullText.indexOf(QChar(0x2014));
            int parenPos = dashPos > 0 ? fullText.lastIndexOf(QStringLiteral(" ("), dashPos) : -1;
            if (parenPos > 0) {
                QString name = fullText.left(parenPos);
                QString meta = fullText.mid(parenPos);

                painter->setPen(m_text);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, name);
                int nameW = opt.fontMetrics.horizontalAdvance(name);

                QRect metaRect = textRect;
                metaRect.setLeft(textRect.left() + nameW);
                painter->setPen(m_textMuted);
                painter->drawText(metaRect, Qt::AlignLeft | Qt::AlignVCenter, meta);
            } else {
                painter->setPen(m_text);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, fullText);
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
};

} // namespace rcx
