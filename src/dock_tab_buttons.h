#pragma once

#include <QWidget>
#include <QToolButton>
#include <QHBoxLayout>
#include <QIcon>

// Dock tab button widget (close button)
// Placed on the right side of each dock tab via QTabBar::setTabButton.
class DockTabButtons : public QWidget {
    Q_OBJECT
public:
    QToolButton* closeBtn;

    explicit DockTabButtons(QWidget* parent = nullptr) : QWidget(parent) {
        auto* hl = new QHBoxLayout(this);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(0);

        closeBtn = new QToolButton(this);
        closeBtn->setAutoRaise(true);
        closeBtn->setCursor(Qt::PointingHandCursor);
        closeBtn->setFixedSize(16, 16);
        closeBtn->setToolTip("Close tab");
        closeBtn->setIcon(QIcon(":/vsicons/close.svg"));
        closeBtn->setIconSize(QSize(12, 12));
        hl->addWidget(closeBtn);
    }

    void applyTheme(const QColor& hover) {
        QString style = QStringLiteral(
            "QToolButton { border: none; padding: 1px; border-radius: 0px; }"
            "QToolButton:hover { background: %1; }").arg(hover.name());
        closeBtn->setStyleSheet(style);
    }
};
