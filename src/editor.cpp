#include "editor.h"
#include "disasm.h"
#include "providerregistry.h"
#include "rcxtooltip.h"
#include "profiler.h"
#include "widgets/hover_preview.h"
#include <QDebug>
#include <QSettings>
#include <QtEndian>
#include <QStackedWidget>
#include <QPainter>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include <Qsci/qscilexercpp.h>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QColor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QTimer>
#include <QCursor>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QToolButton>
#include <QLineEdit>
#include <QScreen>
#include <QScrollBar>
#include <QDateTime>
#include <algorithm>
#include <functional>
#include <cmath>
#include "themes/thememanager.h"

namespace rcx {

// Forward declaration (defined below, after RcxEditor constructor)
static QString getLineText(QsciScintilla* sci, int line);
// Global anchor at the bottom of (line, col) for hover preview popups.
// All three hover popups (value history, disasm, struct preview) call
// this with col = vs.start so they appear at the value column edge of
// the same row — previously each block had its own inline copy of
// SCI_POSITIONFROMLINE + POINTXFROMPOSITION + mapToGlobal and any
// subtle off-by-one would creep in unnoticed.
static QPoint popupAnchorAt(QsciScintilla* sci, int line, int col,
                            const QString& lineText, int* outLineHeight);

// ── Base class for all hover popups ──

class HoverPopup : public QFrame {
protected:
    uint64_t m_nodeId = 0;
    std::function<void(QMouseEvent*)> m_onMouseMove;
public:
    explicit HoverPopup(QWidget* parent)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setMouseTracking(true);
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(true);
    }

    uint64_t nodeId() const { return m_nodeId; }
    void setOnMouseMove(std::function<void(QMouseEvent*)> fn) { m_onMouseMove = std::move(fn); }

    void showAt(const QPoint& globalPos, int lineHeight = 0) {
        QRect screen = QApplication::screenAt(globalPos)
            ? QApplication::screenAt(globalPos)->availableGeometry()
            : QRect(0, 0, 1920, 1080);
        // Constrain max width to fit between the anchor X and the
        // screen's right edge so the popup keeps its left edge at the
        // value column. Previously a too-wide popup was qMin-shifted
        // left, visually decoupling it from the row it describes —
        // user-visible as "the disasm/struct-preview open at a wildly
        // different X than the value-history popup". Now wide content
        // shrinks instead of moving.
        int availW = screen.right() - globalPos.x() - 8;
        if (availW > 200 && maximumWidth() > availW) {
            setMaximumWidth(availW);
            adjustSize();
        }
        QSize sz = sizeHint();
        int x = globalPos.x();   // strictly the value-column anchor
        int y = globalPos.y();
        if (y + sz.height() > screen.bottom())
            y = globalPos.y() - sz.height() - lineHeight - 4;
        move(x, y);
        if (!isVisible()) show();
    }

    virtual void dismiss() {
        if (isVisible()) hide();
        m_nodeId = 0;
    }

protected:
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_onMouseMove) m_onMouseMove(e);
        else QFrame::mouseMoveEvent(e);
    }

    void applyThemePalette(const Theme& t) {
        QPalette pal;
        pal.setColor(QPalette::Window, t.backgroundAlt);
        pal.setColor(QPalette::WindowText, t.text);
        setPalette(pal);
    }

    void styleSeparator(const Theme& t) {
        for (auto* child : findChildren<QFrame*>()) {
            if (child->frameShape() == QFrame::HLine) {
                QPalette sp;
                sp.setColor(QPalette::WindowText, t.border);
                child->setPalette(sp);
                break;
            }
        }
    }
};

// ── Title + body popup (used for disasm/hex-dump and struct preview) ──

class TitleBodyPopup : public HoverPopup {
    QString m_body;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_bodyLabel  = nullptr;
public:
    explicit TitleBodyPopup(QWidget* parent) : HoverPopup(parent) {
        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(8, 6, 8, 6);
        vbox->setSpacing(2);

        m_titleLabel = new QLabel;
        QFont bold = m_titleLabel->font();
        bold.setBold(true);
        m_titleLabel->setFont(bold);
        vbox->addWidget(m_titleLabel);

        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setFixedHeight(1);
        vbox->addWidget(sep);

        m_bodyLabel = new QLabel;
        m_bodyLabel->setTextFormat(Qt::PlainText);
        m_bodyLabel->setWordWrap(false);
        vbox->addWidget(m_bodyLabel);
    }

    void populate(uint64_t nodeId, const QString& title, const QString& body,
                  const QFont& font, const QColor& bodyColor) {
        if (nodeId == m_nodeId && body == m_body && isVisible())
            return;

        m_nodeId = nodeId;
        m_body = body;

        const auto& theme = ThemeManager::instance().current();
        applyThemePalette(theme);

        QFont bold = font;
        bold.setBold(true);
        m_titleLabel->setFont(bold);
        m_titleLabel->setText(title);
        m_titleLabel->setStyleSheet(
            QStringLiteral("color: %1;").arg(theme.text.name()));

        styleSeparator(theme);

        m_bodyLabel->setFont(font);
        m_bodyLabel->setText(body);
        m_bodyLabel->setStyleSheet(
            QStringLiteral("color: %1;").arg(bodyColor.name()));

        setMaximumWidth(600);
        adjustSize();
    }

    void dismiss() override {
        HoverPopup::dismiss();
        m_body.clear();
    }
};

// ── Value history popup ──

class ValueHistoryPopup : public HoverPopup {
    bool     m_hasButtons = false;
    QStringList m_values;
    QVector<QLabel*> m_labels;
    std::function<void(const QString&)> m_onSet;
public:
    explicit ValueHistoryPopup(QWidget* parent) : HoverPopup(parent) {}

    bool hasButtons() const { return m_hasButtons; }
    void setOnSet(std::function<void(const QString&)> fn) { m_onSet = std::move(fn); }

protected:
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!m_hasButtons && m_onMouseMove)
            m_onMouseMove(e);
        else
            QFrame::mouseMoveEvent(e);
    }

public:
    void populate(uint64_t nodeId, const ValueHistory& hist, const QFont& font,
                  bool showButtons = false) {
        QStringList vals;
        hist.forEach([&](const QString& v) { vals.append(v); });

        if (nodeId == m_nodeId && vals == m_values
            && showButtons == m_hasButtons && isVisible())
            return;

        // In-place label update when structure unchanged (avoids flicker)
        if (nodeId == m_nodeId && vals.size() == m_values.size()
            && vals.size() == m_labels.size()
            && showButtons == m_hasButtons && isVisible()) {
            for (int i = 0; i < vals.size(); i++)
                m_labels[i]->setText(vals[i]);
            m_values = vals;
            return;
        }

        m_nodeId = nodeId;
        m_values = vals;
        m_hasButtons = showButtons;
        m_labels.clear();

        delete layout();
        qDeleteAll(findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly));

        const auto& theme = ThemeManager::instance().current();
        applyThemePalette(theme);

        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(8, 6, 8, 6);
        vbox->setSpacing(2);

        auto* title = new QLabel(QStringLiteral("Previous Values"));
        QFont bold = font;
        bold.setBold(true);
        title->setFont(bold);
        title->setStyleSheet(QStringLiteral("color: %1;").arg(theme.text.name()));
        vbox->addWidget(title);

        auto* sep = new QFrame;
        sep->setFrameShape(QFrame::HLine);
        sep->setFrameShadow(QFrame::Plain);
        sep->setFixedHeight(1);
        QPalette sp; sp.setColor(QPalette::WindowText, theme.border);
        sep->setPalette(sp);
        vbox->addWidget(sep);

        qint64 now = QDateTime::currentMSecsSinceEpoch();
        hist.forEachWithTime([&](const QString& v, qint64 msec) {
            auto* row = new QHBoxLayout;
            row->setContentsMargins(0, 1, 0, 1);
            row->setSpacing(8);

            auto* label = new QLabel(v);
            label->setFont(font);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(theme.syntaxNumber.name()));
            row->addWidget(label, 1);
            m_labels.append(label);

            if (msec > 0) {
                qint64 elapsed = now - msec;
                QString timeStr;
                if (elapsed < 1000)
                    timeStr = QStringLiteral("now");
                else if (elapsed < 60000)
                    timeStr = QStringLiteral("%1s ago").arg(elapsed / 1000);
                else if (elapsed < 3600000)
                    timeStr = QStringLiteral("%1m ago").arg(elapsed / 60000);
                else
                    timeStr = QStringLiteral("%1h ago").arg(elapsed / 3600000);

                auto* timeLabel = new QLabel(timeStr);
                timeLabel->setFont(font);
                timeLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textDim.name()));
                row->addWidget(timeLabel);
            }

            if (showButtons) {
                auto* setBtn = new QToolButton;
                setBtn->setText(QStringLiteral("Set"));
                setBtn->setAutoRaise(true);
                setBtn->setCursor(Qt::PointingHandCursor);
                setBtn->setFont(font);
                setBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; border: none; padding: 1px 4px; }"
                    "QToolButton:hover { color: %2; background: %3; }")
                    .arg(theme.textDim.name(), theme.text.name(), theme.hover.name()));
                QString val = v;
                QObject::connect(setBtn, &QToolButton::clicked, [this, val]() {
                    if (m_onSet) m_onSet(val);
                });
                row->addWidget(setBtn);
            }
            vbox->addLayout(row);
        });

        adjustSize();
    }

    void dismiss() override {
        HoverPopup::dismiss();
        m_values.clear();
        m_labels.clear();
    }
};

// ── Body-widget builders (shared between previews) ──

// Build a QLabel-only body widget for text-block previews (hex dump,
// disasm, struct target). The host already paints the title above us,
// so we only return the body content.
static QWidget* buildTextBodyWidget(const QString& body, const QFont& font,
                                     const QColor& bodyColor, QWidget* parent) {
    auto* lbl = new QLabel(parent);
    lbl->setFont(font);
    lbl->setTextFormat(Qt::PlainText);
    lbl->setWordWrap(false);
    lbl->setText(body);
    lbl->setStyleSheet(QStringLiteral("color: %1;").arg(bodyColor.name()));
    return lbl;
}

// Build the value-history rows widget (no "Set" buttons — those live
// on the inline-edit path which uses ValueHistoryPopup directly).
// Zebra-stripes alternate rows with a faint band so a tall history
// is easier to scan top-to-bottom.
static QWidget* buildValueHistoryBody(const ValueHistory& hist,
                                       const QFont& font, const Theme& theme,
                                       QWidget* parent) {
    auto* container = new QWidget(parent);
    auto* vbox = new QVBoxLayout(container);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    int idx = 0;
    // Each row sits in its own QFrame so we can paint a striped bg via
    // setAutoFillBackground — VBoxLayout alone can't do per-row bg.
    QColor stripe = theme.hover;
    stripe.setAlpha(28);  // very subtle band
    hist.forEachWithTime([&](const QString& v, qint64 msec) {
        auto* rowFrame = new QFrame(container);
        rowFrame->setObjectName(QStringLiteral("histRow"));
        rowFrame->setStyleSheet(
            (idx % 2 == 1)
                ? QStringLiteral("#histRow{background:rgba(%1,%2,%3,%4);}")
                      .arg(stripe.red()).arg(stripe.green())
                      .arg(stripe.blue()).arg(stripe.alpha())
                : QString());
        auto* row = new QHBoxLayout(rowFrame);
        // Zero left/right margin — the popup's vbox already provides
        // padding from the frame border. Adding row margin here on top
        // of that double-indented ValueHistory text by 4 px compared to
        // HexDump/Disasm body (whose QLabel sits flush against the
        // stack edge), causing the "pushed over" misalignment.
        row->setContentsMargins(0, 1, 0, 1);
        row->setSpacing(8);
        auto* label = new QLabel(v, rowFrame);
        label->setFont(font);
        // Newest row gets the full-strength number color; older rows
        // fade slightly so the most-recent value reads as primary.
        QColor valColor = theme.syntaxNumber;
        if (idx > 2) {
            valColor.setAlpha(qMax(140, 255 - idx * 18));
        }
        label->setStyleSheet(QStringLiteral("color: rgba(%1,%2,%3,%4);")
            .arg(valColor.red()).arg(valColor.green())
            .arg(valColor.blue()).arg(valColor.alpha()));
        row->addWidget(label, 1);
        if (msec > 0) {
            qint64 elapsed = now - msec;
            QString timeStr;
            if (elapsed < 1000)        timeStr = QStringLiteral("now");
            else if (elapsed < 60000)  timeStr = QStringLiteral("%1s ago").arg(elapsed / 1000);
            else if (elapsed < 3600000) timeStr = QStringLiteral("%1m ago").arg(elapsed / 60000);
            else                       timeStr = QStringLiteral("%1h ago").arg(elapsed / 3600000);
            auto* timeLabel = new QLabel(timeStr, rowFrame);
            timeLabel->setFont(font);
            timeLabel->setStyleSheet(QStringLiteral("color: %1;").arg(theme.textDim.name()));
            row->addWidget(timeLabel);
        }
        vbox->addWidget(rowFrame);
        ++idx;
    });
    vbox->addStretch(1);
    return container;
}

// ── HoverPopupHost ──
//
// Single popup that drives the hover-preview registry. Hosts the active
// preview's widget in a QStackedWidget; shows a title label up top and
// a footer hint "Tab to switch view →" when >1 preview is eligible.
// Activation is owned by the editor (Tab / Shift+Tab); the host just
// stages widgets and reports the new active id through a callback so
// the editor can persist it.
//
// ── Standard host size formula ──
//
// All hover previews share one popup geometry derived from the editor
// font + two constants (kHostCols, kHostRows). Locking the popup to a
// single size means Tab-cycling between previews NEVER changes width
// or height — the user's eye doesn't have to re-track the chip on
// switch. The formula:
//
//   width  = kHostCols * charWidth  + 2 * kPad   (=  64 * ~8  + 16 ≈ 528 px)
//   height = kHostRows * lineHeight              ← preview content area
//          + lineHeight                          ← title
//          + 1                                   ← separator
//          + lineHeight                          ← footer hint
//          + 2 * kPad                            ← top/bottom padding
//          + 2 * kSpacing                        ← layout spacing × 2
//
// kHostCols = 64 fits the widest natural row: a 16-byte hex dump line
// "00007FFE12340000  48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 56  ASCII"
// is ~63 characters. Disasm rows are ~50 chars; ValueHistory rows
// (value + elapsed-time) are ~30 chars. 64 gives slight slack.
//
// kHostRows = 8 fits the cap on every preview:
//   - HexDump / Disasm: capped at 6 lines via capLines()
//   - StructTarget:     capped at 5 lines
//   - ValueHistory:     ring of 10 entries, but typical visible run
//                       is < 8; remainder clips internally
//
// 8 means even the busiest preview sits without scrolling under most
// conditions, and the popup never shrinks below a comfortable read.
static constexpr int kHostCols    = 64;
static constexpr int kHostRows    = 8;
// Match the editor's existing popup chrome exactly: every other
// HoverPopup subclass (TitleBodyPopup, ValueHistoryPopup) uses
// (8, 6, 8, 6) inner margins with 2 px between widgets. The hover
// host must match so the popup doesn't visually drift left/right
// relative to the row when the user previously expected the old
// popups' indent.
static constexpr int kHostPadH    = 8;
static constexpr int kHostPadV    = 6;
static constexpr int kHostSpacing = 2;

static QSize computeHostStandardSize(const QFont& font) {
    QFontMetrics fm(font);
    const int charW = fm.horizontalAdvance(QLatin1Char('0'));
    const int lineH = fm.lineSpacing();
    // 4 widgets in the vbox (title row, separator, content stack,
    // footer) → 3 inter-widget gaps.
    return QSize(
        kHostCols * charW + 2 * kHostPadH,
        kHostRows * lineH        // content area
            + lineH              // title row
            + 1                  // separator
            + lineH              // footer hint row
            + 2 * kHostPadV
            + 3 * kHostSpacing);
}

// Sizes the stack to a CONSTANT — the formula above. Preview content
// shorter than the standard is padded with empty space (still readable);
// preview content longer than the standard is naturally clipped by the
// stack's own clipping (we cap output lines at the preview level so
// this branch rarely fires). Default QStackedWidget reports the max
// of all child sizeHints, which is exactly what we don't want — the
// popup would balloon to the widest child even after switching away.
class ActiveSizeStack : public QStackedWidget {
public:
    explicit ActiveSizeStack(QWidget* parent, QSize fixed)
        : QStackedWidget(parent), m_fixed(fixed) {}
    QSize sizeHint() const override { return m_fixed; }
    QSize minimumSizeHint() const override { return m_fixed; }
    void setFixed(QSize s) { m_fixed = s; updateGeometry(); }
private:
    QSize m_fixed;
};

// Little dots strip showing "you are here" within the eligible-previews
// cycle. ● for active, ○ for the rest. Mouse click on a dot cycles to
// that preview (the only mouse interaction the popup supports — Tab
// remains the primary path).
class PreviewDotsStrip : public QWidget {
public:
    using ClickFn = std::function<void(int idx)>;
    explicit PreviewDotsStrip(QWidget* parent) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }
    void setOnClick(ClickFn fn) { m_onClick = std::move(fn); }
    void setState(int total, int active, QColor fill, QColor empty) {
        m_total = total; m_active = active;
        m_fill = fill; m_empty = empty;
        // Boxy 4×4 px squares matching the editor's pill aesthetic —
        // round dots looked out of place against the straight-edged
        // chip pills, command-row buttons, and footer affordances.
        int w = total * kBoxSide + (total - 1) * kBoxGap;
        setFixedSize(qMax(kBoxSide, w), kBoxSide + 4);
        update();
    }
    QSize sizeHint() const override {
        int w = qMax(0, m_total) * kBoxSide + qMax(0, m_total - 1) * kBoxGap;
        return QSize(qMax(kBoxSide, w), kBoxSide + 4);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (m_total <= 0) return;
        QPainter p(this);
        // No AA — these are 4 px squares, AA just blurs the edges.
        int y = (height() - kBoxSide) / 2;
        for (int i = 0; i < m_total; ++i) {
            int x = i * (kBoxSide + kBoxGap);
            bool isActive = (i == m_active);
            QRect r(x, y, kBoxSide, kBoxSide);
            if (isActive) {
                p.fillRect(r, m_fill);
            } else {
                p.setPen(QPen(m_empty, 1));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(0, 0, -1, -1));
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || !m_onClick) return;
        for (int i = 0; i < m_total; ++i) {
            int x = i * (kBoxSide + kBoxGap);
            QRect hit(x - 1, 0, kBoxSide + 2, height());
            if (hit.contains(e->pos())) { m_onClick(i); return; }
        }
    }
private:
    static constexpr int kBoxSide = 4;
    static constexpr int kBoxGap  = 6;
    int     m_total = 0;
    int     m_active = -1;
    QColor  m_fill;
    QColor  m_empty;
    ClickFn m_onClick;
};

class HoverPopupHost : public HoverPopup {
public:
    using ActiveChangedFn = std::function<void(const LineMeta&, QString id)>;

    explicit HoverPopupHost(QWidget* parent, const QFont& contentFont)
        : HoverPopup(parent), m_standardSize(computeHostStandardSize(contentFont))
    {
        // Boxy chrome — same straight 1 px border the editor uses for
        // every other panel (command row pills, dialog buttons,
        // dialog frame, scanner toolbar). No border-radius, no drop
        // shadow. The earlier rounded-corner + soft-shadow version
        // looked like a foreign tooltip; consistency with the rest of
        // the Reclass UI means flat rectangles and themed borders.
        setAutoFillBackground(true);
        m_shadowMargin = 0;

        auto* vbox = new QVBoxLayout(this);
        vbox->setContentsMargins(kHostPadH, kHostPadV, kHostPadH, kHostPadV);
        vbox->setSpacing(kHostSpacing);

        // ── Top row: title (left) + dots strip (right) ──
        auto* topRow = new QHBoxLayout;
        topRow->setContentsMargins(0, 0, 0, 0);
        topRow->setSpacing(6);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setTextFormat(Qt::RichText);
        // Title uses the editor font at its native size — same as the
        // editor body, which keeps the popup reading as a piece of the
        // same surface rather than a UI overlay with its own style.
        m_titleLabel->setFont(contentFont);
        topRow->addWidget(m_titleLabel, 1);

        m_dots = new PreviewDotsStrip(this);
        m_dots->setOnClick([this](int idx){ setActiveIndex(idx); });
        topRow->addWidget(m_dots, 0, Qt::AlignVCenter);

        vbox->addLayout(topRow);

        m_separator = new QFrame(this);
        m_separator->setFrameShape(QFrame::HLine);
        m_separator->setFrameShadow(QFrame::Plain);
        m_separator->setFixedHeight(1);
        vbox->addWidget(m_separator);

        // ── Content stack ──
        QSize stackSize(m_standardSize.width() - 2 * kHostPadH,
                        kHostRows * QFontMetrics(contentFont).lineSpacing());
        m_content = new ActiveSizeStack(this, stackSize);
        m_content->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        vbox->addWidget(m_content);

        // ── Footer: plain text with bold-colored key letters ──
        m_footerHint = new QLabel(this);
        m_footerHint->setTextFormat(Qt::RichText);
        m_footerHint->setFont(contentFont);
        m_footerHint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        vbox->addWidget(m_footerHint);

        // 1 px themed border on the whole popup — no rounding, no
        // shadow. setFrameShape(Box) draws the actual border; the
        // palette set in applyHostTheme controls its color.
        setFrameShape(QFrame::Box);
        setFrameShadow(QFrame::Plain);
        setLineWidth(1);
    }

    void setOnActiveChanged(ActiveChangedFn fn) { m_onActiveChanged = std::move(fn); }

    int  eligibleCount() const { return m_eligible.size(); }
    int  activeIndex()  const  { return m_active; }
    HoverPreview* activePreview() const {
        return (m_active >= 0 && m_active < m_eligible.size())
               ? m_eligible[m_active] : nullptr;
    }

    // Refresh the host's eligible set + active widget. activeIdx is the
    // initial selection (from QSettings persistence or 0).
    void setEligible(QVector<HoverPreview*> eligible,
                     const LineMeta& lm, const Node& node,
                     const HoverContext& ctx, int activeIdx) {
        // Fingerprint: nodeId + eligible-id-list + ValueHistory count.
        // If the hover tick lands on the same row with the same
        // eligible slate AND no new value was recorded in the history,
        // skip the rebuild — the popup is already showing the right
        // thing, and a rebuild would flicker the size when ValueHistory
        // and a wider preview (HexDump, Disasm) coexist in the stack.
        // Using history.count (not heatLevel, which saturates at 3)
        // means new values past the saturation point still refresh the
        // popup content.
        QString fp;
        fp.reserve(64);
        fp += QString::number(lm.nodeId);
        if (ctx.history) {
            auto it = ctx.history->find(lm.nodeId);
            if (it != ctx.history->end()) {
                fp += '|';
                fp += QString::number(it->count);
            }
        }
        for (auto* p : eligible) { fp += '|'; fp += p->id(); }
        if (fp == m_lastFingerprint && isVisible()
            && m_eligible.size() == eligible.size()) {
            // Same slate — just keep showing what we have.
            return;
        }
        m_lastFingerprint = fp;

        // Clear previous widgets — preview-owned but we deleteLater so
        // they're gone before the next setEligible reuses the stack.
        while (m_content->count() > 0) {
            QWidget* w = m_content->widget(0);
            m_content->removeWidget(w);
            w->deleteLater();
        }
        m_eligible = eligible;
        m_lastLm = lm;
        for (auto* p : eligible) {
            QWidget* w = p->widget(lm, node, ctx, m_content);
            // widget() returning nullptr signals "eligible but no content
            // this tick" (e.g. read failed). Put a stub placeholder so
            // m_content->count() stays in sync with m_eligible.size().
            if (!w) {
                w = new QLabel(QStringLiteral("(no preview)"), m_content);
                w->setStyleSheet(QStringLiteral("color: %1;")
                                     .arg(ctx.theme->textMuted.name()));
            }
            m_content->addWidget(w);
        }
        if (activeIdx < 0 || activeIdx >= m_eligible.size())
            activeIdx = 0;
        m_active = -1;  // force activeChanged emission below
        applyHostTheme();
        setActiveIndex(activeIdx);
        const bool multi = m_eligible.size() > 1;
        m_dots->setVisible(multi);
        // Lock to the standard formula size. No shadow margin — the
        // popup is the frame itself (boxy 1 px border, no rounding).
        setFixedSize(m_standardSize);
    }

    void setActiveIndex(int idx) {
        if (idx < 0 || idx >= m_eligible.size()) return;
        if (idx == m_active) return;
        m_active = idx;
        m_content->setCurrentIndex(idx);
        refreshTitle();
        refreshDots();
        refreshFooter();
        if (m_onActiveChanged)
            m_onActiveChanged(m_lastLm, m_eligible[idx]->id());
    }

    void cycleNext() {
        if (m_eligible.size() < 2) return;
        setActiveIndex((m_active + 1) % m_eligible.size());
    }
    void cyclePrev() {
        if (m_eligible.size() < 2) return;
        setActiveIndex((m_active - 1 + m_eligible.size()) % m_eligible.size());
    }

    void dismiss() override {
        HoverPopup::dismiss();
        // Don't wipe m_eligible here — the next setEligible refreshes.
        // Letting the widgets linger means a re-show with the same
        // node+kind+state can skip rebuild (handled in setEligible's
        // future caching pass if needed).
    }


private:
    // Footer key letter: bold + accent color. No fake keycap boxing —
    // every other Reclass affordance label (command row pills, footer
    // buttons, dialog buttons) is plain text styled by indicator color,
    // and the popup should match.
    static QString keyTagHtml(const QString& label, const Theme& t) {
        return QStringLiteral(
            "<span style='color:%1;font-weight:bold;'>%2</span>")
            .arg(t.indHoverSpan.name(), label.toHtmlEscaped());
    }

    void refreshTitle() {
        if (m_active < 0 || m_active >= m_eligible.size()) return;
        const auto& t = ThemeManager::instance().current();
        const KindMeta* km = kindMeta(m_lastLm.nodeKind);
        QString kindStr = km ? QString::fromLatin1(km->typeName)
                             : QStringLiteral("field");
        // Prefer the preview's row-derived subtitle (e.g. the resolved
        // struct / vtable class name from lm.pointerTargetName) over
        // the generic tabLabel(). Previews opt in by overriding
        // subtitle(); falls back to tabLabel() when empty.
        QString preview = m_eligible[m_active]->subtitle(m_lastLm);
        if (preview.isEmpty())
            preview = m_eligible[m_active]->tabLabel();
        // "kind  ·  Preview Name" — kind in textDim, separator dot in
        // textMuted, preview name in accent (no bold; bold reads as
        // noise next to the editor's already-bold type identifiers).
        m_titleLabel->setText(QStringLiteral(
            "<span style='color:%1;'>%2</span>"
            "<span style='color:%3;'> &middot; </span>"
            "<span style='color:%4;'>%5</span>")
            .arg(t.textDim.name(), kindStr.toHtmlEscaped(),
                 t.textMuted.name(),
                 t.indHoverSpan.name(), preview.toHtmlEscaped()));
    }

    void refreshDots() {
        const auto& t = ThemeManager::instance().current();
        m_dots->setState(m_eligible.size(), m_active,
                         t.indHoverSpan, t.textMuted);
    }

    void refreshFooter() {
        const auto& t = ThemeManager::instance().current();
        const bool multi = m_eligible.size() > 1;
        QString html = QStringLiteral("<span style='color:%1;'>")
                           .arg(t.textMuted.name());
        if (multi) {
            html += keyTagHtml(QStringLiteral("Tab"), t);
            html += QStringLiteral(" cycle &nbsp; ");
        }
        html += keyTagHtml(QStringLiteral("Esc"), t);
        html += QStringLiteral(" dismiss");
        html += QStringLiteral("</span>");
        m_footerHint->setText(html);
    }

    void applyHostTheme() {
        if (!m_titleLabel || !m_footerHint) return;
        const auto& t = ThemeManager::instance().current();
        // Popup background + border palette. With QFrame::Box +
        // lineWidth=1 set in the ctor, the frame draws a 1 px border
        // in WindowText color around a backgroundAlt fill — matching
        // every other panel surface in the editor.
        QPalette pal;
        pal.setColor(QPalette::Window, t.backgroundAlt);
        pal.setColor(QPalette::WindowText, t.border);
        setPalette(pal);
        // Refresh dependent labels so they pick up any theme switch.
        if (m_active >= 0) {
            refreshTitle();
            refreshDots();
            refreshFooter();
        }
        QPalette sp;
        sp.setColor(QPalette::WindowText, t.border);
        m_separator->setPalette(sp);
    }

    QLabel*           m_titleLabel  = nullptr;
    PreviewDotsStrip* m_dots        = nullptr;
    QFrame*           m_separator   = nullptr;
    ActiveSizeStack*  m_content     = nullptr;
    QLabel*           m_footerHint  = nullptr;
    QVector<HoverPreview*> m_eligible;
    int               m_active = -1;
    int               m_shadowMargin = 0;
    LineMeta          m_lastLm;
    ActiveChangedFn   m_onActiveChanged;
    QSize             m_standardSize;
    QString           m_lastFingerprint;
};

// ── Concrete previews ──
//
// Each wraps the populate logic that used to live inline in the three
// editor-cpp hover blocks at lines 4697-5144. Eligibility encodes the
// kind-based conditions verbatim; the widget construction reuses the
// shared body-widget helpers above.

class ValueHistoryPreview : public HoverPreview {
public:
    QString id() const override { return QStringLiteral("value_history"); }
    QString tabLabel() const override { return QStringLiteral("Previous Values"); }
    bool eligible(const LineMeta& lm, const Node& node,
                  const HoverContext& ctx) const override {
        if (lm.lineKind != LineKind::Field) return false;
        if (node.kind == NodeKind::Struct || node.kind == NodeKind::Array) return false;
        if (isFuncPtr(node.kind)) return false;
        if (lm.heatLevel <= 0 || lm.nodeId == 0 || !ctx.history) return false;
        auto it = ctx.history->find(lm.nodeId);
        return it != ctx.history->end() && it->uniqueCount() > 1;
    }
    QWidget* widget(const LineMeta& lm, const Node& /*node*/,
                    const HoverContext& ctx, QWidget* parent) override {
        if (!ctx.history) return nullptr;
        auto it = ctx.history->find(lm.nodeId);
        if (it == ctx.history->end()) return nullptr;
        return buildValueHistoryBody(*it, ctx.editorFont, *ctx.theme, parent);
    }
};

// Helper: read pointer value at the row's address, return 0 if invalid
// (null, sentinel, unreadable). Shared between HexDump and Disasm.
static uint64_t readPointerAtRow(const LineMeta& lm, const HoverContext& ctx) {
    if (!ctx.dataProvider) return 0;
    bool is64 = (lm.nodeKind == NodeKind::FuncPtr64
                 || lm.nodeKind == NodeKind::Pointer64);
    uint64_t v = is64
        ? ctx.dataProvider->readU64(lm.offsetAddr)
        : (uint64_t)ctx.dataProvider->readU32(lm.offsetAddr);
    if (v == 0 || v == UINT64_MAX) return 0;
    if (!is64 && v == 0xFFFFFFFF) return 0;
    return v;
}

// Helper: cap a body string at kMaxLines newlines, appending "..." if
// the cap fired. Matches the existing "compact 6-line popup" behavior.
static QString capLines(QString body, int maxLines = 6) {
    int nth = 0, idx = 0;
    while (nth < maxLines && (idx = body.indexOf('\n', idx)) != -1) {
        ++nth; ++idx;
    }
    if (nth == maxLines && idx < body.size()) {
        body.truncate(idx);
        body += QStringLiteral("...");
    }
    return body;
}

class HexDumpPreview : public HoverPreview {
public:
    QString id() const override { return QStringLiteral("hex_dump"); }
    QString tabLabel() const override { return QStringLiteral("Hex Dump"); }
    bool eligible(const LineMeta& lm, const Node& node,
                  const HoverContext& ctx) const override {
        if (lm.nodeKind != NodeKind::Pointer32
            && lm.nodeKind != NodeKind::Pointer64) return false;
        if (!lm.pointerTargetName.isEmpty()) return false;  // typed → StructTarget
        if (node.refId != 0) return false;
        return readPointerAtRow(lm, ctx) != 0;
    }
    QWidget* widget(const LineMeta& lm, const Node& /*node*/,
                    const HoverContext& ctx, QWidget* parent) override {
        uint64_t target = readPointerAtRow(lm, ctx);
        if (target == 0) return nullptr;
        const Provider* readProv = ctx.codeProvider ? ctx.codeProvider : ctx.dataProvider;
        if (!readProv) return nullptr;
        constexpr int kMax = 128;
        QByteArray bytes(kMax, Qt::Uninitialized);
        if (!readProv->read(target, bytes.data(), kMax)) return nullptr;
        QString body = capLines(hexDump(bytes, target, kMax));
        if (body.isEmpty()) return nullptr;
        return buildTextBodyWidget(body, ctx.editorFont,
                                    ctx.theme->syntaxNumber, parent);
    }
};

class DisasmPreview : public HoverPreview {
public:
    QString id() const override { return QStringLiteral("disasm"); }
    QString tabLabel() const override { return QStringLiteral("Disassembly"); }
    bool eligible(const LineMeta& lm, const Node& /*node*/,
                  const HoverContext& ctx) const override {
        if (!isFuncPtr(lm.nodeKind)) return false;
        return readPointerAtRow(lm, ctx) != 0;
    }
    QWidget* widget(const LineMeta& lm, const Node& /*node*/,
                    const HoverContext& ctx, QWidget* parent) override {
        uint64_t target = readPointerAtRow(lm, ctx);
        if (target == 0) return nullptr;
        const Provider* readProv = ctx.codeProvider ? ctx.codeProvider : ctx.dataProvider;
        if (!readProv) return nullptr;
        constexpr int kMax = 128;
        QByteArray bytes(kMax, Qt::Uninitialized);
        if (!readProv->read(target, bytes.data(), kMax)) return nullptr;
        bool is64 = (lm.nodeKind == NodeKind::FuncPtr64);
        QString body = capLines(disassemble(bytes, target, is64 ? 64 : 32, kMax));
        if (body.isEmpty()) return nullptr;
        return buildTextBodyWidget(body, ctx.editorFont,
                                    ctx.theme->syntaxNumber, parent);
    }
};

class StructTargetPreview : public HoverPreview {
public:
    QString id() const override { return QStringLiteral("struct_target"); }
    QString tabLabel() const override { return QStringLiteral("Struct"); }
    // Surface the actual referent type instead of the generic
    // "Struct" label. lm.pointerTargetName is populated by compose for
    // every Pointer32/64 row whose target resolves (e.g. "rcx::RcxEditor"
    // for a vptr). Empty falls back to the generic label via the host.
    QString subtitle(const LineMeta& lm) const override {
        return lm.pointerTargetName;
    }
    bool eligible(const LineMeta& lm, const Node& node,
                  const HoverContext& ctx) const override {
        if (lm.nodeKind != NodeKind::Pointer32
            && lm.nodeKind != NodeKind::Pointer64) return false;
        if (lm.pointerTargetName.isEmpty()) return false;
        if (!lm.foldCollapsed) return false;
        if (node.refId == 0) return false;
        return ctx.tree && ctx.dataProvider;
    }
    QWidget* widget(const LineMeta& lm, const Node& node,
                    const HoverContext& ctx, QWidget* parent) override {
        if (!ctx.tree || !ctx.dataProvider) return nullptr;
        // compactColumns=true caps the type column at its natural width
        // and overflows long names rather than pre-padding to the layout
        // maximum — the popup is a narrow excerpt, not a full editor
        // pane, and the user explicitly called out the wide spacing in
        // the standard compose output as "expensive". Other compose
        // flags (treeLines, braceWrap, typeHints, showComments) stay
        // at their defaults so the popup body matches the editor's
        // visual language for the same fields.
        ComposeResult cr = rcx::compose(*ctx.tree, *ctx.dataProvider,
            node.refId, /*compactColumns=*/true);
        const QStringList lines = cr.text.split('\n');
        constexpr int kMaxLines = 5;

        // Two-pass build. Pass 1 collects up to kMaxLines candidate
        // rows (skipping continuation lines + blanks), records each
        // row's NodeKind, and pre-strips the leading whitespace + tree
        // connectors. Pass 2 detects whether every row shares the same
        // NodeKind — vtables and similar homogeneous structs do — and
        // if so, drops the now-redundant per-row type column and
        // promotes it to a single header line. Heterogeneous structs
        // keep their inline type column for disambiguation.
        struct Row { QString trimmed; NodeKind kind; uint64_t addr; };
        QVector<Row> rows;
        for (int i = 1; i < lines.size() && i < cr.meta.size()
                                          && rows.size() < kMaxLines; ++i) {
            const QString& line = lines[i];
            if (line.trimmed().isEmpty()) continue;
            const LineMeta& m = cr.meta[i];
            if (m.offsetText.isEmpty()) continue;  // continuation
            QString trimmed = line;
            int j = 0;
            while (j < trimmed.size()) {
                QChar c = trimmed[j];
                if (c.isSpace()
                    || c == QChar(u'├') || c == QChar(u'└')
                    || c == QChar(u'│') || c == QChar(u'▸')
                    || c == QChar(u'▾')) {
                    ++j;
                } else {
                    break;
                }
            }
            trimmed = trimmed.mid(j);
            rows.append({trimmed, m.nodeKind, m.offsetAddr});
        }
        if (rows.isEmpty()) return nullptr;

        // Homogeneous test: all visible rows the same nodeKind.
        bool homogeneous = true;
        for (int i = 1; i < rows.size(); ++i) {
            if (rows[i].kind != rows[0].kind) { homogeneous = false; break; }
        }

        // If homogeneous AND we have more than one row, strip the type
        // prefix from each row (it just repeats "fnptr64" / "hex64" /
        // etc.) and emit a single header showing the row count + shared
        // type. With only one row there's nothing to deduplicate, so
        // we leave the inline type column alone — a "[1 × fnptr64]"
        // header reads as pointless noise.
        QString header;
        if (homogeneous && rows.size() > 1) {
            const KindMeta* km = kindMeta(rows[0].kind);
            const QString typeName = km ? QString::fromLatin1(km->typeName)
                                        : QStringLiteral("field");
            header = QStringLiteral("[%1 × %2]\n")
                .arg(rows.size()).arg(typeName);
            for (auto& r : rows) {
                // Strip the leading type token + its trailing whitespace.
                if (r.trimmed.startsWith(typeName)) {
                    int k = typeName.size();
                    while (k < r.trimmed.size() && r.trimmed[k].isSpace()) ++k;
                    r.trimmed = r.trimmed.mid(k);
                }
            }
        }

        // Emit. Each row gets a +0xNN offset prefix; baseline is the
        // first row's absolute address so the column reads 0/8/10/18/20
        // for an 8-byte-stride layout.
        const uint64_t baseAddr = rows[0].addr;
        QString body = header;
        for (int i = 0; i < rows.size(); ++i) {
            const uint64_t slotOff = (rows[i].addr >= baseAddr)
                ? (rows[i].addr - baseAddr) : 0;
            const QString offCol = QStringLiteral("+0x")
                + QString::number(slotOff, 16).toUpper()
                    .rightJustified(2, QChar('0'));
            if (i > 0) body += '\n';
            body += offCol + QStringLiteral("  ") + rows[i].trimmed;
        }
        if (body.isEmpty()) return nullptr;
        return buildTextBodyWidget(body, ctx.editorFont,
                                    ctx.theme->text, parent);
    }
};

