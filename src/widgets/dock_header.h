#pragma once
// ── rcx::DockHeader — the one title bar for every dock ──
// Project, Scanner, Symbols and Bookmarks each had a different header: 31 px vs
// 24 px vs 24 px vs Fusion's stock one; t.background vs t.backgroundAlt vs an
// autoFillBackground palette hack that never actually repainted on a theme
// change; a close button that was an SVG on one dock and a U+2715 text glyph
// tinted indHoverSpan on two others. Tabifying Project with Bookmarks visibly
// changed the header height on every tab switch. This is the single header:
// kDockHeaderHeight tall, t.background, one device-exact bottom row in
// containerBorderColor (so the Project header's seam lands on the same device
// row as the doc-tab seam beside it — see the project-header/doc-tab alignment
// memory), title in chrome 10 pt textDim at the shared kGutter, an optional
// right slot for 22x22 flat tool buttons, and an SVG close button.
//
// The header is also the drag surface. Qt's native dock drag runs on title-bar
// mouse events that bubble up UNCONSUMED, so we look at press/release only to
// swap the cursor and always ignore() the event — which is why the separate
// DockGripWidget (a 12-px dot column that existed solely to own a SizeAllCursor)
// is gone.
#include <QColor>
#include <QEvent>
#include <QFocusEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSettings>
#include <QSize>
#include <QString>
#include <QToolButton>
#include <QWidget>

#include "fontutil.h"
#include "paintutil.h"
#include "pixelglyphs.h"
#include "svgicon.h"
#include "themes/theme.h"
#include "themes/thememanager.h"

namespace rcx {

// The header's CONTENT band must equal main.cpp's kDocTabBarHeight: the
// Project header sits beside the editor's document tab bar and their bottom
// seams have to land on the same device row. main.cpp static_asserts the two
// against each other.
//
// The seam then costs ONE more logical row on top, which is why the widget is
// 32 and not 31. This is the same total the old layout had — a 31-px
// DockTitleBar followed by a 1-px HairlineSeparator inside the content — only
// now the line belongs to the header instead of being a separate widget the
// other three docks never got. Collapsing it to 31 puts the seam one device
// row ABOVE the doc-tab seam at 125 % scaling (measured: y=189 vs y=190).
inline constexpr int kDockHeaderContentH = 31;
inline constexpr int kDockHeaderSeam     = 1;
inline constexpr int kDockHeaderHeight   = kDockHeaderContentH + kDockHeaderSeam;
inline constexpr int kDockHeaderBtn    = 22;   // flat tool button box
inline constexpr int kDockHeaderIcon   = 14;   // glyph inside it
// Weight of a "this one is current" underline in dock chrome. Same 2 device
// rows the ribbon and both tab families use — one accent grammar everywhere.
inline constexpr int kDockAccentRows   = 2;

// The app's chrome font: the user's editor face at 10 pt. Every dock title is
// this exact font, so a tabified Project/Bookmarks pair no longer changes text
// weight (and, with the fixed header height, geometry) on a tab switch.
inline QFont chromeFont() {
    QSettings s(QStringLiteral("RC"), QStringLiteral("RC"));
    QFont f(s.value(QStringLiteral("font"), QStringLiteral("JetBrains Mono")).toString(), 10);
    f.setFixedPitch(true);
    return f;
}

// A flat 22x22 header button. The icon path is remembered on the button so
// DockHeader::applyTheme can re-tint it without the caller having to.
inline QToolButton* makeHeaderToolButton(const QString& iconPath,
                                         const QString& tip,
                                         QWidget* parent = nullptr) {
    auto* b = new QToolButton(parent);
    b->setProperty("rcxIconPath", iconPath);
    b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    b->setIconSize(QSize(kDockHeaderIcon, kDockHeaderIcon));
    b->setFixedSize(kDockHeaderBtn, kDockHeaderBtn);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

class DockHeader : public QWidget {
public:
    explicit DockHeader(const QString& title, QWidget* parent = nullptr)
        : QWidget(parent) {
        setFixedHeight(kDockHeaderHeight);
        m_lay = new QHBoxLayout(this);
        // The seam row is layout margin, so the title still centres inside the
        // same 31-px band the document tabs centre their labels in.
        m_lay->setContentsMargins(kGutter, 0, kGutter, kDockHeaderSeam);
        m_lay->setSpacing(0);

        m_title = new QLabel(title, this);
        m_title->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_title->setTextInteractionFlags(Qt::NoTextInteraction);
        // CRITICAL: a QLabel's natural minimumSizeHint is its full text width,
        // which QHBoxLayout sums into the title bar's minimumSize and Qt then
        // propagates UP to the QDockWidget as its effective minimum width —
        // silently outranking setMinimumWidth() on the dock. That is why a
        // long title used to make the separator "fight back" at ~300 px and
        // the dock open huge. Force the label horizontally elastic.
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_title->setMinimumWidth(0);
        m_lay->addWidget(m_title, /*stretch=*/1);

        m_close = makeHeaderToolButton(QStringLiteral(":/vsicons/close.svg"),
                                       QStringLiteral("Close panel"), this);
        m_lay->addWidget(m_close, 0, Qt::AlignVCenter);

        applyTheme(ThemeManager::instance().current());
    }

    QLabel*      titleLabel()  const { return m_title; }
    QToolButton* closeButton() const { return m_close; }

    // Right-slot widgets go BEFORE the close button so × stays the last thing
    // on the strip on every dock.
    void addRightWidget(QWidget* w) {
        m_lay->insertWidget(m_lay->count() - 1, w, 0, Qt::AlignVCenter);
        if (auto* b = qobject_cast<QToolButton*>(w)) m_rightBtns.append(b);
    }

    // Docked-only right hairline (the Project dock's edge against the editor).
    // Invalid colour = no line, which is the floating state.
    void setBorderRight(const QColor& c) { m_borderRight = c; update(); }

    void applyTheme(const Theme& t) {
        m_bg = t.background;
        m_seam = containerBorderColor(t);
        m_title->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
        m_title->setFont(chromeFont());
        const QString btnSheet = QStringLiteral(
            "QToolButton { border: none; padding: 0px; background: transparent; }"
            "QToolButton:hover { background: %1; }").arg(t.hover.name());
        const qreal dpr = devicePixelRatioF();
        auto tint = [&](QToolButton* b) {
            b->setStyleSheet(btnSheet);
            const QString path = b->property("rcxIconPath").toString();
            if (!path.isEmpty())
                b->setIcon(themedVsIcon(path, t.textDim, kDockHeaderIcon, dpr));
        };
        tint(m_close);
        for (auto* b : m_rightBtns) tint(b);
        update();
    }

    QSize sizeHint()        const override { return {QWidget::sizeHint().width(), kDockHeaderHeight}; }
    QSize minimumSizeHint() const override { return {0, kDockHeaderHeight}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), m_bg);
        fillBottomDeviceRowOfRect(p, QRectF(rect()), m_seam);
        if (m_borderRight.isValid())
            fillRightDeviceColOfRect(p, QRectF(rect()), m_borderRight);
    }
    // Never accept these — Qt's QDockWidget drag handler only runs on title-bar
    // events that reach it unconsumed. We look, swap the cursor, and pass on.
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) setCursor(Qt::SizeAllCursor);
        e->ignore();
    }
    void mouseReleaseEvent(QMouseEvent* e) override { unsetCursor(); e->ignore(); }
    // The drag ends with the mouse grabbed by the dock, so the release above
    // often never arrives here — reset on both crossings instead.
    void enterEvent(QEnterEvent*) override { unsetCursor(); }
    void leaveEvent(QEvent*)      override { unsetCursor(); }

