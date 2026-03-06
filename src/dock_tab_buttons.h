#pragma once

#include <QWidget>
#include <QToolButton>
#include <QHBoxLayout>
#include <QIcon>

// Dock tab button widget (pin + close)
// Placed on the right side of each dock tab via QTabBar::setTabButton.
class DockTabButtons : public QWidget {
    Q_OBJECT
public:
    QToolButton* pinBtn;
    QToolButton* closeBtn;
    bool pinned = false;

    explicit DockTabButtons(QWidget* parent = nullptr) : QWidget(parent) {
        auto* hl = new QHBoxLayout(this);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(0);

        pinBtn = new QToolButton(this);
        pinBtn->setAutoRaise(true);
        pinBtn->setCursor(Qt::PointingHandCursor);
        pinBtn->setFixedSize(16, 16);
        pinBtn->setToolTip("Pin tab");
        updatePinIcon();
        hl->addWidget(pinBtn);

        closeBtn = new QToolButton(this);
        closeBtn->setAutoRaise(true);
        closeBtn->setCursor(Qt::PointingHandCursor);
        closeBtn->setFixedSize(16, 16);
        closeBtn->setToolTip("Close tab");
        closeBtn->setIcon(QIcon(":/vsicons/close.svg"));
        closeBtn->setIconSize(QSize(12, 12));
        hl->addWidget(closeBtn);

        connect(pinBtn, &QToolButton::clicked, this, [this]() {
            pinned = !pinned;
            updatePinIcon();
            emit pinToggled(pinned);
        });
    }

    void applyTheme(const QColor& hover) {
        QString style = QStringLiteral(
            "QToolButton { border: none; padding: 1px; border-radius: 0px; }"
            "QToolButton:hover { background: %1; }").arg(hover.name());
        pinBtn->setStyleSheet(style);
        closeBtn->setStyleSheet(style);
    }

    void setPinned(bool p) { pinned = p; updatePinIcon(); emit pinToggled(pinned); }

signals:
    void pinToggled(bool pinned);

private:
    void updatePinIcon() {
        pinBtn->setIcon(QIcon(pinned ? ":/vsicons/pinned.svg" : ":/vsicons/pin.svg"));
        pinBtn->setIconSize(QSize(12, 12));
        pinBtn->setToolTip(pinned ? "Unpin tab" : "Pin tab");
    }
};