static constexpr int IND_EDITABLE   = 8;
static constexpr int IND_HEX_DIM    = 9;
static constexpr int IND_BASE_ADDR  = 10;  // Default text color override for command row address
static constexpr int IND_HOVER_SPAN = 11;  // Blue text on hover (link-like)
static constexpr int IND_CMD_PILL   = 12;  // Rounded chip behind command row spans
static constexpr int IND_HEAT_COLD    = 13; // Heatmap level 1 (changed once)
static constexpr int IND_CLASS_NAME   = 14; // Teal text for root class name
static constexpr int IND_HINT_GREEN   = 15; // Green text for hint/comment text
static constexpr int IND_LOCAL_OFF    = 16; // Dim text for inline local offset in relative mode
static constexpr int IND_HEAT_WARM    = 17; // Heatmap level 2 (moderate changes)
static constexpr int IND_HEAT_HOT     = 18; // Heatmap level 3 (frequent changes)
static constexpr int IND_FIND         = 19; // Search match highlight
static constexpr int IND_TYPE_HINT    = 20; // Dimmed type inference hint text on hex nodes
static constexpr int IND_RTTI_HINT    = 21; // Auto-detected RTTI vtable name hint (warm amber)
static constexpr int IND_CHIP_BG      = 22; // Rounded-box background painted under every tail chip
                                            // (Enum/TypeHint/Rtti/Comment) so they read as pills,
                                            // not just colored text. Kind-specific TEXTFORE colors
                                            // layer on top via the chip pass.
static constexpr int IND_CHIP_HOVER   = 23; // Lighter pill overlay on the chip the cursor is over.
                                            // Applied only to clickable chip kinds so the user can
                                            // see at a glance which chips respond to a click.
static constexpr int IND_CHIP_PRESSED = 24; // Darker pill overlay while the mouse button is held
                                            // down inside a clickable chip — the button-press
                                            // feedback that "your click is registering here".
static constexpr int IND_TREE_CONN    = 25; // Tree-connector glyphs (├ │ └) tinted theme.textDim.
                                            // Pulls the connector chars slightly down from the
                                            // default theme.text foreground so the hierarchy is
                                            // legible without competing for attention with the
                                            // actual field type/name/value content.
static constexpr int IND_EDIT_BOUNDS  = 27; // STRAIGHTBOX background fill on the byte ranges
                                            // covered by an active byte-range inline edit. Tints
                                            // the editable digits with a faded indHoverSpan so the
                                            // user can see where the edit zone starts and stops
                                            // (especially useful across rows, where the cursor
                                            // jumps segment-to-segment). Painted by beginByteEdit,
                                            // cleared by endInlineEdit. Distinct from IND_BYTE_SEL
                                            // (TEXTFORE foreground) which is for the not-yet-
                                            // editing selection state.
static constexpr int IND_BYTE_SEL     = 26; // Foreground recolor (TEXTFORE) on hex bytes that
                                            // are part of the drag-selection range. Uses
                                            // theme.indHoverSpan — the link/hover accent every
                                            // theme picks to mean "interactive/active text". The
                                            // earlier attempt at theme.selection failed because
                                            // that token is a background fill across every theme
                                            // (low-luminance dark values designed to sit behind
                                            // white-ish text); as a TEXTFORE it was invisible to
                                            // muddy. Higher slot number than IND_HEAT_* /
                                            // IND_HEX_DIM means selection wins over heat on
                                            // overlapping bytes — selection is an explicit user
                                            // action and should be the dominant visual signal.

static QString g_fontName = "JetBrains Mono";

static QFont editorFont() {
    QFont f(g_fontName, 12);
    f.setFixedPitch(true);
    return f;
}

RcxEditor::RcxEditor(QWidget* parent) : QWidget(parent) {
    PROFILE_SCOPE("RcxEditor::ctor");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_sci = new QsciScintilla(this);
    layout->addWidget(m_sci);

    // Find bar (hidden by default, shown with Ctrl+F)
    m_findBarContainer = new QWidget(this);
    auto* fbLayout = new QHBoxLayout(m_findBarContainer);
    fbLayout->setContentsMargins(4, 1, 4, 1);
    fbLayout->setSpacing(2);
    auto* findPrevBtn = new QToolButton(m_findBarContainer);
    findPrevBtn->setText(QStringLiteral("\u25C0"));
    findPrevBtn->setFixedSize(24, 24);
    auto* findNextBtn = new QToolButton(m_findBarContainer);
    findNextBtn->setText(QStringLiteral("\u25B6"));
    findNextBtn->setFixedSize(24, 24);
    auto* findCloseBtn = new QToolButton(m_findBarContainer);
    findCloseBtn->setText(QStringLiteral("\u2715"));
    findCloseBtn->setFixedSize(24, 24);
    m_findBar = new QLineEdit(m_findBarContainer);
    m_findBar->setPlaceholderText(QStringLiteral("Find..."));
    m_findBar->setFont(editorFont());
    m_findBar->setFixedHeight(24);
    fbLayout->addWidget(findPrevBtn);
    fbLayout->addWidget(findNextBtn);
    fbLayout->addWidget(findCloseBtn);
    fbLayout->addWidget(m_findBar);
    m_findBarContainer->setVisible(false);
    layout->addWidget(m_findBarContainer);

    setupScintilla();
    setupLexer();
    setupMargins();
    setupFolding();
    setupMarkers();
    allocateMarginStyles();

    applyTheme(ThemeManager::instance().current());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &RcxEditor::applyTheme);

    m_sci->installEventFilter(this);
    m_sci->viewport()->installEventFilter(this);
    m_sci->viewport()->setMouseTracking(true);

    // Find bar: indicator-based search (selection is disabled in our Scintilla).
    // doFind paints the indicator at *every* match in the document so the user
    // sees all hits at once, then advances the cursor to the next/prev match
    // from m_findPos.
    auto doFind = [this](bool forward) {
        long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, (long)IND_FIND);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, (long)0, docLen);

        QString text = m_findBar->text();
        if (text.isEmpty()) return;
        QByteArray needle = text.toUtf8();

        // Pass 1: paint every match so the highlight is global.
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS, (long)0);
        {
            long scan = 0;
            while (scan < docLen) {
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, scan);
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, docLen);
                long hit = m_sci->SendScintilla(QsciScintillaBase::SCI_SEARCHINTARGET,
                                                  (uintptr_t)needle.size(), needle.constData());
                if (hit < 0) break;
                m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, hit,
                                      (long)needle.size());
                scan = hit + qMax<long>(needle.size(), 1);
            }
        }

        // Pass 2: advance cursor to the next match from m_findPos.
        long startPos = forward ? m_findPos : (m_findPos > 0 ? m_findPos - 1 : docLen);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, startPos);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                             forward ? docLen : (long)0);
        long pos = m_sci->SendScintilla(QsciScintillaBase::SCI_SEARCHINTARGET,
                                         (uintptr_t)needle.size(), needle.constData());
        if (pos == -1) {  // wrap
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART,
                                 forward ? (long)0 : docLen);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND,
                                 forward ? startPos : (long)0);
            pos = m_sci->SendScintilla(QsciScintillaBase::SCI_SEARCHINTARGET,
                                         (uintptr_t)needle.size(), needle.constData());
        }
        if (pos >= 0) {
            int line = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION, pos);
            m_sci->ensureLineVisible(line);
            m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, pos);
            m_findPos = pos + (forward ? needle.size() : 0);
        }
    };
    connect(m_findBar, &QLineEdit::textChanged, this, [this, doFind]() { m_findPos = 0; doFind(true); });
    connect(m_findBar, &QLineEdit::returnPressed, this, [doFind]() { doFind(true); });
    connect(findNextBtn, &QToolButton::clicked, this, [doFind]() { doFind(true); });
    connect(findPrevBtn, &QToolButton::clicked, this, [doFind]() { doFind(false); });
    connect(findCloseBtn, &QToolButton::clicked, this, [this]() { hideFindBar(); });
    // Escape hides find bar
    {
        auto* escAction = new QAction(m_findBar);
        escAction->setShortcut(QKeySequence(Qt::Key_Escape));
        escAction->setShortcutContext(Qt::WidgetShortcut);
        m_findBar->addAction(escAction);
        connect(escAction, &QAction::triggered, this, [this]() { hideFindBar(); });
    }

    // Recalculate hover when the viewport scrolls (scrollbar drag, wheel
    // deceleration, etc.) so the highlight tracks whatever is under the cursor.
    connect(m_sci->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this]() {
        if (m_editState.active || !m_hoverInside) return;
        m_lastHoverPos = m_sci->viewport()->mapFromGlobal(QCursor::pos());
        m_hoverInside = m_sci->viewport()->rect().contains(m_lastHoverPos);
        auto h = hitTest(m_lastHoverPos);
        uint64_t newHoverId = (m_hoverInside && h.line >= 0) ? h.nodeId : 0;
        int newHoverLine = (m_hoverInside && h.line >= 0) ? h.line : -1;
        if (newHoverId != m_hoveredNodeId || newHoverLine != m_hoveredLine) {
            m_hoveredNodeId = newHoverId;
            m_hoveredLine = newHoverLine;
            applyHoverHighlight();
        }
        applyHoverCursor();
    });

    // Hover cursor is applied synchronously in eventFilter (no timer).

    connect(m_sci, &QsciScintilla::marginClicked,
            this, [this](int margin, int line, Qt::KeyboardModifiers mods) {
        emit marginClicked(margin, line, mods);
    });

    m_sci->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sci, &QWidget::customContextMenuRequested,
            this, [this](const QPoint& pos) {
        // Right-click on active byte selection (and the click landed
        // inside a hex preview row's value column) → byte-ops menu.
        // Checked first so the per-byte actions take precedence over
        // the node-level context menu when there's a live selection.
        if (m_byteSel.has_value()) {
            HitInfo bhi = hitTest(pos);
            if (byteAddrAt(bhi.line, bhi.col).has_value()) {
                QMenu menu;
                auto* aBreak   = menu.addAction(QStringLiteral("Break into new class"));
                menu.addSeparator();
                auto* aCopyHex = menu.addAction(QStringLiteral("Copy as hex"));
                aCopyHex->setShortcut(QKeySequence::Copy);
                auto* aCopyC   = menu.addAction(QStringLiteral("Copy as C array"));
                auto* aCopyPy  = menu.addAction(QStringLiteral("Copy as Python bytes"));
                auto* aSave    = menu.addAction(QStringLiteral("Save as binary file…"));
                menu.addSeparator();
                auto* aPaste   = menu.addAction(QStringLiteral("Paste hex"));
                aPaste->setShortcut(QKeySequence::Paste);
                auto* aEdit    = menu.addAction(QStringLiteral("Edit hex…"));
                auto* aZero    = menu.addAction(QStringLiteral("Zero-fill"));
                aZero->setShortcut(QKeySequence::Delete);
                menu.addSeparator();
                auto* aClear   = menu.addAction(QStringLiteral("Clear selection"));
                aClear->setShortcut(QKeySequence(Qt::Key_Escape));

                QAction* chosen = menu.exec(m_sci->mapToGlobal(pos));
                if (!chosen) return;
                if      (chosen == aCopyHex) emit byteCopyHexRequested();
                else if (chosen == aCopyC)   emit byteCopyAsCArrayRequested();
                else if (chosen == aCopyPy)  emit byteCopyAsPythonRequested();
                else if (chosen == aSave)    emit byteSaveAsFileRequested();
                else if (chosen == aPaste)   emit bytePasteHexRequested();
                else if (chosen == aEdit)    beginByteEdit();
                else if (chosen == aZero)    emit byteZeroFillRequested();
                else if (chosen == aBreak) {
                    auto [lo, hi] = *m_byteSel;
                    emit byteBreakIntoClassRequested(lo, hi);
                }
                else if (chosen == aClear)   {
                    m_byteSel.reset();
                    applyByteSelectionOverlay();
                }
                return;
            }
        }
        // Right-click on offset margin → show margin mode menu
        int margin0Width = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_GETMARGINWIDTHN, 0UL, 0L);
        if (pos.x() < margin0Width) {
            QMenu menu;
            auto* actRel = menu.addAction("Relative Offsets (+0x)");
            auto* actAbs = menu.addAction("Absolute Addresses");
            actRel->setCheckable(true);
            actAbs->setCheckable(true);
            actRel->setChecked(m_relativeOffsets);
            actAbs->setChecked(!m_relativeOffsets);
            QAction* chosen = menu.exec(m_sci->mapToGlobal(pos));
            if (chosen == actRel && !m_relativeOffsets) {
                m_relativeOffsets = true;
                reformatMargins();
                emit relativeOffsetsChanged(true);
            } else if (chosen == actAbs && m_relativeOffsets) {
                m_relativeOffsets = false;
                reformatMargins();
                emit relativeOffsetsChanged(false);
            }
            return;
        }
        HitInfo hi = hitTest(pos);
        int line = hi.line;

        // Right-click on command row keyword → show conversion menu
        if (line == 0 && hi.col >= 0 && !m_meta.isEmpty()
            && m_meta[0].lineKind == LineKind::CommandRow) {
            QString lineText = getLineText(m_sci, 0);
            ColumnSpan rts = commandRowRootTypeSpan(lineText);
            if (rts.valid && hi.col >= rts.start && hi.col < rts.end) {
                // Extract current keyword from span text
                QString kw = lineText.mid(rts.start, rts.end - rts.start).trimmed();
                QMenu menu;
                if (kw == QStringLiteral("class"))
                    menu.addAction("Convert to Struct");
                else if (kw == QStringLiteral("struct"))
                    menu.addAction("Convert to Class");
                // enum: no conversion options
                if (!menu.isEmpty()) {
                    QAction* chosen = menu.exec(m_sci->mapToGlobal(pos));
                    if (chosen) {
                        QString newKw = chosen->text().contains("Class")
                            ? QStringLiteral("class") : QStringLiteral("struct");
                        emit keywordConvertRequested(newKw);
                    }
                }
                return;
            }
        }

        int nodeIdx = -1;
        int subLine = 0;
        if (line >= 0 && line < m_meta.size()) {
            nodeIdx = m_meta[line].nodeIdx;
            subLine = m_meta[line].subLine;
        }
        emit contextMenuRequested(line, nodeIdx, subLine, m_sci->mapToGlobal(pos));
    });

    connect(m_sci, &QsciScintilla::userListActivated,
            this, [this](int id, const QString& text) {
        if (!m_editState.active) return;
        if (id == 1 && (m_editState.target == EditTarget::Type
                     || m_editState.target == EditTarget::ArrayElementType
                     || m_editState.target == EditTarget::PointerTarget)) {
            const LineMeta* lm = metaForLine(m_editState.line);
            uint64_t addr = lm ? lm->offsetAddr : 0;
            auto info = endInlineEdit();
            emit inlineEditCommitted(info.nodeIdx, info.subLine, info.target, text, addr);
        }
    });

    connect(m_sci, &QsciScintilla::cursorPositionChanged,
            this, [this](int line, int /*col*/) { updateEditableIndicators(line); });

    connect(m_sci, &QsciScintilla::textChanged, this, [this]() {
        if (!m_editState.active) return;
        if (m_updatingComment) return;  // Skip queuing during comment update
        if (m_editState.target == EditTarget::Value && !m_editState.hexOverwrite)
            QTimer::singleShot(0, this, &RcxEditor::validateEditLive);

        // Live expression result popup (BaseAddress + value edits with operators)
        if (m_exprEvaluator)
            QTimer::singleShot(0, this, [this]() { updateExprResultPopup(); });

        // Autocomplete for static field expressions — show field names as user types
        if (m_editState.target == EditTarget::StaticExpr && !m_staticCompletions.isEmpty()) {
            // Get word at cursor
            long pos = m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
            long wordStart = m_sci->SendScintilla(QsciScintillaBase::SCI_WORDSTARTPOSITION, pos, (long)1);
            int wordLen = (int)(pos - wordStart);
            if (wordLen >= 1) {
                QByteArray list = m_staticCompletions.join(' ').toUtf8();
                m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCSETSEPARATOR, (long)' ');
                m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCSHOW, (uintptr_t)wordLen, list.constData());
            }
        }
    });

    connect(m_sci, &QsciScintilla::selectionChanged,
            this, &RcxEditor::clampEditSelection);

    // Hover dwell timer for preview popups. Fires once after the cursor
    // has rested on the same (node, line) for the configured interval;
    // the timeout sets m_hoverDwellElapsed and re-enters applyHoverCursor
    // so the matching popup can finally show. RcxTooltip is intentionally
    // not gated (see editor.h comment).
    m_hoverDwellTimer = new QTimer(this);
    m_hoverDwellTimer->setSingleShot(true);
    // 700 ms: long enough that casual mouse-overs while browsing don't
    // pop anything, short enough that "I'm actually inspecting this
    // pointer" doesn't feel laggy. Standard OS tooltip is 500 ms; we
    // sit slightly above because these popups are content-rich.
    m_hoverDwellTimer->setInterval(700);
    connect(m_hoverDwellTimer, &QTimer::timeout, this, [this]() {
        m_hoverDwellElapsed = true;
        applyHoverCursor();
    });

    // ── Preview registry + unified hover host ──
    // Registration order is the default ordering when no QSettings
    // preference is recorded yet. Adding a new preview kind (Matrix,
    // VectorAngle, ColorSwatch, …) is a single line below.
    m_previewRegistry = std::make_unique<HoverPreviewRegistry>();
    m_previewRegistry->add(std::make_unique<StructTargetPreview>());
    m_previewRegistry->add(std::make_unique<DisasmPreview>());
    m_previewRegistry->add(std::make_unique<HexDumpPreview>());
    m_previewRegistry->add(std::make_unique<ValueHistoryPreview>());

    auto* host = new HoverPopupHost(this, editorFont());
    m_popupHost = host;
    // When cursor enters the host's geometry, forward move events back
    // into the editor's hover state so the popup doesn't dismiss while
    // the user moves over it. Same pattern as the three previous popups
    // (their identical setOnMouseMove handlers used to live inline in
    // editor.cpp lines 5268, 5365, 5441 — now consolidated here).
    host->setOnMouseMove([this](QMouseEvent* e) {
        QPoint gp = e->globalPosition().toPoint();
        QPoint vp = m_sci->viewport()->mapFromGlobal(gp);
        m_lastHoverPos = vp;
        m_hoverInside = m_sci->viewport()->rect().contains(vp);
        if (!m_editState.active) {
            auto h2 = hitTest(m_lastHoverPos);
            uint64_t nid = (m_hoverInside && h2.line >= 0) ? h2.nodeId : 0;
            int nln = (m_hoverInside && h2.line >= 0) ? h2.line : -1;
            if (nid != m_hoveredNodeId || nln != m_hoveredLine) {
                m_hoveredNodeId = nid;
                m_hoveredLine = nln;
                applyHoverHighlight();
            }
        }
        applyHoverCursor();
    });
    host->setOnActiveChanged([](const LineMeta& lm, QString id) {
        const KindMeta* km = kindMeta(lm.nodeKind);
        if (!km) return;
        QSettings("Reclass","Reclass").setValue(
            QStringLiteral("hoverPreview/kind/") + QString::fromLatin1(km->name),
            id);
    });
}

RcxEditor::~RcxEditor() {
    delete m_exprResultLabel;
}

void RcxEditor::setupScintilla() {
    m_sci->setFont(editorFont());

    m_sci->setReadOnly(true);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->setCaretLineVisible(false);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETWIDTH, 0);

    // Arrow cursor by default — not the I-beam (this is a structured viewer, not a text editor)
    m_sci->viewport()->setCursor(Qt::ArrowCursor);

    m_sci->setTabWidth(2);
    m_sci->setIndentationsUseTabs(false);

    // Line spacing for readability. Compact mode drops the extra ascent /
    // descent to 1/0 so users with tall structs can fit more rows per
    // screen at the cost of slightly tighter visual rhythm. QSettings
    // key "compactRowSpacing" (View menu toggle in a follow-up) — defaults
    // to false / standard spacing.
    {
        QSettings s("Reclass", "Reclass");
        const bool compact = s.value("compactRowSpacing", false).toBool();
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRAASCENT,
                             (long)(compact ? 1 : 4));
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETEXTRADESCENT,
                             (long)(compact ? 0 : 2));
    }

    // Disable native selection rendering — we use markers for selection
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELFORE, (long)0, (long)0);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELBACK, (long)0, (long)0);

    // Horizontal scrollbar: sized explicitly in applyDocument() to match content
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTHTRACKING, 0);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH, 1);

    // Vertical scrollbar: don't allow scrolling past the last line
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETENDATLASTLINE, 1);

    // Cache entire document layout to avoid re-measuring lines across styling passes
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETLAYOUTCACHE, 3L);  // SC_CACHE_DOCUMENT

    // Editable-field indicator - HIDDEN (no visual)
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_EDITABLE, 5 /*INDIC_HIDDEN*/);

    // Hex node dim indicator — overrides text color
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HEX_DIM, 17 /*INDIC_TEXTFORE*/);

    // Tree connector dim — same INDIC_TEXTFORE but at theme.textDim (not
    // textFaint like IND_HEX_DIM) so the ├ │ └ glyphs are clearly
    // legible while still receding behind the actual content.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_TREE_CONN, 17 /*INDIC_TEXTFORE*/);

    // Base address indicator — text color override on command row
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_BASE_ADDR, 17 /*INDIC_TEXTFORE*/);

    // Hover span indicator — link-like text
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HOVER_SPAN, 17 /*INDIC_TEXTFORE*/);

    // Command-row pill background
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_CMD_PILL, 8 /*INDIC_STRAIGHTBOX*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA,
                         IND_CMD_PILL, (long)100);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_CMD_PILL, (long)1);

    // Tail-chip pill background — STRAIGHTBOX styled to match the footer
    // buttons (+1, +10h, Trim, Top) so chips and footer affordances read
    // as the same kind of "pill button". Same alpha/outline as IND_CMD_PILL
    // → identical fill saturation and edge weight. The earlier high-
    // outline-alpha look made chips read as outlined badges (different
    // visual language from the footer); matching them unifies the UI.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_CHIP_BG, 8 /*INDIC_STRAIGHTBOX*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA,
                         IND_CHIP_BG, (long)100);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_CHIP_BG, (long)1);

    // Chip hover overlay — additive brighten over the base pill so the
    // chip the cursor is on glows like a hovered button.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_CHIP_HOVER, 8 /*INDIC_STRAIGHTBOX*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA,
                         IND_CHIP_HOVER, (long)130);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_CHIP_HOVER, (long)1);

    // Chip pressed overlay — darker fill so the pill "sinks in" while
    // the mouse button is held (button-press feedback).
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_CHIP_PRESSED, 8 /*INDIC_STRAIGHTBOX*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA,
                         IND_CHIP_PRESSED, (long)200);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_CHIP_PRESSED, (long)1);

    // Hex byte selection. INDIC_TEXTFORE so the digits themselves take
    // theme.selection colour — reads like selected text in a code
    // editor. Lives at slot 26 (above IND_HEAT_HOT at 18 and
    // IND_HEX_DIM at 9) so Scintilla picks our colour over heat /
    // dim on bytes where both apply.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_BYTE_SEL, 17 /*INDIC_TEXTFORE*/);

    // Edit-bounds background fill (active byte-range inline edit).
    // STRAIGHTBOX at near-full alpha so the byte range visually reads
    // as a "selected node row" background — matches the M_SELECTED
    // marker's opaque fill at the line level. UNDER=1 paints behind
    // text so typed digits sit cleanly on top.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_EDIT_BOUNDS, 8 /*INDIC_STRAIGHTBOX*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETALPHA,
                         IND_EDIT_BOUNDS, (long)255);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_EDIT_BOUNDS, (long)1);

    // Heatmap indicators (cold / warm / hot)
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HEAT_COLD, 17 /*INDIC_TEXTFORE*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HEAT_WARM, 17 /*INDIC_TEXTFORE*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HEAT_HOT, 17 /*INDIC_TEXTFORE*/);

    // Root class name — type color
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_CLASS_NAME, 17 /*INDIC_TEXTFORE*/);

    // Green text for hint/comment annotations
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_HINT_GREEN, 17 /*INDIC_TEXTFORE*/);

    // Local offset text color (dim, like margin text)
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_LOCAL_OFF, 17 /*INDIC_TEXTFORE*/);

    // Type inference hint — dimmed text appended to hex lines
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_TYPE_HINT, 17 /*INDIC_TEXTFORE*/);

    // RTTI hint — warm amber text appended after typeHint when a vtable
    // is auto-detected. Distinct color so it stands out as "this is real
    // RTTI, not a generic guess."
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_RTTI_HINT, 17 /*INDIC_TEXTFORE*/);

    // Find match highlight — thick underline (avoids box rendering artifacts)
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETSTYLE,
                         IND_FIND, 14 /*INDIC_COMPOSITIONTHICK*/);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETUNDER,
                         IND_FIND, (long)1);
}

void RcxEditor::setupLexer() {
    m_lexer = new QsciLexerCPP(m_sci);
    QFont font = editorFont();
    m_lexer->setFont(font);
    for (int i = 0; i <= 127; i++)
        m_lexer->setFont(font, i);

    m_sci->setLexer(m_lexer);
    m_sci->setBraceMatching(QsciScintilla::NoBraceMatch);  // Disable - this is a structured viewer

    // Add built-in type names to keyword set 1 → blue coloring
    QByteArray kw2 = allTypeNamesForUI(/*stripBrackets=*/true).join(' ').toLatin1();
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETKEYWORDS,
                         (uintptr_t)1, kw2.constData());
}

void RcxEditor::setCustomTypeNames(const QStringList& names) {
    m_customTypeNames = names;
    QByteArray kw = names.join(' ').toLatin1();
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETKEYWORDS,
                         (uintptr_t)3, kw.constData());
}

void RcxEditor::setupMargins() {
    m_sci->setMarginsFont(editorFont());

    // Margin 0: Offset text
    m_sci->setMarginType(0, QsciScintilla::TextMarginRightJustified);
    m_sci->setMarginWidth(0, "  00000000  ");  // default 8-digit; resized dynamically in applyDocument()
    m_sci->setMarginSensitivity(0, true);

    // Margin 1: 2px accent bar (selection indicator)
    m_sci->setMarginType(1, QsciScintilla::SymbolMargin);
    m_sci->setMarginWidth(1, 2);
    m_sci->setMarginSensitivity(1, false);
    m_sci->setMarginMarkerMask(1, 1 << M_ACCENT);
}

void RcxEditor::setupFolding() {
    // Hide fold margin (fold indicators are text-based now)
    m_sci->setMarginWidth(2, 0);

    // Fold indicators are now text in the line content (kFoldCol prefix),
    // so no Scintilla markers needed for fold state.

    // Keep Scintilla fold markers invisible (fold levels still used for click detection)
    for (int i = 25; i <= 31; i++)
        m_sci->markerDefine(QsciScintilla::Invisible, i);

    // Disable automatic fold toggle — we handle collapse at model level
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETAUTOMATICFOLD,
                         (unsigned long)0);

    // Disable lexer-driven folding — we set fold levels manually
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETPROPERTY,
                         (const char*)"fold", (const char*)"0");
}

void RcxEditor::setupMarkers() {
    // M_CONT (0): continuation line (metadata only, no visual)
    m_sci->markerDefine(QsciScintilla::Invisible, M_CONT);

    // M_PTR0 (2): right triangle
    m_sci->markerDefine(QsciScintilla::RightTriangle, M_PTR0);

    // M_CYCLE (3): arrows
    m_sci->markerDefine(QsciScintilla::ThreeRightArrows, M_CYCLE);

    // M_ERR (4): background
    m_sci->markerDefine(QsciScintilla::Background, M_ERR);

    // M_STRUCT_BG (5): struct header/footer
    m_sci->markerDefine(QsciScintilla::Background, M_STRUCT_BG);

    // M_HOVER (6): full-row hover highlight
    m_sci->markerDefine(QsciScintilla::Background, M_HOVER);

    // M_SELECTED (7): full-row selection highlight
    m_sci->markerDefine(QsciScintilla::Background, M_SELECTED);

    // M_CMD_ROW (8): distinct background for CommandRow bar
    m_sci->markerDefine(QsciScintilla::Background, M_CMD_ROW);

    // M_ACCENT (9): 2px accent bar in margin 1 (selection indicator)
    m_sci->markerDefine(QsciScintilla::FullRectangle, M_ACCENT);

    // M_FOCUS (10): presentation mode AI focus glow
    m_sci->markerDefine(QsciScintilla::Background, M_FOCUS);
}

void RcxEditor::allocateMarginStyles() {
    static constexpr int MSTYLE_NORMAL = 0;
    static constexpr int MSTYLE_CONT   = 1;

    long base = m_sci->SendScintilla(QsciScintillaBase::SCI_ALLOCATEEXTENDEDSTYLES, (long)2);
    m_marginStyleBase = (int)base;
    m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETSTYLEOFFSET, base);

    QByteArray fontName = editorFont().family().toUtf8();
    int fontSize = editorFont().pointSize();

    for (int s = MSTYLE_NORMAL; s <= MSTYLE_CONT; s++) {
        unsigned long abs = (unsigned long)(base + s);
        m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT,
                             (uintptr_t)abs, fontName.constData());
        m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETSIZE, abs, (long)fontSize);
    }
}

void RcxEditor::applyTheme(const Theme& theme) {
    // Editor uses a slightly darker background than chrome for visual depth
    const QColor editorBg = theme.background.darker(115);

    // Paper and text
    m_sci->setPaper(editorBg);
    m_sci->setColor(theme.text);
    m_sci->setCaretForegroundColor(theme.text);

    // Indicator colors
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HEX_DIM, theme.textFaint);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_TREE_CONN, theme.textDim);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_BASE_ADDR, theme.text);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HOVER_SPAN, theme.indHoverSpan);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_CMD_PILL, theme.indCmdPill);
    // Heatmap colors
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HEAT_COLD, theme.indHeatCold);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HEAT_WARM, theme.indHeatWarm);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HEAT_HOT, theme.indHeatHot);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_CLASS_NAME, theme.syntaxType);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_HINT_GREEN, theme.indHintGreen);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_LOCAL_OFF, theme.textFaint);
    // TypeHint chip ("[ptr64]", "[float×2]") uses textDim so it reads
    // as annotation, not data. The chip used to be syntaxType (the
    // exact same teal as pointer values), which made the eye land on
    // the chip and the value with equal weight — chip-as-label only
    // works if it's clearly subordinate to the value it annotates.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_TYPE_HINT, theme.textDim);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_RTTI_HINT, theme.indRttiHint);
    // Chip pill background — derive a subtle fill from the existing
    // command-row pill color so it picks up theme changes automatically
    // and never collides with the foreground/text colors.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_CHIP_BG, theme.indCmdPill);
    // Chip hover/pressed overlays reuse the row hover + selection theme
    // colors so they stay coherent with everything else that lights up
    // under the cursor in this editor.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_CHIP_HOVER, theme.hover);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_CHIP_PRESSED, theme.selected);
    // Hex byte selection — re-uses the link/hover accent. Every theme
    // already vets this token for "interactive text foreground", so it
    // pops against the editor bg in every shipped theme without needing
    // a new field. theme.selection (originally tried here) is a
    // background fill colour — invisible to muddy as a TEXTFORE in every
    // dark theme.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_BYTE_SEL, theme.indHoverSpan);
    // Edit-bounds background: theme.selected — the same neutral
    // highlight used for selected-node row backgrounds (M_SELECTED
    // marker). Keeps the edit zone visually subdued and consistent
    // with the rest of the editor's "this is selected" language;
    // avoids the loud accent that an indHoverSpan fill would produce.
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_EDIT_BOUNDS, theme.selected);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICSETFORE,
                         IND_FIND, theme.borderFocused);
    // Lexer colors
    m_lexer->setColor(theme.syntaxKeyword, QsciLexerCPP::Keyword);
    m_lexer->setColor(theme.syntaxKeyword, QsciLexerCPP::KeywordSet2);
    m_lexer->setColor(theme.syntaxNumber, QsciLexerCPP::Number);
    m_lexer->setColor(theme.syntaxString, QsciLexerCPP::DoubleQuotedString);
    m_lexer->setColor(theme.syntaxString, QsciLexerCPP::SingleQuotedString);
    m_lexer->setColor(theme.syntaxComment, QsciLexerCPP::Comment);
    m_lexer->setColor(theme.syntaxComment, QsciLexerCPP::CommentLine);
    m_lexer->setColor(theme.syntaxComment, QsciLexerCPP::CommentDoc);
    m_lexer->setColor(theme.text, QsciLexerCPP::Default);
    m_lexer->setColor(theme.text, QsciLexerCPP::Identifier);
    m_lexer->setColor(theme.syntaxPreproc, QsciLexerCPP::PreProcessor);
    m_lexer->setColor(theme.text, QsciLexerCPP::Operator);
    m_lexer->setColor(theme.syntaxType, QsciLexerCPP::GlobalClass);
    for (int i = 0; i <= 127; i++)
        m_lexer->setPaper(editorBg, i);

    // Margins
    m_sci->setMarginsBackgroundColor(editorBg);
    m_sci->setMarginsForegroundColor(theme.textFaint);
    m_sci->setFoldMarginColors(editorBg, editorBg);

    // Markers
    m_sci->setMarkerBackgroundColor(theme.markerPtr, M_PTR0);
    m_sci->setMarkerForegroundColor(theme.markerPtr, M_PTR0);
    m_sci->setMarkerBackgroundColor(editorBg, M_CYCLE);
    m_sci->setMarkerForegroundColor(editorBg, M_CYCLE);
    m_sci->setMarkerBackgroundColor(theme.markerError, M_ERR);
    m_sci->setMarkerForegroundColor(theme.text, M_ERR);
    m_sci->setMarkerBackgroundColor(editorBg, M_STRUCT_BG);
    m_sci->setMarkerForegroundColor(theme.text, M_STRUCT_BG);
    m_sci->setMarkerBackgroundColor(theme.hover, M_HOVER);
    m_sci->setMarkerBackgroundColor(theme.selected, M_SELECTED);
    m_sci->setMarkerBackgroundColor(editorBg, M_CMD_ROW);
    m_sci->setMarkerBackgroundColor(theme.indHoverSpan, M_ACCENT);
    m_sci->setMarkerBackgroundColor(theme.focusGlow, M_FOCUS);
    m_focusGlowColor = theme.focusGlow;

    // Margin extended styles
    if (m_marginStyleBase >= 0) {
        long base = m_marginStyleBase;
        for (int s = 0; s <= 1; s++) {
            unsigned long abs = (unsigned long)(base + s);
            m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETFORE,
                                 abs, theme.textFaint);
            m_sci->SendScintilla(QsciScintillaBase::SCI_STYLESETBACK,
                                 abs, editorBg);
        }
    }

    // Find bar
    if (m_findBarContainer) {
        m_findBar->setStyleSheet(
            QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3;"
                            " padding: 2px 6px; font-size: 13px; }"
                            "QLineEdit:focus { border-color: %4; }")
                .arg(theme.backgroundAlt.name(), theme.text.name(),
                     theme.border.name(), theme.borderFocused.name()));
        m_findBarContainer->setStyleSheet(
            QStringLiteral("QToolButton { background: %1; color: %2; border: 1px solid %3; border-radius: 2px; }"
                            "QToolButton:hover { background: %4; }"
                            "QToolButton:pressed { background: %5; }")
                .arg(theme.background.name(), theme.text.name(), theme.border.name(),
                     theme.hover.name(), theme.backgroundAlt.name()));
    }
}

