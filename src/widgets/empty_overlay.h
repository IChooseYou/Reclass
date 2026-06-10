#pragma once
// Shared empty-state overlay painter for item views — a centered two-line
// placeholder (primary call-to-action + a dimmer hint beneath) drawn over a
// blank view so an empty panel reads as "nothing here yet — do X" instead of a
// bare gray rectangle. Single source of truth for the Workspace / Bookmarks
// empty-hint views (main.cpp) and the scanner's EmptyResultsTable
// (scannerpanel.cpp), which previously carried byte-identical copies.
#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QRect>
#include <QString>
#include "themes/thememanager.h"

namespace rcx {

inline void paintEmptyOverlay(QWidget* viewport, const QFont& f,
                              const QString& primary, const QString& hint) {
    QPainter p(viewport);
    p.setRenderHint(QPainter::TextAntialiasing);
    const auto& t = ThemeManager::instance().current();
    p.setFont(f);
    const QRect r = viewport->rect();
    const QFontMetrics fm(f);
    const int totalH = fm.height() * 2 + 6;
    QRect primaryR = r;
    primaryR.setHeight(r.height() / 2 + totalH / 2 - fm.height());
    QRect secondaryR = r;
    secondaryR.setTop(primaryR.bottom() + 6);
    p.setPen(t.textMuted);
    p.drawText(primaryR, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, primary);
    if (!hint.isEmpty()) {
        p.setPen(t.textFaint);
        p.drawText(secondaryR, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, hint);
    }
}

} // namespace rcx
