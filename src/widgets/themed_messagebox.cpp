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
    outer->setContentsMargins(22, 20, 22, 16);
    outer->setSpacing(14);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(16);

    m_iconLbl = new QLabel(this);
    m_iconLbl->setFixedSize(32, 32);
    m_iconLbl->setAlignment(Qt::AlignCenter);
    topRow->addWidget(m_iconLbl, 0, Qt::AlignTop);

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
    m_buttonRow->setContentsMargins(0, 8, 0, 0);
    m_buttonRow->setSpacing(8);
    m_buttonRow->addStretch(1);
    outer->addLayout(m_buttonRow);

    setMinimumWidth(380);
    setMaximumWidth(620);

    redrawIcon();
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
    redrawIcon();
}

void ThemedMessageBox::redrawIcon() {
    if (!m_iconLbl) return;
    // Pull from the same vsicon library QMenu / QAction icons use, so
    // the dialog reads as part of the app — not a hand-drawn shape
    // that's never quite centered. The SVGs live in src/vsicons and
    // are exposed by resources.qrc as ":/vsicons/<name>.svg".
    QString iconPath;
    switch (m_severity) {
    case Info:     iconPath = QStringLiteral(":/vsicons/info.svg");     break;
    case Warning:  iconPath = QStringLiteral(":/vsicons/warning.svg");  break;
    case Critical: iconPath = QStringLiteral(":/vsicons/error.svg");    break;
    case Question: iconPath = QStringLiteral(":/vsicons/question.svg"); break;
    }
    const int W = 32, H = 32;  // logical px — fixed-size label bounds
    const qreal dpr = qMax(1.0, devicePixelRatioF());
    QPixmap pm(QSize(W, H) * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QSvgRenderer r(iconPath);
    if (r.isValid()) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // Render into the full logical W×H — QSvgRenderer respects the
        // SVG's viewBox and draws centered within the destination, so
        // off-center shapes (the painter version we used to do) can't
        // happen here.
        r.render(&p, QRectF(0, 0, W, H));
    }
    m_iconLbl->setPixmap(pm);
    m_iconLbl->setAlignment(Qt::AlignCenter);
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