void RcxEditor::applyDocument(const ComposeResult& result) {
    PROFILE_SCOPE("applyDocument");
    // Silently deactivate inline edit (no signal — refresh is already happening)
    if (m_editState.active)
        endInlineEdit();

    // Guard: suppress popup dismiss during setText() which fires synthetic Leave events
    m_applyingDocument = true;

    // Save hover state — setText() triggers viewport Leave events that would clear it
    uint64_t savedHoverId = m_hoveredNodeId;
    int savedHoverLine = m_hoveredLine;
    bool savedHoverInside = m_hoverInside;

    m_meta = result.meta;
    m_layout = result.layout;

    // Build nodeId → display-line index for O(1) hover/selection lookup
    m_nodeLineIndex.clear();
    m_nodeLineIndex.reserve(m_meta.size());
    for (int i = 0; i < m_meta.size(); i++) {
        if (m_meta[i].nodeId != 0)
            m_nodeLineIndex[m_meta[i].nodeId].append(i);
    }

    // Dynamically resize margin to fit the current hex digit tier
    // RVA mode uses half width since relative offsets are much shorter
    {
        int marginDigits = m_relativeOffsets
            ? qMax(m_layout.offsetHexDigits / 2, 4)
            : m_layout.offsetHexDigits;
        QString marginSizer = QString("  %1  ").arg(QString(marginDigits, '0'));
        m_sci->setMarginWidth(0, marginSizer);
    }

    bool didPatch = false;
    long patchByteStart = 0;
    long patchByteLen = 0;
    {
        PROFILE_SCOPE("applyDocument.setText");
        // Diff-and-patch: when the previous-frame text exists and the new
        // text shares a common head/tail, replace only the differing
        // middle. Common case during rapid editing (append at end, value
        // tick, single-field mutation) is one or two changed lines.
        // Falls back to full setText only when the diff covers >50% of
        // the document or the previous text was empty.
        const QString& newText = result.text;
        if (!m_prevText.isEmpty()
            && m_prevText.size() > 0 && newText.size() > 0) {
            PROFILE_SCOPE("applyDocument.diff");
            const int oldN = m_prevText.size();
            const int newN = newText.size();

            // Find longest common prefix (in chars) — but stop at a line
            // boundary so the head/tail boundaries are clean.
            int prefix = 0;
            int maxScan = qMin(oldN, newN);
            while (prefix < maxScan && m_prevText[prefix] == newText[prefix])
                prefix++;
            // Walk back to the previous '\n' (or 0) so we patch whole lines.
            while (prefix > 0 && m_prevText[prefix - 1] != '\n') prefix--;

            // Find longest common suffix the same way.
            int oldEnd = oldN, newEnd = newN;
            while (oldEnd > prefix && newEnd > prefix
                   && m_prevText[oldEnd - 1] == newText[newEnd - 1]) {
                oldEnd--; newEnd--;
            }
            // Walk forward past the next '\n' to align the diff end on a
            // line boundary — but ONLY when we're currently mid-line. If
            // the suffix walk already left us at the start of a line
            // (prev_text[oldEnd-1] == '\n', i.e. the prior char is a line
            // break), do NOT extend forward; otherwise we'd swallow the
            // entire next line into the diff even though it's unchanged
            // (the bug we hit at the bottom of large structs where the
            // footer line ate everything before it).
            bool atLineStart = (oldEnd == 0) || (oldEnd >= oldN)
                               || (m_prevText[oldEnd - 1] == '\n');
            if (!atLineStart) {
                while (oldEnd < oldN && newEnd < newN
                       && m_prevText[oldEnd] != '\n') {
                    if (m_prevText[oldEnd] != newText[newEnd]) break;
                    oldEnd++; newEnd++;
                }
                if (oldEnd < oldN && newEnd < newN
                    && m_prevText[oldEnd] == newText[newEnd]) {
                    oldEnd++; newEnd++;  // include the \n itself
                }
            }

            int oldDiffLen = oldEnd - prefix;
            int newDiffLen = newEnd - prefix;
            // Only patch when the change is small enough to be worth it.
            // Threshold: change covers <= 50% of document size.
            int worstCase = qMax(oldDiffLen, newDiffLen);
            if (worstCase >= 0 && worstCase <= newN / 2) {
                PROFILE_SCOPE("applyDocument.patch");
                m_sci->setReadOnly(false);
                // Convert char offsets to byte offsets — Scintilla works
                // in bytes (UTF-8). Avoid `m_prevText.left(N).toUtf8().size()`:
                // that allocates an O(N) substring + an O(N) UTF-8 buffer just
                // to count bytes. On a 10K-char doc, two of those per refresh
                // dominated the patch path (~600 µs). Count UTF-8 bytes inline.
                auto utf8ByteCount = [](const QChar* d, int n) -> long {
                    long bytes = 0;
                    for (int i = 0; i < n; ++i) {
                        ushort u = d[i].unicode();
                        if (u < 0x80) bytes += 1;
                        else if (u < 0x800) bytes += 2;
                        else if (u >= 0xD800 && u < 0xDC00) {
                            bytes += 4; ++i;  // high surrogate consumes low
                        } else bytes += 3;
                    }
                    return bytes;
                };
                const QChar* prevData = m_prevText.constData();
                long bytePrefix = utf8ByteCount(prevData, prefix);
                long byteOldEnd = bytePrefix
                    + utf8ByteCount(prevData + prefix, oldEnd - prefix);
                QByteArray replacement = newText.mid(prefix, newDiffLen).toUtf8();
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, bytePrefix);
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, byteOldEnd);
                m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                     (uintptr_t)replacement.size(),
                                     replacement.constData());
                m_sci->setReadOnly(true);
                didPatch = true;
                patchByteStart = bytePrefix;
                patchByteLen = replacement.size();
                // If the patch covers line 0, it just stomped Scintilla's
                // command row with compose's placeholder ("[▸] source▾ …").
                // Invalidate the setCommandRowText skip-cache so the
                // controller's updateCommandRow() actually repaints line 0
                // afterward. Without this clear, the cache says "we
                // already wrote that" and the user sees the placeholder
                // flash for as long as it takes for the *next* refresh
                // whose command row genuinely differs. This was the
                // ~1s "source" flash after clicking a chip.
                if (bytePrefix == 0) m_lastCommandRowText.clear();
            }
        }
        if (!didPatch) {
            PROFILE_SCOPE("applyDocument.fullReplace");
            m_sci->setReadOnly(false);
            m_sci->setText(newText);
            m_sci->setReadOnly(true);
            // Full-replace just rewrote line 0 to compose's literal
            // "[▸] source▾  0x0  struct Untitled {" placeholder. Invalidate
            // the setCommandRowText skip-cache so the controller's
            // updateCommandRow() that runs next is forced to re-paint
            // line 0 with the proper text.
            m_lastCommandRowText.clear();
        }
        m_prevText = newText;
        m_lastApplyWasPatch = didPatch;
    }

    // Set horizontal scroll width to match the longest line. compose()
    // already tracked the longest non-trailing-space line length while
    // building text — reuse it instead of re-scanning the entire buffer.
    {
        QFontMetrics fm(editorFont());
        int pixelWidth = fm.horizontalAdvance(QString(result.maxLineLen, QChar('0')));
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSCROLLWIDTH,
                             (unsigned long)qMax(1, pixelWidth));

        // Reset horizontal scroll to 0.  The controller's restoreViewState()
        // will set it back to the (clamped) saved position afterward.
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, (unsigned long)0);
    }

    // Force re-lex of the patched range only (or the full doc on
    // fullReplace). Colouring full-doc on every refresh was 3 ms / call
    // — accounting for ~20% of refresh time on big structs.
    {
        PROFILE_SCOPE("applyDocument.colourise");
        if (didPatch) {
            m_sci->SendScintilla(QsciScintillaBase::SCI_COLOURISE,
                                 (uintptr_t)patchByteStart,
                                 (long)(patchByteStart + patchByteLen));
        } else {
            m_sci->SendScintilla(QsciScintillaBase::SCI_COLOURISE, (uintptr_t)0, (long)-1);
        }
    }

    // Compute changed line range by comparing new meta against m_prevMeta.
    // When small, per-line passes (line attributes, hex dim, heatmap,
    // indicators) operate only on that range — markers/indicators on
    // surrounding lines are preserved across SCI_REPLACETARGET, so
    // there's no need to clear-and-rebuild the entire document.
    int firstChanged = -1, lastChanged = -1;
    if (m_lastApplyWasPatch && !m_prevMeta.isEmpty()) {
        PROFILE_SCOPE("applyDocument.metaDiff");
        const int newN = result.meta.size();
        const int oldN = m_prevMeta.size();
        // Equivalence: same visual state on this line. ANY field consumed
        // by the per-line passes (applyLineAttributes, applyHexDimming,
        // applyHeatmapHighlight, applySymbolColoring, indicator/marker
        // re-paint loops) must be compared here — otherwise a kind change
        // (e.g. Hex8 → Int8 via the U/S/F/P shortcuts) would slip through
        // unchanged, leaving stale IND_HEX_DIM coloring on the new type.
        auto sameLine = [](const LineMeta& a, const LineMeta& b) {
            return a.nodeId == b.nodeId
                && a.subLine == b.subLine
                && a.lineKind == b.lineKind
                && a.nodeKind == b.nodeKind
                && a.elementKind == b.elementKind
                && a.foldLevel == b.foldLevel
                && a.markerMask == b.markerMask
                && a.depth == b.depth
                && a.foldHead == b.foldHead
                && a.foldCollapsed == b.foldCollapsed
                && a.braceCol == b.braceCol
                && a.isContinuation == b.isContinuation
                && a.isRootHeader == b.isRootHeader
                && a.isArrayHeader == b.isArrayHeader
                && a.isArrayElement == b.isArrayElement
                && a.isMemberLine == b.isMemberLine
                && a.isStaticLine == b.isStaticLine
                && a.heatLevel == b.heatLevel
                && a.chips.size() == b.chips.size()
                && std::equal(a.chips.cbegin(), a.chips.cend(), b.chips.cbegin(),
                              [](const LineChip& x, const LineChip& y) {
                                  return x.kind == y.kind
                                      && x.startCol == y.startCol
                                      && x.endCol   == y.endCol
                                      && x.text     == y.text;
                              })
                && a.lineByteCount == b.lineByteCount
                && a.effectiveTypeW == b.effectiveTypeW
                && a.effectiveNameW == b.effectiveNameW
                && a.pointerTargetName == b.pointerTargetName;
        };
        int first = 0;
        while (first < newN && first < oldN
               && sameLine(m_prevMeta[first], result.meta[first]))
            ++first;
        if (first < newN || newN != oldN) {
            int last = newN - 1, oldLast = oldN - 1;
            while (last >= first && oldLast >= first
                   && sameLine(m_prevMeta[oldLast], result.meta[last])) {
                --last; --oldLast;
            }
            firstChanged = first;
            lastChanged = qMax(last, first);  // at least one entry
        }
    }

    // Clear TEXTFORE indicators across the whole doc. Narrowing this to
    // [firstChanged, lastChanged] caused old hex64 lines to lose their
    // IND_HEX_DIM in production (Scintilla edge case: indicators don't
    // always survive REPLACETARGET as cleanly as the docs imply when
    // the patch lands at a line boundary on a styled buffer). The
    // narrow-marker path below still saves the expensive markerAdd
    // loop; indicators are cheap (microseconds even on 1000-line
    // structs) so always-full is the correct default here.
    {
        PROFILE_SCOPE("applyDocument.clearIndicators");
        long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
        for (int ind : {IND_HEX_DIM, IND_BASE_ADDR, IND_HOVER_SPAN, IND_HEAT_COLD,
                        IND_CLASS_NAME, IND_HINT_GREEN, IND_LOCAL_OFF, IND_HEAT_WARM,
                        IND_HEAT_HOT, IND_TYPE_HINT, IND_RTTI_HINT, IND_CHIP_BG,
                        IND_CHIP_HOVER, IND_CHIP_PRESSED, IND_TREE_CONN,
                        IND_BYTE_SEL, IND_EDIT_BOUNDS}) {
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, (long)ind);
            m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, (long)0, docLen);
        }
        // Reset hover/pressed bookkeeping — the chip the cursor *was* on
        // may have moved or disappeared; the next MouseMove will repaint.
        m_chipHoverLine     = -1;
        m_chipHoverStartCol = -1;
        m_chipHoverEndCol   = -1;
        m_chipPressed       = false;
    }

    // Marker/margin work stays narrowed — that's where the real cost
    // was (~2.6 ms/refresh in production for applyLineAttributes on a
    // 1000-line struct). Indicator work below is forced full-pass.
    applyLineAttributes(result.meta, firstChanged, lastChanged);
    applyHexDimming(result.meta, /*firstLine=*/-1, /*lastLine=*/-1);

    // Build line-text cache using the lineStarts array compose() already
    // computed. O(N) slice of the buffer rather than O(N) char scan +
    // O(N) QString allocations from a manual split.
    QVector<QString> lineTexts(result.meta.size());
    {
        const int n = qMin(result.meta.size(), result.lineStarts.size());
        for (int i = 0; i < n; ++i) {
            int start = result.lineStarts[i];
            int end = (i + 1 < n) ? result.lineStarts[i + 1] - 1
                                  : result.text.size();
            lineTexts[i] = result.text.mid(start, end - start);
        }
    }
    // Indicator passes are forced full-doc (see clearIndicators rationale
    // above). Cheap — tens of microseconds even on 1000-line structs.
    applyHeatmapHighlight(result.meta, lineTexts, /*firstLine=*/-1, /*lastLine=*/-1);
    applySymbolColoring(result.meta, lineTexts, /*firstLine=*/-1, /*lastLine=*/-1);

    applyCommandRowPills();

    // Footer pill styling — full-doc (indicators forced full-pass).
    {
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].lineKind != LineKind::Footer) continue;
            const QString& ft = lineTexts[i];
        // Single-field add chip — search ` +1 ` (padded so the token can't
        // collide with +10/+10h/+100h/+1000h, which all have a digit after
        // +1). Paint only the visible `+1` (cols pPlusOne+1..pPlusOne+3)
        // so the pill doesn't visually butt up against the +10h chip
        // beside it; click hit-test below stays generous (4 cols).
        int pPlusOne = ft.indexOf(QStringLiteral(" +1 "));
        if (pPlusOne >= 0)
            fillIndicatorCols(IND_CMD_PILL, i, pPlusOne + 1, pPlusOne + 3);
        // Struct footer: +10h +100h +1000h Trim (search longest first)
        int p1000 = ft.indexOf(QStringLiteral("+1000h"));
        if (p1000 >= 0)
            fillIndicatorCols(IND_CMD_PILL, i, p1000, p1000 + 6);
        int p100 = ft.indexOf(QStringLiteral("+100h"));
        if (p100 >= 0 && p100 != p1000 + 1)
            fillIndicatorCols(IND_CMD_PILL, i, p100, p100 + 5);
        int p10 = ft.indexOf(QStringLiteral("+10h"));
        if (p10 >= 0 && p10 != p100 && p10 != p1000)
            fillIndicatorCols(IND_CMD_PILL, i, p10, p10 + 4);
        // Enum footer: +10 (no 'h'). Skip when the +10 we found is actually
        // the start of "+1000h" / "+100h" / "+10h" we already painted, OR
        // sits inside "+Field" (it doesn't, but be defensive).
        int add10Start = ft.indexOf(QStringLiteral("+10"));
        if (add10Start >= 0 && add10Start != p10 && add10Start != p100 && add10Start != p1000)
            fillIndicatorCols(IND_CMD_PILL, i, add10Start, add10Start + 3);
        int trimStart = ft.indexOf(QStringLiteral("Trim"));
        if (trimStart >= 0)
            fillIndicatorCols(IND_CMD_PILL, i, trimStart, trimStart + 4);
        int topStart = ft.indexOf(QStringLiteral("Top"));
        if (topStart >= 0)
            fillIndicatorCols(IND_CMD_PILL, i, topStart, topStart + 3);
        }
    }

    // Per-chip indicator coloring: Enum / TypeHint / Rtti / Comment all
    // emitted into lm.chips by compose.cpp (one source of truth for spans).
    // Each chip gets a rounded-box pill background (IND_CHIP_BG) plus a
    // kind-specific TEXTFORE color layered on top. Paint the BG over the
    // chip's exact [startCol, endCol) span — earlier code padded ±1 col
    // for "breathing room", but compose emits chips with a one-space gap
    // and the pad made adjacent pills overlap by one column, fusing them
    // into a single "megachip" (Rtti+Symbol+Comment on __vptr was the
    // visible regression). The chip glyphs themselves ({ / [ / () already
    // give natural margin inside, so no pad is needed.
    for (int i = 0; i < result.meta.size(); i++) {
        const auto& lm = result.meta[i];
        if (lm.chips.isEmpty() || i >= lineTexts.size()) continue;
        const int textLen = lineTexts[i].size();
        for (const auto& c : lm.chips) {
            if (c.startCol < 0 || c.startCol >= textLen) continue;
            int end = qMin(c.endCol, textLen);
            // Pill background under every chip kind — exact span, no pad.
            fillIndicatorCols(IND_CHIP_BG, i, c.startCol, end);
            // Kind-specific text foreground.
            int ind = -1;
            switch (c.kind) {
            case ChipKind::Comment:    ind = IND_HINT_GREEN; break;
            case ChipKind::TypeHint:   ind = IND_TYPE_HINT;  break;
            case ChipKind::Rtti:       ind = IND_RTTI_HINT;  break;
            // Enum reuses the hover-link color so it reads as a clickable
            // value, visually distinct from the green Comment chip on
            // adjacent rows.
            case ChipKind::Enum:       ind = IND_HOVER_SPAN; break;
            // Symbol shares the comment-green tint — both are
            // reference/annotation, not actionable values.
            case ChipKind::Symbol:     ind = IND_HINT_GREEN; break;
            case ChipKind::AddComment: ind = IND_HINT_GREEN; break;
            }
            if (ind >= 0)
                fillIndicatorCols(ind, i, c.startCol, end);
        }
    }

    // Reset hint line - applySelectionOverlay will repaint indicators
    m_hintLine = -1;

    // Restore hover state — but clear if the node was deleted
    m_hoveredNodeId = savedHoverId;
    m_hoveredLine = savedHoverLine;
    m_hoverInside = savedHoverInside;
    m_applyingDocument = false;

    if (m_hoveredNodeId != 0 && !m_nodeLineIndex.contains(m_hoveredNodeId)) {
        m_hoveredNodeId = 0;
        m_hoveredLine = -1;
        dismissAllPopups();
    }

    // Re-apply hover markers. Two paths reach here:
    //   - fullReplace: setText() already nuked every Scintilla marker
    //   - patch: SCI_REPLACETARGET leaves markers OUTSIDE the patched
    //     range alive, including stale M_HOVER on previously-hovered
    //     rows. The old code (reset m_prev=0 + applyHoverHighlight)
    //     was correct for fullReplace but a no-op for cleanup on the
    //     patch path → user-visible as rows lighting up and never
    //     turning off, especially on tabs sharing a live provider
    //     where patch ticks dominate.
    // Unconditional markerDeleteAll(M_HOVER) is microseconds and
    // correct for both paths; applyHoverHighlight() then re-adds for
    // the current m_hoveredNodeId. applyHoverCursor() is NOT called
    // here because it evaluates hitTest() against composed text that
    // updateCommandRow() will overwrite — applySelectionOverlays()
    // runs it after all text is finalized.
    m_sci->markerDeleteAll(M_HOVER);
    m_prevHoveredNodeId = 0;
    m_prevHoveredLine = -1;
    applyHoverHighlight();

    // Re-apply focus glow markers (setText() clears all markers)
    if (m_focusNodeId != 0) {
        auto fit = m_nodeLineIndex.constFind(m_focusNodeId);
        if (fit != m_nodeLineIndex.constEnd()) {
            for (int ln : *fit) {
                if (ln < m_meta.size() && m_meta[ln].lineKind != LineKind::Footer)
                    m_sci->markerAdd(ln, M_FOCUS);
            }
        } else {
            // Node was removed — clear focus
            clearFocusNode();
        }
    }

    // Re-apply find indicator (setText() clears all indicators)
    if (m_findBarContainer && m_findBarContainer->isVisible()) {
        QString needle = m_findBar->text();
        if (!needle.isEmpty()) {
            QByteArray nb = needle.toUtf8();
            long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEARCHFLAGS, (long)0);
            long pos = 0;
            while (pos < docLen) {
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, pos);
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, docLen);
                long found = m_sci->SendScintilla(QsciScintillaBase::SCI_SEARCHINTARGET,
                                                   (uintptr_t)nb.size(), nb.constData());
                if (found < 0) break;
                m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, (long)IND_FIND);
                m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, found, (long)nb.size());
                pos = found + nb.size();
            }
        }
    }

    // Re-apply hex byte selection overlay (setText() / patch clears
    // IND_BYTE_SEL same as the other indicators). Address-based, so a
    // structural change that drops the selected rows naturally paints
    // nothing — no cleanup needed.
    applyByteSelectionOverlay();

    // Stash meta for the next frame's diff. Done last so any earlier code
    // that needs the previous-frame state has already consumed it.
    m_prevMeta = result.meta;

    // Notify minimap / any passive mirror that text has been updated. Fired
    // last so receivers see the final Scintilla state (post-indicator apply).
    emit documentApplied(result.text);
}

void RcxEditor::applyLineAttributes(const QVector<LineMeta>& meta, int firstLine, int lastLine) {
    PROFILE_SCOPE("applyLineAttributes");
    bool full = (firstLine < 0);
    int begin = full ? 0 : firstLine;
    int end = full ? meta.size() : qMin(lastLine + 1, meta.size());

    // Margin text is FORCED full-pass even on a narrow update. Reason:
    // SCI_REPLACETARGET that inserts a '\n' renumbers Scintilla's lines,
    // but margin-text storage doesn't reliably follow — older fields end
    // up with blank margins after a spam-append. Margin work is cheap
    // (~10 µs full-pass), so always re-apply across the document.
    if (m_relativeOffsets) {
        reformatMargins(/*firstLine=*/-1, /*lastLine=*/-1);
    } else {
        m_sci->clearMarginText(-1);
    }

    // Clear markers — full clear when full pass; per-line when narrowed.
    if (full) {
        for (int m = M_CONT; m <= M_STRUCT_BG; m++)
            m_sci->markerDeleteAll(m);
        m_sci->markerDeleteAll(M_CMD_ROW);
    } else {
        for (int i = begin; i < end; ++i) {
            for (int m = M_CONT; m <= M_STRUCT_BG; m++)
                m_sci->markerDelete(i, m);
            m_sci->markerDelete(i, M_CMD_ROW);
        }
    }

    // Margin text — full-pass (see rationale above).
    if (!m_relativeOffsets) {
        for (int i = 0; i < meta.size(); i++) {
            const auto& lm = meta[i];
            if (lm.offsetText.isEmpty()) continue;
            QByteArray text = lm.offsetText.toUtf8();
            m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETTEXT,
                                 (uintptr_t)i, text.constData());
            QByteArray styles(text.size(), '\0');
            m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETSTYLES,
                                 (uintptr_t)i, styles.constData());
        }
    }

    // Markers + fold levels — narrow when requested (this is the expensive part).
    for (int i = begin; i < end; i++) {
        const auto& lm = meta[i];

        if (lm.lineKind == LineKind::CommandRow) {
            m_sci->markerAdd(i, M_CMD_ROW);
        } else {
            uint32_t mask = lm.markerMask;
            for (int m = M_CONT; m <= M_STRUCT_BG; m++) {
                if (mask & (1u << m))
                    m_sci->markerAdd(i, m);
            }
        }

        m_sci->SendScintilla(QsciScintillaBase::SCI_SETFOLDLEVEL,
                             (unsigned long)i, (long)lm.foldLevel);
    }
}

void RcxEditor::reformatMargins(int firstLine, int lastLine) {
    PROFILE_SCOPE("reformatMargins");
    uint64_t base = m_layout.baseAddress;
    int hexDigits = m_layout.offsetHexDigits;
    bool full = (firstLine < 0);
    int begin = full ? 0 : firstLine;
    int end = full ? m_meta.size() : qMin(lastLine + 1, (int)m_meta.size());

    // Resize margin: RVA offsets are much shorter than full addresses
    int marginDigits = m_relativeOffsets ? qMax(hexDigits / 2, 4) : hexDigits;
    QString marginSizer = QString("  %1  ").arg(QString(marginDigits, '0'));
    m_sci->setMarginWidth(0, marginSizer);

    // ── Pass 1: margin text (global offset only) ──
    if (full) {
        m_sci->clearMarginText(-1);
    } else {
        for (int i = begin; i < end; ++i)
            m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETTEXT,
                                 (uintptr_t)i, "");
    }
    for (int i = begin; i < end; i++) {
        auto& lm = m_meta[i];

        if (lm.isContinuation || lm.isMemberLine) {
            lm.offsetText = QStringLiteral("  \u00B7 ");
        } else if (lm.offsetText.isEmpty()) {
            continue;
        } else if (m_relativeOffsets) {
            if (lm.lineKind == LineKind::Footer ||
                lm.lineKind == LineKind::ArrayElementSeparator ||
                lm.lineKind == LineKind::CommandRow) {
                lm.offsetText = QString(hexDigits + 1, ' ');
            } else {
                uint64_t rvaBase = lm.ptrBase ? lm.ptrBase : base;
                uint64_t rel = lm.offsetAddr >= rvaBase ? lm.offsetAddr - rvaBase : 0;
                lm.offsetText = (QStringLiteral("+") +
                    QString::number(rel, 16).toUpper())
                    .rightJustified(hexDigits, ' ') + QChar(' ');
            }
        } else {
            lm.offsetText = QString::number(lm.offsetAddr, 16).toUpper()
                .rightJustified(hexDigits, '0') + QChar(' ');
        }

        QByteArray text = lm.offsetText.toUtf8();
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETTEXT,
                             (uintptr_t)i, text.constData());
        QByteArray styles(text.size(), '\0');
        m_sci->SendScintilla(QsciScintillaBase::SCI_MARGINSETSTYLES,
                             (uintptr_t)i, styles.constData());
    }

    // ── Pass 2: inline local offsets in the text indent area ──
    // Skip when tree lines are active — the compose step already placed
    // Unicode tree connectors in the indent area; overwriting with spaces
    // or offsets would destroy them.
    if (m_layout.treeLines)
        return;
    m_sci->setReadOnly(false);
    for (int i = begin; i < end; i++) {
        const auto& lm = m_meta[i];
        if (lm.depth <= 1 || lm.isContinuation) continue;
        if (lm.lineKind != LineKind::Field && lm.lineKind != LineKind::Header)
            continue;

        // Place offset in the indent area before the child's type column.
        // Need at least 4 chars (3 for "+XX" + 1 gap). With kTreeIndent=2
        // there's only 1 char per level of nesting, so skip when too tight.
        int childTypeCol = kFoldCol + lm.depth * kTreeIndent;
        int parentTypeCol = kFoldCol + (lm.depth - 1) * kTreeIndent;
        int slotWidth = childTypeCol - parentTypeCol - 1;  // -1 for gap before type
        if (slotWidth < 3) continue;  // not enough room for "+XX"
        int col = parentTypeCol;

        auto pos = [&](int c) -> long {
            return m_sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                        (unsigned long)i, (long)c);
        };

        if (m_relativeOffsets) {
            // Derive local offset: for pointer-expanded children use ptrBase,
            // otherwise find enclosing header or array element separator
            uint64_t parentAddr = base;
            if (lm.ptrBase != 0) {
                parentAddr = lm.ptrBase;
            } else if (lm.parentAddr != 0) {
                // Use precomputed parent address from compose (avoids O(N) backward scan)
                parentAddr = lm.parentAddr;
            } else {
                for (int j = i - 1; j >= 0; j--) {
                    const auto& pLm = m_meta[j];
                    if (pLm.lineKind == LineKind::Header && pLm.depth < lm.depth) {
                        parentAddr = pLm.offsetAddr;
                        break;
                    }
                    if (pLm.lineKind == LineKind::ArrayElementSeparator && pLm.depth <= lm.depth) {
                        parentAddr = pLm.offsetAddr;
                        break;
                    }
                }
            }
            uint64_t localOff = lm.offsetAddr >= parentAddr ? lm.offsetAddr - parentAddr : 0;

            QString off = QStringLiteral("+") +
                QString::number(localOff, 16).toUpper();
            QString padded = off.size() <= slotWidth
                ? off.rightJustified(slotWidth, ' ')
                : off;
            long posA = pos(col);
            long posB = pos(col + slotWidth);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, posA);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, posB);
            QByteArray utf8 = padded.left(slotWidth).toUtf8();
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                 (uintptr_t)utf8.size(), utf8.constData());
            // Color the local offset dim
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, IND_LOCAL_OFF);
            m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                                 posA, posB - posA);
        } else {
            // Restore spaces when toggling off
            long posA = pos(col);
            long posB = pos(col + slotWidth);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, posA);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, posB);
            QByteArray spaces(slotWidth, ' ');
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                 (uintptr_t)spaces.size(), spaces.constData());
        }
    }
    m_sci->setReadOnly(true);
}


static inline void lineRangeNoEol(QsciScintilla* sci, int line, long& start, long& len) {
    start = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)line);
    long end = sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION, (unsigned long)line);
    len = (end > start) ? (end - start) : 0;
}

// UTF-8 safe column-to-position conversion
static inline long posFromCol(QsciScintilla* sci, int line, int col) {
    return sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                              (unsigned long)line, (long)col);
}

void RcxEditor::clearIndicatorLine(int indic, int line) {
    if (line < 0) return;
    long start, len;
    lineRangeNoEol(m_sci, line, start, len);
    if (len <= 0) return;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indic);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, start, len);
}

void RcxEditor::fillIndicatorCols(int indic, int line, int colA, int colB) {
    long a = posFromCol(m_sci, line, colA);
    long b = posFromCol(m_sci, line, colB);
    if (b > a) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, indic);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, a, b - a);
    }
}

void RcxEditor::applyHexDimming(const QVector<LineMeta>& meta, int firstLine, int lastLine) {
    PROFILE_SCOPE("applyHexDimming");
    bool full = (firstLine < 0);
    int begin = full ? 0 : firstLine;
    int end = full ? meta.size() : qMin(lastLine + 1, (int)meta.size());
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, IND_HEX_DIM);
    for (int i = begin; i < end; i++) {
        // Dim fold arrows (▸/▾) on fold head lines
        if (meta[i].foldHead && meta[i].lineKind != LineKind::CommandRow)
            fillIndicatorCols(IND_HEX_DIM, i, 0, kFoldCol);

        if (isHexPreview(meta[i].nodeKind)) {
            long pos, len; lineRangeNoEol(m_sci, i, pos, len);
            if (len > 0)
                m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, pos, len);
        }
        // Dim struct/array braces: entire footer line, trailing "{" on headers
        if (meta[i].lineKind == LineKind::Footer) {
            long pos, len; lineRangeNoEol(m_sci, i, pos, len);
            if (len > 0)
                m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, pos, len);
        } else if (meta[i].braceCol >= 0) {
            // Use precomputed brace column from compose (avoids per-character IPC scan)
            fillIndicatorCols(IND_HEX_DIM, i, meta[i].braceCol, meta[i].braceCol + 1);
        }
    }

    // Tree-connector tint — apply IND_TREE_CONN (theme.textDim) over the
    // prefix indent region of every nested line so ├ │ └ glyphs read
    // slightly muted vs. the field type/name/value content. The range is
    // [prefixWidth, prefixWidth + depth*kTreeIndent) — see LineGeometry
    // in core.h for the column convention. CommandRow + flush-left
    // footer lines have depth 0 here so they're skipped naturally.
    for (int i = begin; i < end; i++) {
        const LineMeta& lm = meta[i];
        if (lm.depth <= 0) continue;
        if (lm.lineKind == LineKind::CommandRow) continue;
        LineGeometry g = LineGeometry::forLine(lm);
        const int colA = g.prefixWidth;
        const int colB = g.prefixWidth + g.indentWidth;
        if (colB > colA)
            fillIndicatorCols(IND_TREE_CONN, i, colA, colB);
    }
}

void RcxEditor::applySelectionOverlay(const QSet<uint64_t>& selIds) {
    PROFILE_SCOPE("applySelectionOverlay");
    // Skip when nothing changed since the last call AND the previous
    // applyDocument took the patch path (so markers outside the patched
    // range are still intact). Saves ~22 µs/refresh during rapid editing
    // when selection holds steady. Full-replace path always invalidates.
    const bool selChanged = (selIds != m_currentSelIds);
    if (!selChanged && m_lastApplyWasPatch)
        return;
    m_currentSelIds = selIds;
    m_sci->markerDeleteAll(M_SELECTED);
    m_sci->markerDeleteAll(M_ACCENT);

    // Clear all editable indicators, then repaint for selected lines only
    long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, IND_EDITABLE);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE, (unsigned long)0, docLen);

    // Use index: iterate selected IDs, look up their lines
    for (uint64_t selId : selIds) {
        bool isFooterSel = (selId & kFooterIdBit) != 0;
        bool isArrayElemSel = (selId & kArrayElemBit) != 0;
        bool isMemberSel = (selId & kMemberBit) != 0;
        int arrayElemIdx = isArrayElemSel ? arrayElemIdxFromSelId(selId) : -1;
        int memberSubLine = isMemberSel ? memberSubFromSelId(selId) : -1;
        uint64_t nodeId = selId & ~(kFooterIdBit | kArrayElemBit | kArrayElemMask
                                    | kMemberBit | kMemberSubMask);
        auto it = m_nodeLineIndex.constFind(nodeId);
        if (it == m_nodeLineIndex.constEnd()) continue;
        for (int ln : *it) {
            if (isSyntheticLine(m_meta[ln])) continue;
            bool isFooter = (m_meta[ln].lineKind == LineKind::Footer);
            // Match selection type to line type
            if (isFooterSel && !isFooter) continue;
            if (!isFooterSel && isFooter) continue;
            // Array element: match by element index
            if (isArrayElemSel) {
                if (!m_meta[ln].isArrayElement || m_meta[ln].arrayElementIdx != arrayElemIdx)
                    continue;
            } else if (m_meta[ln].isArrayElement) {
                continue;
            }
            // Member line: match by subLine index
            if (isMemberSel) {
                if (!m_meta[ln].isMemberLine || m_meta[ln].subLine != memberSubLine)
                    continue;
            } else if (m_meta[ln].isMemberLine) {
                continue;
            }
            m_sci->markerAdd(ln, M_SELECTED);
            m_sci->markerAdd(ln, M_ACCENT);
            if (!isFooter)
                paintEditableSpans(ln);
        }
    }

    // Reset hint line - updateEditableIndicators will handle cursor hints
    // on actual user navigation (not stale restored positions)
    m_hintLine = -1;

    applyHoverHighlight();
    applyHoverCursor();

    // Dismiss type tooltip ONLY when the selection actually changed.
    // The previous code dismissed unconditionally on every call; live
    // refresh ticks call applySelectionOverlay each frame, and the
    // early-return above can fail to short-circuit when
    // m_lastApplyWasPatch is false (e.g., after endInlineEdit clears
    // m_prevText so the next applyDocument takes the fullReplace
    // path). Result: the command-row arrow tooltip got dismissed
    // every refresh tick while the user held still on its anchor —
    // applyHoverCursor immediately re-showed it on the next event
    // pump, producing visible show/hide flicker that matched the
    // refresh cadence ("flickers if the data is updating" — user
    // verbatim).
    if (selChanged && m_arrowTooltip)
        static_cast<RcxTooltip*>(m_arrowTooltip)->dismiss();
}

void RcxEditor::setHoverEffects(bool on) {
    if (m_hoverEffects == on) return;
    m_hoverEffects = on;
    if (!on) {
        // Clear all hover visuals
        m_hoveredNodeId = 0;
        m_hoveredLine = -1;
        m_prevHoveredNodeId = 0;
        m_prevHoveredLine = -1;
        m_sci->markerDeleteAll(M_HOVER);
        for (int ln : m_hoverSpanLines)
            clearIndicatorLine(IND_HOVER_SPAN, ln);
        m_hoverSpanLines.clear();
        dismissAllPopups();
        m_sci->viewport()->setCursor(Qt::ArrowCursor);
    }
}

// Chips that respond to clicks — only these get hover/pressed visuals.
// RTTI and Symbol are read-only labels (currently); painting button
// feedback on them would misrepresent their behavior.
static bool chipIsClickable(ChipKind k) {
    switch (k) {
    case ChipKind::Comment:
    case ChipKind::TypeHint:
    case ChipKind::Enum:
    case ChipKind::AddComment:
        return true;
    default:
        return false;
    }
}

void RcxEditor::clearChipButtonState() {
    if (m_chipHoverLine < 0 && !m_chipPressed) return;
    long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    for (int ind : {IND_CHIP_HOVER, IND_CHIP_PRESSED}) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                             (long)ind);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                             (long)0, docLen);
    }
    m_chipHoverLine     = -1;
    m_chipHoverStartCol = -1;
    m_chipHoverEndCol   = -1;
    m_chipPressed       = false;
}

void RcxEditor::applyChipButtonOverlay() {
    // Always clear both overlays first — the hover may have moved to a
    // different chip whose previous-frame indicator must come off.
    long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    for (int ind : {IND_CHIP_HOVER, IND_CHIP_PRESSED}) {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                             (long)ind);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                             (long)0, docLen);
    }
    if (m_chipHoverLine < 0) return;
    long lineStartByte = m_sci->SendScintilla(
        QsciScintillaBase::SCI_POSITIONFROMLINE,
        (unsigned long)m_chipHoverLine);
    long startByte = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)m_chipHoverLine, (long)m_chipHoverStartCol);
    long endByte   = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)m_chipHoverLine, (long)m_chipHoverEndCol);
    Q_UNUSED(lineStartByte);
    if (endByte <= startByte) return;
    int ind = m_chipPressed ? IND_CHIP_PRESSED : IND_CHIP_HOVER;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, (long)ind);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE,
                         startByte, endByte - startByte);
}

