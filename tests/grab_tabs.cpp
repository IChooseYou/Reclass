#include <QtTest/QTest>
#include <QApplication>
#include <QMainWindow>
#include <QDockWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QPixmap>
#include <QToolButton>
#include <QHBoxLayout>
#include <QProxyStyle>
#include <QStyleOptionTab>
#include <QSettings>
#include <QPainter>
#include "../src/themes/thememanager.h"

// Minimal replica of the real app's MenuBarStyle for dock tab painting
class TestTabStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    QSize sizeFromContents(ContentsType type, const QStyleOption* opt,
                           const QSize& sz, const QWidget* w) const override {
        QSize s = QProxyStyle::sizeFromContents(type, opt, sz, w);
        if (type == CT_TabBarTab) {
            if (auto* tabBar = qobject_cast<const QTabBar*>(w)) {
                if (tabBar->parent() && qobject_cast<const QMainWindow*>(tabBar->parent()))
                    s.setHeight(28);
            }
        }
        return s;
    }

    void drawControl(ControlElement element, const QStyleOption* opt,
                     QPainter* p, const QWidget* w) const override {
        // Tab shape — background, accent line, borders
        if (element == CE_TabBarTabShape) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt)) {
                auto* tabBar = qobject_cast<const QTabBar*>(w);
                if (tabBar && tabBar->parent() && qobject_cast<QMainWindow*>(tabBar->parent())) {
                    bool selected = tab->state & State_Selected;
                    bool hovered  = tab->state & State_MouseOver;
                    QColor bg = tab->palette.color(QPalette::Window);
                    if (hovered && !selected)
                        bg = tab->palette.color(QPalette::Mid);
                    p->fillRect(tab->rect, bg);
                    if (selected)
                        p->fillRect(QRect(tab->rect.left(), tab->rect.top(),
                                          tab->rect.width(), 2),
                                    tab->palette.color(QPalette::Link));
                    p->setPen(tab->palette.color(QPalette::Dark));
                    p->drawLine(tab->rect.bottomLeft(), tab->rect.bottomRight());
                    return;
                }
            }
        }
        // Tab label — middle-elide long names, editor font
        if (element == CE_TabBarTabLabel) {
            if (auto* tab = qstyleoption_cast<const QStyleOptionTab*>(opt)) {
                auto* tabBar = qobject_cast<const QTabBar*>(w);
                if (tabBar && tabBar->parent() && qobject_cast<QMainWindow*>(tabBar->parent())) {
                    int tabIdx = -1;
                    for (int i = 0; i < tabBar->count(); ++i) {
                        if (tabBar->tabRect(i).contains(tab->rect.center())) { tabIdx = i; break; }
                    }
                    int btnWidth = 0;
                    if (tabIdx >= 0) {
                        auto* btn = tabBar->tabButton(tabIdx, QTabBar::RightSide);
                        if (btn) btnWidth = btn->sizeHint().width() + 4;
                    }
                    QRect textRect = tab->rect.adjusted(8, 0, -(8 + btnWidth), 0);
                    QFont f("JetBrains Mono", 10);
                    f.setFixedPitch(true);
                    p->setFont(f);
                    QFontMetrics fm(f);
                    QString text = (tabIdx >= 0) ? tabBar->tabText(tabIdx) : tab->text;
                    int maxW = textRect.width();
                    if (fm.horizontalAdvance(text) > maxW) {
                        int ellW = fm.horizontalAdvance(QStringLiteral("\u2026"));
                        int avail = maxW - ellW;
                        if (avail > 0) {
                            int half = avail / 2;
                            QString left, right;
                            for (int i = 0; i < text.size(); ++i)
                                if (fm.horizontalAdvance(text.left(i+1)) > half) { left = text.left(i); break; }
                            if (left.isEmpty()) left = text.left(1);
                            for (int i = text.size()-1; i >= 0; --i)
                                if (fm.horizontalAdvance(text.mid(i)) > half) { right = text.mid(i+1); break; }
                            if (right.isEmpty()) right = text.right(1);
                            text = left + QStringLiteral("\u2026") + right;
                        } else {
                            text = QStringLiteral("\u2026");
                        }
                    }
                    bool selected = tab->state & QStyle::State_Selected;
                    p->setPen(selected ? tab->palette.color(QPalette::Text)
                                       : tab->palette.color(QPalette::WindowText));
                    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text);
                    return;
                }
            }
        }
        QProxyStyle::drawControl(element, opt, p, w);
    }
};

