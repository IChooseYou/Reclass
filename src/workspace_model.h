#pragma once
#include "core.h"
#include <QIcon>
#include <QStandardItemModel>
#include <QStandardItem>
#include <algorithm>

namespace rcx {

struct TabInfo {
    const NodeTree* tree;
    QString         name;
    void*           subPtr;   // QDockWidget* as void*
};

// Sentinel value stored in UserRole+1 to mark the Project group node.
static constexpr uint64_t kGroupSentinel = ~uint64_t(0);

inline void buildProjectExplorer(QStandardItemModel* model,
                                 const QVector<TabInfo>& tabs) {
    model->clear();
    model->setHorizontalHeaderLabels({QStringLiteral("Name")});

    // Single "Project" root with folder icon
    void* firstSub = tabs.isEmpty() ? nullptr : tabs[0].subPtr;
    auto* projectItem = new QStandardItem(QIcon(":/vsicons/folder.svg"),
                                          QStringLiteral("Project"));
    projectItem->setData(QVariant::fromValue(firstSub), Qt::UserRole);
    projectItem->setData(QVariant::fromValue(kGroupSentinel), Qt::UserRole + 1);

    // Collect all top-level structs/enums across all tabs
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

    auto nameOf = [](const Node* n) {
        return n->structTypeName.isEmpty() ? n->name : n->structTypeName;
    };

    // Helper: is a Hex padding node
    auto isHexPad = [](NodeKind k) {
        return k == NodeKind::Hex8 || k == NodeKind::Hex16
            || k == NodeKind::Hex32 || k == NodeKind::Hex64;
    };

    // Helper: type display string for a member node
    auto memberTypeName = [](const Node& m) -> QString {
        if (m.kind == NodeKind::Struct) {
            QString stn = m.structTypeName.isEmpty() ? m.resolvedClassKeyword()
                                                     : m.structTypeName;
            return stn;
        }
        return QString::fromLatin1(kindToString(m.kind));
    };

    // Sort structs by visible children count descending (most fields first)
    auto countVisible = [&](const Entry& e) {
        int n = 0;
        for (int idx : e.tree->childrenOf(e.node->id))
            if (!isHexPad(e.tree->nodes[idx].kind)) ++n;
        return n;
    };
    auto cmpChildren = [&](const Entry& a, const Entry& b) {
        int ca = countVisible(a);
        int cb = countVisible(b);
        if (ca != cb) return ca > cb;
        return nameOf(a.node).compare(nameOf(b.node), Qt::CaseInsensitive) < 0;
    };
    std::sort(types.begin(), types.end(), cmpChildren);
    auto cmpName = [&](const Entry& a, const Entry& b) {
        return nameOf(a.node).compare(nameOf(b.node), Qt::CaseInsensitive) < 0;
    };
    std::sort(enums.begin(), enums.end(), cmpName);

    for (const auto& e : types) {
        QVector<int> members = e.tree->childrenOf(e.node->id);

        // Count non-hex members for display
        int visibleCount = 0;
        for (int mi : members)
            if (!isHexPad(e.tree->nodes[mi].kind)) ++visibleCount;

        QString display = QStringLiteral("%1 (%2) \u2014 %3")
            .arg(nameOf(e.node), e.node->resolvedClassKeyword(),
                 QString::number(visibleCount));
        auto* item = new QStandardItem(
            QIcon(":/vsicons/symbol-structure.svg"), display);
        item->setData(QVariant::fromValue(e.subPtr), Qt::UserRole);
        item->setData(QVariant::fromValue(e.node->id), Qt::UserRole + 1);

        // Add child rows sorted by offset (skip Hex padding)
        std::sort(members.begin(), members.end(), [&](int a, int b) {
            return e.tree->nodes[a].offset < e.tree->nodes[b].offset;
        });
        for (int mi : members) {
            const Node& m = e.tree->nodes[mi];
            if (isHexPad(m.kind)) continue;
            QString childDisplay = QStringLiteral("%1 %2")
                .arg(memberTypeName(m), m.name);
            auto* childItem = new QStandardItem(childDisplay);
            childItem->setData(QVariant::fromValue(e.subPtr), Qt::UserRole);
            childItem->setData(QVariant::fromValue(m.id), Qt::UserRole + 1);
            item->appendRow(childItem);
        }

        projectItem->appendRow(item);
    }

    for (const auto& e : enums) {
        int count = e.node->enumMembers.size();
        QString display = QStringLiteral("%1 (%2) \u2014 %3")
            .arg(nameOf(e.node), e.node->resolvedClassKeyword(),
                 QString::number(count));
        auto* item = new QStandardItem(
            QIcon(":/vsicons/symbol-enum.svg"), display);
        item->setData(QVariant::fromValue(e.subPtr), Qt::UserRole);
        item->setData(QVariant::fromValue(e.node->id), Qt::UserRole + 1);
        projectItem->appendRow(item);
    }

    model->appendRow(projectItem);
}

} // namespace rcx