// ── Byte selection (hex preview rows) ──
//
// byteAddrAt returns nullopt for anything that isn't an individual hex
// byte in a hex preview row's value column. The hex value column is laid
// out as "XX XX XX..." — 2 hex digits per byte plus a 1-char gap, so
// (col - vs.start) / 3 maps a column to a byte index. valueSpanFor
// (called by valueSpan) gives a 23-char span for hex rows specifically
// (see core.h kColValue branch), which covers byte 0..7 at columns
// 0..22 inclusive. Out-of-range clicks (past byte 7 of a Hex64, etc.)
// short-circuit to nullopt and fall through to row-click handling.
std::optional<uint64_t> RcxEditor::byteAddrAt(int line, int col) const {
    if (line < 0 || line >= m_meta.size()) return std::nullopt;
    const LineMeta& lm = m_meta[line];
    if (lm.lineKind != LineKind::Field) return std::nullopt;
    if (!isHexPreview(lm.nodeKind)) return std::nullopt;
    QString lineText = getLineText(m_sci, line);
    ColumnSpan vs = valueSpan(lm, lineText.size(),
                              lm.effectiveTypeW, lm.effectiveNameW);
    if (!vs.valid || col < vs.start) return std::nullopt;
    int sz = sizeForKind(lm.nodeKind);
    int byteIdx = (col - vs.start) / 3;
    if (byteIdx < 0 || byteIdx >= sz) return std::nullopt;
    return lm.offsetAddr + static_cast<uint64_t>(byteIdx);
}

void RcxEditor::applyByteSelectionOverlay() {
    // Paint IND_BYTE_SEL across the digits of every selected byte on
    // every hex preview row that overlaps m_byteSel. INDIC_TEXTFORE
    // recolours character pixels; the inter-byte spaces have no
    // pixels and stay visually unaffected, which is the desired
    // outcome (selected bytes are coloured, gaps are not).
    long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                         (long)IND_BYTE_SEL);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                         (long)0, docLen);

    if (m_byteSel.has_value()) {
        const uint64_t selLo = m_byteSel->first;
        const uint64_t selHi = m_byteSel->second;
        for (int i = 0; i < m_meta.size(); ++i) {
            const LineMeta& lm = m_meta[i];
            if (lm.lineKind != LineKind::Field) continue;
            if (!isHexPreview(lm.nodeKind)) continue;
            int sz = sizeForKind(lm.nodeKind);
            uint64_t lineLo = lm.offsetAddr;
            uint64_t lineHi = lineLo + static_cast<uint64_t>(sz);
            if (selHi <= lineLo || selLo >= lineHi) continue;  // no overlap
            QString lineText = getLineText(m_sci, i);
            ColumnSpan vs = valueSpan(lm, lineText.size(),
                                      lm.effectiveTypeW, lm.effectiveNameW);
            if (!vs.valid) continue;
            int firstByte = static_cast<int>(qMax(selLo, lineLo) - lineLo);
            int lastByte  = static_cast<int>(qMin(selHi, lineHi) - lineLo);

            // Paint IND_BYTE_SEL across the entire selected run on this
            // row, including inter-byte spaces. INDIC_TEXTFORE only
            // colours characters with pixels — the spaces stay
            // visually unaffected but are bracketed by coloured
            // digits, which reads as a contiguous selection.
            int hiCol = vs.start + (lastByte - 1) * 3 + 2;
            fillIndicatorCols(IND_BYTE_SEL, i, vs.start + firstByte * 3, hiCol);
        }
    }

    // Tail-call so the status-bar line is rebuilt on every selection
    // change AND on every refresh tick (the refresh path tail-calls
    // applyByteSelectionOverlay). Memory-tracking interpretations
    // ("i32=…") therefore update live without a separate timer.
    updateByteSelStatus();
}

// Build a lowercase-0x, uppercase-digits hex literal — keeps the literal
// `0x` prefix lowercase (which is the C / debugger convention) while
// uppercasing the digits for readability. Replaces ".arg(...).toUpper()"
// pattern which would render "0X1A2B" instead of "0x1A2B".
static QString hexLiteral(qulonglong value, int digits) {
    QString d = QStringLiteral("%1")
        .arg(value, digits, 16, QChar('0')).toUpper();
    return QStringLiteral("0x") + d;
}

void RcxEditor::updateByteSelStatus() {
    // Empty selection → don't clobber whatever the status bar was
    // showing (a "Copied N bytes" toast from the previous action,
    // an MCP status, etc.). The next non-selection statusHint will
    // overwrite naturally.
    if (!m_byteSel.has_value()) return;

    const auto [lo, hi] = *m_byteSel;
    const int n = static_cast<int>(hi - lo);
    if (n <= 0 || n > 65536) return;

    QByteArray data(n, '\0');
    if (m_disasmProvider && m_disasmProvider->isReadable(lo, n))
        data = m_disasmProvider->readBytes(lo, n);
    if (data.size() < n) data.append(QByteArray(n - data.size(), '\0'));

    const bool canWrite = m_disasmProvider
        && m_disasmProvider->isValid()
        && m_disasmProvider->isWritable();

    auto u = [&](int width) -> uint64_t {
        uint64_t v = 0;
        memcpy(&v, data.constData(), qMin<int>(width, data.size()));
        return v;
    };

    QString text = QStringLiteral("%1 byte%2 @ ").arg(n).arg(n == 1 ? "" : "s")
        + hexLiteral(lo, 0);
    if (!canWrite) text += QStringLiteral("  (read-only)");

    const QString sep = QStringLiteral("  ·  ");
    if (n == 1) {
        uint8_t b = (uint8_t)data[0];
        text += sep + hexLiteral(b, 2);
        text += sep + QString::number((int)(int8_t)b);
        if (b >= 0x20 && b <= 0x7E)
            text += sep + QStringLiteral("'%1'").arg(QChar(b));
    } else if (n == 2) {
        uint16_t v = (uint16_t)u(2);
        uint16_t be = qbswap(v);
        text += sep + hexLiteral(v, 4);
        text += sep + QStringLiteral("i16=") + QString::number((int16_t)v);
        text += sep + QStringLiteral("BE ") + hexLiteral(be, 4);
        text += QStringLiteral(" i16=") + QString::number((int16_t)be);
    } else if (n == 3) {
        // 24-bit interpretations — common for RGB triples, int24
        // fields, and the leading 3 bytes of a hash. Show the LE
        // value as 0xBBGGRR (matches what the bytes encode if read as
        // an int24 in memory order), the BE value, and a small RGB
        // hex with channels in conventional R-G-B order. ASCII fires
        // when all three bytes are printable.
        uint8_t b0 = (uint8_t)data[0];
        uint8_t b1 = (uint8_t)data[1];
        uint8_t b2 = (uint8_t)data[2];
        uint32_t leVal = uint32_t(b0) | (uint32_t(b1) << 8) | (uint32_t(b2) << 16);
        uint32_t beVal = (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | uint32_t(b2);
        // Sign-extend the 24-bit LE/BE values for signed display
        auto sext24 = [](uint32_t u) -> int32_t {
            return (u & 0x800000u) ? int32_t(u | 0xFF000000u) : int32_t(u);
        };
        text += sep + hexLiteral(leVal, 6);
        text += sep + QStringLiteral("i24=") + QString::number(sext24(leVal));
        text += sep + QStringLiteral("BE ") + hexLiteral(beVal, 6);
        text += QStringLiteral(" i24=") + QString::number(sext24(beVal));
        // RGB triplet (R-G-B in display order, same as CSS #RRGGBB).
        QString rgbDigits = QStringLiteral("%1")
            .arg(beVal, 6, 16, QChar('0')).toUpper();
        text += sep + QStringLiteral("rgb #") + rgbDigits;
        if (b0 >= 0x20 && b0 <= 0x7E
            && b1 >= 0x20 && b1 <= 0x7E
            && b2 >= 0x20 && b2 <= 0x7E)
            text += sep + QStringLiteral("\"%1%2%3\"")
                .arg(QChar(b0)).arg(QChar(b1)).arg(QChar(b2));
    } else if (n == 4) {
        uint32_t v = (uint32_t)u(4);
        uint32_t be = qbswap(v);
        float f; memcpy(&f, data.constData(), 4);
        float fBe; memcpy(&fBe, &be, 4);
        text += sep + hexLiteral(v, 8);
        text += sep + QStringLiteral("i32=") + QString::number((qint32)v);
        text += sep + QStringLiteral("f32=") + fmt::fmtFloat(f);
        text += sep + QStringLiteral("BE ") + hexLiteral(be, 8);
        text += QStringLiteral(" i32=") + QString::number((qint32)be);
        text += QStringLiteral(" f32=") + fmt::fmtFloat(fBe);
    } else if (n == 8) {
        uint64_t v = u(8);
        uint64_t be = qbswap(v);
        double d; memcpy(&d, data.constData(), 8);
        double dBe; memcpy(&dBe, &be, 8);
        text += sep + hexLiteral((qulonglong)v, 16);
        text += sep + QStringLiteral("i64=") + QString::number((qint64)v);
        text += sep + QStringLiteral("f64=") + fmt::fmtDouble(d);
        text += sep + QStringLiteral("BE ") + hexLiteral((qulonglong)be, 16);
        text += QStringLiteral(" i64=") + QString::number((qint64)be);
        text += QStringLiteral(" f64=") + fmt::fmtDouble(dBe);
    } else if (n >= 5 && n <= 7) {
        // 5/6/7-byte interpretations — niche but useful for MAC
        // addresses (6 bytes), 40-bit/48-bit/56-bit packed fields,
        // and the leading prefix of larger hashes. Show LE/BE as
        // up to 64-bit ints (top bytes are zero).
        uint64_t leVal = 0;
        memcpy(&leVal, data.constData(), n);
        // BE = byte-reverse of n bytes, padded high.
        uint64_t beVal = 0;
        for (int i = 0; i < n; ++i)
            beVal = (beVal << 8) | uint8_t(data[i]);
        const int hexDigits = n * 2;
        text += sep + hexLiteral((qulonglong)leVal, hexDigits);
        text += sep + QStringLiteral("i%1=").arg(n * 8)
            + QString::number((qulonglong)leVal);
        text += sep + QStringLiteral("BE ") + hexLiteral((qulonglong)beVal, hexDigits);
        if (n == 6) {
            // MAC-address shortcut — 6 bytes are usually a MAC. Format
            // each byte's 2-digit hex separately so `.toUpper()` only
            // touches the digits, not the "mac " prefix.
            auto byteHex = [](uint8_t b) {
                return QStringLiteral("%1").arg(b, 2, 16, QChar('0')).toUpper();
            };
            text += sep + QStringLiteral("mac ")
                + byteHex((uint8_t)data[0]) + QChar(':')
                + byteHex((uint8_t)data[1]) + QChar(':')
                + byteHex((uint8_t)data[2]) + QChar(':')
                + byteHex((uint8_t)data[3]) + QChar(':')
                + byteHex((uint8_t)data[4]) + QChar(':')
                + byteHex((uint8_t)data[5]);
        }
    } else {
        // Generic large selection (9+ bytes): preview the first 12.
        const int previewLen = qMin(n, 12);
        text += sep;
        for (int i = 0; i < previewLen; ++i) {
            if (i > 0) text += QLatin1Char(' ');
            text += QStringLiteral("%1")
                .arg((uint8_t)data[i], 2, 16, QChar('0')).toUpper();
        }
        if (n > previewLen) text += QStringLiteral(" …");
    }

    emit statusHintRequested(text);
}

void RcxEditor::beginByteEdit() {
    if (!m_byteSel.has_value()) return;
    const auto [lo, hi] = *m_byteSel;

    // Build one segment per hex preview row the selection covers.
    // Each segment carries the line + the [spanStart, spanEnd) column
    // range that holds its bytes' hex digits. Multi-row works by
    // hopping between segments as the cursor crosses row boundaries.
    QVector<InlineEditState::ByteEditSegment> segs;
    for (int i = 0; i < m_meta.size(); ++i) {
        const LineMeta& lm = m_meta[i];
        if (lm.lineKind != LineKind::Field) continue;
        if (!isHexPreview(lm.nodeKind)) continue;
        int sz = sizeForKind(lm.nodeKind);
        uint64_t lineLo = lm.offsetAddr;
        uint64_t lineHi = lineLo + static_cast<uint64_t>(sz);
        if (hi <= lineLo || lo >= lineHi) continue;

        int firstByte = static_cast<int>(qMax(lo, lineLo) - lineLo);
        int lastByte  = static_cast<int>(qMin(hi, lineHi) - lineLo);
        QString lineText = getLineText(m_sci, i);
        ColumnSpan vs = valueSpan(lm, lineText.size(),
                                  lm.effectiveTypeW, lm.effectiveNameW);
        if (!vs.valid) continue;
        InlineEditState::ByteEditSegment seg;
        seg.line      = i;
        seg.spanStart = vs.start + firstByte * 3;
        // Drop the trailing inter-byte space when the segment doesn't
        // reach the row's last byte; for row-tail segments the row
        // simply ends at the last digit so there's no trailing space
        // either way.
        seg.spanEnd   = vs.start + lastByte  * 3 - 1;
        if (seg.spanEnd < seg.spanStart) seg.spanEnd = seg.spanStart;
        seg.byteCount = lastByte - firstByte;
        segs.append(seg);
    }
    if (segs.isEmpty()) return;

    // Drop the byte-sel visual + tooltip before entering inline edit
    // mode. beginInlineEdit also clears m_byteSel internally, but we
    // do it ahead so the IND_BYTE_SEL paint comes off the row before
    // the edit-state visuals layer on.
    m_byteSel.reset();
    applyByteSelectionOverlay();

    // Use the existing single-row inline edit machinery on the FIRST
    // segment's line. It handles hex-overwrite setup, the comment-area
    // padding, indicator hint colours, etc. We then narrow its span
    // bounds to that segment's byte range and stash the full segment
    // list for the cursor-jump logic in handleHexEditKey.
    setHexEditPending(true);
    if (!beginInlineEdit(EditTarget::Value, segs.first().line)) return;

    const auto& s0 = segs.first();
    QString lineText = getLineText(m_sci, s0.line);
    int origLen = s0.spanEnd - s0.spanStart;
    m_editState.spanStart = s0.spanStart;
    m_editState.original  = lineText.mid(s0.spanStart, origLen);
    m_editState.posStart  = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)s0.line, (long)s0.spanStart);
    m_editState.posEnd    = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)s0.line, (long)s0.spanEnd);
    m_editState.byteRange     = true;
    m_editState.byteRangeAddr = lo;
    m_editState.byteRangeLen  = static_cast<int>(hi - lo);
    m_editState.byteSegments  = segs;
    m_editState.byteSegIdx    = 0;
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, m_editState.posStart);

    // Background highlight over every segment — visible bounds for the
    // edit zone. Stays painted across segment hops because the
    // indicator is on the byte ranges themselves, not bound to the
    // current cursor line. endInlineEdit clears it.
    for (const auto& seg : segs)
        fillIndicatorCols(IND_EDIT_BOUNDS, seg.line, seg.spanStart, seg.spanEnd);
}

bool RcxEditor::advanceToByteSegment(int delta) {
    if (!m_editState.byteRange) return false;
    int newIdx = m_editState.byteSegIdx + delta;
    if (newIdx < 0 || newIdx >= m_editState.byteSegments.size()) return false;

    m_editState.byteSegIdx = newIdx;
    const auto& seg = m_editState.byteSegments[newIdx];
    QString lineText = getLineText(m_sci, seg.line);
    int origLen = seg.spanEnd - seg.spanStart;
    // m_editState.line + linelenAfterReplace + padBytes/padPos all
    // refer to the FIRST segment's line — the one beginInlineEdit
    // padded with comment-area spaces. Keep them untouched as we jump
    // segments; only the per-segment input bookkeeping moves with us.
    // (Earlier rev updated m_editState.line here, which made
    // endInlineEdit's padding strip run on the last-visited segment's
    // row — chopping ~28 chars off an innocent hex preview row.)
    // clampEditSelection's hex branch is multi-row aware and uses the
    // cursor's actual line rather than m_editState.line for the pin,
    // so leaving it on the first line doesn't drag the cursor back.
    m_editState.spanStart = seg.spanStart;
    m_editState.original  = lineText.mid(seg.spanStart, origLen);
    m_editState.posStart  = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)seg.line, (long)seg.spanStart);
    m_editState.posEnd    = m_sci->SendScintilla(
        QsciScintillaBase::SCI_FINDCOLUMN,
        (unsigned long)seg.line, (long)seg.spanEnd);
    int targetCol = (delta > 0) ? seg.spanStart : (seg.spanEnd - 1);
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
        posFromCol(m_sci, seg.line, targetCol));
    return true;
}

void RcxEditor::extendByteSelection(int dByte) {
    if (!m_byteSel.has_value() || dByte == 0) return;
    auto [lo, hi] = *m_byteSel;
    // The anchor (`lo`) is whatever the drag set — never moves under
    // keyboard extension. Shift+Right grows the right edge; Shift+Left
    // pulls it back in, clamped so the selection stays at least 1 byte.
    if (dByte > 0) {
        hi += static_cast<uint64_t>(dByte);
        // Clamp to the highest hex byte's end address in the visible
        // document so Shift+Right past the last byte stops at "end of
        // selectable region" instead of silently growing into nowhere.
        // Walks m_meta once — O(rows), trivial vs. the existing paint
        // pass which already walks every row per selection change.
        uint64_t docHi = 0;
        bool anyHex = false;
        for (const LineMeta& lm : m_meta) {
            if (lm.lineKind != LineKind::Field) continue;
            if (!isHexPreview(lm.nodeKind)) continue;
            uint64_t lineHi = lm.offsetAddr
                + static_cast<uint64_t>(sizeForKind(lm.nodeKind));
            if (!anyHex || lineHi > docHi) { docHi = lineHi; anyHex = true; }
        }
        if (anyHex && hi > docHi) hi = docHi;
    } else {
        uint64_t shrink = static_cast<uint64_t>(-dByte);
        hi = (hi > lo + shrink) ? (hi - shrink) : (lo + 1);
    }
    m_byteSel = QPair<uint64_t, uint64_t>{lo, hi};
    applyByteSelectionOverlay();
}

void RcxEditor::snapByteSelectionToRow(int dir) {
    if (!m_byteSel.has_value() || dir == 0) return;
    auto [lo, hi] = *m_byteSel;

    // Locate the hex preview row whose [offsetAddr, offsetAddr+size) range
    // contains the last selected byte (hi - 1). Walk m_meta once. Bail out
    // silently if the selection's tail isn't on any visible hex row — that
    // typically means the layout has shifted under us and the next overlay
    // pass will re-derive things.
    auto rowRange = [](const LineMeta& lm) -> QPair<uint64_t, uint64_t> {
        uint64_t start = lm.offsetAddr;
        uint64_t end   = start + static_cast<uint64_t>(sizeForKind(lm.nodeKind));
        return {start, end};
    };
    int curIdx = -1;
    uint64_t lastByte = (hi > 0) ? (hi - 1) : 0;
    for (int i = 0; i < m_meta.size(); ++i) {
        const LineMeta& lm = m_meta[i];
        if (lm.lineKind != LineKind::Field) continue;
        if (!isHexPreview(lm.nodeKind)) continue;
        auto [rs, re] = rowRange(lm);
        if (lastByte >= rs && lastByte < re) { curIdx = i; break; }
    }
    if (curIdx < 0) return;

    auto [curStart, curEnd] = rowRange(m_meta[curIdx]);

    if (dir > 0) {
        if (hi < curEnd) {
            // First press from mid-row: snap to end of current row.
            hi = curEnd;
        } else {
            // hi already at row end → walk forward to next field row. Stop
            // at the first non-hex field (caller's invariant: this command
            // only walks across contiguous hex preview rows).
            for (int j = curIdx + 1; j < m_meta.size(); ++j) {
                const LineMeta& lm = m_meta[j];
                if (lm.lineKind != LineKind::Field) continue;
                if (!isHexPreview(lm.nodeKind)) {
                    // Non-hex boundary — stop. No-op for this press.
                    return;
                }
                auto [rs, re] = rowRange(lm);
                hi = re;
                break;
            }
            // No following field row found → no-op.
            if (hi == m_byteSel->second) return;
        }
    } else {
        // dir < 0 → Shift+Up: shrink hi backwards.
        if (hi > curStart + 1) {
            // Mid-row or end-of-row → snap back to row start. Clamp so we
            // never drop below 1 byte selected (lo + 1).
            hi = (curStart > lo + 1) ? curStart : (lo + 1);
        } else {
            // Already at the "1 byte into row" minimum. Walk back to the
            // previous hex preview field row's end.
            for (int j = curIdx - 1; j >= 0; --j) {
                const LineMeta& lm = m_meta[j];
                if (lm.lineKind != LineKind::Field) continue;
                if (!isHexPreview(lm.nodeKind)) {
                    // Non-hex boundary on the way up — stop.
                    return;
                }
                auto [rs, re] = rowRange(lm);
                hi = (re > lo + 1) ? re : (lo + 1);
                break;
            }
            if (hi == m_byteSel->second) return;
        }
    }

    m_byteSel = QPair<uint64_t, uint64_t>{lo, hi};
    applyByteSelectionOverlay();
}

void RcxEditor::selectAllHexBytes() {
    // Scan m_meta, find the lowest + highest byte address among hex
    // preview rows, and span the union as a single half-open range.
    // Non-hex rows in the middle (continuations, headers, footers,
    // non-hex fields) are skipped at PAINT time by the overlay's
    // line-intersection check — the selection address range simply
    // doesn't intersect those rows, so they remain unhighlighted.
    uint64_t lo = UINT64_MAX, hi = 0;
    bool any = false;
    for (const LineMeta& lm : m_meta) {
        if (lm.lineKind != LineKind::Field) continue;
        if (!isHexPreview(lm.nodeKind)) continue;
        int sz = sizeForKind(lm.nodeKind);
        uint64_t lineLo = lm.offsetAddr;
        uint64_t lineHi = lineLo + static_cast<uint64_t>(sz);
        if (!any) { lo = lineLo; hi = lineHi; any = true; }
        else {
            if (lineLo < lo) lo = lineLo;
            if (lineHi > hi) hi = lineHi;
        }
    }
    if (!any) return;
    m_byteSel = QPair<uint64_t, uint64_t>{lo, hi};
    applyByteSelectionOverlay();
}

void RcxEditor::updateChipHover(const HitInfo& h) {
    int newLine = -1;
    int newStart = -1;
    int newEnd = -1;
    if (m_hoverInside && h.line >= 0 && h.line < m_meta.size()) {
        const auto& lm = m_meta[h.line];
        for (const auto& c : lm.chips) {
            if (c.startCol < 0) continue;
            if (h.col < c.startCol || h.col > c.endCol) continue;
            if (!chipIsClickable(c.kind)) continue;
            newLine  = h.line;
            newStart = c.startCol;
            newEnd   = c.endCol;
            break;
        }
    }
    // Nothing changed? Skip the indicator churn.
    if (newLine == m_chipHoverLine
        && newStart == m_chipHoverStartCol
        && newEnd == m_chipHoverEndCol) return;
    // If the chip we were tracking goes out from under the cursor, the
    // pressed state goes with it — a click that finishes outside the
    // original target shouldn't fire (matches button conventions).
    if (newLine != m_chipHoverLine || newStart != m_chipHoverStartCol)
        m_chipPressed = false;
    m_chipHoverLine     = newLine;
    m_chipHoverStartCol = newStart;
    m_chipHoverEndCol   = newEnd;
    applyChipButtonOverlay();
}

void RcxEditor::applyHoverHighlight() {
    if (!m_hoverEffects) return;

    uint64_t prevId = m_prevHoveredNodeId;
    int prevLine = m_prevHoveredLine;
    m_prevHoveredNodeId = m_hoveredNodeId;
    m_prevHoveredLine = m_hoveredLine;

    // Fast path: nothing changed (same node AND same line)
    if (prevId == m_hoveredNodeId && prevLine == m_hoveredLine
        && m_hoveredNodeId != 0) return;

    // Remove old hover markers
    if (prevId != 0) {
        // Check if old hovered line was a single-line highlight (footer or array element)
        bool prevSingleLine = (prevLine >= 0 && prevLine < m_meta.size() &&
            (m_meta[prevLine].lineKind == LineKind::Footer || m_meta[prevLine].isArrayElement
             || m_meta[prevLine].isMemberLine));
        if (prevSingleLine) {
            m_sci->markerDelete(prevLine, M_HOVER);
        } else {
            auto it = m_nodeLineIndex.constFind(prevId);
            if (it != m_nodeLineIndex.constEnd()) {
                for (int ln : *it)
                    m_sci->markerDelete(ln, M_HOVER);
            }
        }
    }

    if (m_editState.active) return;
    if (!m_hoverInside) return;
    if (m_hoveredNodeId == 0) return;

    // Footer, array elements, and member lines highlight only the specific line
    bool hoveringFooter = (m_hoveredLine >= 0 && m_hoveredLine < m_meta.size() &&
                           m_meta[m_hoveredLine].lineKind == LineKind::Footer);
    bool hoveringArrayElem = (m_hoveredLine >= 0 && m_hoveredLine < m_meta.size() &&
                              m_meta[m_hoveredLine].isArrayElement);
    bool hoveringMember = (m_hoveredLine >= 0 && m_hoveredLine < m_meta.size() &&
                           m_meta[m_hoveredLine].isMemberLine);

    // Check if the hovered item is already selected (using appropriate ID)
    uint64_t checkId;
    if (hoveringFooter)
        checkId = m_hoveredNodeId | kFooterIdBit;
    else if (hoveringArrayElem)
        checkId = makeArrayElemSelId(m_hoveredNodeId, m_meta[m_hoveredLine].arrayElementIdx);
    else if (hoveringMember)
        checkId = makeMemberSelId(m_hoveredNodeId, m_meta[m_hoveredLine].subLine);
    else
        checkId = m_hoveredNodeId;
    if (m_currentSelIds.contains(checkId)) return;

    if (hoveringFooter || hoveringArrayElem || hoveringMember) {
        // Single-line highlight for footers, array elements, and member lines
        m_sci->markerAdd(m_hoveredLine, M_HOVER);
    } else {
        // Non-footer, non-array-element: highlight all lines for this node
        auto it = m_nodeLineIndex.constFind(m_hoveredNodeId);
        if (it != m_nodeLineIndex.constEnd()) {
            for (int ln : *it) {
                if (m_meta[ln].lineKind != LineKind::Footer &&
                    !m_meta[ln].isArrayElement)
                    m_sci->markerAdd(ln, M_HOVER);
            }
        }
    }
}

ViewState RcxEditor::saveViewState() const {
    ViewState vs;
    vs.scrollLine = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
    int line, col;
    m_sci->getCursorPosition(&line, &col);
    vs.cursorLine = line;
    vs.cursorCol  = col;
    // Anchor the cursor by node so a layout shift across the refresh
    // can't desync the caret onto a different node. Stash sub-line too
    // (multi-line nodes restore to the same continuation row).
    if (auto* lm = metaForLine(line)) {
        vs.cursorNodeId  = lm->nodeId;
        vs.cursorSubLine = lm->subLine;
    }
    vs.xOffset = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETXOFFSET);
    return vs;
}

void RcxEditor::restoreViewState(const ViewState& vs) {
    int maxLine = std::max(0, m_sci->lines() - 1);
    int line = std::clamp(vs.cursorLine, 0, maxLine);
    // Prefer node-id-anchored restore: find the line in the freshly-applied
    // meta that owns the same node + sub-line as before. If the node is
    // gone (deleted/renamed-but-not-found), fall through to the saved
    // (cursorLine, cursorCol) coordinates so behaviour matches the legacy
    // path rather than snapping somewhere weird.
    if (vs.cursorNodeId != 0) {
        auto it = m_nodeLineIndex.find(vs.cursorNodeId);
        if (it != m_nodeLineIndex.end()) {
            for (int candidate : *it) {
                if (candidate < 0 || candidate >= m_meta.size()) continue;
                if (m_meta[candidate].subLine == vs.cursorSubLine) {
                    line = std::clamp(candidate, 0, maxLine);
                    break;
                }
            }
        }
    }
    long pos = m_sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                    (unsigned long)line,
                                    (long)std::max(0, vs.cursorCol));
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, (unsigned long)pos);

    // During smooth scroll animation, keep the animation's current position
    // instead of snapping to the saved ViewState (which is pre-animation).
    if (m_scrollAnimActive && m_scrollAnim) {
        int animPos = m_scrollAnim->currentValue().toInt();
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                             (unsigned long)animPos);
    } else {
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                             (unsigned long)vs.scrollLine);
    }

    // Clamp xOffset so it doesn't exceed the current content width.
    // After a rename that shrinks content, the saved offset may be stale.
    int scrollW = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETSCROLLWIDTH);
    int vpW = m_sci->viewport() ? m_sci->viewport()->width() : 0;
    int maxXOff = qMax(0, scrollW - vpW);
    int xOff = qBound(0, vs.xOffset, maxXOff);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETXOFFSET, (unsigned long)xOff);
}

const LineMeta* RcxEditor::metaForLine(int line) const {
    if (line >= 0 && line < m_meta.size())
        return &m_meta[line];
    return nullptr;
}

int RcxEditor::currentNodeIndex() const {
    int line, col;
    m_sci->getCursorPosition(&line, &col);
    auto* lm = metaForLine(line);
    return lm ? lm->nodeIdx : -1;
}

void RcxEditor::showFindBar() {
    // Leave m_findPos alone so reopening the bar continues from where the
    // user last was. Esc-then-reopen is the normal "I lost my place" flow,
    // resetting here would defeat that. textChanged in the find input
    // resets to 0 when the user starts a new query.
    m_findBarContainer->setVisible(true);
    m_findBar->setFocus();
    m_findBar->selectAll();
}

void RcxEditor::dismissHistoryPopup() {
    if (m_historyPopup)
        static_cast<HoverPopup*>(m_historyPopup)->dismiss();
}

void RcxEditor::dismissAllPopups() {
    if (m_historyPopup)    static_cast<HoverPopup*>(m_historyPopup)->dismiss();
    if (m_popupHost)       static_cast<HoverPopup*>(m_popupHost)->dismiss();
    if (m_arrowTooltip)    static_cast<RcxTooltip*>(m_arrowTooltip)->dismiss();
}

QWidget* RcxEditor::hoverPopup() const { return m_popupHost; }

QString RcxEditor::hoverPopupActiveId() const {
    if (!m_popupHost || !m_popupHost->isVisible()) return {};
    auto* p = static_cast<HoverPopupHost*>(m_popupHost)->activePreview();
    return p ? p->id() : QString();
}

// pickLastUsedPreviewIdx — read QSettings("Reclass","Reclass")
// "hoverPreview/kind/<kindName>" and return the index of the matching
// preview in `eligible`. Falls back to 0 (registry-order default) when
// nothing is stored or the stored id is no longer eligible.
int RcxEditor::pickLastUsedPreviewIdx(
        const QVector<HoverPreview*>& eligible, NodeKind kind) const {
    if (eligible.isEmpty()) return -1;
    const KindMeta* km = kindMeta(kind);
    if (!km) return 0;
    QString stored = QSettings("Reclass","Reclass")
        .value(QStringLiteral("hoverPreview/kind/") + QString::fromLatin1(km->name))
        .toString();
    if (stored.isEmpty()) return 0;
    for (int i = 0; i < eligible.size(); ++i) {
        if (eligible[i]->id() == stored) return i;
    }
    return 0;
}

void RcxEditor::hideFindBar() {
    // Keep IND_FIND highlights and m_findPos intact so the user can see
    // where they were and resume from there. Reopening the find bar
    // continues from the last position; typing a fresh query clears the
    // old highlights via the textChanged hook in doFind's first pass.
    m_findBarContainer->setVisible(false);
    m_sci->setFocus();
}

void RcxEditor::scrollToNodeId(uint64_t nodeId) {
    for (int i = 0; i < m_meta.size(); i++) {
        if (m_meta[i].nodeId == nodeId && m_meta[i].lineKind != LineKind::Footer) {
            m_sci->setCursorPosition(i, 0);
            m_sci->ensureLineVisible(i);
            return;
        }
    }
}

// ── Presentation mode: smooth scroll ──

void RcxEditor::smoothScrollToNodeId(uint64_t nodeId) {
    if (!m_presentationMode) {
        scrollToNodeId(nodeId);
        return;
    }

    // Find target line
    int targetLine = -1;
    for (int i = 0; i < m_meta.size(); i++) {
        if (m_meta[i].nodeId == nodeId && m_meta[i].lineKind != LineKind::Footer) {
            targetLine = i;
            break;
        }
    }
    if (targetLine < 0) return;

    // Compute target firstVisibleLine (center node on screen)
    int visibleLines = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
    int targetFirst = qMax(0, targetLine - visibleLines / 2);
    int currentFirst = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);

    // Already visible — just set cursor
    if (targetLine >= currentFirst && targetLine < currentFirst + visibleLines) {
        m_sci->setCursorPosition(targetLine, 0);
        return;
    }

    // Long distance: snap close, then animate the last stretch
    int distance = qAbs(targetFirst - currentFirst);
    if (distance > 50) {
        int snapTo = (targetFirst > currentFirst) ? targetFirst - 30 : targetFirst + 30;
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                             (unsigned long)qMax(0, snapTo));
        currentFirst = qMax(0, snapTo);
    }

    // Create or restart animation
    if (!m_scrollAnim) {
        m_scrollAnim = new QVariantAnimation(this);
        m_scrollAnim->setEasingCurve(QEasingCurve::OutExpo);
        connect(m_scrollAnim, &QVariantAnimation::valueChanged,
                this, [this](const QVariant& val) {
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE,
                                 (unsigned long)val.toInt());
        });
        connect(m_scrollAnim, &QVariantAnimation::finished,
                this, [this]() {
            m_scrollAnimActive = false;
        });
    }
    m_scrollAnim->stop();
    m_scrollAnim->setStartValue(currentFirst);
    m_scrollAnim->setEndValue(targetFirst);
    m_scrollAnim->setDuration(400);
    m_scrollAnimActive = true;
    m_scrollAnim->start();

    // Set cursor at target (non-animated, instant)
    m_sci->setCursorPosition(targetLine, 0);
}

// ── Presentation mode: focus node glow ──

// Blend two colors: result = fg * t + bg * (1-t), fully opaque
static QColor blendColor(const QColor& fg, const QColor& bg, double t) {
    return QColor(
        qBound(0, (int)(fg.red()   * t + bg.red()   * (1.0 - t)), 255),
        qBound(0, (int)(fg.green() * t + bg.green() * (1.0 - t)), 255),
        qBound(0, (int)(fg.blue()  * t + bg.blue()  * (1.0 - t)), 255));
}

void RcxEditor::setFocusNode(uint64_t nodeId) {
    if (nodeId == m_focusNodeId && nodeId != 0) return;

    // Clear previous
    m_sci->markerDeleteAll(M_FOCUS);
    m_focusNodeId = nodeId;
    m_glowPhase = 0;

    if (nodeId == 0) {
        if (m_focusGlowTimer) m_focusGlowTimer->stop();
        return;
    }

    // Pre-blend glow colors against editor background (fully opaque — Scintilla ignores alpha)
    QColor editorBg = m_sci->paper();
    m_glowDim    = blendColor(m_focusGlowColor, editorBg, 0.25);
    m_glowBright = blendColor(m_focusGlowColor, editorBg, 0.55);
    m_sci->setMarkerBackgroundColor(m_glowBright, M_FOCUS);

    // Apply M_FOCUS on all lines for this node
    auto it = m_nodeLineIndex.constFind(nodeId);
    if (it != m_nodeLineIndex.constEnd()) {
        for (int ln : *it) {
            if (ln < m_meta.size() && m_meta[ln].lineKind != LineKind::Footer)
                m_sci->markerAdd(ln, M_FOCUS);
        }
    }

    // Start glow timer — cycles between dim and bright (both opaque)
    if (!m_focusGlowTimer) {
        m_focusGlowTimer = new QTimer(this);
        m_focusGlowTimer->setInterval(30);
        connect(m_focusGlowTimer, &QTimer::timeout, this, [this]() {
            m_glowPhase++;
            double t = 0.5 + 0.5 * std::sin(m_glowPhase * 3.14159265 / 12.0);
            QColor c(
                m_glowDim.red()   + (int)((m_glowBright.red()   - m_glowDim.red())   * t),
                m_glowDim.green() + (int)((m_glowBright.green() - m_glowDim.green()) * t),
                m_glowDim.blue()  + (int)((m_glowBright.blue()  - m_glowDim.blue())  * t));
            m_sci->setMarkerBackgroundColor(c, M_FOCUS);
        });
    }
    m_focusGlowTimer->start();
}

void RcxEditor::clearFocusNode() {
    if (m_focusGlowTimer) m_focusGlowTimer->stop();
    m_sci->markerDeleteAll(M_FOCUS);
    m_focusNodeId = 0;
    m_glowPhase = 0;
}

// ── Column span computation ──

ColumnSpan RcxEditor::typeSpan(const LineMeta& lm, int typeW)  { return typeSpanFor(lm, typeW); }
ColumnSpan RcxEditor::nameSpan(const LineMeta& lm, int typeW, int nameW)  { return nameSpanFor(lm, typeW, nameW); }
ColumnSpan RcxEditor::valueSpan(const LineMeta& lm, int lineLength, int typeW, int nameW) { return valueSpanFor(lm, lineLength, typeW, nameW); }

