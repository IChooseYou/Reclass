#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <cstdint>

class QNetworkAccessManager;
class QNetworkReply;

namespace rcx {

class SymbolDownloader : public QObject {
    Q_OBJECT
public:
    explicit SymbolDownloader(QObject* parent = nullptr);

    struct DownloadRequest {
        QString moduleName;   // display name (e.g. "ntoskrnl.exe")
        QString pdbName;      // PDB filename (e.g. "ntoskrnl.pdb")
        QString guidString;   // 32 hex chars, no dashes
        uint32_t age = 0;
    };

    // Check if PDB exists in local cache. Returns path or empty.
    QString findCached(const DownloadRequest& req) const;

    // Check if PDB exists next to the module on disk. Returns path or empty.
    static QString findLocal(const QString& moduleFullPath, const QString& pdbName);

    // Start downloading a PDB from MS symbol server.
    // Emits finished() when done (success or failure).
    void download(const DownloadRequest& req);

    // Cancel any in-progress download.
    void cancel();

    // Local symbol cache directory.
    static QString cacheDir();

signals:
    void progress(const QString& moduleName, int bytesReceived, int bytesTotal);
    void finished(const QString& moduleName, const QString& localPath,
                  bool success, const QString& error);

private:
    QNetworkAccessManager* m_nam = nullptr;
    QNetworkReply* m_activeReply = nullptr;
};

} // namespace rcx
