#pragma once

#include "paintutil.h"
#include "themes/thememanager.h"
#include "widgets/fuzzy_match.h"
#include "widgets/popup_chrome.h"

#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QApplication>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QFontMetrics>

namespace rcx {

// EnumPickerPopup — themed popup for picking an enum member, replacing
// the plain QMenu used previously. Visual conventions mirror
// src/typeselectorpopup.cpp (search field, custom-painted rows with a
// 2 px accent stripe + colored pip, fuzzy filter, footer crumb) so an
// enum click feels like the same family of UI as opening the type
// chooser. Its chrome is the popup family rule (the comment above
// SourceChooserPopup::paintEvent, src/sourcechooserpopup.cpp): ONE
// surface inside ONE device-exact frame, the field with its own seam, the
// footer's seam a device row of the popup's, the local 6-px scrollbar.
// Intentionally no Q_OBJECT — we'd otherwise need to add this header to
// every test target's source list for AUTOMOC. A std::function callback
// gets the same job done with zero moc footprint.
class EnumPickerPopup : public QFrame {
public:
    struct Member {
        QString  name;
        int64_t  value;
    };
    using ChosenFn = std::function<void(int64_t)>;

    explicit EnumPickerPopup(QWidget* parent = nullptr)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        auto* outer = new QVBoxLayout(this);
        // 1 logical px on every side: the frame's device row / column lives
        // inside the first logical px (paintEvent), so the field, the list
        // and its scrollbar end AT the frame instead of on it, and the rows
        // and every seam span this interior width.
        outer->setContentsMargins(1, 1, 1, 1);
        outer->setSpacing(0);

        // ── Title row: "enum Name" + Esc hint, at the house gutter ──
        auto* topRow = new QHBoxLayout;
        topRow->setContentsMargins(kGutter, 6, 6, 2);
        topRow->setSpacing(6);
        m_title = new QLabel(this);
        m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        topRow->addWidget(m_title, 1);
        m_escLabel = new QLabel(QStringLiteral("Esc"), this);
        m_escLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        topRow->addWidget(m_escLabel);
        outer->addLayout(topRow);

        // ── Filter (visible only when >10 members): the popup surface plus
        // its own device-exact seam (PopupFilterField) ──
        m_filter = new PopupFilterField(this);
        m_filter->setPlaceholderText(QStringLiteral("Filter members..."));
        m_filter->setFrame(false);
        m_filter->installEventFilter(this);
        connect(m_filter, &QLineEdit::textChanged, this,
                [this] { applyFilter(); });
        outer->addWidget(m_filter);

        // ── List ──
        m_model = new Model(this);
        m_view = new ListView(this);
        m_view->setModel(m_model);
        m_view->setUniformItemSizes(true);
        m_view->setFrameShape(QFrame::NoFrame);
        m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_view->setSelectionMode(QAbstractItemView::SingleSelection);
        m_view->setMouseTracking(true);
        m_delegate = new Delegate(m_model, this);
        m_view->setItemDelegate(m_delegate);
        m_view->installEventFilter(this);
        connect(m_view, &QAbstractItemView::activated, this,
                [this](const QModelIndex& idx) { acceptRow(idx); });
        connect(m_view, &QAbstractItemView::clicked, this,
                [this](const QModelIndex& idx) { acceptRow(idx); });
        outer->addWidget(m_view, 1);

        // ── Footer crumb: the same surface; the popup paints its seam ──
        m_footer = new QLabel(this);
        m_footer->setFixedHeight(20);
        m_footer->setTextFormat(Qt::RichText);
        m_footer->setContentsMargins(kGutter, 0, 6, 0);
        m_footer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        outer->addWidget(m_footer);

        applyTheme();
    }

