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
    void*           subPtr;   // QMdiSubWindow* as void*
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
    QVector<std::pair<const Node*, void*>> types, enums;
    for (const auto& tab : tabs) {
        QVector<int> topLevel = tab.tree->childrenOf(0);
        for (int idx : topLevel) {
            const Node& n = tab.tree->nodes[idx];
            if (n.kind != NodeKind::Struct) continue;
            if (n.resolvedClassKeyword() == QStringLiteral("enum"))
                enums.append({&n, tab.subPtr});
            else
                types.append({&n, tab.subPtr});
        }
    }

    auto nameOf = [](const Node* n) {
        return n->structTypeName.isEmpty() ? n->name : n->structTypeName;
    };
    auto cmpName = [&](const std::pair<const Node*, void*>& a,
                       const std::pair<const Node*, void*>& b) {
        return nameOf(a.first).compare(nameOf(b.first), Qt::CaseInsensitive) < 0;
    };
    std::sort(types.begin(), types.end(), cmpName);
    std::sort(enums.begin(), enums.end(), cmpName);

    for (const auto& [n, subPtr] : types) {
        QString display = QStringLiteral("%1 (%2)")
            .arg(nameOf(n), n->resolvedClassKeyword());
        auto* item = new QStandardItem(
            QIcon(":/vsicons/symbol-structure.svg"), display);
        item->setData(QVariant::fromValue(subPtr), Qt::UserRole);
        item->setData(QVariant::fromValue(n->id), Qt::UserRole + 1);
        projectItem->appendRow(item);
    }

    for (const auto& [n, subPtr] : enums) {
        QString display = QStringLiteral("%1 (%2)")
            .arg(nameOf(n), n->resolvedClassKeyword());
        auto* item = new QStandardItem(
            QIcon(":/vsicons/symbol-enum.svg"), display);
        item->setData(QVariant::fromValue(subPtr), Qt::UserRole);
        item->setData(QVariant::fromValue(n->id), Qt::UserRole + 1);
        projectItem->appendRow(item);
    }

    model->appendRow(projectItem);
}

} // namespace rcx
