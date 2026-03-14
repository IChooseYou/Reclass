#include "symbol_downloader.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

namespace rcx {

SymbolDownloader::SymbolDownloader(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

QString SymbolDownloader::cacheDir() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base + QStringLiteral("/SymbolCache");
}

QString SymbolDownloader::findCached(const DownloadRequest& req) const {
    // Cache layout: cacheDir/pdbName/GUID+age/pdbName
    QString path = cacheDir() + QStringLiteral("/%1/%2%3/%1")
        .arg(req.pdbName, req.guidString, QString::number(req.age, 16));
    if (QFile::exists(path))
        return path;
    return {};
}

QString SymbolDownloader::findLocal(const QString& moduleFullPath, const QString& pdbName) {
    if (moduleFullPath.isEmpty() || pdbName.isEmpty())
        return {};
    // Check same directory as the module
    QString dir = QFileInfo(moduleFullPath).absolutePath();
    QString candidate = dir + QStringLiteral("/") + pdbName;
    if (QFile::exists(candidate))
        return candidate;
    return {};
}

void SymbolDownloader::download(const DownloadRequest& req) {
    // URL: https://msdl.microsoft.com/download/symbols/{pdbName}/{GUID}{age}/{pdbName}
    QString url = QStringLiteral("https://msdl.microsoft.com/download/symbols/%1/%2%3/%1")
        .arg(req.pdbName, req.guidString, QString::number(req.age, 16));

    QUrl reqUrl(url);
    QNetworkRequest netReq(reqUrl);
    netReq.setHeader(QNetworkRequest::UserAgentHeader,
        QStringLiteral("Microsoft-Symbol-Server/10.0.0.0"));
    netReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    cancel(); // cancel any previous
    m_activeReply = m_nam->get(netReq);

    QString moduleName = req.moduleName;
    QString pdbName = req.pdbName;
    QString guidString = req.guidString;
    uint32_t age = req.age;

    connect(m_activeReply, &QNetworkReply::downloadProgress,
            this, [this, moduleName](qint64 received, qint64 total) {
        emit progress(moduleName, static_cast<int>(received), static_cast<int>(total));
    });

    connect(m_activeReply, &QNetworkReply::finished,
            this, [this, moduleName, pdbName, guidString, age]() {
        auto* reply = m_activeReply;
        m_activeReply = nullptr;

        if (!reply) return;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit finished(moduleName, {}, false,
                QStringLiteral("Download failed: %1").arg(reply->errorString()));
            return;
        }

        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus != 200) {
            emit finished(moduleName, {}, false,
                QStringLiteral("HTTP %1").arg(httpStatus));
            return;
        }

        QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            emit finished(moduleName, {}, false, QStringLiteral("Empty response"));
            return;
        }

        // Save to cache
        QString dir = cacheDir() + QStringLiteral("/%1/%2%3")
            .arg(pdbName, guidString, QString::number(age, 16));
        QDir().mkpath(dir);
        QString path = dir + QStringLiteral("/") + pdbName;

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            emit finished(moduleName, {}, false,
                QStringLiteral("Cannot write: %1").arg(f.errorString()));
            return;
        }
        f.write(data);
        f.close();

        emit finished(moduleName, path, true, {});
    });
}

void SymbolDownloader::cancel() {
    if (m_activeReply) {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }
}

} // namespace rcx
