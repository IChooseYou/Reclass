#pragma once
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QTreeWidget>
#include <QTabWidget>
#include <QPushButton>
#include <QApplication>
#include <QClipboard>
#include <QSettings>
#include "rtti.h"
#include "themes/thememanager.h"

namespace rcx {

class Provider;  // unused but lets future extensions read more memory

// Modal viewer that takes a parsed RttiInfo and displays:
//   - Top: class name (demangled), raw name, COL/imageBase metadata
//   - Tab 1: class hierarchy (base classes in resolution order)
//   - Tab 2: virtual method table (slot, address, symbol)
//
// "Copy as Tree" dumps a textual report to the clipboard — useful for
// pasting into bug reports or shared notes about a target.
class RttiBrowserDialog : public QDialog {
public:
    explicit RttiBrowserDialog(const RttiInfo& info, Provider* prov = nullptr,
                               QWidget* parent = nullptr)
        : QDialog(parent), m_info(info) {
        Q_UNUSED(prov);
        setWindowTitle(QStringLiteral("RTTI: ") +
            (info.demangledName.isEmpty() ? info.rawName : info.demangledName));
        setModal(true);
        resize(720, 480);

        const auto& t = ThemeManager::instance().current();
        {
            QPalette pal = palette();
            pal.setColor(QPalette::Window, t.background);
            pal.setColor(QPalette::WindowText, t.text);
            setPalette(pal);
            setAutoFillBackground(true);
        }

        QSettings s("Reclass", "Reclass");
        QFont font(s.value("font", "JetBrains Mono").toString(), 10);
        font.setFixedPitch(true);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(8);

        // Header summary
        auto* head = new QLabel;
        head->setTextFormat(Qt::RichText);
        head->setWordWrap(true);
        QString headHtml;
        headHtml += QStringLiteral("<div style='color:%1;font-size:13pt;'><b>%2</b>%3</div>")
            .arg(t.text.name(),
                 (info.demangledName.isEmpty() ? info.rawName : info.demangledName)
                     .toHtmlEscaped(),
                 info.abi.isEmpty()
                    ? QString()
                    : QStringLiteral(" <span style='color:%1;font-size:10pt;'>(%2)</span>")
                          .arg(t.textDim.name(), info.abi.toHtmlEscaped()));
        if (!info.rawName.isEmpty() && info.rawName != info.demangledName)
            headHtml += QStringLiteral("<div style='color:%1;'>%2</div>")
                .arg(t.textDim.name(), info.rawName.toHtmlEscaped());
        headHtml += QStringLiteral("<div style='color:%1;'>")
            .arg(t.textDim.name());
        headHtml += QStringLiteral("vtable&nbsp;0x%1").arg(info.vtableAddress, 0, 16);
        if (!info.moduleName.isEmpty())
            headHtml += QStringLiteral("&nbsp;·&nbsp;module&nbsp;<b>%1</b>")
                .arg(info.moduleName.toHtmlEscaped());
        if (info.imageBase)
            headHtml += QStringLiteral("&nbsp;·&nbsp;imagebase&nbsp;0x%1")
                .arg(info.imageBase, 0, 16);
        headHtml += QStringLiteral("&nbsp;·&nbsp;COL&nbsp;0x%1")
            .arg(info.completeLocator, 0, 16);
        headHtml += QStringLiteral("&nbsp;·&nbsp;offset&nbsp;%1").arg(info.offset);
        headHtml += QStringLiteral("</div>");
        head->setText(headHtml);
        layout->addWidget(head);

        // Tabs
        auto* tabs = new QTabWidget;
        tabs->setFont(font);

        // Hierarchy tab
        auto* hierTree = new QTreeWidget;
        hierTree->setFont(font);
        hierTree->setHeaderLabels({QStringLiteral("Class"), QStringLiteral("Raw")});
        hierTree->setRootIsDecorated(false);
        hierTree->setAlternatingRowColors(true);
        for (const auto& b : info.bases) {
            auto* item = new QTreeWidgetItem(hierTree);
            item->setText(0, b.demangledName.isEmpty() ? b.rawName : b.demangledName);
            item->setText(1, b.rawName);
        }
        hierTree->resizeColumnToContents(0);
        tabs->addTab(hierTree, QStringLiteral("Hierarchy (%1)").arg(info.bases.size()));

        // Vtable tab
        auto* vtree = new QTreeWidget;
        vtree->setFont(font);
        vtree->setHeaderLabels({QStringLiteral("Slot"),
                                QStringLiteral("Address"),
                                QStringLiteral("Symbol")});
        vtree->setRootIsDecorated(false);
        vtree->setAlternatingRowColors(true);
        for (const auto& m : info.vtable) {
            auto* item = new QTreeWidgetItem(vtree);
            item->setText(0, QString::number(m.slot));
            item->setText(1, QStringLiteral("0x%1").arg(m.address, 0, 16));
            item->setText(2, m.symbol.isEmpty() ? QStringLiteral("(no symbol)") : m.symbol);
        }
        for (int i = 0; i < 3; i++) vtree->resizeColumnToContents(i);
        tabs->addTab(vtree, QStringLiteral("Vtable (%1)").arg(info.vtable.size()));

        layout->addWidget(tabs, /*stretch=*/1);

        // Buttons
        auto* btnRow = new QHBoxLayout;
        auto* copyBtn = new QPushButton(QStringLiteral("Copy as Tree"));
        copyBtn->setCursor(Qt::PointingHandCursor);
        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            QApplication::clipboard()->setText(buildTextReport(m_info));
        });
        auto* closeBtn = new QPushButton(QStringLiteral("Close"));
        closeBtn->setCursor(Qt::PointingHandCursor);
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
        btnRow->addWidget(copyBtn);
        btnRow->addStretch();
        btnRow->addWidget(closeBtn);
        layout->addLayout(btnRow);
    }

    // Static text-report builder, used by "Copy as Tree" and tests.
    static QString buildTextReport(const RttiInfo& info) {
        QString out;
        out += QStringLiteral("Class: %1\n")
            .arg(info.demangledName.isEmpty() ? info.rawName : info.demangledName);
        if (!info.abi.isEmpty())
            out += QStringLiteral("ABI:   %1\n").arg(info.abi);
        if (!info.rawName.isEmpty())
            out += QStringLiteral("Raw:   %1\n").arg(info.rawName);
        out += QStringLiteral("Vtable: 0x%1\n").arg(info.vtableAddress, 0, 16);
        if (!info.moduleName.isEmpty())
            out += QStringLiteral("Module: %1\n").arg(info.moduleName);
        out += QStringLiteral("COL:    0x%1\n").arg(info.completeLocator, 0, 16);
        out += QStringLiteral("Offset: %1\n\n").arg(info.offset);

        out += QStringLiteral("Hierarchy (%1):\n").arg(info.bases.size());
        for (const auto& b : info.bases)
            out += QStringLiteral("  %1\n")
                .arg(b.demangledName.isEmpty() ? b.rawName : b.demangledName);

        out += QStringLiteral("\nVtable (%1):\n").arg(info.vtable.size());
        for (const auto& m : info.vtable)
            out += QStringLiteral("  [%1] 0x%2  %3\n")
                .arg(m.slot, 2)
                .arg(m.address, 0, 16)
                .arg(m.symbol.isEmpty() ? QStringLiteral("(no symbol)") : m.symbol);
        return out;
    }

private:
    RttiInfo m_info;
};

} // namespace rcx