private:
    QHBoxLayout*        m_lay   = nullptr;
    QLabel*             m_title = nullptr;
    QToolButton*        m_close = nullptr;
    QList<QToolButton*> m_rightBtns;
    QColor              m_bg;
    QColor              m_seam;
    QColor              m_borderRight;
};

// ── Collapsed-Project rail visibility ──
// The rail is a handle to bring back a dock the user CLOSED. QDockWidget also
// emits visibilityChanged(false) when it is merely deselected inside a tab
// group, and keying the rail off that alone made it pop into the left column
// every time the user clicked the Bookmarks tab. Extracted as a free function
// so the truth table is unit-testable without a MainWindow.
inline bool railVisibleFor(bool dockVisible, bool userClosed) {
    return !dockVisible && userClosed;
}

// ── Rail chevron ──
// Kept here rather than in pixelglyphs.h: it is a dock-chrome glyph, and the
// ribbon owns that file. 4x7 so it fits PixelBitmap's 8-row store.
inline constexpr PixelBitmap kChevronRight4x7{4, 7, {
    "#...",
    ".#..",
    "..#.",
    "...#",
    "..#.",
    ".#..",
    "#..."}};

// Paints the chevron centred on `centerLogical`, in DEVICE pixels with
// antialiasing off — the previous rail drew it with a 1.8-wide RoundCap pen,
// which is a soft grey smear at any fractional DPR. Rendered into an unscaled
// image and stamped with the dpr AFTER painting (the pixel-glyph rule; the
// opposite of the SVG rule).
inline void drawRailChevron(QPainter& p, const QPointF& centerLogical,
                            const QColor& ink, qreal dpr) {
    const qreal s = dpr > 0 ? dpr : 1.0;
    const int scale = pixelBitmapScale(s);
    const int wDev = kChevronRight4x7.w * scale;
    const int hDev = kChevronRight4x7.h * scale;
    QImage img(wDev, hDev, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter ip(&img);
        drawBitmap(ip, 0, 0, kChevronRight4x7, scale, ink);
    }
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(s);
    p.drawPixmap(QPointF(centerLogical.x() - wDev / (2.0 * s),
                         centerLogical.y() - hDev / (2.0 * s)), pm);
}

}  // namespace rcx