    // Show the popup for an enum with the given title (e.g. "MyEnum"),
    // member list, current value, and accent color. Resolves to its
    // preferred size and pops at the requested global position.
    void show(const QString& enumName,
              const QVector<Member>& members,
              int64_t currentValue,
              const QColor& accent,
              const QPoint& globalPos) {
        m_accent = accent;
        m_currentValue = currentValue;
        m_allMembers = members;
        m_title->setText(QStringLiteral("<b>enum</b> %1").arg(enumName.toHtmlEscaped()));
        m_filter->clear();
        m_filter->setVisible(members.size() > 10);
        applyTheme();
        applyFilter();
        // Pre-select the row matching the current value
        for (int i = 0; i < m_model->rowCount(); i++) {
            const auto& m = m_model->memberAt(i);
            if (m.value == currentValue) {
                m_view->setCurrentIndex(m_model->index(i, 0));
                m_view->scrollTo(m_model->index(i, 0),
                                 QAbstractItemView::PositionAtCenter);
                break;
            }
        }
        // Height from the real hints: the rows through ListView::sizeHint
        // (at most 14 of them), the chrome through the layout — not a
        // hand-summed table of paddings that drifts from the widgets.
        layout()->invalidate();
        layout()->activate();
        const int totalH = layout()->sizeHint().height();
        const int totalW = qMax(280, computePreferredWidth());

        // Clamp to screen so we don't render off-edge.
        QRect screen = QGuiApplication::screenAt(globalPos)
                         ? QGuiApplication::screenAt(globalPos)->availableGeometry()
                         : QGuiApplication::primaryScreen()->availableGeometry();
        QPoint pos = globalPos;
        if (pos.x() + totalW > screen.right()) pos.rx() = screen.right() - totalW;
        if (pos.y() + totalH > screen.bottom()) pos.ry() = globalPos.y() - totalH;
        if (pos.x() < screen.left()) pos.rx() = screen.left();
        if (pos.y() < screen.top())  pos.ry() = screen.top();

        resize(totalW, totalH);
        move(pos);
        QFrame::show();
        m_filter->isVisible() ? m_filter->setFocus() : m_view->setFocus();
    }

    void setOnChosen(ChosenFn fn) { m_onChosen = std::move(fn); }

protected:
    // The popup family rule: ONE surface, ONE device-exact theme.border
    // frame (fillDeviceFrameOfRect), the footer's seam one device row of
    // containerBorderColor across the interior. Was a 1-logical QPen in
    // borderFocused — two device rows at some phases of 125 %, and the
    // focus colour on a frame that has no focus.
    void paintEvent(QPaintEvent*) override {
        const auto& t = ThemeManager::instance().current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), t.background);
        fillTopDeviceRowOfRect(p, QRectF(m_footer->geometry()), containerBorderColor(t));
        fillDeviceFrameOfRect(p, QRectF(rect()), t.border);
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) { hide(); return true; }
            if (obj == m_filter) {
                if (ke->key() == Qt::Key_Down
                    || ke->key() == Qt::Key_Return
                    || ke->key() == Qt::Key_Enter) {
                    if (m_model->rowCount() > 0) {
                        if (!m_view->currentIndex().isValid())
                            m_view->setCurrentIndex(m_model->index(0, 0));
                        if (ke->key() != Qt::Key_Down)
                            acceptRow(m_view->currentIndex());
                        else
                            m_view->setFocus();
                    }
                    return true;
                }
            }
            if (obj == m_view) {
                if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                    acceptRow(m_view->currentIndex());
                    return true;
                }
                if (ke->key() == Qt::Key_Backspace && m_filter->isVisible()) {
                    QString t = m_filter->text();
                    if (!t.isEmpty()) m_filter->setText(t.chopped(1));
                    m_filter->setFocus();
                    return true;
                }
                QString s = ke->text();
                if (m_filter->isVisible() && !s.isEmpty() && s.at(0).isPrint()
                    && !ke->modifiers().testFlag(Qt::ControlModifier)
                    && !ke->modifiers().testFlag(Qt::AltModifier)) {
                    m_filter->setText(m_filter->text() + s);
                    m_filter->setFocus();
                    return true;
                }
            }
        }
        return QFrame::eventFilter(obj, ev);
    }

