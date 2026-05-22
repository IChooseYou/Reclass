#pragma once

#include "themed_dialog.h"
#include <QLabel>
#include <QPushButton>

class QListWidget;

namespace rcx {

// Drop-in replacement for the QMessageBox calls scattered across the
// app. Use the static helpers — they keep call sites small while
// guaranteeing every modal alert in the app shares one theme, one
// icon set, and one tone.
//
// Wording conventions (apply when writing the title/text args):
//
//   * Window titles are noun phrases describing the *surface*, not
//     the severity. Prefer "Translation Failed" or "Plugin" over a
//     bare "Error".
//   * Body is one full sentence stating what happened, then a second
//     short sentence with the remediation hint if useful. Avoid
//     parenthetical hints — they don't translate cleanly.
//   * Button labels are verbs that complete the title's intent
//     ("Delete", "Discard", "Save changes"), not "OK" or "Yes/No".
//   * Build messages with QString::arg, never with `+`. Translators
//     need the whole sentence to reorder grammar.
//   * The codebase uses QStringLiteral, not tr(). That's fine for
//     today (translation isn't wired up), but keep strings structured
//     as if they *were* tr()-wrapped: positional placeholders (%1,
//     %2), one self-contained sentence per message, no string-built
//     plurals ("project" vs "projects" → write the two sentences in
//     full and pick by count). When tr() does come, it becomes a
//     mechanical search-and-replace.
//
// Native QFileDialog and QColorDialog are deliberately *not* wrapped.
// File pickers should feel like the OS, and the colour picker is too
// specialised to re-skin without functional regressions.
class ThemedMessageBox : public ThemedDialog {
    Q_OBJECT
public:
    enum Severity { Info, Warning, Critical, Question };
    enum class UnsavedChoice { Save, Discard, Cancel };

    // Static helpers — block until the user dismisses.
    static void info(QWidget* parent, const QString& title,
                     const QString& text);
    static void warn(QWidget* parent, const QString& title,
                     const QString& text);
    static void critical(QWidget* parent, const QString& title,
                         const QString& text);

    // Two-button confirmation. acceptLabel must be a verb
    // ("Delete", "Replace", "Reload"); rejectLabel defaults to
    // "Cancel". Returns true iff the user chose accept. Set
    // `destructive = true` for irreversible actions — the accept
    // button picks up the red Destructive variant and the default
    // focus shifts to Cancel (so a stray Enter is safe).
    static bool confirm(QWidget* parent, const QString& title,
                        const QString& text,
                        const QString& acceptLabel,
                        const QString& rejectLabel = QString(),
                        bool destructive = false);

    // Three-button save-or-discard prompt. Used at app exit when one
    // or more documents have unsaved changes. `detail` lists the
    // affected document names (one per line).
    static UnsavedChoice unsavedChanges(QWidget* parent,
                                        const QString& title,
                                        const QString& text,
                                        const QString& detail);

    // Constructor for the rare case where a caller wants to add a
    // custom button or read clickedButton(). Static helpers cover
    // every common case.
    explicit ThemedMessageBox(QWidget* parent, Severity sev,
                              const QString& title, const QString& text);

    void setDetailText(const QString& detail);
    void appendButton(QPushButton* button);
    void setDefault(QPushButton* button);

protected:
    void applyTheme() override;

private:
    Severity     m_severity;
    QLabel*      m_iconLbl;
    QLabel*      m_textLbl;
    QLabel*      m_detailLbl = nullptr;
    QListWidget* m_detailList = nullptr;
    QHBoxLayout* m_buttonRow;
    void redrawIcon();
};

} // namespace rcx
