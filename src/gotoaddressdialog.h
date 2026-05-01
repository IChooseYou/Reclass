#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QSettings>
#include <QKeyEvent>
#include <QStringList>
#include "addressparser.h"
#include "themes/thememanager.h"

namespace rcx {

// Modal address-jump dialog. Independent of the address-bar editor: takes
// any expression the AddressParser understands ("0x7FF6...", "<game.exe>+0x100",
// "[<ntdll>+0x58]", "ntdll!RtlAllocateHeap"), evaluates it, and emits the
// resulting absolute address. Recent entries are persisted via QSettings
// (key: "gotoAddress/recent") and shared with anything else that reuses
// the same key — e.g. the bookmarks dock could pick them up later.
//
// Live-validation: as the user types, the dialog evaluates and shows
// either the resolved hex address or the parser's error message.
class GotoAddressDialog : public QDialog {
public:
    static constexpr const char* kSettingsKey = "gotoAddress/recent";
    static constexpr int kMaxRecent = 12;

    explicit GotoAddressDialog(const AddressParserCallbacks& cbs,
                               int pointerSize = 8,
                               QWidget* parent = nullptr)
        : QDialog(parent), m_cbs(cbs), m_ptrSize(pointerSize) {
        setWindowTitle(QStringLiteral("Go to Address"));
        setModal(true);
        resize(440, 320);

        const auto& t = ThemeManager::instance().current();
        {
            QPalette pal = palette();
            pal.setColor(QPalette::Window, t.background);
            pal.setColor(QPalette::WindowText, t.text);
            setPalette(pal);
            setAutoFillBackground(true);
        }

        QSettings s("Reclass", "Reclass");
        QFont font(s.value("font", "JetBrains Mono").toString(), 10);
        font.setFixedPitch(true);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(8);

        auto* hint = new QLabel(QStringLiteral(
            "Enter an absolute address or expression "
            "(e.g. <code>0x7FF6...</code>, <code>&lt;game.exe&gt;+0x40</code>, "
            "<code>[ntdll!Ldr]</code>):"));
        hint->setTextFormat(Qt::RichText);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
        layout->addWidget(hint);

        m_input = new QLineEdit;
        m_input->setFont(font);
        m_input->setPlaceholderText(QStringLiteral("0x..."));
        m_input->setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: 1px solid %3;"
            " padding: 6px 8px; font-size: 11pt; }")
            .arg(t.backgroundAlt.name(), t.text.name(), t.border.name()));
        layout->addWidget(m_input);

        m_status = new QLabel(QStringLiteral(" "));
        m_status->setFont(font);
        m_status->setMinimumHeight(QFontMetrics(font).height() + 4);
        layout->addWidget(m_status);

        auto* recentLabel = new QLabel(QStringLiteral("Recent:"));
        recentLabel->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
        layout->addWidget(recentLabel);

        m_recentList = new QListWidget;
        m_recentList->setFont(font);
        m_recentList->setStyleSheet(QStringLiteral(
            "QListWidget { background: %1; color: %2; border: 1px solid %3; }"
            "QListWidget::item:selected { background: %4; color: %5; }")
            .arg(t.backgroundAlt.name(), t.text.name(), t.border.name(),
                 t.selected.name(), t.text.name()));
        for (const QString& entry : loadRecent())
            m_recentList->addItem(entry);
        layout->addWidget(m_recentList, /*stretch=*/1);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Go"));
        layout->addWidget(buttons);

        m_okButton = buttons->button(QDialogButtonBox::Ok);
        m_okButton->setEnabled(false);

        connect(m_input, &QLineEdit::textChanged,
                this, &GotoAddressDialog::onTextChanged);
        connect(m_input, &QLineEdit::returnPressed, this, [this]() {
            if (m_okButton->isEnabled()) accept();
        });
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_recentList, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* item) {
            if (item) m_input->setText(item->text());
            if (m_okButton->isEnabled()) accept();
        });
        connect(m_recentList, &QListWidget::currentTextChanged,
                this, [this](const QString& text) {
            if (!text.isEmpty()) m_input->setText(text);
        });

        m_input->setFocus();
        onTextChanged(QString());  // initial state
    }

    // After accept(): the formula the user typed (preserved for rebases).
    QString formula() const { return m_input->text().trimmed(); }
    // After accept(): the resolved absolute address. 0 if not yet evaluated.
    uint64_t resolvedAddress() const { return m_resolved; }

    static QStringList loadRecent() {
        QSettings s("Reclass", "Reclass");
        return s.value(kSettingsKey).toStringList();
    }

    static void pushRecent(const QString& entry) {
        QString trimmed = entry.trimmed();
        if (trimmed.isEmpty()) return;
        QSettings s("Reclass", "Reclass");
        QStringList list = s.value(kSettingsKey).toStringList();
        list.removeAll(trimmed);
        list.prepend(trimmed);
        while (list.size() > kMaxRecent) list.removeLast();
        s.setValue(kSettingsKey, list);
    }

    static void clearRecent() {
        QSettings s("Reclass", "Reclass");
        s.remove(kSettingsKey);
    }

protected:
    void accept() override {
        if (m_resolved == 0 && !m_lastOk) return;  // can't go nowhere
        pushRecent(m_input->text().trimmed());
        QDialog::accept();
    }

    void keyPressEvent(QKeyEvent* e) override {
        // Escape always cancels (default), but route up/down from the input
        // into the recent list for keyboard-only navigation.
        if (e->key() == Qt::Key_Down && m_input->hasFocus()
            && m_recentList->count() > 0) {
            m_recentList->setFocus();
            m_recentList->setCurrentRow(0);
            return;
        }
        QDialog::keyPressEvent(e);
    }

private:
    void onTextChanged(const QString& text) {
        QString trimmed = text.trimmed();
        if (trimmed.isEmpty()) {
            m_status->setText(QStringLiteral(" "));
            m_okButton->setEnabled(false);
            m_resolved = 0;
            m_lastOk = false;
            return;
        }
        auto result = AddressParser::evaluate(trimmed, m_ptrSize, &m_cbs);
        if (result.ok) {
            const auto& t = ThemeManager::instance().current();
            m_resolved = result.value;
            m_lastOk = true;
            m_status->setStyleSheet(QStringLiteral("color: %1;")
                .arg(t.indHeatCold.name()));
            m_status->setText(QStringLiteral("→ 0x%1")
                .arg(result.value, 0, 16));
            m_okButton->setEnabled(true);
        } else {
            const auto& t = ThemeManager::instance().current();
            m_resolved = 0;
            m_lastOk = false;
            m_status->setStyleSheet(QStringLiteral("color: %1;")
                .arg(t.markerError.name()));
            m_status->setText(result.error.isEmpty()
                ? QStringLiteral("invalid expression") : result.error);
            m_okButton->setEnabled(false);
        }
    }

    AddressParserCallbacks m_cbs;
    int          m_ptrSize    = 8;
    QLineEdit*   m_input      = nullptr;
    QLabel*      m_status     = nullptr;
    QListWidget* m_recentList = nullptr;
    QPushButton* m_okButton   = nullptr;
    uint64_t     m_resolved   = 0;
    bool         m_lastOk     = false;
};

} // namespace rcx
