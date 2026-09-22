#pragma once
#include "themes/theme.h"
#include <QFrame>
#include <QFont>
#include <QVector>
#include <QString>
#include <cstdint>

class QAction;
class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;

namespace rcx {

// ── Provider icon + kind label helpers (shared between popup and controller) ──

inline QString iconForProvider(const QString& identifier) {
    if (identifier == QStringLiteral("processmemory"))
        return QStringLiteral(":/vsicons/server-process.svg");
    if (identifier == QStringLiteral("remoteprocessmemory"))
        return QStringLiteral(":/vsicons/remote.svg");
    if (identifier == QStringLiteral("windbgmemory"))
        return QStringLiteral(":/vsicons/debug.svg");
    if (identifier == QStringLiteral("RC.netcompatlayer"))
        return QStringLiteral(":/vsicons/plug.svg");
    if (identifier == QStringLiteral("kernelmemory"))
        return QStringLiteral(":/vsicons/symbol-key.svg");
    if (identifier == QStringLiteral("File"))
        return QStringLiteral(":/vsicons/file-binary.svg");
    return QStringLiteral(":/vsicons/extensions.svg");
}

inline QString kindLabelFor(const QString& identifier) {
    if (identifier == QStringLiteral("processmemory"))       return QStringLiteral("Process");
    if (identifier == QStringLiteral("remoteprocessmemory")) return QStringLiteral("Remote");
    if (identifier == QStringLiteral("windbgmemory"))        return QStringLiteral("Debug");
    if (identifier == QStringLiteral("kernelmemory"))        return QStringLiteral("Kernel");
    if (identifier == QStringLiteral("File"))                return QStringLiteral("File");
    return QStringLiteral("Plugin");
}

// ── Source entry for the popup list ──

struct SourceEntry {
    enum Kind { SavedSource, ProviderAction, SectionHeader, ClearAction };

    Kind    entryKind   = SavedSource;
    QString displayName;          // "notepad.exe" or "dump.bin"
    QString kindLabel;            // "Process" / "File" / "Remote" / "Debug"
    QString providerIdentifier;   // "processmemory", "File", etc.
    QString providerTarget;       // "1234:notepad.exe"
    QString filePath;
    QString baseAddress;          // formatted hex "0x7FF61234ABCD"
    QString pid;                  // extracted from providerTarget
    QString arch;                 // "x64" / "x86"
    QString iconPath;             // ":/vsicons/server-process.svg"
    QString dllFileName;          // plugin DLL name (provider actions only)
    int     savedIndex   = -1;    // index into controller's m_savedSources (-1 for providers)
    bool    isActive     = false; // currently active source
    bool    isStale      = false; // process exited or file missing
    bool    enabled      = true;  // false = section header / not selectable
};

// ── Popup widget ──
// The address bar's data-source chooser: a title row, a filter field, the
// Connected / Add Source list and a key-hint footer, on ONE surface inside
// ONE device-exact frame (the popup family rule — see paintEvent in the
// .cpp). Everything paints from the Theme applyTheme() was handed, so a
// preview theme or a harness never shows chrome in one theme and rows in
// another.

class SourceChooserPopup : public QFrame {
    Q_OBJECT
public:
    explicit SourceChooserPopup(QWidget* parent = nullptr);

    void setFont(const QFont& font);
    void applyTheme(const Theme& theme);
    // Rebuilds the list; while the popup is up (a per-row x delete) it also
    // re-fits the height to the new rows, top edge anchored.
    void setSources(const QVector<SourceEntry>& entries);
    void setLivenessResults(const QVector<bool>& alive);
    // `globalPos` is the anchor — the bar's bottom edge under the chip — and
    // `anchorTop` the bar's top edge (global y), so a popup that has to flip
    // above the bar ends one device row above it instead of covering the bar
    // and the chip. -1 = no bar: a flipped popup ends on the anchor.
    void popup(const QPoint& globalPos, int anchorTop = -1);
    void warmUp();

    const Theme& theme() const { return m_theme; }

signals:
    void sourceSelected(int savedIndex);
    void providerSelected(const QString& identifier);
    void removeRequested(int savedIndex);
    void clearRequested();
    void dismissed();   // hidden by any route (pick, Esc, outside click); the chip that opened it drops its hover

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel*           m_titleLabel  = nullptr;
    QWidget*          m_escBtn      = nullptr;
    QLineEdit*        m_filterEdit  = nullptr;   // a PopupFilterField (widgets/popup_chrome.h)
    QAction*          m_clearAction = nullptr;   // the field's trailing × — shown only with text
    QListView*        m_listView    = nullptr;   // a SourceListView (sourcechooserpopup.cpp)
    QStringListModel* m_model       = nullptr;
    QLabel*           m_footerLabel = nullptr;

    // The theme applyTheme() received: the palette, the QSS, the frame, the
    // field's seam and every delegate row paint from this one copy.
    Theme m_theme;
    QFont m_font;
    QVector<SourceEntry> m_allEntries;
    QVector<SourceEntry> m_filteredEntries;
    QVector<QVector<int>> m_matchPositions;
    int m_cachedMaxNameLen = 0;
    // Where popup() was asked to open: re-read by the re-fit setSources()
    // runs while the popup is up.
    QPoint m_anchor;
    int    m_anchorTop = -1;

    void applyFilter(const QString& text);
    // Sizes the popup to its rows, capped by the screen; `inPlace` re-fits
    // while up, keeping the edge that sits on the bar (the top edge under
    // it, the bottom edge when flipped above it).
    void fitToScreen(bool inPlace);
    void acceptCurrent();
    void acceptIndex(int row);
    int  nextSelectableRow(int from, int direction) const;
};

} // namespace rcx
