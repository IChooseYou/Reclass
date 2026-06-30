#pragma once

#include "core.h"
#include "themes/thememanager.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QLayoutItem>
#include <functional>

namespace rcx {

// BreadcrumbBar — compact, clickable drill-down trail shown above the command
// row in RcxEditor. ALWAYS visible while a class is in view; reflects the
// inline-expansion focus path (the chain of expanded typed pointers from the
// root class down to the deepest drilled one), e.g. `RcxEditor › __vptr ›
// QWidgetVTable`. The controller flattens its focus path into a Crumb list:
// clickable CLASS crumbs (isField=false; rootId carries the crumb INDEX — 0 =
// root, i = the class shown by focus pointer i-1) separated by inert FIELD
// connectors (isField=true). Clicking a class crumb fires onCrumb(index); the
// editor re-emits it as crumbClicked so the controller collapses everything
// below that class and scrolls to it. There is NO back button (clicking a
// crumb is the back affordance). Deep trails collapse the middle to an
// ellipsis: Head › field › … › field › Tail-1 › field › Tail.
//
// Intentionally no Q_OBJECT (mirrors EnumPickerPopup) — a std::function
// callback avoids dragging this header into every test target's AUTOMOC. It is
// still a QObject (via QWidget) so findChildren<QToolButton*>() and the
// QToolButton::clicked connects below work without a custom moc.
class BreadcrumbBar : public QWidget {
public:
    using CrumbFn = std::function<void(uint64_t /*crumbIndex*/)>;

    explicit BreadcrumbBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("rcxBreadcrumbBar"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(24);  // fixed strip height so Qt::AlignVCenter has a
                             // stable axis — labels and buttons of differing
                             // natural heights then center on the same line.
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(6, 0, 6, 0);
        m_layout->setSpacing(2);
        applyTheme(ThemeManager::instance().current());
        setVisible(false);  // until first setCrumbs (no document yet)
    }

    void setOnCrumb(CrumbFn fn) { m_onCrumb = std::move(fn); }

    // Beyond this many class crumbs the middle collapses to an ellipsis.
    void setMaxClassCrumbs(int n) { m_maxClassCrumbs = qMax(2, n); rebuild(); }

    // Replace the rendered trail. Interleaves clickable class crumbs
    // (isField=false, rootId = crumb index) and inert "fieldName" connectors
    // (isField=true). Always shown when there is ≥1 class crumb.
    void setCrumbs(const QVector<Crumb>& crumbs) {
        m_crumbs = crumbs;
        rebuild();
    }

    void applyTheme(const Theme& t) {
        m_theme = t;
        const QColor strip = menuBarColor(t);
        setStyleSheet(QStringLiteral(
            "#rcxBreadcrumbBar { background:%1; border-bottom:1px solid %2; }")
            .arg(strip.name(), t.border.name()));
        rebuild();
    }

    // ── Test accessors ──
    bool barVisible() const { return isVisible(); }
    // Rendered tokens left→right: class names, "›" separators, field
    // connectors, and "…" for a collapsed gap.
    QStringList segments() const { return m_segments; }

private:
    struct Item { QString cls; uint64_t index = 0; QString incoming; };

    void clearLayout() {
        QLayoutItem* it;
        while ((it = m_layout->takeAt(0)) != nullptr) {
            if (it->widget()) {
                // hide() NOW: a deleteLater'd widget stays VISIBLE (and painted
                // at its old position) until the event loop processes the
                // deferred delete — which plain processEvents() skips — so
                // without this the previous crumbs overlap the new ones on
                // every rebuild. deleteLater (not delete) is still required:
                // rebuild() runs from a crumb button's own clicked handler, so
                // deleting it synchronously would be a use-after-free.
                it->widget()->hide();
                it->widget()->deleteLater();
            }
            delete it;
        }
        m_segments.clear();
    }

    QLabel* addLabel(const QString& text, const QColor& fg, bool italic, bool bold) {
        auto* l = new QLabel(text, this);
        l->setStyleSheet(QStringLiteral("color:%1;%2%3")
            .arg(fg.name(),
                 italic ? QStringLiteral("font-style:italic;") : QString(),
                 bold   ? QStringLiteral("font-weight:bold;")  : QString()));
        m_layout->addWidget(l, 0, Qt::AlignVCenter);
        return l;
    }

    void addSep() {
        addLabel(QStringLiteral("›"), m_theme.textFaint, false, false);  // ›
        m_segments.push_back(QStringLiteral("›"));
    }

    void makeClickable(const QString& text, uint64_t index, bool current) {
        auto* btn = new QToolButton(this);
        btn->setText(text);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { border:none; padding:0 2px; color:%1; background:transparent;%2 }"
            "QToolButton:hover { color:%3; text-decoration:underline; }")
            .arg(m_theme.text.name(),
                 current ? QStringLiteral(" font-weight:bold;") : QString(),
                 m_theme.indHoverSpan.name()));
        connect(btn, &QToolButton::clicked, this, [this, index]() {
            if (m_onCrumb) m_onCrumb(index);
        });
        m_layout->addWidget(btn, 0, Qt::AlignVCenter);
    }

    void rebuild() {
        if (!m_layout) return;
        clearLayout();

        // Flatten Crumb list → class items carrying their incoming field label.
        QVector<Item> items;
        QString pendingField;
        for (const Crumb& c : m_crumbs) {
            if (c.isField) { pendingField = c.label; }
            else { items.push_back({ c.label, c.rootId, pendingField }); pendingField.clear(); }
        }

        if (items.isEmpty()) { setVisible(false); return; }

        // Which items render in full; -1 = the collapsed ellipsis gap.
        QVector<int> show;
        if (items.size() <= m_maxClassCrumbs) {
            for (int i = 0; i < items.size(); ++i) show.push_back(i);
        } else {
            show.push_back(0);
            show.push_back(-1);
            for (int i = items.size() - (m_maxClassCrumbs - 1); i < items.size(); ++i)
                show.push_back(i);
        }

        bool first = true;
        for (int idx : show) {
            if (idx < 0) {  // ellipsis gap
                if (!first) addSep();
                auto* ell = addLabel(QStringLiteral("…"), m_theme.textMuted, false, false);
                QStringList hidden;
                for (int h = 1; h < items.size() - (m_maxClassCrumbs - 1); ++h)
                    hidden << items[h].cls;
                ell->setToolTip(hidden.join(QStringLiteral(" › ")));
                m_segments.push_back(QStringLiteral("…"));
                first = false;
                continue;
            }
            const Item& item = items[idx];
            if (!first) addSep();
            if (!item.incoming.isEmpty() && idx != 0) {
                addLabel(item.incoming, m_theme.textMuted, true, false);
                m_segments.push_back(item.incoming);
                addSep();
            }
            const bool isCurrent = (idx == items.size() - 1);
            // Every class crumb is clickable (collapse-to + scroll). The
            // current/deepest one just scrolls (nothing deeper to collapse);
            // it is bolded to mark "you are here".
            makeClickable(item.cls, item.index, isCurrent);
            m_segments.push_back(item.cls);
            first = false;
        }

        m_layout->addStretch(1);
        setVisible(true);
    }

    QHBoxLayout*   m_layout = nullptr;
    QVector<Crumb> m_crumbs;
    QStringList    m_segments;
    CrumbFn        m_onCrumb;
    Theme          m_theme;
    int            m_maxClassCrumbs = 4;
};

} // namespace rcx
