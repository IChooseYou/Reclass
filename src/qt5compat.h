#pragma once
#include <QtGlobal>
#include <QMenu>
#include <QAction>
#include <QKeySequence>
#include <QIcon>

namespace rcx { namespace compat {

// QMenu::addAction with shortcut — Qt6 puts shortcut before receiver,
// Qt5 puts it after.  These normalize to the Qt6 argument order.

template<typename Obj, typename Func>
inline QAction* addAction(QMenu* menu, const QString& text,
                          const QKeySequence& shortcut,
                          const Obj* receiver, Func slot)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return menu->addAction(text, shortcut, receiver, slot);
#else
    return menu->addAction(text, receiver, slot, shortcut);
#endif
}

template<typename Obj, typename Func>
inline QAction* addAction(QMenu* menu, const QIcon& icon, const QString& text,
                          const QKeySequence& shortcut,
                          const Obj* receiver, Func slot)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return menu->addAction(icon, text, shortcut, receiver, slot);
#else
    return menu->addAction(icon, text, receiver, slot, shortcut);
#endif
}

}} // namespace rcx::compat
