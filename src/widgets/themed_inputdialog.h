#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>
#include <optional>

namespace rcx {

// Themed counterparts to QInputDialog::getText / getInt / getItem.
// Static-only — instances are built and destroyed inside each call.
//
// std::optional<T> return shape avoids the bool-out-param pattern of
// the Qt originals. `nullopt` means the user dismissed the dialog.
//
// Build composed labels with QString::arg, never with `+` —
// translators need the whole sentence to reorder grammar. (The
// codebase currently uses QStringLiteral; tr() can be swapped in
// later as a mechanical pass once .ts files exist.)
class ThemedInputDialog {
public:
    static std::optional<QString> getText(QWidget* parent,
        const QString& title, const QString& label,
        const QString& defaultText = QString(),
        const QString& placeholder = QString());

    static std::optional<int> getInt(QWidget* parent,
        const QString& title, const QString& label,
        int value, int min, int max);

    static std::optional<QString> getItem(QWidget* parent,
        const QString& title, const QString& label,
        const QStringList& items, int currentIndex = 0);
};

} // namespace rcx