// Narrow a Value column span when chips have been appended to the row.
// Pre-chip refactor the symbol annotation lived in the value text as
// "0x… // Module+0x1" and we clipped at "  // ". With chips, anything
// past the actual value characters belongs to a chip (RTTI, Symbol,
// Comment, …) — clip valueSpan at the FIRST chip's startCol so the
// editable span is just the address, not "address {RTTI: …}".
//
// Falls back to the legacy "  // " sniff for any compose path that
// hasn't been migrated yet (no current callers, but cheap safety).
static ColumnSpan narrowPtrValueSpan(const LineMeta& lm, const ColumnSpan& vs,
                                     const QString& lineText) {
    if (!vs.valid) return vs;
    int clipEnd = vs.end;
    for (const auto& c : lm.chips) {
        if (c.startCol > vs.start && c.startCol < clipEnd)
            clipEnd = c.startCol;
    }
    if (clipEnd != vs.end)
        return {vs.start, clipEnd, true};
    // No chips inside the span — fall back to the historical "  // "
    // separator sniff for any remaining un-migrated text.
    if (!isFuncPtr(lm.nodeKind)
        && lm.nodeKind != NodeKind::Pointer32
        && lm.nodeKind != NodeKind::Pointer64)
        return vs;
    QString valText = lineText.mid(vs.start, vs.end - vs.start);
    int sep = valText.indexOf(QLatin1String("  // "));
    if (sep > 0)
        return {vs.start, vs.start + sep, true};
    return vs;
}

// ── Multi-selection ──

QSet<int> RcxEditor::selectedNodeIndices() const {
    int lineFrom, indexFrom, lineTo, indexTo;
    m_sci->getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
    if (lineFrom < 0) {
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        auto* lm = metaForLine(line);
        return lm && lm->nodeIdx >= 0 ? QSet<int>{lm->nodeIdx} : QSet<int>{};
    }
    QSet<int> result;
    for (int line = lineFrom; line <= lineTo; line++) {
        auto* lm = metaForLine(line);
        if (lm && lm->nodeIdx >= 0) result.insert(lm->nodeIdx);
    }
    return result;
}

// ── Inline edit helpers ──

static QString getLineText(QsciScintilla* sci, int line) {
    int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)line);
    if (len <= 0) return {};
    QByteArray buf(len + 1, '\0');
    sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)line, (void*)buf.data());
    QString text = QString::fromUtf8(buf.data(), len);
    while (text.endsWith('\n') || text.endsWith('\r'))
        text.chop(1);
    return text;
}

static QPoint popupAnchorAt(QsciScintilla* sci, int line, int col,
                            const QString& lineText, int* outLineHeight) {
    // Convert char column → UTF-8 byte position. Lines contain
    // multi-byte glyphs (fold arrows ▸/▾, tree connectors, middle
    // dots), so a column index ≠ byte offset; left(col).toUtf8().size()
    // does the right thing for any BMP content we emit.
    long linePos = sci->SendScintilla(
        QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)line);
    long byteOff = lineText.left(col).toUtf8().size();
    int px = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTXFROMPOSITION,
        (unsigned long)0, linePos + byteOff);
    int py = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_POINTYFROMPOSITION,
        (unsigned long)0, linePos);
    int lh = (int)sci->SendScintilla(
        QsciScintillaBase::SCI_TEXTHEIGHT, (unsigned long)line);
    if (outLineHeight) *outLineHeight = lh;
    return sci->viewport()->mapToGlobal(QPoint(px, py + lh));
}

void RcxEditor::applyHeatmapHighlight(const QVector<LineMeta>& meta,
                                       const QVector<QString>& lineTexts,
                                       int firstLine, int lastLine) {
    PROFILE_SCOPE("applyHeatmapHighlight");
    static constexpr int heatIndicators[] = { IND_HEAT_COLD, IND_HEAT_WARM, IND_HEAT_HOT };
    bool full = (firstLine < 0);
    int begin = full ? 0 : firstLine;
    int end = full ? meta.size() : qMin(lastLine + 1, (int)meta.size());

    for (int i = begin; i < end; i++) {
        const LineMeta& lm = meta[i];
        if (isSyntheticLine(lm)) continue;

        int heat = lm.heatLevel;
        int typeW = lm.effectiveTypeW;
        int nameW = lm.effectiveNameW;

        // Always clear all three heat indicators first — cheaper than
        // trying to figure out which one was previously set, and keeps
        // the "rebuild from scratch" mental model.
        for (int hi : heatIndicators)
            clearIndicatorLine(hi, i);

        if (heat <= 0) continue;

        // Pick the right indicator for this heat level (1→cold, 2→warm, 3→hot)
        int activeInd = heatIndicators[qBound(0, heat - 1, 2)];

        // Apply heat-level indicator to value span (narrowed for pointer-like nodes)
        const QString& lineText = lineTexts[i];
        ColumnSpan vs = narrowPtrValueSpan(lm,
            valueSpan(lm, lineText.size(), typeW, nameW), lineText);
        if (!vs.valid) continue;

        // Hex preview rows: paint only the byte spans that actually
        // changed this tick, not the whole 8/16-byte run. The old
        // behavior lit up every byte amber whenever a single byte in
        // the run flipped, which made `C5 13 00 00 00 00 00 00` look
        // like all 8 bytes were churning when really just the low
        // word moved. Per-byte highlighting makes which bits flipped
        // visually obvious. If the bytes haven't changed this tick
        // (heated historically but stable now) we paint nothing —
        // calmer screen, the heat is implicit in "we'll relight when
        // it moves again". Byte N's two hex digits live at columns
        // [vs.start + N*3, vs.start + N*3 + 2).
        if (isHexPreview(lm.nodeKind)) {
            if (lm.changedByteIndices.isEmpty()) continue;
            int sz = sizeForKind(lm.nodeKind);
            for (int b : lm.changedByteIndices) {
                if (b < 0 || b >= sz) continue;
                int charStart = vs.start + b * 3;
                fillIndicatorCols(activeInd, i, charStart, charStart + 2);
            }
            continue;
        }

        // Non-hex value: keep full-value highlight (no per-byte info).
        fillIndicatorCols(activeInd, i, vs.start, vs.end);
    }
}

void RcxEditor::applySymbolColoring(const QVector<LineMeta>& meta,
                                     const QVector<QString>& lineTexts,
                                     int firstLine, int lastLine) {
    // Symbol annotations were inlined into the value text as "// Foo+0x1"
    // and colored here. The chip refactor moved them to ChipKind::Symbol —
    // the per-chip indicator pass paints + pill-backgrounds them now, so
    // this function is left empty as a no-op for callers/timing
    // compatibility (still showed up in profiler traces if removed
    // outright). Drop the body, keep the symbol.
    Q_UNUSED(meta);
    Q_UNUSED(lineTexts);
    Q_UNUSED(firstLine);
    Q_UNUSED(lastLine);
    return;
    // Old body kept under an always-false guard so a search for
    // "applySymbolColoring" still finds the historical implementation
    // (removed once the chip migration ships).
    if (false) {
    PROFILE_SCOPE("applySymbolColoring");
    bool full = (firstLine < 0);
    int begin = full ? 0 : firstLine;
    int end = full ? meta.size() : qMin(lastLine + 1, (int)meta.size());
    for (int i = begin; i < end; i++) {
        const LineMeta& lm = meta[i];
        if (!isFuncPtr(lm.nodeKind)
            && lm.nodeKind != NodeKind::Pointer32
            && lm.nodeKind != NodeKind::Pointer64)
            continue;
        const QString& lineText = lineTexts[i];
        // Find "  // " within the value region and color "// sym" portion green
        ColumnSpan vs = valueSpan(lm, lineText.size(), lm.effectiveTypeW, lm.effectiveNameW);
        if (!vs.valid) continue;
        int searchFrom = vs.start;
        int sep = lineText.indexOf(QLatin1String("  // "), searchFrom);
        if (sep < 0 || sep >= vs.end) continue;
        int symStart = sep + 2;  // start of "// sym"
        int symEnd = vs.end;
        while (symEnd > symStart && lineText[symEnd - 1] == ' ') symEnd--;
        if (symEnd > symStart)
            fillIndicatorCols(IND_HINT_GREEN, i, symStart, symEnd);
    }
    }  // end if (false) dead-code guard
}

void RcxEditor::applyBaseAddressColoring(const QVector<LineMeta>& meta) {
    if (meta.isEmpty() || meta[0].lineKind != LineKind::CommandRow) return;

    clearIndicatorLine(IND_BASE_ADDR, 0);
    // Override lexer's green number coloring on the address with default text color
    QString t = getLineText(m_sci, 0);
    ColumnSpan addr = commandRowAddrSpan(t);
    if (addr.valid)
        fillIndicatorCols(IND_BASE_ADDR, 0, addr.start, addr.end);
}

void RcxEditor::applyCommandRowPills() {
    PROFILE_SCOPE("applyCommandRowPills");
    if (m_meta.isEmpty() || m_meta[0].lineKind != LineKind::CommandRow) return;

    constexpr int line = 0;
    QString t = getLineText(m_sci, line);

    clearIndicatorLine(IND_HEX_DIM, line);
    clearIndicatorLine(IND_CLASS_NAME, line);

    // Dim the [▾] type-selector chevron
    ColumnSpan chevron = commandRowChevronSpan(t);
    if (chevron.valid)
        fillIndicatorCols(IND_HEX_DIM, line, chevron.start, chevron.end);

    // Dim label text: source arrow/placeholder + its ▾ dropdown arrow
    ColumnSpan srcSpan = commandRowSrcSpan(t);
    if (srcSpan.valid) {
        int quotePos = t.indexOf('\'', srcSpan.start);
        int kindEnd = (quotePos > srcSpan.start) ? quotePos : srcSpan.end;
        while (kindEnd > srcSpan.start && t[kindEnd - 1].isSpace()) kindEnd--;
        if (kindEnd > srcSpan.start)
            fillIndicatorCols(IND_HEX_DIM, line, srcSpan.start, kindEnd);
        // Dim the source ▾ dropdown arrow to match (like struct▾)
        int srcDrop = t.indexOf(QChar(0x25BE));
        int rootStart = commandRowRootStart(t);
        if (srcDrop >= 0 && (rootStart < 0 || srcDrop < rootStart))
            fillIndicatorCols(IND_HEX_DIM, line, srcDrop, srcDrop + 1);
    }
    // Dim base address to match source/struct grey
    ColumnSpan addrSpan = commandRowAddrSpan(t);
    if (addrSpan.valid)
        fillIndicatorCols(IND_HEX_DIM, line, addrSpan.start, addrSpan.end);

    // Root class styling (type dim + class-name teal, no underline)
    ColumnSpan rt = commandRowRootTypeSpan(t);
    if (rt.valid) {
        fillIndicatorCols(IND_HEX_DIM, line, rt.start, rt.end);
        int drop = t.indexOf(QChar(0x25BE), rt.start);
        if (drop >= 0)
            fillIndicatorCols(IND_HEX_DIM, line, drop, qMin(drop + 2, t.size()));
    }
    ColumnSpan rn = commandRowRootNameSpan(t);
    if (rn.valid) {
        fillIndicatorCols(IND_CLASS_NAME, line, rn.start, rn.end);
    }

    // Dim trailing opening brace to match the rest of the command row grey
    for (int i = t.size() - 1; i >= 0; --i) {
        if (t[i] == ' ' || t[i] == '\t') continue;
        if (t[i] == '{')
            fillIndicatorCols(IND_HEX_DIM, line, i, i + 1);
        break;
    }
}

// ── Shared inline-edit shutdown ──

RcxEditor::EndEditInfo RcxEditor::endInlineEdit() {
    // Dismiss any open user list / autocomplete popup
    m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCCANCEL);
    if (m_exprResultLabel) m_exprResultLabel->hide();
    // Clear edit comment and error marker before deactivating
    if (m_editState.target == EditTarget::Value
        || (m_editState.hexOverwrite && m_editState.target == EditTarget::Name)) {
        setEditComment({});  // Clear to spaces
        m_sci->markerDelete(m_editState.line, M_ERR);
    }
    // Restore the trailing-padding span that beginInlineEdit wrote so this
    // line stays byte-identical to its pre-edit shape — keeping m_prevText
    // valid for the next applyDocument's diff/patch. Without this the next
    // diff computes against a stale prefix and overwrites an unrelated line.
    //
    // Two anchor models:
    //   • Value / BaseAddress: padding is always the trailing N spaces.
    //     User edits happen *inside* the value column (insert/replace shifts
    //     position but never count). Anchor = end-of-line; strip the last
    //     padBytes chars.
    //   • Comment: padding is the placeholder ("  / ") at padPos plus any
    //     user-typed comment text after it, all of which sits between
    //     padPos and end-of-line. Anchor = padPos.
    if (m_editState.padBytes > 0 && m_editState.line >= 0) {
        bool wasReadOnly = m_sci->isReadOnly();
        if (wasReadOnly) m_sci->setReadOnly(false);
        long lineEnd = m_sci->SendScintilla(
            QsciScintillaBase::SCI_GETLINEENDPOSITION,
            (unsigned long)m_editState.line);
        long stripStart, stripEnd;
        if (m_editState.target == EditTarget::Comment) {
            stripStart = m_editState.padPos;
            stripEnd   = lineEnd;
        } else {
            stripEnd   = lineEnd;
            stripStart = lineEnd - m_editState.padBytes;
        }
        if (stripStart >= 0 && stripEnd >= stripStart) {
            QByteArray restoreBytes(m_editState.padRestoreSpaces, ' ');
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, stripStart);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, stripEnd);
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                 (uintptr_t)restoreBytes.size(),
                                 restoreBytes.constData());
        }
        if (wasReadOnly) m_sci->setReadOnly(true);
        m_editState.padBytes = 0;
        m_editState.padPos   = 0;
        m_editState.padRestoreSpaces = 0;
    }

    // Invalidate the diff base. Inline edits leave Scintilla diverged
    // from m_prevText: the user's typed value sits in Scintilla, while
    // m_prevText still reflects the pre-edit compose output. Letting the
    // next applyDocument run its diff/patch against the stale base
    // shifts byte offsets by the value-edit delta and corrupts an
    // unrelated line further down (the bit_depth-row missing-2-spaces
    // bug). Clearing prevText forces fullReplace on the next refresh —
    // one extra setText per edit, which is bounded and worth it.
    m_prevText.clear();
    EndEditInfo info{m_editState.nodeIdx, m_editState.subLine, m_editState.target};
    m_editState.active = false;
    // Clear IND_EDIT_BOUNDS + reset byte-range state if this was a
    // byte-range edit. The next inline edit gets a clean InlineEditState
    // so commitInlineEdit's byteRange branch doesn't trigger on leftover
    // state from this edit.
    if (m_editState.byteRange) {
        long docLen = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLENGTH);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                             (long)IND_EDIT_BOUNDS);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORCLEARRANGE,
                             (long)0, docLen);
        m_editState.byteRange     = false;
        m_editState.byteRangeAddr = 0;
        m_editState.byteRangeLen  = 0;
        m_editState.byteSegments.clear();
        m_editState.byteSegIdx    = 0;
    }
    m_sci->setReadOnly(true);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETWIDTH, 0);
    // Switch back to Arrow cursor (widget-local, doesn't fight splitters/menus)
    m_sci->viewport()->setCursor(Qt::ArrowCursor);
    // Disable selection rendering again
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELFORE, (long)0, (long)0);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELBACK, (long)0, (long)0);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, (long)1);
    m_sci->SendScintilla(QsciScintillaBase::SCI_EMPTYUNDOBUFFER);
    return info;
}

// ── Span helpers ──

// Name span for struct/array headers - uses column-based positioning
// Format: [fold][indent][type col][sep][name col][sep][suffix]
static ColumnSpan headerNameSpan(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header) return {};

    int ind = kFoldCol + lm.depth * kTreeIndent;
    int typeW = lm.effectiveTypeW;
    int nameStart = ind + typeW + kSepWidth;

    if (nameStart >= lineText.size()) return {};

    // Name ends before " {" suffix (expanded) or at line end (collapsed)
    int nameEnd = lineText.size();
    if (lineText.endsWith(QStringLiteral(" {")))
        nameEnd = lineText.size() - 2;

    if (nameEnd <= nameStart) return {};

    // Don't allow editing array element names like "[0]", "[1]", etc.
    QString name = lineText.mid(nameStart, nameEnd - nameStart).trimmed();
    if (name.isEmpty()) return {};
    if (name.startsWith('[') && name.endsWith(']'))
        return {};

    return {nameStart, nameEnd, true};
}

// Type name span for struct headers (not arrays)
// Named structs format as: "_MMPTE  OriginalPte {" (type column = just the name)
// Anonymous structs format as: "union {" or "struct {" (no clickable type)
static ColumnSpan headerTypeNameSpan(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header) return {};
    if (lm.isArrayHeader) return {};

    int ind = kFoldCol + lm.depth * kTreeIndent;
    int typeW = lm.effectiveTypeW;
    int typeEnd = ind + typeW;
    if (typeEnd > lineText.size()) typeEnd = lineText.size();

    QString typeCol = lineText.mid(ind, typeEnd - ind).trimmed();
    if (typeCol.isEmpty()) return {};

    // Anonymous structs use bare keywords — not clickable
    static const QStringList kKeywords = {
        QStringLiteral("struct"), QStringLiteral("union"), QStringLiteral("class")
    };
    if (kKeywords.contains(typeCol)) return {};

    // Static field headers: "static hex64 target {" — skip "static " prefix
    if (lm.isStaticLine) {
        int cursor = ind;
        while (cursor < typeEnd && lineText[cursor] == ' ') cursor++;
        if (lineText.mid(cursor, 7) == QLatin1String("static "))
            cursor += 7;
        while (cursor < typeEnd && lineText[cursor] == ' ') cursor++;
        int tStart = cursor;
        while (cursor < typeEnd && lineText[cursor] != ' ') cursor++;
        if (cursor > tStart)
            return {tStart, cursor, true};
        return {};
    }

    // Named struct: entire type column is the type name (e.g. "_MMPTE")
    // Find the actual text bounds within the padded column
    int start = ind;
    while (start < typeEnd && lineText[start] == ' ') start++;
    int end = start;
    while (end < typeEnd && lineText[end] != ' ') end++;
    if (end <= start) return {};

    return {start, end, true};
}

// Type span for array headers: "int32_t[10]" in "int32_t[10] positions {"
static ColumnSpan arrayHeaderTypeSpan(const LineMeta& lm, const QString& lineText) {
    if (lm.lineKind != LineKind::Header || !lm.isArrayHeader) return {};
    int ind = kFoldCol + lm.depth * kTreeIndent;
    int typeEnd = lineText.indexOf(' ', ind);
    if (typeEnd <= ind) return {};
    return {ind, typeEnd, true};
}

RcxEditor::NormalizedSpan RcxEditor::normalizeSpan(
    const ColumnSpan& raw, const QString& lineText,
    EditTarget target, bool skipPrefixes) const
{
    if (!raw.valid) return {};
    int textLen = lineText.size();
    if (raw.start >= textLen) return {};

    int start = raw.start;
    int end   = qMin(raw.end, textLen);
    if (end <= start) return {};

    if (skipPrefixes && target == EditTarget::Value) {
        QString spanText = lineText.mid(start, end - start);
        int arrow = spanText.indexOf(QStringLiteral("->"));
        if (arrow >= 0) {
            int i = arrow + 2;
            while (i < spanText.size() && spanText[i].isSpace()) i++;
            start += i;
        } else {
            int eq = spanText.indexOf('=');
            if (eq >= 0 && eq <= 3) {
                int i = eq + 1;
                while (i < spanText.size() && spanText[i].isSpace()) i++;
                start += i;
            }
        }
        if (start >= end) return {};
    }

    QString inner = lineText.mid(start, end - start);
    int lead = 0;
    while (lead < inner.size() && inner[lead].isSpace()) lead++;
    int trail = inner.size();
    while (trail > lead && inner[trail - 1].isSpace()) trail--;
    if (trail <= lead) return {};

    return {start + lead, start + trail, true};
}

bool RcxEditor::resolvedSpanFor(int line, EditTarget t,
                                NormalizedSpan& out, QString* lineTextOut) const {
    const LineMeta* lm = metaForLine(line);
    if (!lm) return false;

    // CommandRow: Source / BaseAddress / Root class (type+name) editing
    if (lm->lineKind == LineKind::CommandRow) {
        if (t != EditTarget::BaseAddress && t != EditTarget::Source
            && t != EditTarget::RootClassType && t != EditTarget::RootClassName
            && t != EditTarget::TypeSelector) return false;
        QString lineText = getLineText(m_sci, line);
        ColumnSpan s;
        if (t == EditTarget::TypeSelector)       s = commandRowChevronSpan(lineText);
        else if (t == EditTarget::Source)        s = commandRowSrcSpan(lineText);
        else if (t == EditTarget::BaseAddress)   s = commandRowAddrSpan(lineText);
        else if (t == EditTarget::RootClassType) s = commandRowRootTypeSpan(lineText);
        else                                     s = commandRowRootNameSpan(lineText);
        out = normalizeSpan(s, lineText, t, /*skipPrefixes=*/(t == EditTarget::BaseAddress));
        if (lineTextOut) *lineTextOut = lineText;
        return out.valid;
    }

    if (lm->nodeIdx < 0) return false;

    // Hex nodes: only Type is editable (ASCII preview + hex bytes are display-only)
    // Exception: static field names are always editable (they're function names)
    if ((t == EditTarget::Name || t == EditTarget::Value) && isHexNode(lm->nodeKind) && !lm->isStaticLine)
        return false;

    QString lineText = getLineText(m_sci, line);
    int textLen = lineText.size();

    // Use per-line effective widths (set during compose based on containing scope)
    int typeW = lm->effectiveTypeW;
    int nameW = lm->effectiveNameW;

    ColumnSpan s;
    switch (t) {
    case EditTarget::Type:        s = typeSpan(*lm, typeW); break;
    case EditTarget::Name:        s = nameSpan(*lm, typeW, nameW); break;
    case EditTarget::Value:       s = narrowPtrValueSpan(*lm,
                                      valueSpan(*lm, textLen, typeW, nameW), lineText); break;
    case EditTarget::BaseAddress: break;  // No longer on header lines
    case EditTarget::ArrayIndex:
    case EditTarget::ArrayCount:
        break;  // Array navigation removed
    case EditTarget::ArrayElementType:
        s = arrayElemTypeSpanFor(*lm, lineText); break;
    case EditTarget::ArrayElementCount:
        s = arrayElemCountSpanFor(*lm, lineText); break;
    case EditTarget::PointerTarget:
        s = pointerTargetSpanFor(*lm, lineText); break;
    case EditTarget::StaticExpr:
        if (lm->isStaticLine)
            s = staticExprSpanFor(*lm, lineText);
        break;
    case EditTarget::Comment: {
        const LineChip* cc = findChip(*lm, ChipKind::Comment);
        if (cc && cc->startCol >= 0 && cc->startCol < textLen)
            s = {cc->startCol, qMin(cc->endCol, textLen), true};
        // Fallback: allow comment creation on any node-bearing row that
        // isn't a continuation/member helper. Header lines (typed-pointer
        // and struct headers) need this too — the user owns Node::comment
        // for those just like for plain Field rows.
        else if (lm->nodeIdx >= 0
                 && (lm->lineKind == LineKind::Field
                  || lm->lineKind == LineKind::Header)
                 && !lm->isContinuation && !lm->isMemberLine)
            s = {textLen - 1, textLen, true};
        break;
    }
    case EditTarget::Source: break;
    }

    // Fallback spans for header lines
    if (!s.valid && t == EditTarget::Type) {
        // For pointer fields, the full type span acts as "kind" span
        // For array headers, fall back to the full type[count] span
        s = arrayHeaderTypeSpan(*lm, lineText);
        if (!s.valid)
            s = headerTypeNameSpan(*lm, lineText);
        if (!s.valid)
            s = pointerKindSpanFor(*lm, lineText);
    }
    if (!s.valid && t == EditTarget::Name)
        s = headerNameSpan(*lm, lineText);

    // Member lines: override Name/Value spans
    if (!s.valid && lm->isMemberLine) {
        if (t == EditTarget::Name)  s = memberNameSpanFor(*lm, lineText);
        if (t == EditTarget::Value) s = memberValueSpanFor(*lm, lineText);
    }

    out = normalizeSpan(s, lineText, t, /*skipPrefixes=*/true);
    if (lineTextOut) *lineTextOut = lineText;
    return out.valid;
}

// ── Point → line/col/nodeId resolution ──

RcxEditor::HitInfo RcxEditor::hitTest(const QPoint& vp) const {
    HitInfo h;

    // Try precise position first (works when cursor is over actual text)
    long pos = m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
                                     (unsigned long)vp.x(), (long)vp.y());
    if (pos >= 0) {
        h.line = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_LINEFROMPOSITION, (unsigned long)pos);
        h.col = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_GETCOLUMN, (unsigned long)pos);
    } else {
        // Fallback: calculate line from Y coordinate (for empty space past text)
        int firstVisible = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_GETFIRSTVISIBLELINE);
        int lineHeight = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_TEXTHEIGHT, 0);
        if (lineHeight > 0)
            h.line = firstVisible + vp.y() / lineHeight;
    }

    if (h.line >= 0 && h.line < m_meta.size()) {
        h.nodeId = m_meta[h.line].nodeId;
        h.inFoldCol = (h.col >= 0 && h.col < kFoldCol + 1 && m_meta[h.line].foldHead);
    }
    return h;
}

// ── Double-click hit test ──

static bool hitTestTarget(QsciScintilla* sci,
                          const QVector<LineMeta>& meta,
                          const QPoint& viewportPos,
                          int& outLine, int& outCol, EditTarget& outTarget)
{
    long pos = sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
                                  (unsigned long)viewportPos.x(), (long)viewportPos.y());
    if (pos < 0) return false;
    int line = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINEFROMPOSITION,
                                       (unsigned long)pos);
    int col  = (int)sci->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN,
                                       (unsigned long)pos);
    outCol = col;
    if (line < 0 || line >= meta.size()) return false;

    QString lineText = getLineText(sci, line);
    int textLen = lineText.size();

    const LineMeta& lm = meta[line];

    if (lm.lineKind == LineKind::ArrayElementSeparator) return false;

    auto inSpan = [&](const ColumnSpan& s) {
        return s.valid && col >= s.start && col < s.end;
    };

    // CommandRow: interactive chevron/SRC/ADDR + root class (type+name)
    if (lm.lineKind == LineKind::CommandRow) {
        ColumnSpan chevron = commandRowChevronSpan(lineText);
        if (inSpan(chevron)) { outTarget = EditTarget::TypeSelector; outLine = line; return true; }
        ColumnSpan ss = commandRowSrcSpan(lineText);
        if (inSpan(ss)) { outTarget = EditTarget::Source; outLine = line; return true; }
        ColumnSpan as = commandRowAddrSpan(lineText);
        if (inSpan(as)) { outTarget = EditTarget::BaseAddress; outLine = line; return true; }

        // RootClassType is no longer clickable — use right-click to convert
        ColumnSpan rns = commandRowRootNameSpan(lineText);
        if (inSpan(rns)) { outTarget = EditTarget::RootClassName; outLine = line; return true; }
        return false;
    }

    // Use per-line effective widths from LineMeta
    int typeW = lm.effectiveTypeW;
    int nameW = lm.effectiveNameW;

    ColumnSpan ts = RcxEditor::typeSpan(lm, typeW);
    ColumnSpan ns = RcxEditor::nameSpan(lm, typeW, nameW);
    ColumnSpan vs = narrowPtrValueSpan(lm,
        RcxEditor::valueSpan(lm, textLen, typeW, nameW), lineText);

    // Pointer fields/headers: check sub-spans within type column first
    if (lm.nodeKind == NodeKind::Pointer32 || lm.nodeKind == NodeKind::Pointer64) {
        ColumnSpan ptrTarget = pointerTargetSpanFor(lm, lineText);
        ColumnSpan ptrKind = pointerKindSpanFor(lm, lineText);
        if (inSpan(ptrTarget)) { outTarget = EditTarget::PointerTarget; outLine = line; return true; }
        if (inSpan(ptrKind))   { outTarget = EditTarget::Type; outLine = line; return true; }
    }

    // Array headers: check element type and count sub-spans first
    // Count click area includes brackets [N] so clicking [ or ] edits the count
    if (lm.isArrayHeader) {
        ColumnSpan elemCountClick = arrayElemCountClickSpanFor(lm, lineText);
        ColumnSpan elemType = arrayElemTypeSpanFor(lm, lineText);
        if (inSpan(elemCountClick)) { outTarget = EditTarget::ArrayElementCount; outLine = line; return true; }
        if (inSpan(elemType))       { outTarget = EditTarget::ArrayElementType; outLine = line; return true; }
    }

    // Fallback spans for header lines
    if (!ts.valid) {
        ts = arrayHeaderTypeSpan(lm, lineText);
        if (!ts.valid)
            ts = headerTypeNameSpan(lm, lineText);
    }
    if (!ns.valid)
        ns = headerNameSpan(lm, lineText);

    // Member lines: use name/value spans from line text (no type span)
    if (lm.isMemberLine) {
        ns = memberNameSpanFor(lm, lineText);
        vs = memberValueSpanFor(lm, lineText);
    }

    // Comment span: from chip startCol to end of chip (if comment exists)
    ColumnSpan cs;
    if (const LineChip* cc = findChip(lm, ChipKind::Comment))
        if (cc->startCol >= 0 && cc->startCol < textLen)
            cs = {cc->startCol, qMin(cc->endCol, textLen), true};

    if (inSpan(ts))      outTarget = EditTarget::Type;
    else if (inSpan(ns)) outTarget = EditTarget::Name;
    else if (inSpan(vs)) outTarget = EditTarget::Value;
    else if (inSpan(cs)) outTarget = EditTarget::Comment;
    else return false;

    // Array headers: redirect generic Type hit to ArrayElementType (uses popup, not inline edit)
    if (lm.isArrayHeader && outTarget == EditTarget::Type) {
        outTarget = EditTarget::ArrayElementType;
        outLine = line;
        return true;
    }
    // Array element lines: type/name click opens element type picker on the parent array header
    if (lm.isArrayElement && (outTarget == EditTarget::Type || outTarget == EditTarget::Name)) {
        outTarget = EditTarget::ArrayElementType;
        // Find the array header line (previous line with isArrayHeader and same nodeIdx)
        for (int l = line - 1; l >= 0; l--) {
            if (l >= meta.size()) continue;
            const LineMeta& hdr = meta[l];
            if (hdr.isArrayHeader && hdr.nodeIdx == lm.nodeIdx) {
                outLine = l;
                return true;
            }
        }
        return false;
    }
    // Hex nodes: only Type is editable (ASCII preview + hex bytes are display-only)
    if ((outTarget == EditTarget::Name || outTarget == EditTarget::Value) && isHexNode(lm.nodeKind))
        return false;

    outLine = line;
    return true;
}

// ── Event filter ──

