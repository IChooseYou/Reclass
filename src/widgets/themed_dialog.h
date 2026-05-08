#pragma once

#include <QDialog>
#include <QHBoxLayout>
#include <QPushButton>
#include <initializer_list>
#include "themes/thememanager.h"

namespace rcx {

// Base class for any custom dialog in the app. Two responsibilities:
//
//   1. Apply theme palette (background, text, base) on construction
//      and on every theme change. Subclasses get a dark-on-dark canvas
//      for free instead of inheriting Qt's default Fusion light grey.
//   2. Provide makeButtonRow() — a right-aligned QHBoxLayout that
//      caller-supplied DialogButtons drop into. Replaces every
//      QDialogButtonBox in the codebase so OK/Cancel buttons share the
//      same look as buttons in the rest of the app.
//
// Subclasses that style their own child widgets should override
// applyTheme() and call the base implementation first.
//
// Note: deliberately keeps the standard Qt window frame (Qt::Dialog).
// Going frameless would let us paint our own title bar, but it adds
// drag/move/resize bookkeeping for marginal polish gain. The big win
// here is themed *content* — the OS title bar is a known, accepted
// inconsistency.
class ThemedDialog : public QDialog {
    Q_OBJECT
public:
    explicit ThemedDialog(QWidget* parent = nullptr);

    // Build a right-aligned button row from the supplied buttons.
    // Buttons are added left-to-right after a stretch, so caller
    // order = visual order. Conventional ordering: { Cancel, OK }
    // on Windows / Linux (we follow Windows native habit since the
    // app's primary target is Windows).
    static QHBoxLayout* makeButtonRow(std::initializer_list<QPushButton*> buttons);

protected:
    virtual void applyTheme();
};

} // namespace rcx
