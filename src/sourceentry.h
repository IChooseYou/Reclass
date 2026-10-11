#pragma once
#include <QString>
#include <QStringList>

namespace rcx {

// Presentation stays separate from the provider's routing/persistence ID.
inline QString iconForProvider(const QString& id) {
    if (id == QStringLiteral("processmemory")) return QStringLiteral(":/vsicons/server-process.svg");
    if (id == QStringLiteral("remoteprocessmemory")) return QStringLiteral(":/vsicons/remote.svg");
    if (id == QStringLiteral("windbgmemory")) return QStringLiteral(":/vsicons/debug.svg");
    if (id == QStringLiteral("reclass.netcompatlayer")) return QStringLiteral(":/vsicons/plug.svg");
    if (id == QStringLiteral("kernelmemory")) return QStringLiteral(":/vsicons/shield.svg");
    if (id == QStringLiteral("File")) return QStringLiteral(":/vsicons/file-binary.svg");
    return QStringLiteral(":/vsicons/extensions.svg");
}

inline QString providerLabel(const QString& id, const QString& fallback = {}) {
    if (id == QStringLiteral("processmemory")) return QStringLiteral("Process");
    if (id == QStringLiteral("remoteprocessmemory")) return QStringLiteral("Process via IPC");
    if (id == QStringLiteral("windbgmemory")) return QStringLiteral("WinDbg");
    if (id == QStringLiteral("reclass.netcompatlayer")) return QStringLiteral("ReClass.NET");
    if (id == QStringLiteral("kernelmemory")) return QStringLiteral("Kernel");
    if (id == QStringLiteral("File")) return QStringLiteral("File");
    return fallback.isEmpty() ? id : fallback;
}

inline QString kindLabelFor(const QString& id) {
    return providerLabel(id, QStringLiteral("Plugin"));
}

inline QString providerDescription(const QString& id, const QString& fallback = {}) {
    if (id == QStringLiteral("processmemory")) return QStringLiteral("Read/write a local process directly.");
    if (id == QStringLiteral("remoteprocessmemory"))
        return QStringLiteral("Read/write through an injected payload using shared memory.");
    if (id == QStringLiteral("kernelmemory")) return QStringLiteral("Access process memory through the kernel driver.");
    if (id == QStringLiteral("File")) return QStringLiteral("Open a binary file as a memory source.");
    if (id == QStringLiteral("windbgmemory")) return QStringLiteral("Connect through the WinDbg debugger engine.");
    if (id == QStringLiteral("reclass.netcompatlayer")) return QStringLiteral("Use a ReClass.NET memory provider plugin.");
    return fallback;
}

inline int providerOrder(const QString& id) {
    const QStringList order = {QStringLiteral("processmemory"), QStringLiteral("File"),
        QStringLiteral("kernelmemory"), QStringLiteral("remoteprocessmemory"),
        QStringLiteral("windbgmemory"), QStringLiteral("reclass.netcompatlayer")};
    const int index = order.indexOf(id);
    return index < 0 ? order.size() : index;
}

inline QString sourcePid(const QString& id, const QString& target) {
    QString pid;
    if (id == QStringLiteral("processmemory")) pid = target.section(':', 0, 0);
    else if (id == QStringLiteral("remoteprocessmemory") || id == QStringLiteral("kernelmemory"))
        pid = target.section(':', 1, 1);
    else if (id == QStringLiteral("reclass.netcompatlayer"))
        pid = target.section('|', -1).section(':', 0, 0);
    bool ok = false;
    pid.toUInt(&ok);
    return ok ? pid : QString();
}

struct SourceEntry {
    enum Kind { SavedSource, ProviderAction, SectionHeader };
    Kind entryKind = SavedSource;
    QString displayName;
    QString kindLabel;
    QString providerIdentifier;
    QString providerTarget;
    QString filePath;
    QString baseAddress;
    QString pid;
    QString arch;
    QString iconPath;
    QString dllFileName;
    QString description;
    QString searchAliases;
    QString unavailableReason;
    int savedIndex = -1;
    bool isActive = false;
    bool isStale = false;
    bool enabled = true;

    // Independent of vector indices, which shift after removing an entry.
    QString identity() const {
        if (entryKind == ProviderAction) return QStringLiteral("provider:") + providerIdentifier;
        return QStringLiteral("saved:") + providerIdentifier + QChar(0x1f)
            + (filePath.isEmpty() ? providerTarget : filePath) + QChar(0x1f) + displayName;
    }
    QString statusText() const {
        if (!unavailableReason.isEmpty()) return unavailableReason;
        if (!isStale) return {};
        return providerIdentifier == QStringLiteral("File") ? QStringLiteral("Missing file")
             : !pid.isEmpty() ? QStringLiteral("Exited") : QStringLiteral("Unavailable");
    }
    QString details() const {
        QStringList lines = {displayName};
        if (isActive) lines << QStringLiteral("Current source");
        if (!statusText().isEmpty()) lines << statusText();
        if (!description.isEmpty()) lines << description;
        if (!pid.isEmpty()) lines << QStringLiteral("PID ") + pid;
        if (!arch.isEmpty()) lines << arch;
        if (!filePath.isEmpty()) lines << filePath;
        else if (!providerTarget.isEmpty()) lines << providerTarget;
        if (!baseAddress.isEmpty()) lines << QStringLiteral("Base: ") + baseAddress;
        if (!dllFileName.isEmpty()) lines << dllFileName;
        return lines.join('\n');
    }
};

} // namespace rcx