bool RcxEditor::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_sci && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->matches(QKeySequence::Find)) {
            showFindBar();
            return true;
        }
        bool handled = m_editState.active ? handleEditKey(ke) : handleNormalKey(ke);
        if (!handled && !m_editState.active) {
            // Clear hover on keyboard navigation (stale after scroll)
            m_hoveredNodeId = 0;
            m_hoveredLine = -1;
            applyHoverHighlight();
        }
        return handled;
    }
    if (obj == m_sci->viewport() && event->type() == QEvent::MouseButtonPress
        && m_editState.active) {
        auto* me = static_cast<QMouseEvent*>(event);
        auto h = hitTest(me->pos());

        if (h.line == m_editState.line) {
            int editEnd = editEndCol();
            bool insideTrimmed = (h.col >= m_editState.spanStart && h.col <= editEnd);

            if (insideTrimmed)
                return false;  // inside trimmed text: let Scintilla position cursor

            // Check raw span (full column width) - click in padding moves cursor to end
            const LineMeta* lm = metaForLine(m_editState.line);
            if (lm) {
                QString lineText = getLineText(m_sci, h.line);
                // Use per-line effective widths
                int typeW = lm->effectiveTypeW;
                int nameW = lm->effectiveNameW;
                ColumnSpan raw;
                switch (m_editState.target) {
                case EditTarget::Type:        raw = typeSpan(*lm, typeW); break;
                case EditTarget::Name:        raw = nameSpan(*lm, typeW, nameW); break;
                case EditTarget::Value:       raw = valueSpan(*lm, lineText.size(), typeW, nameW); break;
                case EditTarget::BaseAddress: raw = commandRowAddrSpan(lineText); break;
                case EditTarget::Source:      raw = commandRowSrcSpan(lineText); break;
                case EditTarget::ArrayIndex:  raw = arrayIndexSpanFor(*lm, lineText); break;
                case EditTarget::ArrayCount:  raw = arrayCountSpanFor(*lm, lineText); break;
                case EditTarget::ArrayElementType:  raw = arrayElemTypeSpanFor(*lm, lineText); break;
                case EditTarget::ArrayElementCount: raw = arrayElemCountSpanFor(*lm, lineText); break;
                case EditTarget::PointerTarget:     raw = pointerTargetSpanFor(*lm, lineText); break;
                }
                if (raw.valid && h.col >= raw.start && h.col < raw.end) {
                    // Within raw span but outside trimmed text → move cursor to end
                    long endPos = posFromCol(m_sci, m_editState.line, editEnd);
                    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, endPos);
                    return true;  // consume event
                }
            }
        }

        commitInlineEdit();
        m_currentSelIds.clear();
        return true;  // consume — metadata was recomposed; stale coords unsafe
    }
    // Single-click on fold column (" - " / " + ") toggles fold
    // Other left-clicks emit nodeClicked for selection
    if (obj == m_sci->viewport() && !m_editState.active
        && event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            // Sync hover to click position (prevents hover/selection desync)
            m_lastHoverPos = me->pos();
            m_hoverInside = true;
            auto h = hitTest(me->pos());
            uint64_t newHoverId = (h.line >= 0) ? h.nodeId : 0;
            if (newHoverId != m_hoveredNodeId || h.line != m_hoveredLine) {
                m_hoveredNodeId = newHoverId;
                m_hoveredLine = h.line;
                applyHoverHighlight();
            }

            // Any plain (no Ctrl, no Shift) LMB press clears the byte
            // selection, regardless of whether the press is on a hex
            // byte or anywhere else. If it IS on a byte, m_byteSelAnchor
            // is armed further down and a subsequent drag past 8 px
            // creates a fresh selection — the cleared state is what the
            // user wants between drags. Shift / Ctrl modifiers are
            // preserved (Shift+Click extends, Ctrl is for node-multi-
            // select); the existing extend / toggle handlers below run
            // against the still-active selection.
            if (m_byteSel.has_value()
                && !(me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
                m_byteSel.reset();
                applyByteSelectionOverlay();
            }

            // Shift+Click on a hex byte → extend the byte selection
            // to that byte, matching the standard text-editor "click
            // sets one end, Shift+Click sets the other end" idiom.
            // Anchor is whichever byte the original drag set; if no
            // selection is active yet, Shift+Click is a no-op for
            // byte selection (the node-level Shift+Click handler
            // below still runs).
            if (m_byteSel.has_value()
                && (me->modifiers() & Qt::ShiftModifier)
                && !(me->modifiers() & Qt::ControlModifier)) {
                auto clicked = byteAddrAt(h.line, h.col);
                if (clicked.has_value()) {
                    // Treat lo as the anchor and adjust hi (or flip if
                    // user clicks BEFORE the anchor — pulling lo down).
                    auto [lo, hi] = *m_byteSel;
                    uint64_t anchor = lo;     // anchor = start of selection
                    uint64_t target = *clicked;
                    uint64_t newLo = qMin(anchor, target);
                    uint64_t newHi = qMax(anchor, target) + 1; // half-open
                    m_byteSel = QPair<uint64_t, uint64_t>{newLo, newHi};
                    applyByteSelectionOverlay();
                    return true;  // consume — don't fall through to node click
                }
            }

            if (h.inFoldCol) {
                emit marginClicked(0, h.line, me->modifiers());
                return true;
            }

            // Tail-chip click router. Recompute h.col from the click's
            // pixel x — SCI_GETCOLUMN walks tab-stops which can drift when
            // the line ends in INDIC_ROUNDBOX padding. Pixel-x mapped via
            // SCI_POSITIONFROMPOINT (no "close" fallback) gives the exact
            // glyph the user landed on; everything past line-end maps to
            // the line-end position which is what we want for the chip
            // hit-test (chip ends at line end).
            if (h.line >= 0 && h.line < m_meta.size()
                && m_meta[h.line].lineKind != LineKind::CommandRow) {
                const auto& lm = m_meta[h.line];
                long lineStartPos = m_sci->SendScintilla(
                    QsciScintillaBase::SCI_POSITIONFROMLINE,
                    (unsigned long)h.line);
                long lineEndPos = m_sci->SendScintilla(
                    QsciScintillaBase::SCI_GETLINEENDPOSITION,
                    (unsigned long)h.line);
                int hitCol = h.col;
                long hitPos = m_sci->SendScintilla(
                    QsciScintillaBase::SCI_POSITIONFROMPOINTCLOSE,
                    (unsigned long)me->pos().x(), (long)me->pos().y());
                if (hitPos >= 0 && hitPos >= lineStartPos && hitPos <= lineEndPos) {
                    // Convert byte pos → display column via Scintilla so we
                    // stay in the same coordinate space the chip uses.
                    hitCol = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_GETCOLUMN, (unsigned long)hitPos);
                }
                for (const auto& c : lm.chips) {
                    if (c.startCol < 0) continue;
                    // Inclusive endCol so clicking the right edge of the
                    // last char (or the trailing chip-pill padding column)
                    // still counts as a hit. Without this the closing
                    // glyph (}, ], )) is hard to land on with a fast click.
                    if (hitCol < c.startCol || hitCol > c.endCol) continue;
                    // Paint the pressed overlay on the chip the user just
                    // mouse-down'd on. For chips that fire an action
                    // synchronously below (Enum/TypeHint), the next
                    // applyDocument clears it; for chips that don't
                    // (popups, no-op kinds), MouseButtonRelease clears it.
                    if (chipIsClickable(c.kind)) {
                        m_chipHoverLine     = h.line;
                        m_chipHoverStartCol = c.startCol;
                        m_chipHoverEndCol   = c.endCol;
                        m_chipPressed       = true;
                        applyChipButtonOverlay();
                    }
                    if (c.kind == ChipKind::Enum && c.enumRefNodeId != 0
                        && lm.nodeIdx >= 0) {
                        long pos = posFromCol(m_sci, h.line, c.startCol);
                        int x = (int)m_sci->SendScintilla(
                            QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
                        int y = (int)m_sci->SendScintilla(
                            QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
                        int lh = (int)m_sci->SendScintilla(
                            QsciScintillaBase::SCI_TEXTHEIGHT, (unsigned long)h.line);
                        QPoint gp = m_sci->viewport()->mapToGlobal(QPoint(x, y + lh));
                        emit enumChipClicked(lm.nodeIdx, c.enumRefNodeId,
                                             c.enumCurrentValue, gp);
                        return true;
                    }
                    // RTTI chip click is intentionally not routed —
                    // the chip is visual only. (Both "auto-create class
                    // + retype pointer" and "rename root struct"
                    // semantics were considered and rejected; the chip
                    // is most useful as a read-only data label.)
                    if (c.kind == ChipKind::TypeHint
                        && lm.nodeIdx >= 0
                        && !c.typeHintKinds.isEmpty()) {
                        emit typeHintChipClicked(lm.nodeIdx, c.typeHintKinds);
                        return true;
                    }
                    break;
                }
            }

            // Footer buttons: +1, +10h/+100h/+1000h, +10 (enum), Trim, Top
            if (h.line >= 0 && h.line < m_meta.size()
                && m_meta[h.line].lineKind == LineKind::Footer) {
                QString ft = getLineText(m_sci, h.line);
                uint64_t nid = m_meta[h.line].nodeId;
                // Single-field add — " +1 " padded so it can't collide with
                // +10/+10h/+100h/+1000h (digit after +1 in those, not space).
                int pPlusOne = ft.indexOf(QStringLiteral(" +1 "));
                if (pPlusOne >= 0 && h.col >= pPlusOne && h.col < pPlusOne + 4) {
                    emit appendSingleFieldRequested(nid);
                    return true;
                }
                // Struct: +1000h (0x1000 = 4096 bytes)
                int p1000 = ft.indexOf(QStringLiteral("+1000h"));
                if (p1000 >= 0 && h.col >= p1000 && h.col < p1000 + 6) {
                    emit appendBytesRequested(nid, 0x1000);
                    return true;
                }
                // Struct: +100h (0x100 = 256 bytes)
                int p100 = ft.indexOf(QStringLiteral("+100h"));
                if (p100 >= 0 && p100 != p1000 + 1 && h.col >= p100 && h.col < p100 + 5) {
                    emit appendBytesRequested(nid, 0x100);
                    return true;
                }
                // Struct: +10h (0x10 = 16 bytes)
                int p10 = ft.indexOf(QStringLiteral("+10h"));
                if (p10 >= 0 && p10 != p100 && p10 != p1000 && h.col >= p10 && h.col < p10 + 4) {
                    emit appendBytesRequested(nid, 0x10);
                    return true;
                }
                // Enum: +10 (10 members)
                int add10Start = ft.indexOf(QStringLiteral("+10"));
                if (add10Start >= 0 && add10Start != p10 && add10Start != p100 && add10Start != p1000
                    && h.col >= add10Start && h.col < add10Start + 3) {
                    emit appendEnumMembersRequested(nid, 10);
                    return true;
                }
                int trimStart = ft.indexOf(QStringLiteral("Trim"));
                if (trimStart >= 0 && h.col >= trimStart && h.col < trimStart + 4) {
                    emit trimHexRequested(nid);
                    return true;
                }
                int topStart = ft.indexOf(QStringLiteral("Top"));
                if (topStart >= 0 && h.col >= topStart && h.col < topStart + 3) {
                    m_sci->SendScintilla(QsciScintillaBase::SCI_SETFIRSTVISIBLELINE, (unsigned long)0);
                    return true;
                }
            }
            // CommandRow: try chevron/ADDR edit or consume
            if (h.nodeId == kCommandRowId) {
                int tLine, tCol; EditTarget t;
                if (hitTestTarget(m_sci, m_meta, me->pos(), tLine, tCol, t)) {
                    if (t == EditTarget::TypeSelector)
                        emit typeSelectorRequested();
                    else
                        beginInlineEdit(t, tLine, tCol);
                }
                return true;  // consume all CommandRow clicks
            }
            if (h.nodeId != 0) {
                bool alreadySelected = m_currentSelIds.contains(h.nodeId);
                bool plain = !(me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier));

                // Ctrl+Click on Type/Name of a navigable parent — open the
                // referenced struct in a new tab. Restricted to Header lines
                // (struct/array/pointer with children, collapsed or expanded)
                // so child member rows under an expanded parent don't fire.
                // Multi-select toggle on plain rows is preserved.
                if ((me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
                    == Qt::ControlModifier) {
                    int tLine, tCol; EditTarget t;
                    if (hitTestTarget(m_sci, m_meta, me->pos(), tLine, tCol, t)
                        && (t == EditTarget::Type || t == EditTarget::Name
                            || t == EditTarget::PointerTarget)) {
                        const LineMeta* lm = (tLine >= 0 && tLine < m_meta.size())
                            ? &m_meta[tLine] : nullptr;
                        if (lm && lm->nodeIdx >= 0
                            && lm->lineKind == LineKind::Header) {
                            m_pendingClickNodeId = 0;
                            emit openTypeInNewTabRequested(lm->nodeIdx);
                            return true;
                        }
                    }
                }

                // Click on already-selected node → edit the clicked token
                // (type column opens picker, name opens rename, value opens value edit)
                int tLine, tCol; EditTarget t;
                if (alreadySelected && plain) {
                    if (hitTestTarget(m_sci, m_meta, me->pos(), tLine, tCol, t)) {
                        m_pendingClickNodeId = 0;
                        return beginInlineEdit(t, tLine, tCol);
                    }
                }

                m_dragging = true;
                m_dragStarted = false;
                m_dragStartPos = me->pos();
                m_dragLastLine = h.line;
                m_dragInitMods = me->modifiers();

                // Byte-selection arm: if the press lands on a hex byte
                // AND no modifier is held (Ctrl/Shift mean "extend node
                // selection" — row-drag wins there), record the anchor
                // address. The next MouseMove past the 8-px threshold
                // upgrades from row-drag to byte-drag. Click-without-
                // movement falls through to the row-click below.
                //
                // The top-level "clear byte selection on non-byte click"
                // pass earlier in this handler already dropped any
                // stale m_byteSel when the press wasn't on a hex byte,
                // so we don't repeat that check here.
                auto pressByteAddr = byteAddrAt(h.line, h.col);
                if (pressByteAddr.has_value()
                    && !(me->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) {
                    m_byteSelAnchor = pressByteAddr;
                    m_byteSelDragging = false;
                }

                bool multi = m_currentSelIds.size() > 1;

                if (alreadySelected && multi && plain) {
                    m_pendingClickNodeId = h.nodeId;
                    m_pendingClickLine = h.line;
                    m_pendingClickMods = me->modifiers();
                } else {
                    m_sci->setCursorPosition(h.line, 0);
                    emit nodeClicked(h.line, h.nodeId, me->modifiers());
                    m_pendingClickNodeId = 0;
                }
            }
            return true;  // consume ALL left-clicks (prevent QScintilla caret/cursor)
        }
    }
    // Drag-select: extend selection as mouse moves with button held.
    // Requires minimum drag distance to prevent accidental micro-drag
    // selection. The outer guard now also fires when a byte-drag is
    // armed or active so byte-mode upgrade can pre-empt the row-drag.
    if (obj == m_sci->viewport() && !m_editState.active
        && event->type() == QEvent::MouseMove
        && (m_dragging || m_byteSelDragging || m_byteSelAnchor.has_value())) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->buttons() & Qt::LeftButton) {
            // Byte-drag upgrade check. Fires once when the press anchor
            // is on a hex byte and the user has moved past the 8-px
            // threshold. After upgrade, row-drag state is cleared so
            // we don't double-extend.
            if (m_byteSelAnchor.has_value() && !m_byteSelDragging) {
                QPoint d = me->pos() - m_dragStartPos;
                if (d.manhattanLength() >= 8) {
                    m_byteSelDragging = true;
                    m_dragging          = false;
                    m_dragStarted       = false;
                    m_pendingClickNodeId = 0;
                }
            }
            if (m_byteSelDragging) {
                auto h2 = hitTest(me->pos());
                auto cur = byteAddrAt(h2.line, h2.col);
                if (cur.has_value()) {
                    uint64_t lo = qMin(*m_byteSelAnchor, *cur);
                    uint64_t hi = qMax(*m_byteSelAnchor, *cur) + 1; // half-open
                    m_byteSel = QPair<uint64_t,uint64_t>{lo, hi};
                    applyByteSelectionOverlay();
                }
                // Cursor drifted off any hex byte column → don't shrink
                // the selection. Drag-out-and-back-in stays stable. Swallow
                // the event so row-drag doesn't try to extend underneath.
                return true;
            }
            // Byte arm was set but the drag never moved past threshold —
            // fall through to row-drag handling. Once threshold IS hit
            // and arm goes inactive, row-drag takes over normally.
            if (!m_dragging) return false;

            // Check drag threshold (8 pixels) before starting drag-selection
            if (!m_dragStarted) {
                int dy = me->pos().y() - m_dragStartPos.y();
                if (qAbs(dy) < 8)
                    return true;  // not yet a drag, but still consume (don't let Scintilla handle)
                m_dragStarted = true;
            }

            // Flush deferred click before extending drag
            if (m_pendingClickNodeId != 0) {
                emit nodeClicked(m_pendingClickLine, m_pendingClickNodeId,
                                 m_pendingClickMods);
                m_pendingClickNodeId = 0;
            }
            auto h = hitTest(me->pos());
            if (h.line >= 0 && h.line != m_dragLastLine && h.nodeId != 0) {
                emit nodeClicked(h.line, h.nodeId, m_dragInitMods | Qt::ShiftModifier);
                m_dragLastLine = h.line;
            }
        } else {
            m_dragging = false;
            m_dragStarted = false;
            m_byteSelDragging = false;
            m_byteSelAnchor.reset();
        }
    }
    if (obj == m_sci->viewport() && event->type() == QEvent::MouseButtonRelease) {
        // Byte-drag end: m_byteSel persists after release (so the user
        // can see what they picked). Anchor + dragging flag reset so a
        // subsequent press starts fresh. Pending row click is dropped
        // because the upgrade already cleared m_pendingClickNodeId in
        // MouseMove, but we re-clear here defensively.
        if (m_byteSelDragging) {
            m_byteSelDragging = false;
            m_byteSelAnchor.reset();
            m_dragging        = false;
            m_dragStarted     = false;
            m_pendingClickNodeId = 0;
            return true;
        }
        // Byte arm without drag (click-only on a hex byte) → just drop
        // the arm and let the row-click logic below finish the press
        // (it's already fired on press for new selections, or it fires
        // here for deferred clicks on already-selected rows).
        m_byteSelAnchor.reset();
        m_dragging = false;
        m_dragStarted = false;
        if (m_pendingClickNodeId != 0) {
            m_sci->setCursorPosition(m_pendingClickLine, 0);
            emit nodeClicked(m_pendingClickLine, m_pendingClickNodeId,
                             m_pendingClickMods);
            m_pendingClickNodeId = 0;
        }
        // Drop the pressed-pill overlay back to plain hover (or off, if
        // the cursor is no longer on the chip — updateChipHover handles
        // that on the next MouseMove).
        if (m_chipPressed) {
            m_chipPressed = false;
            applyChipButtonOverlay();
        }
        return true;  // consume release (prevent QScintilla from acting on it)
    }
    // Double-click on offset margin → toggle absolute/relative
    if (obj == m_sci->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(event);
        int margin0Width = (int)m_sci->SendScintilla(
            QsciScintillaBase::SCI_GETMARGINWIDTHN, 0UL, 0L);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if ((int)me->position().x() < margin0Width) {
#else
        if ((int)me->pos().x() < margin0Width) {
#endif
            m_relativeOffsets = !m_relativeOffsets;
            reformatMargins();
            emit relativeOffsetsChanged(m_relativeOffsets);
            return true;
        }
    }
    // Double-click during edit mode: select entire editable text
    if (obj == m_sci->viewport() && m_editState.active
        && event->type() == QEvent::MouseButtonDblClick) {
        m_sci->setSelection(m_editState.line, m_editState.spanStart,
                           m_editState.line, editEndCol());
        return true;
    }
    if (obj == m_sci->viewport() && !m_editState.active
        && event->type() == QEvent::MouseButtonDblClick) {
        auto* me = static_cast<QMouseEvent*>(event);
        auto h = hitTest(me->pos());
        // Hex byte double-click → enter hex-overwrite mode editing the
        // entire hex node's byte range (8 for Hex64, 4 for Hex32, etc).
        // Matches the hex-overwrite mode users get from Enter on a
        // drag-selection. The byteAddrAt() lookup is the same one the
        // press handler uses for byte-drag arming, so the click target
        // is consistent: cursor was I-beam, click acts on bytes.
        if (h.line >= 0 && byteAddrAt(h.line, h.col).has_value()) {
            const LineMeta* lm = metaForLine(h.line);
            if (lm && isHexPreview(lm->nodeKind)) {
                m_pendingClickNodeId = 0;
                if (h.nodeId != 0 && h.nodeId != kCommandRowId)
                    emit nodeClicked(h.line, h.nodeId, Qt::NoModifier);
                int sz = sizeForKind(lm->nodeKind);
                m_byteSel = QPair<uint64_t, uint64_t>{
                    lm->offsetAddr, lm->offsetAddr + uint64_t(sz)};
                applyByteSelectionOverlay();
                beginByteEdit();
                return true;
            }
        }
        int line, tCol; EditTarget t;
        if (hitTestTarget(m_sci, m_meta, me->pos(), line, tCol, t)) {
            m_pendingClickNodeId = 0;   // cancel deferred selection change
            // Narrow selection to this node before editing
            if (h.nodeId != 0 && h.nodeId != kCommandRowId)
                emit nodeClicked(h.line, h.nodeId, Qt::NoModifier);
            return beginInlineEdit(t, line, tCol);
        }
        return true;  // consume even on miss (prevent QScintilla word-select)
    }
    if (obj == m_sci && event->type() == QEvent::FocusOut) {
        auto* fe = static_cast<QFocusEvent*>(event);
        // Commit active edit on focus loss (click-away = save)
        // Deferred so autocomplete popup has time to register as active
        if (m_editState.active && fe->reason() != Qt::PopupFocusReason) {
            QTimer::singleShot(0, this, [this]() {
                if (m_editState.active && !m_sci->hasFocus()
                    && !m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCACTIVE))
                    commitInlineEdit();
            });
        }
        // Clear editable indicators when editor loses focus
        clearIndicatorLine(IND_EDITABLE, m_hintLine);
        m_hintLine = -1;
    }
    if (obj == m_sci && event->type() == QEvent::FocusIn) {
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        updateEditableIndicators(line);
    }
    // Track mouse position for cursor updates (both edit and non-edit mode)
    if (obj == m_sci->viewport()) {
        // Ignore synthetic Leave from setText() during document refresh
        if (m_applyingDocument && event->type() == QEvent::Leave)
            return true;

        if (event->type() == QEvent::MouseMove) {
            m_lastHoverPos = static_cast<QMouseEvent*>(event)->pos();
            m_hoverInside = true;
        } else if (event->type() == QEvent::Leave) {
            // Don't dismiss if cursor moved onto one of our own popups
            QPoint globalCursor = QCursor::pos();
            bool onPopup = false;
            if (m_historyPopup && m_historyPopup->isVisible()
                && m_historyPopup->geometry().contains(globalCursor))
                onPopup = true;
            if (m_popupHost && m_popupHost->isVisible()
                && m_popupHost->geometry().contains(globalCursor))
                onPopup = true;
            if (!onPopup) {
                m_hoverInside = false;
                if (!m_editState.active) {
                    m_hoveredNodeId = 0;
                    m_hoveredLine = -1;
                    applyHoverHighlight();
                }
                clearChipButtonState();
            }
        } else if (event->type() == QEvent::Wheel) {
            m_lastHoverPos = m_sci->viewport()->mapFromGlobal(QCursor::pos());
            m_hoverInside = m_sci->viewport()->rect().contains(m_lastHoverPos);
        }
        // Resolve hovered nodeId on move/wheel (non-edit mode only)
        // Guard: skip during applyDocument — m_nodeLineIndex may be stale
        if (!m_editState.active && !m_applyingDocument &&
            (event->type() == QEvent::MouseMove || event->type() == QEvent::Wheel)) {
            auto h = hitTest(m_lastHoverPos);
            uint64_t newHoverId = (m_hoverInside && h.line >= 0) ? h.nodeId : 0;
            int newHoverLine = (m_hoverInside && h.line >= 0) ? h.line : -1;
            if (newHoverId != m_hoveredNodeId || newHoverLine != m_hoveredLine) {
                m_hoveredNodeId = newHoverId;
                m_hoveredLine = newHoverLine;
                applyHoverHighlight();
            }

            // Chip hover/pressed states — gives chips a button feel so
            // their clickability is visually obvious without needing a
            // tooltip. Only clickable kinds (Comment/TypeHint/Enum/
            // AddComment) participate; RTTI and Symbol stay flat since
            // clicking them does nothing.
            updateChipHover(h);
        }
        // Update cursor on move/leave/wheel (both edit and non-edit mode)
        if (event->type() == QEvent::MouseMove
         || event->type() == QEvent::Leave
         || event->type() == QEvent::Wheel)
            applyHoverCursor();

        // Consume MouseMove in non-edit mode so QScintilla's internal handler
        // doesn't override our cursor (it resets to Arrow for read-only widgets)
        if (!m_editState.active && event->type() == QEvent::MouseMove)
            return true;
    }
    return QWidget::eventFilter(obj, event);
}

// ── Normal mode key handling ──

bool RcxEditor::handleNormalKey(QKeyEvent* ke) {
    // Hover-preview key bindings:
    //   • Tab / Shift+Tab cycle the active preview (no-op when only
    //     one preview is eligible; still consumed so it doesn't fall
    //     through to the comment / type-chooser cycle below)
    //   • Esc dismisses the popup
    //
    // Outside the popup-visible window these keys keep their existing
    // behavior (inline edit cycle, find-bar dismiss, etc.).
    if (m_popupHost && m_popupHost->isVisible()) {
        auto* host = static_cast<HoverPopupHost*>(m_popupHost);
        if (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) {
            if (host->eligibleCount() > 1) {
                if (ke->key() == Qt::Key_Tab) host->cycleNext();
                else                          host->cyclePrev();
            }
            return true;
        }
        if (ke->key() == Qt::Key_Escape) {
            host->dismiss();
            return true;
        }
    }
    switch (ke->key()) {
    case Qt::Key_F2:
        return beginInlineEdit(EditTarget::Name);
    case Qt::Key_F12: {
        // Go to definition — emit for the current node; controller decides
        // whether to navigate (typed pointer, struct ref, etc.) or no-op.
        int ni = currentNodeIndex();
        if (ni >= 0) emit goToDefinitionRequested(ni);
        return true;
    }
    case Qt::Key_T:
        if (ke->modifiers() == Qt::NoModifier)
            return beginInlineEdit(EditTarget::Type);
        return false;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // Byte selection → start hex-overwrite edit on the byte range.
        // beginByteEdit emits statusHintRequested on cross-row refusal.
        if (m_byteSel.has_value()) {
            beginByteEdit();
            return true;
        }
        return beginInlineEdit(EditTarget::Value);
    case Qt::Key_Backspace:
        // Backspace is normally a no-op in normal mode; with a byte
        // selection it zero-fills, matching the Delete behaviour
        // below. Consume only when we own the action so other widgets
        // (find bar) still see the key when no selection is active.
        if (m_byteSel.has_value()) {
            emit byteZeroFillRequested();
            return true;
        }
        return false;
    case Qt::Key_Delete:
        // Byte selection → zero-fill the range. Node selection →
        // existing delete-node behaviour. No selection at all → silent
        // consume (matches today).
        if (m_byteSel.has_value()) {
            emit byteZeroFillRequested();
            return true;
        }
        if (!m_currentSelIds.isEmpty())
            emit deleteSelectedRequested();
        return true;
    case Qt::Key_D:
        if ((ke->modifiers() & Qt::ControlModifier) && !m_currentSelIds.isEmpty()) {
            emit duplicateSelectedRequested();
            return true;
        }
        return false;
    case Qt::Key_C:
        // Ctrl+Shift+C → legacy "copy node address" shortcut. Kept for muscle
        // memory after the clipboard rework. Byte-selection mode takes
        // priority: copies the SELECTION RANGE ("0xLO..0xHI (N bytes)")
        // instead of the line's node address — that's the more useful
        // datum when a range is highlighted.
        if ((ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            == (Qt::ControlModifier | Qt::ShiftModifier)) {
            if (m_byteSel.has_value()) {
                auto [lo, hi] = *m_byteSel;
                QString text = QStringLiteral("0x%1..0x%2 (%3 bytes)")
                    .arg(lo, 0, 16).arg(hi, 0, 16).arg(hi - lo);
                QApplication::clipboard()->setText(text);
                return true;
            }
            int line, col;
            m_sci->getCursorPosition(&line, &col);
            const LineMeta* lm = metaForLine(line);
            if (lm && lm->offsetAddr != 0) {
                QApplication::clipboard()->setText(
                    QStringLiteral("0x") + QString::number(lm->offsetAddr, 16).toUpper());
            }
            return true;
        }
        // Ctrl+C → byte selection wins: copy "DE AD BE EF" hex string.
        // Otherwise copy selected nodes as a portable blob
        // (rcx-clipboard/v1 + plain-text fallback). Controller owns
        // the serialization in both cases.
        if (ke->modifiers() == Qt::ControlModifier) {
            if (m_byteSel.has_value()) {
                emit byteCopyHexRequested();
                return true;
            }
            emit copyNodesRequested();
            return true;
        }
        return false;
    case Qt::Key_X:
        if (ke->modifiers() == Qt::ControlModifier) {
            // Ctrl+X → cut selection: copy then delete.
            emit cutNodesRequested();
            return true;
        }
        return false;
    case Qt::Key_V:
        if (ke->modifiers() == Qt::ControlModifier) {
            // Byte selection → paste hex string over the range.
            // Otherwise paste nodes; controller reads clipboard and
            // inserts under the current parent at cursor offset.
            if (m_byteSel.has_value()) {
                emit bytePasteHexRequested();
                return true;
            }
            emit pasteNodesRequested();
            return true;
        }
        return false;
    case Qt::Key_Insert:
        if (ke->modifiers() & Qt::ShiftModifier)
            emit insertAboveRequested(currentNodeIndex(), NodeKind::Hex32);
        else
            emit insertAboveRequested(currentNodeIndex(), NodeKind::Hex64);
        return true;
    case Qt::Key_Semicolon:
        if (ke->modifiers() == Qt::NoModifier) {
            emit commentEditRequested();
            return true;
        }
        return false;
    case Qt::Key_Up:
    case Qt::Key_Down: {
        int dir = (ke->key() == Qt::Key_Up) ? -1 : 1;
        // Byte-selection mode owns plain Shift+Up/Down: snap the
        // selection's high end to the next/previous hex row boundary.
        // Checked before Ctrl+Shift (reorder) and before normal node
        // navigation so byte-selection-active is the higher-precedence
        // mode. Matches the Left/Right precedence pattern above.
        if (m_byteSel.has_value()
            && ke->modifiers() == Qt::ShiftModifier) {
            snapByteSelectionToRow(dir);
            return true;
        }
        // Ctrl+Shift+Up/Down: reorder field in sibling list
        if ((ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            == (Qt::ControlModifier | Qt::ShiftModifier)) {
            int ni = currentNodeIndex();
            if (ni >= 0)
                emit moveNodeRequested(ni, dir);
            return true;
        }
        // Normal arrow: navigate between nodes
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        for (int i = line + dir; i >= 0 && i < m_meta.size(); i += dir) {
            const auto& lm = m_meta[i];
            if (lm.nodeId == 0 || lm.nodeId == kCommandRowId) continue;
            if (lm.isContinuation) continue;
            m_sci->setCursorPosition(i, 0);
            m_sci->ensureLineVisible(i);
            emit nodeClicked(i, lm.nodeId, ke->modifiers());
            return true;
        }
        // Forward walk fell off the end — auto-append a new field to the
        // enclosing struct of the last visible data node. Mirrors the
        // "+1" footer pill behavior for keyboard users. Up-at-top is a
        // silent no-op (no field inserted above).
        if (dir == 1) {
            for (int i = m_meta.size() - 1; i >= 0; --i) {
                const auto& lm = m_meta[i];
                if (lm.nodeId == 0 || lm.nodeId == kCommandRowId) continue;
                if (lm.isContinuation) continue;
                emit appendSingleFieldRequested(lm.nodeId);
                break;
            }
        }
        return true;
    }
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        // Jump by a screenful of lines (like normal PageUp/Down but landing on a node)
        int visible = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_LINESONSCREEN);
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        int dir = (ke->key() == Qt::Key_PageUp) ? -1 : 1;
        int target = qBound(0, line + dir * visible, m_meta.size() - 1);
        // Find nearest node line from target position
        for (int i = target; i >= 0 && i < m_meta.size(); i += dir) {
            const auto& lm = m_meta[i];
            if (lm.nodeId == 0 || lm.nodeId == kCommandRowId || lm.isContinuation) continue;
            m_sci->setCursorPosition(i, 0);
            m_sci->ensureLineVisible(i);
            emit nodeClicked(i, lm.nodeId, ke->modifiers());
            break;
        }
        return true;
    }
    case Qt::Key_Tab: {
        // Core targets for all field nodes; Type opens popup so put it last
        EditTarget order[] = {EditTarget::Name, EditTarget::Value, EditTarget::Comment,
                              EditTarget::ArrayElementType, EditTarget::ArrayElementCount,
                              EditTarget::PointerTarget, EditTarget::Type};
        constexpr int N = 7;
        // Determine current node kind for filtering
        int curLine, curCol;
        m_sci->getCursorPosition(&curLine, &curCol);
        const LineMeta* curLm = metaForLine(curLine);
        NodeKind nk = curLm ? curLm->nodeKind : NodeKind::Hex8;
        bool isArray = curLm && curLm->isArrayHeader;
        bool isPtr = (nk == NodeKind::Pointer32 || nk == NodeKind::Pointer64);

        int start = 0;
        for (int i = 0; i < N; i++)
            if (order[i] == m_lastTabTarget) { start = (i + 1) % N; break; }
        for (int i = 0; i < N; i++) {
            EditTarget t = order[(start + i) % N];
            // Skip targets that don't apply to this node kind
            if ((t == EditTarget::ArrayElementType || t == EditTarget::ArrayElementCount) && !isArray) continue;
            if (t == EditTarget::PointerTarget && !isPtr) continue;
            if (beginInlineEdit(t)) { m_lastTabTarget = t; return true; }
        }
        return true;
    }
    // ── Type shortcuts ──
    case Qt::Key_Space: {
        // Space = next hex size, Shift+Space = prev hex size
        // Non-hex nodes: convert to hex equivalent first
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        int sz = sizeForKind(lm->nodeKind);
        if (sz <= 0) return false;  // skip containers
        static constexpr NodeKind hexCycle[] = {
            NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
            NodeKind::Hex64
        };
        if (!isHexNode(lm->nodeKind)) {
            // Convert to hex equivalent of same size
            for (auto hk : hexCycle) {
                if (sizeForKind(hk) == sz) {
                    emit quickTypeChangeRequested(ni, hk);
                    return true;
                }
            }
            return false;
        }
        int cur = -1;
        constexpr int N = 4;
        for (int i = 0; i < N; i++) if (hexCycle[i] == lm->nodeKind) { cur = i; break; }
        if (cur < 0) return false;  // hex128 or unknown — not in cycle
        int dir = (ke->modifiers() & Qt::ShiftModifier) ? -1 : 1;
        NodeKind next = hexCycle[(cur + dir + N) % N];
        emit quickTypeChangeRequested(ni, next);
        return true;
    }
    case Qt::Key_1: case Qt::Key_2: case Qt::Key_3: case Qt::Key_4: case Qt::Key_5: {
        // 1=Hex8, 2=Hex16, 3=Hex32, 4=Hex64, 5=Hex128 (any non-container node)
        if (ke->modifiers() != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm || sizeForKind(lm->nodeKind) <= 0) return false;
        static constexpr NodeKind sizes[] = {
            NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
            NodeKind::Hex64, NodeKind::Hex128
        };
        emit quickTypeChangeRequested(ni, sizes[ke->key() - Qt::Key_1]);
        return true;
    }
    case Qt::Key_P: {
        // P = convert to pointer (any non-container node with size >= 4)
        if (ke->modifiers() != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        int sz = sizeForKind(lm->nodeKind);
        if (sz < 4) return false;
        NodeKind target = (sz >= 8) ? NodeKind::Pointer64 : NodeKind::Pointer32;
        emit quickTypeChangeRequested(ni, target);
        return true;
    }
    case Qt::Key_F: {
        // F = convert to float (4-byte) or double (8-byte) from any same-size node
        if (ke->modifiers() != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        int sz = sizeForKind(lm->nodeKind);
        if (sz == 4) emit quickTypeChangeRequested(ni, NodeKind::Float);
        else if (sz == 8) emit quickTypeChangeRequested(ni, NodeKind::Double);
        else return false;
        return true;
    }
    case Qt::Key_S: {
        // S = convert to signed int of same byte size (from any type)
        if (ke->modifiers() != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        int sz = sizeForKind(lm->nodeKind);
        NodeKind target;
        switch (sz) {
        case 1: target = NodeKind::Int8;  break;
        case 2: target = NodeKind::Int16; break;
        case 4: target = NodeKind::Int32; break;
        case 8: target = NodeKind::Int64; break;
        default: return false;
        }
        if (target == lm->nodeKind) return false;  // already signed
        emit quickTypeChangeRequested(ni, target);
        return true;
    }
    case Qt::Key_U: {
        // U = convert to unsigned int of same byte size (from any type)
        if (ke->modifiers() != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        int sz = sizeForKind(lm->nodeKind);
        NodeKind target;
        switch (sz) {
        case 1: target = NodeKind::UInt8;  break;
        case 2: target = NodeKind::UInt16; break;
        case 4: target = NodeKind::UInt32; break;
        case 8: target = NodeKind::UInt64; break;
        default: return false;
        }
        if (target == lm->nodeKind) return false;  // already unsigned
        emit quickTypeChangeRequested(ni, target);
        return true;
    }
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const int dir = (ke->key() == Qt::Key_Right) ? 1 : -1;
        const Qt::KeyboardModifiers m = ke->modifiers();

        // ── Byte selection takes priority ──
        // Shift+arrow OR Ctrl+Shift+arrow → extend selection by 1 byte
        //   (positive dir grows `hi` rightward; negative dir shrinks
        //   it). Both bindings do the same thing — Ctrl+Shift is a
        //   common "extend" muscle memory from other text editors.
        // Plain ← / → → no-op (absorbed). Users clear with Esc and
        //   extend with Shift; plain arrows must not silently destroy
        //   a multi-byte selection.
        // Other modifier combos fall through to the existing variant
        // cycle path.
        if (m_byteSel.has_value()) {
            if (m == Qt::ShiftModifier
                || m == (Qt::ControlModifier | Qt::ShiftModifier)) {
                extendByteSelection(dir);
                return true;
            }
            if (m == Qt::NoModifier) {
                // Absorb the keypress so Scintilla's cursor doesn't
                // move (which would fire nodeClicked and clobber the
                // current selection state).
                return true;
            }
        }

        // Cycle through same-size type variants (non-container nodes only)
        if (m != Qt::NoModifier) return false;
        int ni = currentNodeIndex();
        if (ni < 0) return false;
        const LineMeta* lm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
        if (!lm) return false;
        // Only cycle for nodes with a fixed byte size (skip Struct/Array)
        int sz = sizeForKind(lm->nodeKind);
        if (sz <= 0) return false;
        emit cycleSameSizeTypeRequested(ni, dir);
        return true;
    }
    // ── General navigation shortcuts ──
    case Qt::Key_Escape:
        if (ke->modifiers() == Qt::NoModifier) {
            // Two-stage Esc: first drop byte selection if any (so the
            // user can clear the byte highlight without losing their
            // row-level node selection); a second Esc then clears the
            // node selection. Matches the "Esc undoes the smallest
            // visible state" pattern from text editors.
            if (m_byteSel.has_value()) {
                m_byteSel.reset();
                applyByteSelectionOverlay();
                return true;
            }
            // Clear selection (deselect all)
            emit nodeClicked(-1, 0, Qt::NoModifier);
            return true;
        }
        return false;
    case Qt::Key_Home: {
        // Byte-selection-mode: Shift+Home collapses the selection back
        // to a single byte at the anchor. Mirrors the Shift+End extend
        // case below. Only handles the Shift+Home combination; plain
        // Home still does node nav.
        if (m_byteSel.has_value()
            && ke->modifiers() == Qt::ShiftModifier) {
            auto [lo, _] = *m_byteSel;
            m_byteSel = QPair<uint64_t, uint64_t>{lo, lo + 1};
            applyByteSelectionOverlay();
            return true;
        }
        // Jump to first data node
        for (int i = 0; i < m_meta.size(); i++) {
            const auto& lm = m_meta[i];
            if (lm.nodeId != 0 && lm.nodeId != kCommandRowId && !lm.isContinuation) {
                m_sci->setCursorPosition(i, 0);
                m_sci->ensureLineVisible(i);
                emit nodeClicked(i, lm.nodeId, Qt::NoModifier);
                return true;
            }
        }
        return true;
    }
    case Qt::Key_End: {
        // Byte-selection-mode: Shift+End extends `hi` to the last hex
        // byte across every hex preview row. Same docHi math as
        // extendByteSelection's positive clamp — single-shot version
        // of "Shift+Down until you can't".
        if (m_byteSel.has_value()
            && ke->modifiers() == Qt::ShiftModifier) {
            auto [lo, hi] = *m_byteSel;
            uint64_t docHi = 0;
            bool anyHex = false;
            for (const LineMeta& lm : m_meta) {
                if (lm.lineKind != LineKind::Field) continue;
                if (!isHexPreview(lm.nodeKind)) continue;
                uint64_t lineHi = lm.offsetAddr
                    + static_cast<uint64_t>(sizeForKind(lm.nodeKind));
                if (!anyHex || lineHi > docHi) { docHi = lineHi; anyHex = true; }
            }
            if (anyHex && docHi > hi) {
                m_byteSel = QPair<uint64_t, uint64_t>{lo, docHi};
                applyByteSelectionOverlay();
            }
            return true;
        }
        // Jump to last data node
        for (int i = m_meta.size() - 1; i >= 0; i--) {
            const auto& lm = m_meta[i];
            if (lm.nodeId != 0 && lm.nodeId != kCommandRowId
                && !lm.isContinuation && lm.lineKind != LineKind::Footer) {
                m_sci->setCursorPosition(i, 0);
                m_sci->ensureLineVisible(i);
                emit nodeClicked(i, lm.nodeId, Qt::NoModifier);
                return true;
            }
        }
        return true;
    }
    case Qt::Key_A:
        if (ke->modifiers() & Qt::ControlModifier) {
            // Byte-selection mode wins: select all bytes of the
            // containing hex node. Also fires when no byte selection
            // is active but the cursor sits on a hex preview row's
            // value column — creates a fresh whole-node byte sel.
            if (m_byteSel.has_value()) {
                selectAllHexBytes();
                return true;
            }
            {
                int curLine = 0, curCol = 0;
                m_sci->getCursorPosition(&curLine, &curCol);
                const LineMeta* lm = metaForLine(curLine);
                if (lm && lm->lineKind == LineKind::Field
                    && isHexPreview(lm->nodeKind)) {
                    selectAllHexBytes();
                    if (m_byteSel.has_value()) return true;
                }
            }
            // Select all siblings of current node
            int ni = currentNodeIndex();
            if (ni < 0) return true;
            // Find all lines with the same parent and emit shift-click on first/last
            const LineMeta* curLm = metaForLine([&]{ int l,c; m_sci->getCursorPosition(&l,&c); return l; }());
            if (!curLm) return true;
            // Click first node without modifier, then shift-click last to range-select
            int first = -1, last = -1;
            for (int i = 0; i < m_meta.size(); i++) {
                const auto& lm = m_meta[i];
                if (lm.nodeId == 0 || lm.nodeId == kCommandRowId || lm.isContinuation) continue;
                if (lm.lineKind == LineKind::Footer) continue;
                if (first < 0) first = i;
                last = i;
            }
            if (first >= 0 && last >= 0) {
                emit nodeClicked(first, m_meta[first].nodeId, Qt::NoModifier);
                if (last != first)
                    emit nodeClicked(last, m_meta[last].nodeId, Qt::ShiftModifier);
            }
            return true;
        }
        return false;
    case Qt::Key_BracketLeft:
        if ((ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            == (Qt::ControlModifier | Qt::ShiftModifier)) {
            emit collapseAllRequested();
            return true;
        }
        return false;
    case Qt::Key_BracketRight:
        if ((ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            == (Qt::ControlModifier | Qt::ShiftModifier)) {
            emit expandAllRequested();
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ── Edit mode key handling ──

bool RcxEditor::handleEditKey(QKeyEvent* ke) {
    // Hex/ASCII overwrite mode: fully custom key handling
    if (m_editState.hexOverwrite)
        return handleHexEditKey(ke);

    // User list is handled via userListActivated signal, not here
    // SCI_AUTOCACTIVE is for autocomplete, not user lists

    switch (ke->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commitInlineEdit();
        return true;
    case Qt::Key_Tab:
        m_lastTabTarget = m_editState.target;
        commitInlineEdit();
        return true;
    case Qt::Key_Escape:
        cancelInlineEdit();
        return true;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;  // block line navigation
    case Qt::Key_Delete: {
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        if (col >= editEndCol()) return true;  // block at end
        return false;  // allow delete within span
    }
    case Qt::Key_Left:
    case Qt::Key_Backspace: {
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        int minCol = m_editState.spanStart;
        // If there's an active selection, collapse it to the left end (Left only, not Backspace)
        if (ke->key() == Qt::Key_Left) {
            int sL, sC, eL, eC;
            m_sci->getSelection(&sL, &sC, &eL, &eC);
            if (sL >= 0 && (sL != eL || sC != eC)) {
                int leftEnd = qMax(qMin(sC, eC), minCol);
                m_sci->setCursorPosition(m_editState.line, leftEnd);
                return true;
            }
        }
        if (col <= minCol) return true;
        return false;
    }
    case Qt::Key_Right: {
        int line, col;
        m_sci->getCursorPosition(&line, &col);
        // If there's an active selection, collapse it to the right end first
        int sL, sC, eL, eC;
        m_sci->getSelection(&sL, &sC, &eL, &eC);
        if (sL >= 0 && (sL != eL || sC != eC)) {
            int rightEnd = qMin(qMax(sC, eC), editEndCol());
            m_sci->setCursorPosition(m_editState.line, rightEnd);
            return true;
        }
        if (col >= editEndCol()) return true;  // block past end
        return false;
    }
    case Qt::Key_Home:
        m_sci->setCursorPosition(m_editState.line, m_editState.spanStart);
        return true;
    case Qt::Key_End:
        m_sci->setCursorPosition(m_editState.line, editEndCol());
        return true;
    case Qt::Key_V:
        if (ke->modifiers() & Qt::ControlModifier) {
            // Sanitized paste: strip newlines (and backticks for base addresses)
            QString clip = QApplication::clipboard()->text();
            clip.remove('\n');
            clip.remove('\r');
            if (m_editState.target == EditTarget::BaseAddress)
                clip.remove('`');
            if (!clip.isEmpty()) {
                QByteArray utf8 = clip.toUtf8();
                m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                                     (uintptr_t)0, utf8.constData());
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

// ── Hex/ASCII overwrite-mode key handling ──

bool RcxEditor::handleHexEditKey(QKeyEvent* ke) {
    const bool isHexMode = (m_editState.target == EditTarget::Value);
    // isHexMode = true: editing "00 00 00 00 00 00 00 00" (hex bytes)
    // isHexMode = false: editing "........" (ASCII preview)

    int line, col;
    m_sci->getCursorPosition(&line, &col);
    const int spanStart = m_editState.spanStart;
    const int spanEnd = spanStart + m_editState.original.size();

    // Helper: replace a single character and re-apply hex dimming indicator
    // (SCI_REPLACETARGET clears indicators at the edges of an indicator
    // range — i.e. the very first / very last char of the active edit-
    // bounds span loses its background highlight on replace. Re-fill
    // IND_EDIT_BOUNDS at the replacement pos while a byte-range edit is
    // in flight to keep the highlight contiguous as the user types past
    // segment boundaries.)
    auto replaceCharAt = [this](long pos, char ch) {
        QByteArray buf(1, ch);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, pos);
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, pos + 1);
        m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                             (uintptr_t)1, buf.constData());
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, IND_HEX_DIM);
        m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, pos, 1);
        if (m_editState.byteRange) {
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT,
                                 IND_EDIT_BOUNDS);
            m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, pos, 1);
        }
    };

    switch (ke->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commitInlineEdit();
        return true;
    case Qt::Key_Escape:
        cancelInlineEdit();
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return true;  // block

    case Qt::Key_Home:
        m_sci->setCursorPosition(line, spanStart);
        return true;
    case Qt::Key_End: {
        // Last data position (last char of span)
        int endCol = spanEnd - 1;
        if (endCol < spanStart) endCol = spanStart;
        m_sci->setCursorPosition(line, endCol);
        return true;
    }

    case Qt::Key_Left: {
        if (col <= spanStart) {
            // Multi-row: hop to the previous segment (last byte). For
            // single-row edits advanceToByteSegment is a no-op.
            if (advanceToByteSegment(-1)) return true;
            return true;
        }
        int newCol = col - 1;
        // In hex mode, skip over space separators
        if (isHexMode) {
            QString lineText = getLineText(m_sci, line);
            if (newCol >= spanStart && newCol < lineText.size() && lineText[newCol] == ' ')
                newCol--;
        }
        if (newCol < spanStart) newCol = spanStart;
        m_sci->setCursorPosition(line, newCol);
        return true;
    }

    case Qt::Key_Right: {
        if (col >= spanEnd - 1) {
            // Multi-row: hop to the next segment (first byte).
            if (advanceToByteSegment(+1)) return true;
            return true;
        }
        int newCol = col + 1;
        if (isHexMode) {
            QString lineText = getLineText(m_sci, line);
            if (newCol < spanEnd && newCol < lineText.size() && lineText[newCol] == ' ')
                newCol++;
        }
        if (newCol >= spanEnd) newCol = spanEnd - 1;
        m_sci->setCursorPosition(line, newCol);
        return true;
    }

    case Qt::Key_Backspace: {
        if (col <= spanStart) {
            // Multi-row: hop to previous segment, reset its last byte
            // to the reset value. Same UX as backspacing through a
            // soft wrap in a regular text editor.
            if (advanceToByteSegment(-1)) {
                int newLine, newCol;
                m_sci->getCursorPosition(&newLine, &newCol);
                long resetPos = posFromCol(m_sci, newLine, newCol);
                replaceCharAt(resetPos, isHexMode ? '0' : '.');
                m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, resetPos);
            }
            return true;
        }
        int prevCol = col - 1;
        if (isHexMode) {
            QString lineText = getLineText(m_sci, line);
            if (prevCol >= spanStart && prevCol < lineText.size() && lineText[prevCol] == ' ')
                prevCol--;
        }
        if (prevCol < spanStart) return true;
        // Replace previous char with reset value
        long pos = posFromCol(m_sci, line, prevCol);
        replaceCharAt(pos, isHexMode ? '0' : '.');
        m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, pos);
        return true;
    }

    case Qt::Key_Delete: {
        if (col >= spanEnd) return true;
        // Skip space separators in hex mode
        if (isHexMode) {
            QString lineText = getLineText(m_sci, line);
            if (col < lineText.size() && lineText[col] == ' ') return true;
        }
        // Reset current char
        long pos = posFromCol(m_sci, line, col);
        replaceCharAt(pos, isHexMode ? '0' : '.');
        m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, pos);
        return true;
    }

    case Qt::Key_Z:
        if (ke->modifiers() & Qt::ControlModifier)
            return true;  // block Ctrl+Z during hex overwrite
        break;

    case Qt::Key_V:
        if (ke->modifiers() & Qt::ControlModifier) {
            QString clip = QApplication::clipboard()->text();
            clip.remove('\n');
            clip.remove('\r');
            if (!clip.isEmpty()) {
                QString lineText = getLineText(m_sci, line);
                int writeCol = col;
                for (int i = 0; i < clip.size() && writeCol < spanEnd; i++) {
                    QChar ch = clip[i];
                    if (isHexMode) {
                        // Skip spaces in paste content
                        if (ch == ' ') continue;
                        // Skip over space separators in the target
                        if (writeCol < lineText.size() && lineText[writeCol] == ' ')
                            writeCol++;
                        if (writeCol >= spanEnd) break;
                        // Only accept hex digits
                        if (!ch.isDigit() && !(ch >= 'a' && ch <= 'f') && !(ch >= 'A' && ch <= 'F'))
                            continue;
                        ch = ch.toUpper();
                    } else {
                        // Only accept printable ASCII
                        if (ch.unicode() < 0x20 || ch.unicode() > 0x7E) continue;
                    }
                    long pos = posFromCol(m_sci, line, writeCol);
                    replaceCharAt(pos, (char)ch.toLatin1());
                    writeCol++;
                    // Re-read after each replace for hex space skip
                    if (isHexMode) lineText = getLineText(m_sci, line);
                }
                int finalCol = qMin(writeCol, spanEnd - 1);
                // In hex mode, if we landed on a space, advance past it
                if (isHexMode) {
                    lineText = getLineText(m_sci, line);
                    if (finalCol < spanEnd && finalCol < lineText.size() && lineText[finalCol] == ' ')
                        finalCol++;
                    if (finalCol >= spanEnd) finalCol = spanEnd - 1;
                }
                m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                     posFromCol(m_sci, line, finalCol));
            }
            return true;
        }
        break;

    default:
        break;
    }

    // Character input: overwrite current position and advance
    QString text = ke->text();
    if (text.isEmpty() || text[0].unicode() < 0x20)
        return true;  // consume non-printable (block default Scintilla handling)

    QChar ch = text[0];

    if (isHexMode) {
        // Only accept hex digits
        if (!ch.isDigit() && !(ch >= 'a' && ch <= 'f') && !(ch >= 'A' && ch <= 'F'))
            return true;
        ch = ch.toUpper();

        // If cursor is on a space, skip to next byte
        QString lineText = getLineText(m_sci, line);
        int writeCol = col;
        if (writeCol < lineText.size() && lineText[writeCol] == ' ')
            writeCol++;
        if (writeCol >= spanEnd) {
            // End of current segment — for multi-row, advance to the
            // next segment and write the digit there. Single-row just
            // clamps (no-op).
            if (!advanceToByteSegment(+1)) return true;
            m_sci->getCursorPosition(&line, &writeCol);
            // After advance, m_editState.spanStart/original reflect the
            // new segment — re-read the locals for the write below.
        }

        // Overwrite current digit
        long pos = posFromCol(m_sci, line, writeCol);
        replaceCharAt(pos, (char)ch.toLatin1());

        // Advance cursor, skip over spaces. If we'd run past the
        // segment's end, hop to the next segment's first byte. At the
        // VERY end of the edit (last segment, last digit) place the
        // caret AT spanEnd (one past the last digit) so it reads as
        // "you've filled everything" rather than sitting on top of
        // the final character.
        int newSpanEnd = m_editState.spanStart + m_editState.original.size();
        int nextCol = writeCol + 1;
        lineText = getLineText(m_sci, line);
        if (nextCol < newSpanEnd && nextCol < lineText.size()
            && lineText[nextCol] == ' ')
            nextCol++;
        if (nextCol >= newSpanEnd) {
            if (advanceToByteSegment(+1)) return true;
            // End of last segment — caret past the last digit.
            m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                                 posFromCol(m_sci, line, newSpanEnd));
            return true;
        }
        m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                             posFromCol(m_sci, line, nextCol));
    } else {
        // ASCII mode: only printable ASCII
        if (ch.unicode() < 0x20 || ch.unicode() > 0x7E)
            return true;
        if (col >= spanEnd) return true;

        // Overwrite current char
        long pos = posFromCol(m_sci, line, col);
        replaceCharAt(pos, (char)ch.toLatin1());

        // Advance cursor
        int nextCol = col + 1;
        if (nextCol >= spanEnd) nextCol = spanEnd - 1;
        m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS,
                             posFromCol(m_sci, line, nextCol));
    }
    return true;
}

