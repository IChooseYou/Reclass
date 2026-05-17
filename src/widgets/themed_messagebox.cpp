#include "themed_messagebox.h"
#include "dialog_button.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QFontMetrics>
#include <QSvgRenderer>

namespace rcx {

ThemedMessageBox::ThemedMessageBox(QWidget* parent, Severity sev,
                                   const QString& title, const QString& text)
    : ThemedDialog(parent), m_severity(sev) {
    setWindowTitle(title);
    setModal(true);

    auto* outer = new QVBoxLayout(this);
    // Tighter top, more bottom — buttons sit flush against the bottom
    // edge of the dialog rather than floating in extra padding.
    outer->setContentsMargins(24, 22, 24, 18);
    outer->setSpacing(18);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(16);

    // Severity icon removed — title bar + message text convey severity
    // without the noisy colored glyph. m_iconLbl kept null so existing
    // code that touches it stays safe.
    m_iconLbl = nullptr;

    m_textLbl = new QLabel(text, this);
    m_textLbl->setWordWrap(true);
    m_textLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {
        QFont f = m_textLbl->font();
        f.setPointSizeF(f.pointSizeF() + 0.5);
        m_textLbl->setFont(f);
    }
    topRow->addWidget(m_textLbl, 1);
    outer->addLayout(topRow);

    m_buttonRow = new QHBoxLayout;
    // Extra top padding above the button row so the buttons feel like
    // a distinct action zone, not text-row sibling. 12 px between
    // buttons (was 8) — chunkier buttons need more breathing room or
    // they read as a fused strip.
    m_buttonRow->setContentsMargins(0, 4, 0, 0);
    m_buttonRow->setSpacing(12);
    m_buttonRow->addStretch(1);
    outer->addLayout(m_buttonRow);

    setMinimumWidth(420);
    setMaximumWidth(640);

    applyTheme();
}

void ThemedMessageBox::setDetailText(const QString& detail) {
    if (detail.isEmpty()) return;
    if (!m_detailLbl) {
        m_detailLbl = new QLabel(detail, this);
        m_detailLbl->setWordWrap(true);
        m_detailLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // Slot the detail block above the button row, indented to align
        // with the body text (32 px icon + 16 px gap).
        auto* outer = static_cast<QVBoxLayout*>(layout());
        auto* indent = new QHBoxLayout;
        indent->setContentsMargins(48, 0, 0, 0);
        indent->setSpacing(0);
        indent->addWidget(m_detailLbl);
        outer->insertLayout(outer->count() - 1, indent);
    } else {
        m_detailLbl->setText(detail);
    }
    applyTheme();  // make sure the new label picks up theme color
}

void ThemedMessageBox::appendButton(QPushButton* button) {
    button->setParent(this);
    m_buttonRow->addWidget(button);
}

void ThemedMessageBox::setDefault(QPushButton* button) {
    button->setDefault(true);
    button->setFocus();
}

void ThemedMessageBox::applyTheme() {
    ThemedDialog::applyTheme();
    const auto& t = ThemeManager::instance().current();
    if (m_textLbl)
        m_textLbl->setStyleSheet(QStringLiteral("color: %1;").arg(t.text.name()));
    if (m_detailLbl)
        m_detailLbl->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
}

void ThemedMessageBox::redrawIcon() {
    // Severity icon removed — see ctor for the rationale.
}

void ThemedMessageBox::info(QWidget* parent, const QString& title,
                            const QString& text) {
    ThemedMessageBox box(parent, Info, title, text);
    auto* ok = new DialogButton(QObject::tr("OK"),
                                DialogButton::Primary, &box);
    QObject::connect(ok, &QPushButton::clicked, &box, &QDialog::accept);
    box.appendButton(ok);
    box.setDefault(ok);
    box.exec();
}

void ThemedMessageBox::warn(QWidget* parent, const QString& title,
                            const QString& text) {
    ThemedMessageBox box(parent, Warning, title, text);
    auto* ok = new DialogButton(QObject::tr("OK"),
                                DialogButton::Primary, &box);
    QObject::connect(ok, &QPushButton::clicked, &box, &QDialog::accept);
    box.appendButton(ok);
    box.setDefault(ok);
    box.exec();
}

void ThemedMessageBox::critical(QWidget* parent, const QString& title,
                                const QString& text) {
    ThemedMessageBox box(parent, Critical, title, text);
    auto* ok = new DialogButton(QObject::tr("OK"),
                                DialogButton::Primary, &box);
    QObject::connect(ok, &QPushButton::clicked, &box, &QDialog::accept);
    box.appendButton(ok);
    box.setDefault(ok);
    box.exec();
}

bool ThemedMessageBox::confirm(QWidget* parent, const QString& title,
                               const QString& text,
                               const QString& acceptLabel,
                               const QString& rejectLabel,
                               bool destructive) {
    ThemedMessageBox box(parent, Question, title, text);

    auto* cancel = new DialogButton(
        rejectLabel.isEmpty() ? QObject::tr("Cancel") : rejectLabel,
        DialogButton::Secondary, &box);
    QObject::connect(cancel, &QPushButton::clicked, &box, &QDialog::reject);
    box.appendButton(cancel);

    auto* accept = new DialogButton(acceptLabel,
        destructive ? DialogButton::Destructive : DialogButton::Primary, &box);
    QObject::connect(accept, &QPushButton::clicked, &box, &QDialog::accept);
    box.appendButton(accept);

    // Destructive prompts default-focus the safe choice — a stray
    // Enter does not destroy work.
    box.setDefault(destructive ? cancel : accept);
    return box.exec() == QDialog::Accepted;
}

ThemedMessageBox::UnsavedChoice ThemedMessageBox::unsavedChanges(
        QWidget* parent, const QString& title,
        const QString& text, const QString& detail) {
    ThemedMessageBox box(parent, Warning, title, text);
    if (!detail.isEmpty()) box.setDetailText(detail);

    UnsavedChoice result = UnsavedChoice::Cancel;

    auto* cancel = new DialogButton(QObject::tr("Cancel"),
        DialogButton::Secondary, &box);
    QObject::connect(cancel, &QPushButton::clicked, &box, [&](){
        result = UnsavedChoice::Cancel;
        box.reject();
    });
    box.appendButton(cancel);

    auto* discard = new DialogButton(QObject::tr("Discard"),
        DialogButton::Destructive, &box);
    QObject::connect(discard, &QPushButton::clicked, &box, [&](){
        result = UnsavedChoice::Discard;
        box.accept();
    });
    box.appendButton(discard);

    auto* save = new DialogButton(QObject::tr("Save changes"),
        DialogButton::Primary, &box);
    QObject::connect(save, &QPushButton::clicked, &box, [&](){
        result = UnsavedChoice::Save;
        box.accept();
    });
    box.appendButton(save);

    box.setDefault(save);
    box.exec();
    return result;
}

} // namespace rcx
