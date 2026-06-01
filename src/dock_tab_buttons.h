#pragma once

#include <QWidget>
#include <QToolButton>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QSvgRenderer>
#include <QFile>
#include <QByteArray>

// Source-status icon shown on the LEFT side of a doc tab, indicating
// which provider (File / Process / Kernel / etc) is currently active
// for that document. Full opacity = source is connected and live;
// muted = no source set, or source went stale (file missing, process
// exited, debugger disconnected).
class DockTabSourceIcon : public QLabel {
    Q_OBJECT
public:
    explicit DockTabSourceIcon(QWidget* parent = nullptr) : QLabel(parent) {
        setFixedSize(16, 16);
        setAlignment(Qt::AlignCenter);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setSourceIcon(const QString& iconPath, bool live, const QString& tip) {
        if (iconPath == m_currentPath && live == m_currentLive
            && m_currentDpr == devicePixelRatioF()) {
            return;
        }
        m_currentPath = iconPath;
        m_currentLive = live;
        m_currentDpr  = devicePixelRatioF();

        // Render the SVG at native screen pixel density. Without
        // setDevicePixelRatio the rasterized pixmap is upscaled by the
        // platform on hi-DPI displays — that's what made the previous
        // QIcon::pixmap(12,12) approach blurry. The source-chooser
        // dropdown stays crisp because its delegate's QPainter implicitly
        // forwards the dpr; for a standalone QLabel we must do it ourselves.
        const int target = 14;  // logical px (16x16 widget with a 1px halo)
        const qreal dpr  = m_currentDpr > 0 ? m_currentDpr : 1.0;
        QPixmap pm(QSize(target, target) * dpr);
        pm.fill(Qt::transparent);
        if (!iconPath.isEmpty()) {
            QSvgRenderer r(iconPath);
            if (r.isValid()) {
                QPainter sp(&pm);
                sp.setRenderHint(QPainter::Antialiasing, true);
                sp.setRenderHint(QPainter::SmoothPixmapTransform, true);
                if (!live) sp.setOpacity(0.35);
                r.render(&sp, QRectF(0, 0,
                                      pm.width()  / dpr,
                                      pm.height() / dpr));
            }
        }
        pm.setDevicePixelRatio(dpr);
        setPixmap(pm);
        setToolTip(tip);
    }

private:
    QString m_currentPath;
    bool    m_currentLive = false;
    qreal   m_currentDpr  = 0.0;
};

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
        closeBtn->setIconSize(QSize(12, 12));
        hl->addWidget(closeBtn);
    }

    // Re-tint the close X with the theme's text color so it stays
    // visible on light chrome (the source SVG bakes #C5C5C5 which
    // disappears against a near-white background). Same pattern as
    // titlebar.cpp's themedSvgIcon helper.
    void applyTheme(const QColor& tint, const QColor& hover) {
        QFile f(QStringLiteral(":/vsicons/close.svg"));
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray svg = f.readAll();
            svg.replace("#C5C5C5", tint.name().toLatin1());
            svg.replace("#c5c5c5", tint.name().toLatin1());
            QSvgRenderer r(svg);
            const qreal dpr = devicePixelRatioF();
            QPixmap pm(QSize(12, 12) * (dpr > 0 ? dpr : 1.0));
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            r.render(&p, QRectF(0, 0, pm.width() / (dpr > 0 ? dpr : 1.0),
                                       pm.height() / (dpr > 0 ? dpr : 1.0)));
            p.end();
            pm.setDevicePixelRatio(dpr);
            closeBtn->setIcon(QIcon(pm));
        }
        QString style = QStringLiteral(
            "QToolButton { border: none; padding: 1px; border-radius: 0px; }"
            "QToolButton:hover { background: %1; }").arg(hover.name());
        closeBtn->setStyleSheet(style);
    }
};