// ── Begin inline edit ──

bool RcxEditor::beginInlineEdit(EditTarget target, int line, int col) {
    if (target == EditTarget::TypeSelector) return false;  // handled by popup, not inline edit

    // Edit and byte selection don't coexist this pass — clear any
    // active byte highlight so the edit visuals own the row. If we
    // ever wire "Enter on a byte selection → overwrite those bytes",
    // this is the point that would change.
    if (m_byteSel.has_value()) {
        m_byteSel.reset();
        applyByteSelectionOverlay();
    }

    // Type, array element type and pointer target: handled by TypeSelectorPopup, not inline edit
    if (target == EditTarget::Type || target == EditTarget::ArrayElementType || target == EditTarget::PointerTarget) {
        if (line < 0) {
            int c;
            m_sci->getCursorPosition(&line, &c);
        }
        auto* lm = metaForLine(line);
        if (!lm) return false;
        // Reject lines that don't support type editing
        if (lm->nodeIdx < 0) return false;              // CommandRow etc.
        if (lm->lineKind == LineKind::Footer) return false;
        // Position popup at the type column start
        ColumnSpan ts = typeSpan(*lm);
        long typePos = posFromCol(m_sci, line, ts.valid ? ts.start : 0);
        int lineH = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, (unsigned long)line);
        int x = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, (unsigned long)0, typePos);
        int y = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, (unsigned long)0, typePos);
        QPoint pos = m_sci->viewport()->mapToGlobal(QPoint(x, y + lineH));
        emit typePickerRequested(target, lm->nodeIdx, pos);
        return true;
    }

    // Source: handled by SourceChooserPopup, not inline edit
    if (target == EditTarget::Source) {
        // Position popup below the source span in the command row
        QString cmdText = getLineText(m_sci, 0);
        ColumnSpan srcSpan = commandRowSrcSpan(cmdText);
        int col = srcSpan.valid ? srcSpan.start : 0;
        long srcPos = m_sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN, 0UL, (long)col);
        int lineH = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0);
        int sx = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, srcPos);
        int sy = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, srcPos);
        QPoint pos = m_sci->viewport()->mapToGlobal(QPoint(sx, sy + lineH));
        emit sourcePopupRequested(pos);
        return true;
    }

    if (m_editState.active) return false;
    // Padding tracker is per-edit — reset before any padding extension may run.
    m_editState.padBytes = 0;
    m_editState.padPos   = 0;
    m_editState.padRestoreSpaces = 0;
    m_hoveredNodeId = 0;
    m_hoveredLine = -1;
    applyHoverHighlight();
    // Dismiss hover popups so they get recreated with Set buttons once edit starts
    dismissAllPopups();
    // Clear editable-token color hints (de-emphasize non-active tokens)
    clearIndicatorLine(IND_EDITABLE, m_hintLine);
    m_hintLine = -1;

    if (line >= 0) {
        m_sci->setCursorPosition(line, col >= 0 ? col : 0);
    }
    if (col < 0) {
        m_sci->getCursorPosition(&line, &col);
    }
    auto* lm = metaForLine(line);
    if (!lm) return false;
    // Allow nodeIdx=-1 only for CommandRow editing (command bar)
    if (lm->nodeIdx < 0 && !(lm->lineKind == LineKind::CommandRow &&
        (target == EditTarget::BaseAddress || target == EditTarget::Source
         || target == EditTarget::RootClassType || target == EditTarget::RootClassName)))
        return false;
    // Hex nodes: only Type is editable via normal flow (double-click, F2, Enter)
    // Exception: context-menu-initiated hex/ASCII edits bypass this via m_hexEditPending
    bool isHexEdit = m_hexEditPending && isHexNode(lm->nodeKind) && !lm->isStaticLine
        && (target == EditTarget::Name || target == EditTarget::Value);
    m_hexEditPending = false;
    if ((target == EditTarget::Name || target == EditTarget::Value)
        && isHexNode(lm->nodeKind) && !lm->isStaticLine && !isHexEdit)
        return false;

    QString lineText;
    NormalizedSpan norm;

    if (isHexEdit) {
        // Compute hex spans directly (bypasses resolvedSpanFor which also blocks hex)
        lineText = getLineText(m_sci, line);
        int typeW = lm->effectiveTypeW;
        int nameW = lm->effectiveNameW;
        int byteCount = sizeForKind(lm->nodeKind);
        if (target == EditTarget::Name) {
            // ASCII preview: exactly byteCount chars (no trailing-space trim)
            ColumnSpan s = nameSpanFor(*lm, typeW, nameW);
            if (!s.valid) return false;
            norm = {s.start, s.start + byteCount, true};
        } else {
            // Hex bytes: "XX XX XX..." = byteCount*3-1 chars
            ColumnSpan s = valueSpanFor(*lm, lineText.size(), typeW, nameW);
            if (!s.valid) return false;
            int hexWidth = byteCount * 3 - 1;
            norm = {s.start, s.start + hexWidth, true};
        }
    } else if (target == EditTarget::Comment) {
        // Comment editing: find Comment chip span or create new at end of line
        lineText = getLineText(m_sci, line);
        const LineChip* cc = findChip(*lm, ChipKind::Comment);
        if (cc && cc->startCol >= 0 && cc->startCol < lineText.size()) {
            norm = {cc->startCol, qMin(cc->endCol, (int)lineText.size()), true};
        } else {
            // No existing comment — create at end of line
            norm = {(int)lineText.size(), (int)lineText.size(), true};
        }
    } else {
        if (!resolvedSpanFor(line, target, norm, &lineText)) return false;
    }

    QString trimmed = lineText.mid(norm.start, norm.end - norm.start);

    int vecComponent = 0;  // which vector/matrix component

    // Helper: parse comma-separated components, narrow span to clicked one
    auto narrowToComponent = [&](const QString& inner, int innerAbsStart) {
        QVector<int> compStarts, compEnds;
        for (int i = 0; i < inner.size(); i++) {
            if (inner[i] == ',') {
                compEnds.append(i);
                int next = i + 1;
                while (next < inner.size() && inner[next] == ' ') next++;
                compStarts.append(next);
            }
        }
        compStarts.prepend(0);
        compEnds.append(inner.size());

        int relCol = col - innerAbsStart;
        vecComponent = 0;
        for (int i = 0; i < compStarts.size(); i++) {
            if (relCol >= compStarts[i] && (i == compStarts.size() - 1 || relCol < compStarts[i + 1]))
                { vecComponent = i; break; }
        }
        if (vecComponent >= compStarts.size()) vecComponent = compStarts.size() - 1;

        int cStart = innerAbsStart + compStarts[vecComponent];
        int cEnd = innerAbsStart + compEnds[vecComponent];
        while (cEnd > cStart && lineText[cEnd - 1] == ' ') cEnd--;
        norm.start = cStart;
        norm.end = cEnd;
        trimmed = lineText.mid(norm.start, norm.end - norm.start);
    };

    // For vector value editing: narrow span to the clicked component
    if (target == EditTarget::Value && isVectorKind(lm->nodeKind)) {
        narrowToComponent(trimmed, norm.start);
    }

    // For Mat4x4 value editing: skip "rowN [...]" and narrow to clicked component
    if (target == EditTarget::Value && isMatrixKind(lm->nodeKind)) {
        int bracketOpen = trimmed.indexOf('[');
        int bracketClose = trimmed.lastIndexOf(']');
        if (bracketOpen < 0 || bracketClose <= bracketOpen)
            return false;
        QString inner = trimmed.mid(bracketOpen + 1, bracketClose - bracketOpen - 1);
        int innerAbsStart = norm.start + bracketOpen + 1;
        narrowToComponent(inner, innerAbsStart);
    }

    // Comment editing: strip the chip-marker prefix ("/ ", "/", or legacy
    // "// "/"//") from trimmed text so the user edits the comment body only,
    // not the marker. Save path doesn't re-apply the marker — compose
    // re-emits the chip on the next pass.
    if (target == EditTarget::Comment) {
        if (trimmed.startsWith(QStringLiteral("// ")))
            trimmed = trimmed.mid(3);
        else if (trimmed.startsWith(QStringLiteral("//")))
            trimmed = trimmed.mid(2);
        else if (trimmed.startsWith(QStringLiteral("/ ")))
            trimmed = trimmed.mid(2);
        else if (trimmed.startsWith(QStringLiteral("/")))
            trimmed = trimmed.mid(1);
    }

    m_editState.active = true;
    m_editState.line = line;
    m_editState.nodeIdx = lm->nodeIdx;
    m_editState.subLine = lm->subLine;
    m_editState.target = target;
    m_editState.spanStart = norm.start;
    m_editState.original = trimmed;
    m_editState.linelenAfterReplace = lineText.size();
    m_editState.editKind = lm->nodeKind;
    m_editState.hexOverwrite = isHexEdit;
    if (isVectorKind(lm->nodeKind)) {
        m_editState.subLine = vecComponent;
        m_editState.editKind = NodeKind::Float;
    }
    if (isMatrixKind(lm->nodeKind)) {
        m_editState.subLine = lm->subLine * 4 + vecComponent;  // flat index 0-15
        m_editState.editKind = NodeKind::Float;
    }

    // Store fixed comment column position for value editing (and hex ASCII edits)
    // Use large lineLength so commentCol is always computed (padding added dynamically)
    if (target == EditTarget::Value || (isHexEdit && target == EditTarget::Name)) {
        ColumnSpan cs = commentSpanFor(*lm, 9999, lm->effectiveTypeW, lm->effectiveNameW);
        m_editState.commentCol = cs.valid ? cs.start : -1;
        m_editState.lastValidationOk = true;  // original value is always valid
    } else if (target == EditTarget::BaseAddress) {
        m_editState.commentCol = (int)lineText.size() + 2;  // after full command row content
    } else {
        m_editState.commentCol = -1;
    }

    // Keep undo collection enabled during inline edit so CellBuffer::DeleteChars
    // returns valid text pointers (collectingUndo=false returns nullptr, which
    // crashes QsciAccessibleBase::textDeleted). We clear the buffer on edit end.
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETUNDOCOLLECTION, (long)1);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCARETWIDTH, 1);
    m_sci->setReadOnly(false);

    // For value/hex editing: extend line with trailing spaces for the edit comment area
    // (comment padding is no longer baked into every line to avoid unnecessary scroll width)
    if ((target == EditTarget::Value || target == EditTarget::BaseAddress
         || (isHexEdit && target == EditTarget::Name))
        && m_editState.commentCol >= 0) {
        int commentStart = m_editState.commentCol;
        int commentWidth = (target == EditTarget::BaseAddress) ? 60 : kColComment;
        int neededLen = commentStart + commentWidth;
        int currentLen = (int)lineText.size();
        if (currentLen < neededLen) {
            int extend = neededLen - currentLen;
            long lineEndPos = posFromCol(m_sci, line, currentLen);
            QString pad(extend, ' ');
            QByteArray padUtf8 = pad.toUtf8();
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, lineEndPos);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, lineEndPos);
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                 (uintptr_t)padUtf8.size(), padUtf8.constData());
            m_editState.linelenAfterReplace += extend;
            // Remember where padding lives so endInlineEdit can strip it.
            // Spaces are ASCII so byte count == char count.
            m_editState.padPos   = lineEndPos;
            m_editState.padBytes = (long)padUtf8.size();
        }
    }

    // Comment editing: if no existing comment, append "  / " placeholder at
    // end of line. The "/" prefix matches the Comment chip emitted by
    // compose.cpp ("  / <text>"); legacy "//" was retired in the chip
    // refactor — single-slash reads cleanly next to the other chip
    // markers (TypeHint=[…], Rtti={RTTI: …}, Enum=(…)).
    if (target == EditTarget::Comment) {
        QString curLine = getLineText(m_sci, line);
        const LineChip* commentChip = findChip(*lm, ChipKind::Comment);
        int commentStart = commentChip ? commentChip->startCol : -1;
        if (commentStart < 0 || commentStart >= (int)curLine.size()) {
            // Trim trailing whitespace so the cursor sits close to content
            // (not after the value column's padding). The deleted spaces
            // + appended placeholder are tracked as a single
            // [padPos, padPos+padBytes) edit so endInlineEdit can restore
            // both atomically — leaving Scintilla in sync with m_prevText.
            int trimEnd = (int)curLine.size();
            while (trimEnd > 0 && curLine[trimEnd - 1] == ' ') trimEnd--;
            int trimmedCount = (int)curLine.size() - trimEnd;
            // Plain "  " placeholder — comment chip no longer carries a
            // "/" marker (it renders as a green pill, the color carries
            // the meaning).
            QString placeholder = QStringLiteral("  ");
            QByteArray placeholderUtf8 = placeholder.toUtf8();
            long trimStart = posFromCol(m_sci, line, trimEnd);
            long trimEndPos = posFromCol(m_sci, line, (int)curLine.size());
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, trimStart);
            m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, trimEndPos);
            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                                 (uintptr_t)placeholderUtf8.size(),
                                 placeholderUtf8.constData());
            m_editState.linelenAfterReplace =
                m_editState.linelenAfterReplace - trimmedCount + placeholder.size();
            m_editState.spanStart = trimEnd + placeholder.size();
            m_editState.original = QString();
            m_editState.padPos   = trimStart;
            m_editState.padBytes = (long)placeholderUtf8.size();
            m_editState.padRestoreSpaces = trimmedCount;
        } else {
            // Existing "/ comment" — strip "/ " prefix, edit the text part only.
            // Stay tolerant of the legacy "// " marker so projects saved
            // before the chip refactor still load cleanly into the editor.
            int textStart = commentStart;
            if (textStart + 3 <= (int)curLine.size() && curLine.mid(textStart, 3) == QStringLiteral("// "))
                textStart += 3;
            else if (textStart + 2 <= (int)curLine.size() && curLine.mid(textStart, 2) == QStringLiteral("/ "))
                textStart += 2;
            else if (textStart + 1 <= (int)curLine.size() && curLine.mid(textStart, 1) == QStringLiteral("/"))
                textStart += 1;
            m_editState.spanStart = textStart;
            m_editState.original = curLine.mid(textStart).trimmed();
        }
        // Sync norm with adjusted spanStart for correct cursor placement
        norm.start = m_editState.spanStart;
        norm.end = m_editState.spanStart + m_editState.original.size();
    }

    // Switch to I-beam for editing (skip for picker-based targets)
    if (target != EditTarget::Type && target != EditTarget::Source
        && target != EditTarget::ArrayElementType && target != EditTarget::PointerTarget
        && target != EditTarget::RootClassType) {
        m_sci->viewport()->setCursor(Qt::IBeamCursor);
    }

    // Re-enable selection rendering for inline edit (skip for picker-based targets)
    bool isPicker = (target == EditTarget::Type || target == EditTarget::Source
                     || target == EditTarget::ArrayElementType
                     || target == EditTarget::PointerTarget
                     || target == EditTarget::RootClassType);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELFORE, (long)0, (long)0);
    if (!isPicker) {
        // Subtle tint derived from theme background (neutral, not blue)
        const auto& bg = ThemeManager::instance().current().background;
        int shift = (bg.lightness() < 128) ? 25 : -25;
        QColor tint(qBound(0, bg.red() + shift, 255),
                    qBound(0, bg.green() + shift, 255),
                    qBound(0, bg.blue() + shift, 255));
        m_sci->SendScintilla(QsciScintillaBase::SCI_SETSELBACK, (long)1, tint);
    }

    // Use correct UTF-8 position conversion (not lineStart + col!)
    m_editState.posStart = posFromCol(m_sci, line, norm.start);
    m_editState.posEnd = posFromCol(m_sci, line, norm.end);

    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL, m_editState.posStart, m_editState.posEnd);

    // Hex overwrite: place cursor at start, no selection
    if (m_editState.hexOverwrite)
        m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, m_editState.posStart);

    // Show initial edit hint in comment column
    if (target == EditTarget::Value) {
        if (m_editState.hexOverwrite)
            setEditComment(QStringLiteral("Hex edit: Enter=Save Esc=Cancel"));
        else
            setEditComment(QStringLiteral("Enter=Save Esc=Cancel"));
    } else if (target == EditTarget::Name && m_editState.hexOverwrite) {
        setEditComment(QStringLiteral("ASCII edit: Enter=Save Esc=Cancel"));
    } else if (target == EditTarget::BaseAddress) {
        // No inline hint — the hover tooltip already shows examples
    }

    // Note: Type, ArrayElementType, PointerTarget, Source are handled by popups
    // and exit early above (never reach here).
    // Refresh hover cursor so value history popup appears with Set buttons immediately
    if (target == EditTarget::Value)
        QTimer::singleShot(0, this, &RcxEditor::applyHoverCursor);
    return true;
}

int RcxEditor::editEndCol() const {
    QString lineText = getLineText(m_sci, m_editState.line);
    int delta = lineText.size() - m_editState.linelenAfterReplace;
    return m_editState.spanStart + m_editState.original.size() + delta;
}

int RcxEditor::editEnd() const { return editEndCol(); }

void RcxEditor::clampEditSelection() {
    if (!m_editState.active) return;

    if (m_clampingSelection) return;
    m_clampingSelection = true;

    // Hex overwrite: collapse any selection to cursor (no selection allowed)
    if (m_editState.hexOverwrite) {
        int sL, sC, eL, eC;
        m_sci->getSelection(&sL, &sC, &eL, &eC);
        if (sL != eL || sC != eC) {
            int curLine, curCol;
            m_sci->getCursorPosition(&curLine, &curCol);
            // Single-row: pin cursor back to the edit's line.
            // Multi-row byte edit: cursor legitimately moves between
            // segment lines — preserve whichever it's currently on
            // (advanceToByteSegment already placed it there).
            int targetLine = (m_editState.byteRange
                              && m_editState.byteSegments.size() > 1)
                ? curLine
                : m_editState.line;
            m_sci->setCursorPosition(targetLine, curCol);
        }
        m_clampingSelection = false;
        return;
    }

    int selStartLine, selStartCol, selEndLine, selEndCol;
    m_sci->getSelection(&selStartLine, &selStartCol, &selEndLine, &selEndCol);

    int editEnd = editEndCol();
    bool isCursor = (selStartLine == selEndLine && selStartCol == selEndCol);

    // Don't fight cursor positioning - only clamp actual selections
    if (isCursor) {
        m_clampingSelection = false;
        return;
    }

    // Actual selection - clamp both ends to edit span
    bool clamped = false;

    // Force to edit line
    if (selStartLine != m_editState.line || selEndLine != m_editState.line) {
        m_sci->setSelection(m_editState.line, m_editState.spanStart,
                           m_editState.line, editEnd);
        m_clampingSelection = false;
        return;
    }

    if (selStartCol < m_editState.spanStart) { selStartCol = m_editState.spanStart; clamped = true; }
    if (selEndCol < m_editState.spanStart) { selEndCol = m_editState.spanStart; clamped = true; }
    if (selStartCol > editEnd) { selStartCol = editEnd; clamped = true; }
    if (selEndCol > editEnd) { selEndCol = editEnd; clamped = true; }

    if (clamped)
        m_sci->setSelection(selStartLine, selStartCol, selEndLine, selEndCol);

    m_clampingSelection = false;
}

// ── Commit inline edit ──

void RcxEditor::commitInlineEdit() {
    if (!m_editState.active) return;

    QString lineText = getLineText(m_sci, m_editState.line);
    int currentLen = lineText.size();
    int delta = currentLen - m_editState.linelenAfterReplace;
    int editedLen = m_editState.original.size() + delta;

    QString editedText;
    if (editedLen > 0) {
        editedText = lineText.mid(m_editState.spanStart, editedLen);
        if (!m_editState.hexOverwrite)
            editedText = editedText.trimmed();
    }

    // ── Byte-range commit branch ──
    // beginByteEdit marked this edit as a byte-range overwrite. For
    // single-row selections (1 segment) the edited text in
    // `editedText` carries the typed digits. For multi-row, the typed
    // digits live across multiple Scintilla lines — walk each segment
    // and pull its current line's slice. Parse digit pairs (skipping
    // inter-byte spaces) to raw bytes, concatenate, and route through
    // byteRangeCommitRequested.
    if (m_editState.byteRange) {
        QByteArray raw;
        raw.reserve(m_editState.byteRangeLen);
        bool parseOk = true;
        auto parseSegment = [&](const QString& segText, int wantBytes) -> bool {
            int parsedBytes = 0;
            int i = 0;
            while (i < segText.size() && parsedBytes < wantBytes) {
                while (i < segText.size() && segText[i] == QLatin1Char(' '))
                    ++i;
                if (i + 1 >= segText.size()) return false;
                bool byteOk = false;
                uint8_t b = (uint8_t)segText.mid(i, 2).toUInt(&byteOk, 16);
                if (!byteOk) return false;
                raw.append((char)b);
                ++parsedBytes;
                i += 2;
            }
            return parsedBytes == wantBytes;
        };
        if (m_editState.byteSegments.size() <= 1) {
            // Single-row — use editedText already sliced from m_editState.line.
            int want = m_editState.byteRangeLen;
            if (!parseSegment(editedText, want)) parseOk = false;
        } else {
            // Multi-row — read each segment's current line slice.
            for (const auto& seg : m_editState.byteSegments) {
                QString segText = getLineText(m_sci, seg.line)
                    .mid(seg.spanStart, seg.spanEnd - seg.spanStart);
                if (!parseSegment(segText, seg.byteCount)) {
                    parseOk = false;
                    break;
                }
            }
        }
        uint64_t addr = m_editState.byteRangeAddr;
        int wantLen   = m_editState.byteRangeLen;
        endInlineEdit();
        if (parseOk && raw.size() == wantLen)
            emit byteRangeCommitRequested(addr, raw);
        return;
    }

    // For Type edits: if nothing changed, commit original
    if (m_editState.target == EditTarget::Type && editedText.isEmpty())
        editedText = m_editState.original;

    // Grab resolved address from LineMeta before endInlineEdit clears state
    const LineMeta* lm = metaForLine(m_editState.line);
    uint64_t addr = lm ? lm->offsetAddr : 0;

    auto info = endInlineEdit();
    emit inlineEditCommitted(info.nodeIdx, info.subLine, info.target, editedText, addr);
}

// ── Cancel inline edit ──

void RcxEditor::cancelInlineEdit() {
    if (!m_editState.active) return;

    endInlineEdit();
    emit inlineEditCancelled();
}

// ── Type picker (user list) ──

void RcxEditor::showTypeAutocomplete() {
    if (!m_editState.active ||
        (m_editState.target != EditTarget::Type && m_editState.target != EditTarget::ArrayElementType))
        return;
    // Replace original type with spaces (keeps layout, clears for typing)
    int len = m_editState.original.size();
    QString spaces(len, ' ');
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                         m_editState.posStart, m_editState.posEnd);
    m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                         (uintptr_t)0, spaces.toUtf8().constData());

    // Position cursor at start
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, m_editState.posStart);

    showTypeListFiltered(QString());  // Show full list initially
}

void RcxEditor::showTypeListFiltered(const QString& filter) {
    if (!m_editState.active ||
        (m_editState.target != EditTarget::Type && m_editState.target != EditTarget::ArrayElementType))
        return;

    // Combine native types with custom (struct) type names
    QStringList all = allTypeNamesForUI();
    for (const QString& ct : m_customTypeNames) {
        if (!all.contains(ct))
            all << ct;
    }
    all.sort(Qt::CaseInsensitive);

    // Filter by prefix
    QStringList filtered;
    for (const QString& t : all) {
        if (filter.isEmpty() || t.startsWith(filter, Qt::CaseInsensitive))
            filtered << t;
    }
    if (filtered.isEmpty()) return;  // No matches - keep list hidden

    // Show user list (id=1 for types) - selection handled by userListActivated signal
    QByteArray list = filtered.join('\n').toUtf8();
    m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCSETSEPARATOR, (long)'\n');
    m_sci->SendScintilla(QsciScintillaBase::SCI_USERLISTSHOW,
                         (uintptr_t)1, list.constData());
    // Force Arrow cursor immediately (don't wait for mouse move)
    m_sci->viewport()->setCursor(Qt::ArrowCursor);
}

void RcxEditor::showSourcePicker() {
    // Replaced by SourceChooserPopup — Source target now early-returns
    // from beginInlineEdit() and emits sourcePopupRequested().
}

void RcxEditor::updateTypeListFilter() {
    if (!m_editState.active ||
        (m_editState.target != EditTarget::Type && m_editState.target != EditTarget::ArrayElementType))
        return;

    // Get currently typed text from line
    QString lineText = getLineText(m_sci, m_editState.line);
    long curPos = m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
    int col = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN,
                                         (unsigned long)curPos);

    // Extract text from spanStart to cursor
    int len = col - m_editState.spanStart;
    if (len <= 0) {
        showTypeListFiltered(QString());  // Show full list
        return;
    }

    QString typed = lineText.mid(m_editState.spanStart, len);
    showTypeListFiltered(typed);
}

// ── Pointer target picker ──

void RcxEditor::showPointerTargetPicker() {
    if (!m_editState.active || m_editState.target != EditTarget::PointerTarget)
        return;
    // Replace original target with spaces (keeps layout, clears for typing)
    int len = m_editState.original.size();
    QString spaces(len, ' ');
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                         m_editState.posStart, m_editState.posEnd);
    m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                         (uintptr_t)0, spaces.toUtf8().constData());
    m_sci->SendScintilla(QsciScintillaBase::SCI_GOTOPOS, m_editState.posStart);
    showPointerTargetListFiltered(QString());
}

void RcxEditor::showPointerTargetListFiltered(const QString& filter) {
    if (!m_editState.active || m_editState.target != EditTarget::PointerTarget)
        return;

    // Build list: "void" + all struct type names
    QStringList all;
    all << QStringLiteral("void");
    for (const QString& ct : m_customTypeNames) {
        if (!all.contains(ct))
            all << ct;
    }
    all.sort(Qt::CaseInsensitive);
    // Ensure "void" is always first
    all.removeAll(QStringLiteral("void"));
    all.prepend(QStringLiteral("void"));

    QStringList filtered;
    for (const QString& t : all) {
        if (filter.isEmpty() || t.startsWith(filter, Qt::CaseInsensitive))
            filtered << t;
    }
    if (filtered.isEmpty()) return;

    QByteArray list = filtered.join('\n').toUtf8();
    m_sci->SendScintilla(QsciScintillaBase::SCI_AUTOCSETSEPARATOR, (long)'\n');
    m_sci->SendScintilla(QsciScintillaBase::SCI_USERLISTSHOW,
                         (uintptr_t)1, list.constData());
    // Force Arrow cursor immediately (don't wait for mouse move)
    m_sci->viewport()->setCursor(Qt::ArrowCursor);
}