private:
    // ── The list ── sizeHint is the real content height, at most 14 rows,
    // so show() sizes the popup from layout()->sizeHint().
    class ListView : public QListView {
    public:
        using QListView::QListView;
        QSize sizeHint() const override {
            const int rows = model() ? qMin(model()->rowCount(), 14) : 0;
            const int rowH = static_cast<Delegate*>(itemDelegate())->rowHeight();
            return QSize(QListView::sizeHint().width(),
                         qMax(rowH, rowH * rows) + 2 * frameWidth());
        }
    };

    // ── Custom model ── (no Q_OBJECT; subclass uses only inherited signals)
    class Model : public QAbstractListModel {
    public:
        explicit Model(QObject* parent) : QAbstractListModel(parent) {}
        int rowCount(const QModelIndex& p = {}) const override {
            return p.isValid() ? 0 : m_rows.size();
        }
        QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override {
            if (!idx.isValid() || idx.row() >= m_rows.size()) return {};
            if (role == Qt::DisplayRole) return m_rows.at(idx.row()).name;
            return {};
        }
        const Member& memberAt(int row) const { return m_rows.at(row); }
        const QVector<int>& matchPositionsAt(int row) const { return m_match.at(row); }
        void setRows(QVector<Member> rows, QVector<QVector<int>> mp) {
            beginResetModel();
            m_rows = std::move(rows);
            m_match = std::move(mp);
            endResetModel();
        }
    private:
        QVector<Member> m_rows;
        QVector<QVector<int>> m_match;
    };

    // ── Custom delegate ──
    // The row's accent stripe ends at kGutter (see paint); the row's
    // first ink follows it.
    static constexpr int kStripeW = 2;

    class Delegate : public QStyledItemDelegate {
    public:
        Delegate(Model* m, EnumPickerPopup* owner)
            : QStyledItemDelegate(owner), m_model(m), m_owner(owner) {}
        int rowHeight() const {
            QFontMetrics fm(m_owner->font());
            return fm.height() + 6;
        }
        QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
            return QSize(0, rowHeight());
        }
        void paint(QPainter* p, const QStyleOptionViewItem& opt,
                   const QModelIndex& idx) const override {
            const auto& t = ThemeManager::instance().current();
            const auto& m = m_model->memberAt(idx.row());
            QRect r = opt.rect;
            bool selected = (opt.state & QStyle::State_Selected);
            bool hovered  = (opt.state & QStyle::State_MouseOver);
            bool isCurrent = (m.value == m_owner->m_currentValue);
            if (selected)     p->fillRect(r, t.selected);
            else if (hovered) p->fillRect(r, t.hover);

            // Left accent stripe — brighter when this row is the current
            // value (a visual "you are here" cue, equivalent to the
            // checkmark a stock QMenu draws but in our visual idiom). It
            // ends at the house gutter: at r.x() it sat on the device
            // columns right against the frame, a coloured edge welded to
            // the popup's left border down the whole list — the "bar on
            // the border" the family rule took out of the source chooser.
            QColor stripeCol = m_owner->m_accent;
            if (!isCurrent && !selected) {
                stripeCol = QColor(stripeCol.red(), stripeCol.green(),
                                   stripeCol.blue(), 160);
            }
            const int stripeX = r.x() + kGutter - kStripeW;
            p->fillRect(stripeX, r.y(), kStripeW, r.height(), stripeCol);

            QFontMetrics fm(opt.font);
            int baseline = r.y() + (r.height() + fm.ascent() - fm.descent()) / 2;
            int x = stripeX + kStripeW + 4;

            // Source pip (4×4)
            p->fillRect(x, r.y() + (r.height() - 4) / 2, 4, 4, m_owner->m_accent);
            x += 4 + 6;

            // "Current value" marker — small triangle, only on the matching row.
            if (isCurrent) {
                int tx = x;
                int ty = r.y() + r.height() / 2;
                QPolygon tri;
                tri << QPoint(tx, ty - 3) << QPoint(tx + 5, ty) << QPoint(tx, ty + 3);
                p->setPen(Qt::NoPen);
                p->setBrush(m_owner->m_accent);
                p->drawPolygon(tri);
                x += 9;
            } else {
                x += 9;
            }

            // Value column (right-aligned hex + decimal)
            QString valHex = QStringLiteral("0x%1").arg((qulonglong)m.value, 0, 16);
            QString valDec = QString::number(m.value);
            int hexW = fm.horizontalAdvance(valHex);
            int decW = fm.horizontalAdvance(valDec);
            int rightPad = 8;
            int decX = r.right() - decW - rightPad;
            int hexX = decX - hexW - 12;

            // Name with fuzzy highlight
            int nameMaxX = hexX - 8;
            int nameW = qMax(0, nameMaxX - x);
            QString elided = fm.elidedText(m.name, Qt::ElideRight, nameW);
            const QVector<int>& mp = m_model->matchPositionsAt(idx.row());
            if (!mp.isEmpty()) {
                int cx = x;
                for (int i = 0; i < elided.size(); i++) {
                    bool hit = std::find(mp.constBegin(), mp.constEnd(), i) != mp.constEnd();
                    QString ch(elided.at(i));
                    int cw = fm.horizontalAdvance(ch);
                    if (hit) p->fillRect(cx, r.y() + 1, cw, r.height() - 2, t.selection);
                    p->setPen(t.text);
                    p->drawText(cx, baseline, ch);
                    cx += cw;
                }
            } else {
                p->setPen(isCurrent ? t.text : t.text);
                p->drawText(x, baseline, elided);
            }

            // Value columns
            p->setPen(t.textDim);
            p->drawText(hexX, baseline, valHex);
            p->setPen(t.textFaint);
            p->drawText(decX, baseline, valDec);
        }
    private:
        Model* m_model;
        EnumPickerPopup* m_owner;
    };

    int computePreferredWidth() const {
        QFontMetrics fm(font());
        int maxName = 0;
        for (const auto& m : m_allMembers)
            maxName = qMax(maxName, fm.horizontalAdvance(m.name));
        int valW = fm.horizontalAdvance(QStringLiteral("0x7FFFFFFFFFFFFFFF"))
                 + fm.horizontalAdvance(QStringLiteral("-9223372036854775808"));
        return kGutter + 4 + 4 + 6 + 9 + maxName + 8 + valW + 8 + 16;
    }

    void applyFilter() {
        QString pat = m_filter->text().trimmed();
        QVector<Member> visible;
        QVector<QVector<int>> mp;
        QVector<int> scores;
        for (const auto& m : m_allMembers) {
            QVector<int> hits;
            int sc = 1;
            if (!pat.isEmpty()) {
                sc = fuzzyScore(pat, m.name, &hits);
                if (sc == 0) continue;
            }
            visible.append(m);
            mp.append(std::move(hits));
            scores.append(sc);
        }
        // Sort by score (search active) else by value ascending.
        QVector<int> order(visible.size());
        for (int i = 0; i < order.size(); i++) order[i] = i;
        bool searchActive = !pat.isEmpty();
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (searchActive && scores[a] != scores[b]) return scores[a] > scores[b];
            return visible[a].value < visible[b].value;
        });
        QVector<Member> sortedVis(order.size());
        QVector<QVector<int>> sortedMp(order.size());
        for (int i = 0; i < order.size(); i++) {
            sortedVis[i] = std::move(visible[order[i]]);
            sortedMp[i] = std::move(mp[order[i]]);
        }
        m_model->setRows(std::move(sortedVis), std::move(sortedMp));
        // The list's sizeHint follows its rows; the layout caches it until
        // the widget says otherwise, and show() sizes from the layout.
        m_view->updateGeometry();
        updateFooter();
    }

    void updateFooter() {
        int vis = m_model->rowCount();
        int tot = m_allMembers.size();
        const auto& t = ThemeManager::instance().current();
        if (tot == 0) {
            m_footer->setText(QStringLiteral("(no members)"));
            return;
        }
        QString hint;
        if (vis < tot) hint = QStringLiteral("%1 of %2 · ").arg(vis).arg(tot);
        else           hint = QStringLiteral("%1 members · ").arg(tot);
        m_footer->setText(hint + QStringLiteral(
            "<span style='color:%1'>↑↓ navigate · Enter set · Esc dismiss</span>")
            .arg(t.textFaint.name()));
    }

    void acceptRow(const QModelIndex& idx) {
        if (!idx.isValid()) return;
        const auto& m = m_model->memberAt(idx.row());
        int64_t v = m.value;
        hide();
        if (m_onChosen) m_onChosen(v);
    }

    void applyTheme() {
        const auto& t = ThemeManager::instance().current();
        QSettings s("RC", "RC");
        QFont mono(s.value("font", "JetBrains Mono").toString(), 10);
        mono.setFixedPitch(true);
        setFont(mono);
        m_title->setFont(mono);
        m_filter->setFont(mono);
        m_view->setFont(mono);

        QFont small = mono;
        if (small.pixelSize() > 0) small.setPixelSize(qMax(7, small.pixelSize() - 2));
        else small.setPointSize(qMax(6, small.pointSize() - 1));
        m_footer->setFont(small);
        m_escLabel->setFont(small);

        // The ground is painted (paintEvent); the palette still names it
        // for children that ask.
        QPalette pp = palette();
        pp.setColor(QPalette::Window, t.background);
        pp.setColor(QPalette::WindowText, t.text);
        setPalette(pp);

        m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(t.text.name()));
        m_escLabel->setStyleSheet(QStringLiteral("QLabel { color: %1; padding: 0 4px; }")
                                  .arg(t.textFaint.name()));
        // The field: the popup surface plus its own device-exact seam — no
        // box, no radius (it was the one rounded QSS box in the family).
        m_filter->applyTheme(t, t.background, kGutter - 2);
        m_filter->setFixedHeight(qMax(PopupFilterField::kMinHeight,
                                      QFontMetrics(mono).height() + 8));
        QPalette vp = m_view->palette();
        vp.setColor(QPalette::Base, t.background);
        vp.setColor(QPalette::Text, t.text);
        vp.setColor(QPalette::Highlight, t.selected);
        vp.setColor(QPalette::HighlightedText, t.text);
        m_view->setPalette(vp);
        m_view->setStyleSheet(QStringLiteral(
            "QListView { background: %1; border: none; }"
            "QAbstractScrollArea::corner { background: %1; border: none; }")
            .arg(t.background.name()));
        // The scrollbar: the popup's local 6-px rule, not the app-wide rod.
        m_view->verticalScrollBar()->setStyleSheet(popupScrollBarQss(t, t.background));
        // The footer is the same surface (it was a backgroundAlt band under
        // a 1-logical border-top); its seam is the popup's (paintEvent).
        m_footer->setStyleSheet(QStringLiteral("QLabel { color: %1; }")
                                .arg(t.textFaint.name()));
        update();
    }

    QLabel* m_title = nullptr;
    QLabel* m_escLabel = nullptr;
    PopupFilterField* m_filter = nullptr;
    QListView* m_view = nullptr;
    QLabel* m_footer = nullptr;
    Model* m_model = nullptr;
    Delegate* m_delegate = nullptr;

    QVector<Member> m_allMembers;
    int64_t m_currentValue = 0;
    QColor m_accent;
    ChosenFn m_onChosen;
};

} // namespace rcx
