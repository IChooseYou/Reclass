#include "titlebar.h"
#include "themes/thememanager.h"
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWindow>

namespace rcx {

TitleBarWidget::TitleBarWidget(QWidget* parent)
    : QWidget(parent)
    , m_theme(ThemeManager::instance().current())
{
    setFixedHeight(32);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // App name
    m_appLabel = new QLabel(QStringLiteral("Reclass"), this);
    m_appLabel->setContentsMargins(10, 0, 4, 0);
    m_appLabel->setAlignment(Qt::AlignVCenter);
    m_appLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_appLabel);

    // Menu bar
    m_menuBar = new QMenuBar(this);
    m_menuBar->setNativeMenuBar(false);
    m_menuBar->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    layout->addWidget(m_menuBar);

    layout->addStretch();

    // Chrome buttons
    m_btnMin   = makeChromeButton(":/vsicons/chrome-minimize.svg");
    m_btnMax   = makeChromeButton(":/vsicons/chrome-maximize.svg");
    m_btnClose = makeChromeButton(":/vsicons/chrome-close.svg");

    layout->addWidget(m_btnMin);
    layout->addWidget(m_btnMax);
    layout->addWidget(m_btnClose);

    connect(m_btnMin, &QToolButton::clicked, this, [this]() {
        window()->showMinimized();
    });
    connect(m_btnMax, &QToolButton::clicked, this, [this]() {
        toggleMaximize();
    });
    connect(m_btnClose, &QToolButton::clicked, this, [this]() {
        window()->close();
    });
}

QToolButton* TitleBarWidget::makeChromeButton(const QString& iconPath) {
    auto* btn = new QToolButton(this);
    btn->setIcon(QIcon(iconPath));
    btn->setIconSize(QSize(16, 16));
    btn->setFixedSize(46, 32);
    btn->setAutoRaise(true);
    btn->setFocusPolicy(Qt::NoFocus);
    return btn;
}

void TitleBarWidget::applyTheme(const Theme& theme) {
    m_theme = theme;

    // Title bar background
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme.background);
    setPalette(pal);

    // App label
    m_appLabel->setStyleSheet(
        QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: bold; }")
            .arg(theme.textDim.name()));

    // Menu bar styling — transparent background, themed text
    m_menuBar->setStyleSheet(
        QStringLiteral(
            "QMenuBar { background: transparent; border: none; }"
            "QMenuBar::item { background: transparent; color: %1; padding: 8px 8px 4px 8px; }"
            "QMenuBar::item:selected { background: %2; }"
            "QMenuBar::item:pressed { background: %2; }")
            .arg(theme.textDim.name(), theme.hover.name()));

    // Chrome buttons
    QString btnStyle = QStringLiteral(
        "QToolButton { background: transparent; border: none; }"
        "QToolButton:hover { background: %1; }")
        .arg(theme.hover.name());
    m_btnMin->setStyleSheet(btnStyle);
    m_btnMax->setStyleSheet(btnStyle);

    // Close button: red hover
    m_btnClose->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; }"
        "QToolButton:hover { background: #c42b1c; }"));

    update();
}

void TitleBarWidget::setShowIcon(bool show) {
    if (show) {
        m_appLabel->setText(QString());
        m_appLabel->setPixmap(QIcon(":/icons/class.png").pixmap(24, 24));
    } else {
        m_appLabel->setPixmap(QPixmap());
        m_appLabel->setText(QStringLiteral("Reclass"));
        m_appLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; font-size: 12px; font-weight: bold; }")
                .arg(m_theme.textDim.name()));
    }
}

void TitleBarWidget::updateMaximizeIcon() {
    if (window()->isMaximized())
        m_btnMax->setIcon(QIcon(":/vsicons/chrome-restore.svg"));
    else
        m_btnMax->setIcon(QIcon(":/vsicons/chrome-maximize.svg"));
}

void TitleBarWidget::toggleMaximize() {
    if (window()->isMaximized())
        window()->showNormal();
    else
        window()->showMaximized();
    updateMaximizeIcon();
}

void TitleBarWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        window()->windowHandle()->startSystemMove();
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void TitleBarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        toggleMaximize();
        event->accept();
    } else {
        QWidget::mouseDoubleClickEvent(event);
    }
}

void TitleBarWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);

    // 1px bottom border
    QPainter p(this);
    p.setPen(m_theme.border);
    p.drawLine(0, height() - 1, width() - 1, height() - 1);
}

} // namespace rcx
