#pragma once
#include <QFrame>
#include <QFont>
#include <QVector>
#include <QString>
#include <cstdint>

class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;

namespace rcx {

struct Theme;

// ── Provider icon + kind label helpers (shared between popup and controller) ──

inline QString iconForProvider(const QString& identifier) {
    if (identifier == QStringLiteral("processmemory"))
        return QStringLiteral(":/vsicons/server-process.svg");
    if (identifier == QStringLiteral("remoteprocessmemory"))
        return QStringLiteral(":/vsicons/remote.svg");
    if (identifier == QStringLiteral("windbgmemory"))
        return QStringLiteral(":/vsicons/debug.svg");
    if (identifier == QStringLiteral("reclass.netcompatlayer"))
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

class SourceChooserPopup : public QFrame {
    Q_OBJECT
public:
    explicit SourceChooserPopup(QWidget* parent = nullptr);

    void setFont(const QFont& font);
    void applyTheme(const Theme& theme);
    void setSources(const QVector<SourceEntry>& entries);
    void setLivenessResults(const QVector<bool>& alive);
    void popup(const QPoint& globalPos);
    void warmUp();

signals:
    void sourceSelected(int savedIndex);
    void providerSelected(const QString& identifier);
    void clearRequested();
    void dismissed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel*           m_titleLabel  = nullptr;
    QWidget*          m_escBtn      = nullptr;
    QLineEdit*        m_filterEdit  = nullptr;
    QFrame*           m_separator   = nullptr;
    QListView*        m_listView    = nullptr;
    QStringListModel* m_model       = nullptr;
    QLabel*           m_footerLabel = nullptr;

    QFont m_font;
    QVector<SourceEntry> m_allEntries;
    QVector<SourceEntry> m_filteredEntries;
    QVector<QVector<int>> m_matchPositions;
    int m_cachedMaxNameLen = 0;

    void applyFilter(const QString& text);
    void acceptCurrent();
    void acceptIndex(int row);
    int  nextSelectableRow(int from, int direction) const;
};

} // namespace rcx