void RcxEditor::updatePointerTargetFilter() {
    if (!m_editState.active || m_editState.target != EditTarget::PointerTarget)
        return;

    QString lineText = getLineText(m_sci, m_editState.line);
    long curPos = m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
    int col = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETCOLUMN,
                                         (unsigned long)curPos);
    int len = col - m_editState.spanStart;
    if (len <= 0) {
        showPointerTargetListFiltered(QString());
        return;
    }
    QString typed = lineText.mid(m_editState.spanStart, len);
    showPointerTargetListFiltered(typed);
}

// ── Editable-field text-color indicator ──

void RcxEditor::paintEditableSpans(int line) {
    const LineMeta* lm = metaForLine(line);
    if (!lm) return;
    // CommandRow: paint Source/BaseAddress + root class (type+name) spans
    if (lm->lineKind == LineKind::CommandRow) {
        NormalizedSpan norm;
        if (resolvedSpanFor(line, EditTarget::Source, norm))
            fillIndicatorCols(IND_EDITABLE, line, norm.start, norm.end);
        if (resolvedSpanFor(line, EditTarget::BaseAddress, norm))
            fillIndicatorCols(IND_EDITABLE, line, norm.start, norm.end);
        // RootClassType no longer shown as editable — right-click conversion instead
        if (resolvedSpanFor(line, EditTarget::RootClassName, norm))
            fillIndicatorCols(IND_EDITABLE, line, norm.start, norm.end);
        return;
    }
    if (isSyntheticLine(*lm)) return;
    NormalizedSpan norm;
    for (EditTarget t : {EditTarget::Type, EditTarget::Name, EditTarget::Value,
                         EditTarget::ArrayElementType, EditTarget::ArrayElementCount,
                         EditTarget::PointerTarget, EditTarget::Comment}) {
        if (resolvedSpanFor(line, t, norm))
            fillIndicatorCols(IND_EDITABLE, line, norm.start, norm.end);
    }
}

void RcxEditor::updateEditableIndicators(int line) {
    if (m_editState.active) return;
    if (line == m_hintLine) return;

    // No cursor hints when selection is empty (prevents desync during batch ops)
    if (m_currentSelIds.isEmpty()) {
        if (m_hintLine >= 0) {
            clearIndicatorLine(IND_EDITABLE, m_hintLine);
            m_hintLine = -1;
        }
        return;
    }

    // Helper to check if a line's node is selected (handles footer/array element IDs)
    auto isLineSelected = [this](const LineMeta* lm) -> bool {
        if (!lm) return false;
        uint64_t checkId;
        if (lm->lineKind == LineKind::Footer)
            checkId = lm->nodeId | kFooterIdBit;
        else if (lm->isArrayElement && lm->arrayElementIdx >= 0)
            checkId = makeArrayElemSelId(lm->nodeId, lm->arrayElementIdx);
        else if (lm->isMemberLine && lm->subLine >= 0)
            checkId = makeMemberSelId(lm->nodeId, lm->subLine);
        else
            checkId = lm->nodeId;
        return m_currentSelIds.contains(checkId);
    };

    // If new line is selected, its indicators are managed by applySelectionOverlay
    // But we still need to clear the old non-selected hint line
    const LineMeta* newLm = metaForLine(line);
    if (isLineSelected(newLm)) {
        if (m_hintLine >= 0) {
            const LineMeta* oldLm = metaForLine(m_hintLine);
            if (!isLineSelected(oldLm))
                clearIndicatorLine(IND_EDITABLE, m_hintLine);
        }
        m_hintLine = line;
        return;
    }

    // Clear old cursor line (only if not a selected node)
    if (m_hintLine >= 0) {
        const LineMeta* oldLm = metaForLine(m_hintLine);
        if (!isLineSelected(oldLm))
            clearIndicatorLine(IND_EDITABLE, m_hintLine);
    }

    m_hintLine = line;
    paintEditableSpans(line);
}

// ── Hover cursor ──

void RcxEditor::applyHoverCursor() {
    // Clear previous hover span indicators
    for (int ln : m_hoverSpanLines)
        clearIndicatorLine(IND_HOVER_SPAN, ln);
    m_hoverSpanLines.clear();

    // Lock cursor to Arrow during drag-selection (prevents flicker)
    if (m_dragStarted) {
        m_sci->viewport()->setCursor(Qt::ArrowCursor);
        return;
    }

    // Hover effects disabled: keep Arrow cursor, dismiss popups, skip all hover visuals
    // (edit mode cursor + value history popup during editing still work)
    if (!m_hoverEffects && !m_editState.active) {
        if (!m_hoverInside || !m_applyingDocument)
            dismissAllPopups();
        m_sci->viewport()->setCursor(Qt::ArrowCursor);
        return;
    }

    // Edit mode: IBeam inside edit span, Arrow outside
    if (m_editState.active) {
        if (m_sci->isListActive()) {
            m_sci->viewport()->setCursor(Qt::ArrowCursor);
        } else {
            auto h = hitTest(m_lastHoverPos);
            if (h.line == m_editState.line &&
                h.col >= m_editState.spanStart && h.col <= editEndCol()) {
                m_sci->viewport()->setCursor(Qt::IBeamCursor);
            } else {
                m_sci->viewport()->setCursor(Qt::ArrowCursor);
            }
        }
        // Value history popup — only during inline value editing on a heated node
        {
            bool showPopup = false;
            if (m_valueHistory && m_editState.target == EditTarget::Value
                && m_editState.line >= 0 && m_editState.line < m_meta.size()) {
                const LineMeta& lm = m_meta[m_editState.line];
                if (lm.heatLevel > 0 && lm.nodeId != 0) {
                    auto it = m_valueHistory->find(lm.nodeId);
                    if (it != m_valueHistory->end() && it->uniqueCount() > 1) {
                        if (!m_historyPopup) {
                            m_historyPopup = new ValueHistoryPopup(this);
                            static_cast<ValueHistoryPopup*>(m_historyPopup)->setOnMouseMove([this](QMouseEvent* e) {
                                QPoint gp = e->globalPosition().toPoint();
                                QPoint vp = m_sci->viewport()->mapFromGlobal(gp);
                                m_lastHoverPos = vp;
                                m_hoverInside = m_sci->viewport()->rect().contains(vp);
                                if (!m_editState.active) {
                                    auto h2 = hitTest(m_lastHoverPos);
                                    uint64_t nid = (m_hoverInside && h2.line >= 0) ? h2.nodeId : 0;
                                    int nln = (m_hoverInside && h2.line >= 0) ? h2.line : -1;
                                    if (nid != m_hoveredNodeId || nln != m_hoveredLine) {
                                        m_hoveredNodeId = nid;
                                        m_hoveredLine = nln;
                                        applyHoverHighlight();
                                    }
                                }
                                applyHoverCursor();
                            });
                        }
                        auto* popup = static_cast<ValueHistoryPopup*>(m_historyPopup);
                        popup->setOnSet([this](const QString& val) {
                            if (!m_editState.active) return;
                            long endPos = posFromCol(m_sci, m_editState.line, editEndCol());
                            m_sci->SendScintilla(QsciScintillaBase::SCI_SETSEL,
                                                 m_editState.posStart, endPos);
                            QByteArray utf8 = val.toUtf8();
                            m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACESEL,
                                                 (uintptr_t)0, utf8.constData());
                        });
                        popup->populate(lm.nodeId, *it, editorFont(), true);
                        int px = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION,
                                                           (unsigned long)0, m_editState.posStart);
                        int py = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION,
                                                           (unsigned long)0, m_editState.posStart);
                        int lh = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT,
                                                           (unsigned long)m_editState.line);
                        QPoint anchor = m_sci->viewport()->mapToGlobal(QPoint(px, py + lh));
                        popup->showAt(anchor, lh);
                        showPopup = true;
                    }
                }
            }
            if (!showPopup && m_historyPopup && m_historyPopup->isVisible())
                static_cast<HoverPopup*>(m_historyPopup)->dismiss();
        }
        // Always dismiss the hover host during inline editing — inline
        // edit uses m_historyPopup (the buttons-enabled variant) and
        // owns the hover slot exclusively.
        if (m_popupHost) static_cast<HoverPopup*>(m_popupHost)->dismiss();
        return;
    }

    // Mouse left viewport - set Arrow, dismiss popups
    // (but not during applyDocument — the Leave is synthetic from setText)
    if (!m_hoverInside) {
        if (!m_applyingDocument)
            dismissAllPopups();
        m_sci->viewport()->setCursor(Qt::ArrowCursor);
        // Cancel any pending preview popup so a fresh entry starts a
        // new wait. Without this, a long pause outside the editor
        // would let the timer fire and pop a popup the moment the
        // user re-enters — defeating the dwell.
        m_hoverDwellElapsed = false;
        m_dwellNodeId = 0;
        m_dwellLine = -1;
        if (m_hoverDwellTimer) m_hoverDwellTimer->stop();
        return;
    }

    // If autocomplete/user list popup is active, use arrow cursor
    if (m_sci->isListActive()) {
        m_sci->viewport()->setCursor(Qt::ArrowCursor);
        return;
    }

    auto h = hitTest(m_lastHoverPos);
    int line, hCol; EditTarget t;
    bool tokenHit = hitTestTarget(m_sci, m_meta, m_lastHoverPos, line, hCol, t);

    // Skip hover span on footer lines (nothing editable)
    int hoverLine = h.line;
    bool isFooterLine = (hoverLine >= 0 && hoverLine < m_meta.size()
                         && m_meta[hoverLine].lineKind == LineKind::Footer);

    // Apply hover span indicator for editable tokens
    if (tokenHit && !isFooterLine) {
        NormalizedSpan span;
        QString lineText;
        if (resolvedSpanFor(line, t, span, &lineText)) {
            // For vector/matrix values: narrow hover to the component under cursor
            bool narrowed = false;
            if (t == EditTarget::Value && line >= 0 && line < m_meta.size()) {
                const auto& lm = m_meta[line];
                if (isVectorKind(lm.nodeKind) || isMatrixKind(lm.nodeKind)) {
                    QString val = lineText.mid(span.start, span.end - span.start);
                    int innerStart = span.start;
                    QString inner = val;
                    if (isMatrixKind(lm.nodeKind)) {
                        int bo = val.indexOf('['), bc = val.lastIndexOf(']');
                        if (bo >= 0 && bc > bo) {
                            inner = val.mid(bo + 1, bc - bo - 1);
                            innerStart = span.start + bo + 1;
                        }
                    }
                    QVector<int> starts, ends;
                    starts.append(0);
                    for (int i = 0; i < inner.size(); i++) {
                        if (inner[i] == ',') {
                            ends.append(i);
                            int n = i + 1;
                            while (n < inner.size() && inner[n] == ' ') n++;
                            starts.append(n);
                        }
                    }
                    ends.append(inner.size());
                    // Trim trailing spaces from last component to get true end
                    int lastEnd = ends.last();
                    while (lastEnd > 0 && inner[lastEnd - 1] == ' ') lastEnd--;
                    // Skip highlight if cursor is past the last component
                    int relCol = h.col - innerStart;
                    if (relCol >= lastEnd) {
                        narrowed = true;  // suppress highlight entirely
                    } else {
                        int comp = 0;
                        for (int i = 0; i < starts.size(); i++) {
                            if (relCol >= starts[i] && (i == starts.size() - 1 || relCol < starts[i + 1])) {
                                comp = i; break;
                            }
                        }
                        int cS = innerStart + starts[comp];
                        int cE = innerStart + ends[comp];
                        while (cE > cS && lineText[cE - 1] == ' ') cE--;
                        span.start = cS;
                        span.end = cE;
                        narrowed = true;
                        fillIndicatorCols(IND_HOVER_SPAN, line, span.start, span.end);
                        m_hoverSpanLines.append(line);
                    }
                }
                // Narrow pointer-like nodes to address portion only (exclude symbol)
                if (!narrowed && (isFuncPtr(lm.nodeKind)
                    || lm.nodeKind == NodeKind::Pointer32
                    || lm.nodeKind == NodeKind::Pointer64)) {
                    ColumnSpan full = valueSpan(lm, lineText.size(), lm.effectiveTypeW, lm.effectiveNameW);
                    ColumnSpan narrow = narrowPtrValueSpan(lm, full, lineText);
                    if (h.col >= narrow.start && h.col < narrow.end) {
                        fillIndicatorCols(IND_HOVER_SPAN, line, narrow.start, narrow.end);
                        m_hoverSpanLines.append(line);
                    }
                    narrowed = true;
                }
            }
            if (!narrowed && h.col >= span.start && h.col < span.end) {
                fillIndicatorCols(IND_HOVER_SPAN, line, span.start, span.end);
                m_hoverSpanLines.append(line);
            }
        }
    }

    // Apply hover span on fold arrows (▸/▾) — same visual feedback as editable tokens
    if (h.inFoldCol && h.line >= 0 && h.line < m_meta.size()) {
        fillIndicatorCols(IND_HOVER_SPAN, h.line, 0, kFoldCol);
        m_hoverSpanLines.append(h.line);
    }

    // Apply hover span on footer pills (+Field, +10h/+100h/+1000h, +10, Trim)
    if (h.line >= 0 && h.line < m_meta.size()
        && m_meta[h.line].lineKind == LineKind::Footer) {
        QString ft = getLineText(m_sci, h.line);
        auto tryPill = [&](const QString& text, int pos) {
            if (pos >= 0 && h.col >= pos && h.col < pos + text.size()) {
                fillIndicatorCols(IND_HOVER_SPAN, h.line, pos, pos + text.size());
                m_hoverSpanLines.append(h.line);
            }
        };
        // Search padded token, but hover-paint only the `+1` chars so the
        // glyph doesn't bleed into the gap before +10h.
        int pPlusOne = ft.indexOf(QStringLiteral(" +1 "));
        if (pPlusOne >= 0)
            tryPill(QStringLiteral("+1"), pPlusOne + 1);
        int p1000 = ft.indexOf(QStringLiteral("+1000h"));
        tryPill(QStringLiteral("+1000h"), p1000);
        int p100 = ft.indexOf(QStringLiteral("+100h"));
        if (p100 >= 0 && p100 != p1000 + 1)
            tryPill(QStringLiteral("+100h"), p100);
        int p10 = ft.indexOf(QStringLiteral("+10h"));
        if (p10 >= 0 && p10 != p100 && p10 != p1000)
            tryPill(QStringLiteral("+10h"), p10);
        int add10Start = ft.indexOf(QStringLiteral("+10"));
        if (add10Start >= 0 && add10Start != p10 && add10Start != p100 && add10Start != p1000)
            tryPill(QStringLiteral("+10"), add10Start);
        tryPill(QStringLiteral("Trim"), ft.indexOf(QStringLiteral("Trim")));
        tryPill(QStringLiteral("Top"), ft.indexOf(QStringLiteral("Top")));
    }

    // ── Hover dwell tracking ──
    // Restart the dwell timer whenever the hover target (node + line)
    // changes; otherwise the existing elapsed flag stays as-is so the
    // popup keeps showing while the user moves within the same row.
    // Each preview popup block below gates its show on
    // m_hoverDwellElapsed so they don't flash on incidental hovers.
    if (m_dwellNodeId != m_hoveredNodeId || m_dwellLine != m_hoveredLine) {
        m_dwellNodeId = m_hoveredNodeId;
        m_dwellLine   = m_hoveredLine;
        m_hoverDwellElapsed = false;
        if (m_hoverDwellTimer) m_hoverDwellTimer->stop();
        if (m_hoveredNodeId != 0 && m_hoveredLine >= 0 && m_hoverDwellTimer)
            m_hoverDwellTimer->start();
    }

    // ── Unified hover preview ──
    //
    // The host asks the preview registry which previews are eligible for
    // this row. If any are, we pick the one the user last switched to
    // for this node-kind (QSettings), build its widget, and show. Tab /
    // Shift+Tab in handleNormalKey cycles between eligible previews.
    //
    // This single block replaced three near-duplicate blocks (value-
    // history, disasm/hex-dump, struct-preview) that fought each other
    // via explicit mutual `dismiss()` calls. New preview kinds drop in
    // as one `m_previewRegistry->add(std::make_unique<MatrixPreview>())`
    // call in setupScintilla — no edits here.
    {
        bool showHost = false;
        if (m_popupHost && m_previewRegistry && m_disasmTree && m_disasmProvider
            && h.line >= 0 && h.line < m_meta.size() && m_hoverDwellElapsed) {
            const LineMeta& lm = m_meta[h.line];
            if (lm.nodeIdx >= 0 && lm.nodeIdx < m_disasmTree->nodes.size()) {
                const Node& node = m_disasmTree->nodes[lm.nodeIdx];
                QString lineText = getLineText(m_sci, h.line);
                // Compute both span flavors — full value span (for non-
                // pointer previews) and the narrowed address-only span
                // (for pointer previews). Hover counts as "in value
                // column" if the cursor is inside the UNION of the two,
                // which matches the per-block behavior of the old code.
                ColumnSpan vsFull = valueSpan(lm, lineText.size(),
                    lm.effectiveTypeW, lm.effectiveNameW);
                ColumnSpan vsNarrow = narrowPtrValueSpan(lm, vsFull, lineText);
                bool inFull   = vsFull.valid   && h.col >= vsFull.start   && h.col < vsFull.end;
                bool inNarrow = vsNarrow.valid && h.col >= vsNarrow.start && h.col < vsNarrow.end;
                if (inFull || inNarrow) {
                    HoverContext ctx;
                    ctx.editorFont   = editorFont();
                    ctx.theme        = &ThemeManager::instance().current();
                    ctx.dataProvider = m_disasmProvider;
                    ctx.codeProvider = m_disasmRealProv ? m_disasmRealProv : m_disasmProvider;
                    ctx.tree         = m_disasmTree;
                    ctx.history      = m_valueHistory;
                    auto eligible = m_previewRegistry->eligibleFor(lm, node, ctx);
                    if (!eligible.isEmpty()) {
                        int initial = pickLastUsedPreviewIdx(eligible, lm.nodeKind);
                        auto* host = static_cast<HoverPopupHost*>(m_popupHost);
                        host->setEligible(eligible, lm, node, ctx, initial);
                        ColumnSpan anchorSpan = vsNarrow.valid ? vsNarrow : vsFull;
                        int lh = 0;
                        QPoint anchor = popupAnchorAt(
                            m_sci, h.line, anchorSpan.start, lineText, &lh);
                        host->showAt(anchor, lh);
                        showHost = true;
                        // Inline-edit history popup is a separate widget
                        // (m_historyPopup) — dismiss it if it lingers
                        // since we now own the hover slot.
                        if (m_historyPopup && m_historyPopup->isVisible())
                            static_cast<HoverPopup*>(m_historyPopup)->dismiss();
                    }
                }
            }
        }
        if (!showHost && m_popupHost && m_popupHost->isVisible())
            static_cast<HoverPopup*>(m_popupHost)->dismiss();
    }

    // Determine cursor shape based on interaction type
    Qt::CursorShape desired = Qt::ArrowCursor;

    if (h.inFoldCol) {
        desired = Qt::PointingHandCursor;  // fold toggle = button
    } else if (h.line >= 0 && h.line < m_meta.size()
               && m_meta[h.line].lineKind == LineKind::Footer) {
        QString ft = getLineText(m_sci, h.line);
        int pPlusOne = ft.indexOf(QStringLiteral(" +1 "));
        if (pPlusOne >= 0 && h.col >= pPlusOne && h.col < pPlusOne + 4)
            desired = Qt::PointingHandCursor;
        int p1000 = ft.indexOf(QStringLiteral("+1000h"));
        if (p1000 >= 0 && h.col >= p1000 && h.col < p1000 + 6)
            desired = Qt::PointingHandCursor;
        int p100 = ft.indexOf(QStringLiteral("+100h"));
        if (p100 >= 0 && p100 != p1000 + 1 && h.col >= p100 && h.col < p100 + 5)
            desired = Qt::PointingHandCursor;
        int p10 = ft.indexOf(QStringLiteral("+10h"));
        if (p10 >= 0 && p10 != p100 && p10 != p1000 && h.col >= p10 && h.col < p10 + 4)
            desired = Qt::PointingHandCursor;
        int add10Start = ft.indexOf(QStringLiteral("+10"));
        if (add10Start >= 0 && add10Start != p10 && add10Start != p100 && add10Start != p1000
            && h.col >= add10Start && h.col < add10Start + 3)
            desired = Qt::PointingHandCursor;
        int trimStart = ft.indexOf(QStringLiteral("Trim"));
        if (trimStart >= 0 && h.col >= trimStart && h.col < trimStart + 4)
            desired = Qt::PointingHandCursor;
        int topStart = ft.indexOf(QStringLiteral("Top"));
        if (topStart >= 0 && h.col >= topStart && h.col < topStart + 3)
            desired = Qt::PointingHandCursor;
    } else if (tokenHit) {
        // Check if mouse is actually over trimmed text content (not column padding)
        NormalizedSpan trimmed;
        bool overText = resolvedSpanFor(line, t, trimmed)
                        && h.col >= trimmed.start && h.col < trimmed.end;
        if (overText) {
            switch (t) {
            case EditTarget::Type:
            case EditTarget::Source:
            case EditTarget::ArrayElementType:
            case EditTarget::PointerTarget:
            case EditTarget::RootClassType:
            case EditTarget::TypeSelector:
                desired = Qt::PointingHandCursor;
                break;
            default:
                desired = Qt::IBeamCursor;
                break;
            }
        }
        // else: desired stays Arrow (hovering over column padding)
    }

    // Hex-byte override — wins over the rest of the chain. Hovering over
    // any hex byte digit in a hex preview row's value column resolves
    // via byteAddrAt(), which is exactly the press-handler's "this is a
    // byte-selection arming zone" test. Matching the cursor to the
    // press behaviour telegraphs the upcoming mode before the user
    // drags. Falls through to whatever the chain above decided in any
    // non-byte column.
    if (h.line >= 0 && byteAddrAt(h.line, h.col).has_value())
        desired = Qt::IBeamCursor;

    // Clickable chip override — type-hint suggestions ("int32x2"),
    // comment chips, enum chips, AddComment all respond to clicks.
    // updateChipHover() (run from MouseMove just before this function)
    // sets m_chipHoverLine when one is hovered; matching the cursor
    // makes them feel like the other clickable RcxEditor items (fold
    // toggle, footer pills, editable tokens).
    if (m_chipHoverLine >= 0)
        desired = Qt::PointingHandCursor;

    // ── Arrow tooltip on command row spans ──
    {
        bool showTip = false;
        if (tokenHit && h.line == 0 && h.line < m_meta.size()
            && m_meta[0].lineKind == LineKind::CommandRow) {
            NormalizedSpan span;
            QString lineText;
            if (resolvedSpanFor(0, t, span, &lineText)
                && h.col >= span.start && h.col < span.end) {
                QString tipTitle, tipBody;
                switch (t) {
                case EditTarget::Source:
                    tipTitle = QStringLiteral("Data Source");
                    tipBody = QStringLiteral("Click to change the attached\nmemory source (process, file)");
                    break;
                case EditTarget::BaseAddress:
                    tipTitle = QStringLiteral("Base Address");
                    tipBody = QStringLiteral(
                        "0x7FF61234ABCD          hex address\n"
                        "<app.exe>               module base\n"
                        "<app.exe> + 0x1A0       module + offset\n"
                        "[<app.exe> + 0x58]      follow pointer\n"
                        "ntdll!SymbolName        PDB symbol\n"
                        "\n"
                        "Operators: + - * << >> & | ^\n"
                        "All numbers are hexadecimal");
                    break;
                case EditTarget::RootClassName:
                    tipTitle = QStringLiteral("Class Name");
                    tipBody = QStringLiteral("Click to rename this type");
                    break;
                case EditTarget::TypeSelector:
                    tipTitle = QStringLiteral("Switch View");
                    tipBody = QStringLiteral("View a different struct in this tab");
                    break;
                default: break;
                }
                if (!tipTitle.isEmpty()) {
                    if (!m_arrowTooltip) {
                        m_arrowTooltip = new RcxTooltip(this);
                        static_cast<RcxTooltip*>(m_arrowTooltip)->onMouseMove =
                            [this](QMouseEvent* e) {
                            QPoint gp = e->globalPosition().toPoint();
                            QPoint vp = m_sci->viewport()->mapFromGlobal(gp);
                            m_lastHoverPos = vp;
                            m_hoverInside = m_sci->viewport()->rect().contains(vp);
                            applyHoverCursor();
                        };
                    }
                    auto* tip = static_cast<RcxTooltip*>(m_arrowTooltip);
                    const auto& theme = ThemeManager::instance().current();
                    tip->setTheme(theme.backgroundAlt, theme.border,
                                  theme.text, theme.textDim, theme.border);
                    tip->populate(tipTitle, tipBody, editorFont());
                    // Anchor at center of the hovered span, bottom edge of line
                    long posA = posFromCol(m_sci, 0, span.start);
                    long posB = posFromCol(m_sci, 0, span.end);
                    int xA = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, posA);
                    int xB = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, posB);
                    int py = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, posA);
                    int lh = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
                    QPoint anchor = m_sci->viewport()->mapToGlobal(
                        QPoint((xA + xB) / 2, py + lh));
                    tip->showAt(anchor);
                    showTip = true;
                }
            }
        }
        // Ctrl-held hover on a navigable Header line — show "Open in new
        // tab" hint. Restricted to Header (the parent row that has
        // children) so child member rows under an expanded parent don't
        // light up while Ctrl is held.
        if (!showTip && tokenHit
            && (QApplication::keyboardModifiers() & Qt::ControlModifier)
            && (t == EditTarget::Type || t == EditTarget::Name
                || t == EditTarget::PointerTarget)
            && line > 0 && line < m_meta.size()) {
            const LineMeta& lm = m_meta[line];
            if (lm.nodeIdx >= 0 && lm.lineKind == LineKind::Header) {
                // Validate target — only show for struct/pointer/array nodes
                // that actually point somewhere. Plumbing here mirrors the
                // controller's resolution in goToDefinitionRequested.
                NormalizedSpan span;
                QString lineText;
                if (resolvedSpanFor(line, t, span, &lineText)
                    && h.col >= span.start && h.col < span.end) {
                    if (!m_arrowTooltip) {
                        m_arrowTooltip = new RcxTooltip(this);
                        static_cast<RcxTooltip*>(m_arrowTooltip)->onMouseMove =
                            [this](QMouseEvent* e) {
                            QPoint gp = e->globalPosition().toPoint();
                            QPoint vp = m_sci->viewport()->mapFromGlobal(gp);
                            m_lastHoverPos = vp;
                            m_hoverInside = m_sci->viewport()->rect().contains(vp);
                            applyHoverCursor();
                        };
                    }
                    auto* tip = static_cast<RcxTooltip*>(m_arrowTooltip);
                    const auto& theme = ThemeManager::instance().current();
                    tip->setTheme(theme.backgroundAlt, theme.border,
                                  theme.text, theme.textDim, theme.border);
                    tip->populate(QStringLiteral("Open in new tab"),
                                  QStringLiteral("Ctrl+Click — open this type\nin a new editor tab"),
                                  editorFont());
                    long posA = posFromCol(m_sci, line, span.start);
                    long posB = posFromCol(m_sci, line, span.end);
                    int xA = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, posA);
                    int xB = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, posB);
                    int py = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, posA);
                    int lh = (int)m_sci->SendScintilla(
                        QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
                    QPoint anchor = m_sci->viewport()->mapToGlobal(
                        QPoint((xA + xB) / 2, py + lh));
                    tip->showAt(anchor);
                    showTip = true;
                    desired = Qt::PointingHandCursor;
                }
            }
        }
        if (!showTip && m_arrowTooltip && m_arrowTooltip->isVisible())
            static_cast<RcxTooltip*>(m_arrowTooltip)->dismiss();
    }

    m_sci->viewport()->setCursor(desired);
}

// ── Live value validation ──

void RcxEditor::setEditComment(const QString& comment) {
    // Value edit must be active
    if (m_editState.commentCol < 0) return;

    // Prevent re-entrancy from textChanged signal
    if (m_updatingComment) return;
    m_updatingComment = true;

    QString lineText = getLineText(m_sci, m_editState.line);

    // Place comment 2 spaces after current value, prefixed with //
    int valueEnd = editEndCol();
    int startCol = qMax(valueEnd + 2, m_editState.commentCol);
    int endCol = lineText.size();
    int availWidth = endCol - startCol;
    if (availWidth <= 0) { m_updatingComment = false; return; }

    // Format as "//<comment>" (no space after //)
    QString formatted = QStringLiteral("//") + comment;
    QString padded = formatted.leftJustified(availWidth, ' ').left(availWidth);

    // Use UTF-8 safe column-to-position conversion
    long posA = posFromCol(m_sci, m_editState.line, startCol);
    long posB = posFromCol(m_sci, m_editState.line, endCol);

    QByteArray utf8 = padded.toUtf8();
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, posA);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, posB);
    m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET,
                         (uintptr_t)utf8.size(), utf8.constData());

    // Apply green color to hint text
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETINDICATORCURRENT, IND_HINT_GREEN);
    m_sci->SendScintilla(QsciScintillaBase::SCI_INDICATORFILLRANGE, posA, posB - posA);

    m_updatingComment = false;
}

void RcxEditor::validateEditLive() {
    QString lineText = getLineText(m_sci, m_editState.line);
    int delta = lineText.size() - m_editState.linelenAfterReplace;
    int editedLen = m_editState.original.size() + delta;
    QString text = (editedLen > 0)
        ? lineText.mid(m_editState.spanStart, editedLen).trimmed() : QString();
    QString errorMsg = (m_editState.target == EditTarget::BaseAddress)
        ? fmt::validateBaseAddress(text)
        : fmt::validateValue(m_editState.editKind, text);

    const LineMeta* lm = metaForLine(m_editState.line);
    const bool isSelected = lm && m_currentSelIds.contains(lm->nodeId);
    const bool isValid = errorMsg.isEmpty();

    // Only update comment when validation state changes (avoid lag)
    const bool stateChanged = (isValid != m_editState.lastValidationOk);
    m_editState.lastValidationOk = isValid;

    // Show/hide error marker (red background)
    // M_SELECTED has higher priority than M_ERR, so temporarily remove it when error
    if (isValid) {
        m_sci->markerDelete(m_editState.line, M_ERR);
        if (isSelected) m_sci->markerAdd(m_editState.line, M_SELECTED);
        if (stateChanged)
            setEditComment("Enter=Save Esc=Cancel");
    } else {
        if (isSelected) m_sci->markerDelete(m_editState.line, M_SELECTED);
        m_sci->markerAdd(m_editState.line, M_ERR);
        if (stateChanged) setEditComment("! " + errorMsg);
    }
}

void RcxEditor::updateExprResultPopup() {
    if (!m_editState.active || !m_exprEvaluator) return;
    bool isAddr = (m_editState.target == EditTarget::BaseAddress);
    bool isVal  = (m_editState.target == EditTarget::Value && !m_editState.hexOverwrite);
    if (!isAddr && !isVal) return;

    // Extract current edit text
    QString lineText = getLineText(m_sci, m_editState.line);
    int delta = lineText.size() - m_editState.linelenAfterReplace;
    int editedLen = m_editState.original.size() + delta;
    QString text = (editedLen > 0)
        ? lineText.mid(m_editState.spanStart, editedLen).trimmed() : QString();

    // Only show popup if text contains an operator (otherwise it's a plain value)
    if (isVal && !text.contains('+') && !text.contains('-') && !text.contains('*')
        && !text.contains('/') && !text.contains('<') && !text.contains('&')
        && !text.contains('|') && !text.contains('^') && !text.contains('~')) {
        if (m_exprResultLabel) m_exprResultLabel->hide();
        return;
    }

    QString result = m_exprEvaluator(text);
    if (result.isEmpty()) {
        if (m_exprResultLabel) m_exprResultLabel->hide();
        return;
    }

    // Create label lazily as a top-level tooltip window (not clipped by viewport)
    if (!m_exprResultLabel) {
        m_exprResultLabel = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        m_exprResultLabel->setProperty("DarkTitleBar", true);
        m_exprResultLabel->setAttribute(Qt::WA_ShowWithoutActivating);
        m_exprResultLabel->setFocusPolicy(Qt::NoFocus);
    }

    const auto& theme = ThemeManager::instance().current();
    m_exprResultLabel->setFont(editorFont());
    m_exprResultLabel->setStyleSheet(
        QStringLiteral("background:%1; border:1px solid %2; padding:2px 6px;")
            .arg(theme.backgroundAlt.name(), theme.border.name()));
    m_exprResultLabel->setText(
        QStringLiteral("<span style='color:%1'>Result: </span><span style='color:%2'>%3</span>")
            .arg(theme.textDim.name(), theme.text.name(), result.toHtmlEscaped()));
    m_exprResultLabel->setTextFormat(Qt::RichText);
    m_exprResultLabel->adjustSize();

    // Position just above the edit line, aligned to the edit span start
    long spanPos = posFromCol(m_sci, m_editState.line, m_editState.spanStart);
    int px = (int)m_sci->SendScintilla(
        QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, spanPos);
    int py = (int)m_sci->SendScintilla(
        QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, spanPos);
    QPoint global = m_sci->viewport()->mapToGlobal(QPoint(px, py));
    m_exprResultLabel->move(global.x(), global.y() - m_exprResultLabel->height() - 2);
    m_exprResultLabel->show();
    m_exprResultLabel->raise();
}

void RcxEditor::setCommandRowText(const QString& line) {
    if (m_sci->lines() <= 0) return;
    QString s = line;
    s.replace('\n', ' ');
    s.replace('\r', ' ');
    // Skip the SCI_REPLACETARGET + COLOURISE + applyCommandRowPills work
    // when nothing changed. Common during rapid editing when only field
    // text on later lines is mutating but the command row stays put.
    if (s == m_lastCommandRowText) return;
    m_lastCommandRowText = s;

    bool wasReadOnly = m_sci->isReadOnly();
    bool wasModified = m_sci->SendScintilla(QsciScintillaBase::SCI_GETMODIFY);
    long savedPos    = m_sci->SendScintilla(QsciScintillaBase::SCI_GETCURRENTPOS);
    long savedAnchor = m_sci->SendScintilla(QsciScintillaBase::SCI_GETANCHOR);

    m_sci->setReadOnly(false);

    long start = m_sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, 0);
    long end   = m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINEENDPOSITION, 0);
    QByteArray utf8 = s.toUtf8();
    long oldLen = end - start;
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETSTART, start);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETTARGETEND, end);
    m_sci->SendScintilla(QsciScintillaBase::SCI_REPLACETARGET, (uintptr_t)utf8.size(), utf8.constData());

    // Sync m_prevText with the rewritten line 0. Without this the
    // applyDocument diff/patch path on the NEXT refresh computes
    // byte offsets against a stale prefix (compose's placeholder
    // command-row text) while Scintilla actually holds the real
    // command-row text — and SCI_REPLACETARGET ends up overwriting
    // the WRONG range, producing a truncated-bytes-line + merged-
    // footer corruption further down the document. Bug repros
    // when opening multiple "New Class" tabs (tabs 2, 3 hit it).
    int nl = m_prevText.indexOf('\n');
    int oldLine0End = nl >= 0 ? nl : m_prevText.size();
    m_prevText.replace(0, oldLine0End, s);

    // Adjust saved cursor/anchor for length change in line 0
    long delta = (long)utf8.size() - oldLen;
    if (savedPos > end)    savedPos    += delta;
    if (savedAnchor > end) savedAnchor += delta;

    if (wasReadOnly) m_sci->setReadOnly(true);
    if (!wasModified) m_sci->SendScintilla(QsciScintillaBase::SCI_SETSAVEPOINT);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETCURRENTPOS, savedPos);
    m_sci->SendScintilla(QsciScintillaBase::SCI_SETANCHOR, savedAnchor);
    m_sci->SendScintilla(QsciScintillaBase::SCI_COLOURISE, start, start + utf8.size());
    applyCommandRowPills();
}

void RcxEditor::setEditorFont(const QString& fontName) {
    g_fontName = fontName;
    QFont f = editorFont();

    m_sci->setFont(f);
    m_lexer->setFont(f);
    for (int i = 0; i <= 127; i++)
        m_lexer->setFont(f, i);
    m_sci->setMarginsFont(f);
    if (m_findBar) m_findBar->setFont(f);

    // Re-apply margin styles and width with new font metrics
    allocateMarginStyles();
    applyTheme(ThemeManager::instance().current());
    {
        int marginDigits = m_relativeOffsets
            ? qMax(m_layout.offsetHexDigits / 2, 4)
            : m_layout.offsetHexDigits;
        QString marginSizer = QString("  %1  ").arg(QString(marginDigits, '0'));
        m_sci->setMarginWidth(0, marginSizer);
    }

}

void RcxEditor::setGlobalFontName(const QString& fontName) {
    g_fontName = fontName;
}

QString RcxEditor::globalFontName() {
    return g_fontName;
}

QString RcxEditor::textWithMargins() const {
    int lineCount = (int)m_sci->SendScintilla(QsciScintillaBase::SCI_GETLINECOUNT);
    QStringList lines;
    lines.reserve(lineCount);
    for (int i = 0; i < lineCount; i++) {
        QString margin;
        if (i < m_meta.size())
            margin = m_meta[i].offsetText;
        QString lineText = getLineText(m_sci, i);
        lines.append(margin + lineText);
    }
    return lines.join('\n');
}

} // namespace rcx
