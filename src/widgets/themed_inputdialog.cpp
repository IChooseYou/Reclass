#include "themed_inputdialog.h"
#include "themed_dialog.h"
#include "dialog_button.h"
#include "themes/thememanager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>

namespace rcx {

namespace {

void styleLabel(QLabel* lbl) {
    const auto& t = ThemeManager::instance().current();
    lbl->setStyleSheet(QStringLiteral("color: %1;").arg(t.text.name()));
}

// Focus border on input widgets always uses t.borderFocused — the
// semantic token meant for this state. Earlier rev pulled t.indHoverSpan
// here; the two happen to be identical in the VS theme but a custom
// theme could keep them distinct, in which case the line-edit focus
// ring would no longer agree with DialogButton's :focus colour.
void styleLineEdit(QLineEdit* edit) {
    const auto& t = ThemeManager::instance().current();
    edit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
        " padding: 5px 7px; selection-background-color: %4; }"
        "QLineEdit:focus { border-color: %5; }")
        .arg(t.backgroundAlt.name(), t.text.name(),
             t.border.name(), t.selection.name(),
             t.borderFocused.name()));
}

void styleSpinBox(QSpinBox* sb) {
    const auto& t = ThemeManager::instance().current();
    sb->setStyleSheet(QStringLiteral(
        "QSpinBox { background: %1; color: %2; border: 1px solid %3;"
        " padding: 4px 6px; }"
        "QSpinBox:focus { border-color: %4; }")
        .arg(t.backgroundAlt.name(), t.text.name(),
             t.border.name(), t.borderFocused.name()));
}

void styleCombo(QComboBox* cb) {
    const auto& t = ThemeManager::instance().current();
    cb->setStyleSheet(QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3;"
        " padding: 4px 6px; }"
        "QComboBox:focus { border-color: %5; }"
        "QComboBox QAbstractItemView { background: %1; color: %2;"
        " selection-background-color: %4; }")
        .arg(t.backgroundAlt.name(), t.text.name(),
             t.border.name(), t.selected.name(),
             t.borderFocused.name()));
}

// Build the bottom button row for the input dialogs. Returns the
// layout; ok/cancel are returned via out-params so the caller can
// wire return-pressed and default-button focus.
QHBoxLayout* makeOkCancelRow(ThemedDialog* dlg,
                             DialogButton** outOk,
                             DialogButton** outCancel) {
    auto* cancel = new DialogButton(QObject::tr("Cancel"),
        DialogButton::Secondary, dlg);
    auto* ok = new DialogButton(QObject::tr("OK"),
        DialogButton::Primary, dlg);
    QObject::connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ok, &QPushButton::clicked, dlg, &QDialog::accept);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 8, 0, 0);
    row->setSpacing(8);
    row->addStretch(1);
    row->addWidget(cancel);
    row->addWidget(ok);
    *outOk = ok;
    *outCancel = cancel;
    return row;
}

} // anon

std::optional<QString> ThemedInputDialog::getText(QWidget* parent,
        const QString& title, const QString& label,
        const QString& defaultText, const QString& placeholder) {
    ThemedDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setModal(true);
    dlg.setMinimumWidth(380);

    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(20, 18, 20, 14);
    outer->setSpacing(10);

    auto* lbl = new QLabel(label, &dlg);
    styleLabel(lbl);
    lbl->setWordWrap(true);
    outer->addWidget(lbl);

    auto* edit = new QLineEdit(defaultText, &dlg);
    if (!placeholder.isEmpty()) edit->setPlaceholderText(placeholder);
    styleLineEdit(edit);
    outer->addWidget(edit);

    DialogButton* ok = nullptr;
    DialogButton* cancel = nullptr;
    outer->addLayout(makeOkCancelRow(&dlg, &ok, &cancel));

    QObject::connect(edit, &QLineEdit::returnPressed, &dlg, &QDialog::accept);
    ok->setDefault(true);
    edit->setFocus();
    edit->selectAll();

    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    return edit->text();
}

std::optional<int> ThemedInputDialog::getInt(QWidget* parent,
        const QString& title, const QString& label,
        int value, int min, int max) {
    ThemedDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setModal(true);
    dlg.setMinimumWidth(320);

    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(20, 18, 20, 14);
    outer->setSpacing(10);

    auto* lbl = new QLabel(label, &dlg);
    styleLabel(lbl);
    lbl->setWordWrap(true);
    outer->addWidget(lbl);

    auto* sb = new QSpinBox(&dlg);
    sb->setRange(min, max);
    sb->setValue(value);
    styleSpinBox(sb);
    outer->addWidget(sb);

    DialogButton* ok = nullptr;
    DialogButton* cancel = nullptr;
    outer->addLayout(makeOkCancelRow(&dlg, &ok, &cancel));

    ok->setDefault(true);
    sb->setFocus();
    sb->selectAll();

    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    return sb->value();
}

std::optional<QString> ThemedInputDialog::getItem(QWidget* parent,
        const QString& title, const QString& label,
        const QStringList& items, int currentIndex) {
    ThemedDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.setModal(true);
    dlg.setMinimumWidth(360);

    auto* outer = new QVBoxLayout(&dlg);
    outer->setContentsMargins(20, 18, 20, 14);
    outer->setSpacing(10);

    auto* lbl = new QLabel(label, &dlg);
    styleLabel(lbl);
    lbl->setWordWrap(true);
    outer->addWidget(lbl);

    auto* combo = new QComboBox(&dlg);
    combo->addItems(items);
    if (currentIndex >= 0 && currentIndex < items.size())
        combo->setCurrentIndex(currentIndex);
    styleCombo(combo);
    outer->addWidget(combo);

    DialogButton* ok = nullptr;
    DialogButton* cancel = nullptr;
    outer->addLayout(makeOkCancelRow(&dlg, &ok, &cancel));

    ok->setDefault(true);
    combo->setFocus();

    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    return combo->currentText();
}

} // namespace rcx