class TabBtns : public QWidget {
public:
    explicit TabBtns(const QColor& hover, QWidget* parent = nullptr) : QWidget(parent) {
        auto* hl = new QHBoxLayout(this);
        hl->setContentsMargins(2, 0, 0, 0);
        hl->setSpacing(0);
        QString style = QStringLiteral(
            "QToolButton { border: none; padding: 1px; border-radius: 0px; }"
            "QToolButton:hover { background: %1; }").arg(hover.name());
        auto* pin = new QToolButton(this);
        pin->setFixedSize(16, 16);
        pin->setAutoRaise(true);
        pin->setIcon(QIcon(":/vsicons/pin.svg"));
        pin->setIconSize(QSize(12, 12));
        pin->setStyleSheet(style);
        hl->addWidget(pin);
        auto* close = new QToolButton(this);
        close->setFixedSize(16, 16);
        close->setAutoRaise(true);
        close->setIcon(QIcon(":/vsicons/close.svg"));
        close->setIconSize(QSize(12, 12));
        close->setStyleSheet(style);
        hl->addWidget(close);
    }
};

class GrabTabs : public QObject {
    Q_OBJECT
private slots:
    void grab() {
        const auto& t = rcx::ThemeManager::instance().current();

        // Install custom style (no stylesheet — all painting via style)
        QApplication::setStyle(new TestTabStyle("Fusion"));

        // Apply dark palette globally
        QPalette pal;
        pal.setColor(QPalette::Window, t.background);
        pal.setColor(QPalette::WindowText, t.textDim);
        pal.setColor(QPalette::Base, t.background);
        pal.setColor(QPalette::Text, t.text);
        pal.setColor(QPalette::Mid, t.hover);
        pal.setColor(QPalette::Dark, t.border);
        pal.setColor(QPalette::Link, t.indHoverSpan);
        QApplication::setPalette(pal);

        auto* win = new QMainWindow;
        win->resize(700, 500);
        win->setDockNestingEnabled(true);
        win->setTabPosition(Qt::TopDockWidgetArea, QTabWidget::North);

        auto* central = new QWidget(win);
        central->setMaximumSize(0, 0);
        central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        win->setCentralWidget(central);
        win->setStyleSheet(QStringLiteral(
            "QMainWindow::separator { width: 0px; height: 0px; background: transparent; }"));

        QStringList names = {
            "shader_color_helper.hpp",
            "shader_crypt.cpp",
            "EPROCESS (class)",
            "very_long_struct_name_that_should_elide.h"
        };

        QVector<QDockWidget*> docks;
        for (const auto& name : names) {
            auto* dock = new QDockWidget(name, win);
            dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
            auto* emptyTitle = new QWidget(dock);
            emptyTitle->setFixedHeight(0);
            dock->setTitleBarWidget(emptyTitle);
            dock->setWidget(new QTextEdit(dock));
            if (!docks.isEmpty())
                win->tabifyDockWidget(docks.last(), dock);
            else
                win->addDockWidget(Qt::TopDockWidgetArea, dock);
            docks.append(dock);
        }
        // Select first tab
        docks.first()->raise();

        win->show();
        QVERIFY(QTest::qWaitForWindowExposed(win));
        QApplication::processEvents();

        // No stylesheet on dock tab bars — painting handled by TestTabStyle
        for (auto* tabBar : win->findChildren<QTabBar*>()) {
            if (tabBar->parent() != win) continue;
            tabBar->setStyleSheet(QString());
            tabBar->setElideMode(Qt::ElideNone);
            tabBar->setExpanding(false);

            QPalette tp = tabBar->palette();
            tp.setColor(QPalette::WindowText, t.textDim);
            tp.setColor(QPalette::Text, t.text);
            tp.setColor(QPalette::Window, t.background);
            tp.setColor(QPalette::Mid, t.hover);
            tp.setColor(QPalette::Dark, t.border);
            tp.setColor(QPalette::Link, t.indHoverSpan);
            tabBar->setPalette(tp);

            for (int i = 0; i < tabBar->count(); ++i)
                tabBar->setTabButton(i, QTabBar::RightSide, new TabBtns(t.hover, tabBar));
        }
        QApplication::processEvents();
        QApplication::processEvents();

        QPixmap shot = win->grab(QRect(0, 0, win->width(), 50));
        shot.save(QStringLiteral("tab_screenshot.png"));
        qDebug() << "Saved" << shot.size();
        delete win;
    }
};

QTEST_MAIN(GrabTabs)
#include "grab_tabs.moc"
