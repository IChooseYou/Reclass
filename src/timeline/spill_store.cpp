#include "spill_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace rcx::tl {

namespace {

constexpr quint16 kSpillVersion = 1;

const QRegularExpression& sessionName() {
    static const QRegularExpression re(QStringLiteral("^s\\d+-[0-9a-f]+$"));
    return re;
}

bool removeTree(const QString& path) {
    QFileInfo fi(path);
    if (fi.isSymLink()) return QFile::remove(path);   // never follow a link
    return QDir(path).removeRecursively();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  SpillDir
// ─────────────────────────────────────────────────────────────────────────

std::shared_ptr<SpillDir> SpillDir::create(const QString& root) {
    if (root.isEmpty() || !QDir().mkpath(root)) return nullptr;
    const QString name = QStringLiteral("s%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(quint64(QDateTime::currentMSecsSinceEpoch()), 0, 16);
    const QString path = QDir(root).filePath(name);
    if (!QDir().mkpath(path)) return nullptr;
    std::shared_ptr<SpillDir> dir(new SpillDir);
    dir->m_path = path;
    dir->m_tag = quint32(qHash(name));
    dir->m_lock = std::make_unique<QLockFile>(QDir(path).filePath(QStringLiteral("LOCK")));
    // Only a dead owner makes a lock stale — never its age: a session can
    // run for days holding the same lock.
    dir->m_lock->setStaleLockTime(0);
    if (!dir->m_lock->tryLock(0)) {
        dir->m_lock.reset();
        removeTree(path);
        return nullptr;
    }
    return dir;
}

SpillDir::~SpillDir() {
    if (m_lock) m_lock->unlock();
    m_lock.reset();
    if (!m_path.isEmpty()) removeTree(m_path);
}

QString SpillDir::name() const {
    return QFileInfo(m_path).fileName();
}

int SpillDir::removeStaleSessions(const QString& root, qint64 orphanAgeMs, const QString& keepName) {
    QDir dir(root);
    if (root.isEmpty() || !dir.exists()) return 0;
    int removed = 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& fi : entries) {
        const QString name = fi.fileName();
        if (name == keepName || fi.isSymLink() || !sessionName().match(name).hasMatch()) continue;
        const QString lockPath = QDir(fi.absoluteFilePath()).filePath(QStringLiteral("LOCK"));
        bool stale = false;
        if (QFileInfo::exists(lockPath)) {
            QLockFile lock(lockPath);
            lock.setStaleLockTime(0);        // stale only if its process is gone
            if (lock.tryLock(0)) {
                lock.unlock();
                stale = true;
            }
        } else {
            stale = fi.lastModified().toUTC().msecsTo(now) >= orphanAgeMs;
        }
        if (stale && removeTree(fi.absoluteFilePath())) ++removed;
    }
    return removed;
}

// ─────────────────────────────────────────────────────────────────────────
//  SpillFile
// ─────────────────────────────────────────────────────────────────────────

SpillFile::SpillFile(std::shared_ptr<SpillDir> dir, quint32 stream, char kind,
                     qint64 segmentBytes, qint64 minFreeBytes)
    : m_dir(std::move(dir))
    , m_stream(stream)
    , m_kind(kind)
    , m_segmentBytes(std::max<qint64>(segmentBytes, kHeaderBytes + 1))
    , m_minFree(std::max<qint64>(minFreeBytes, 0)) {
    if (!m_dir) m_failed = true;
}

SpillFile::~SpillFile() {
    finishWriter();
    for (auto& [id, seg] : m_segments) {
        seg.reader.reset();
        QFile::remove(seg.path);
    }
    m_segments.clear();
    for (const QString& p : std::as_const(m_undeleted)) QFile::remove(p);
}

QString SpillFile::segmentPath(int segment) const {
    auto it = m_segments.find(segment);
    return it == m_segments.end() ? QString() : it->second.path;
}

void SpillFile::fail() {
    m_failed = true;
    finishWriter();
}

void SpillFile::finishWriter() {
    if (!m_writer) return;
    m_writer->close();
    m_writer.reset();
    const int finished = m_current;
    m_current = -1;
    auto it = m_segments.find(finished);
    if (it != m_segments.end() && it->second.liveEntries == 0) deleteSegment(finished);
}

bool SpillFile::roomFor(qint64 bytes) {
    if (m_minFree <= 0) return true;
    // Asking the volume costs a syscall; a 64 MiB segment is the slack.
    if (m_writer && m_writesSinceRoomCheck++ < 32) return true;
    m_writesSinceRoomCheck = 0;
    QStorageInfo volume(m_dir->path());
    volume.refresh();
    return !volume.isValid() || volume.bytesAvailable() - bytes >= m_minFree;
}

bool SpillFile::openSegment() {
    finishWriter();
    const int id = m_nextSegment++;
    const QString path = QDir(m_dir->path()).filePath(
        QStringLiteral("%1%2-%3.rt%1").arg(QChar(m_kind)).arg(m_stream).arg(id));
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Unbuffered)) return false;
    char header[kHeaderBytes];
    std::memcpy(header, "RTLF", 4);
    qToLittleEndian<quint16>(kSpillVersion, header + 4);
    qToLittleEndian<quint16>(quint16(m_kind), header + 6);
    qToLittleEndian<quint32>(m_dir->sessionTag(), header + 8);
    qToLittleEndian<quint32>(m_stream, header + 12);
    if (file->write(header, kHeaderBytes) != kHeaderBytes) {
        file->close();
        QFile::remove(path);
        return false;
    }
    Segment seg;
    seg.path = path;
    seg.size = kHeaderBytes;
    m_segments.emplace(id, std::move(seg));
    m_writer = std::move(file);
    m_current = id;
    return true;
}

SpillLocation SpillFile::write(const QByteArray& bytes) {
    if (m_failed || bytes.isEmpty()) return {};
    if (m_failWritesForTest) { fail(); return {}; }
    retryDeletes();
    const bool full = m_writer && m_segments[m_current].size > kHeaderBytes
                   && m_segments[m_current].size + bytes.size() > m_segmentBytes;
    if (!m_writer || full) {
        if (!roomFor(qMax<qint64>(bytes.size(), m_segmentBytes)) || !openSegment()) { fail(); return {}; }
    } else if (!roomFor(bytes.size())) {
        fail();
        return {};
    }
    Segment& seg = m_segments[m_current];
    if (m_writer->write(bytes) != bytes.size()) {
        // The entry may be half written; nothing points at it, the segment
        // is finished and deleted with its last live entry.
        fail();
        return {};
    }
    SpillLocation loc;
    loc.segment = m_current;
    loc.offset = seg.size;
    loc.size = bytes.size();
    seg.size += bytes.size();
    seg.live += bytes.size();
    ++seg.liveEntries;
    m_liveBytes += bytes.size();
    return loc;
}

bool SpillFile::read(const SpillLocation& loc, QByteArray& out) const {
    auto it = m_segments.find(loc.segment);
    if (!loc.isValid() || it == m_segments.end() || loc.size <= 0) return false;
    const Segment& seg = it->second;
    if (loc.offset < kHeaderBytes || loc.offset + loc.size > seg.size) return false;
    if (!seg.reader) {
        seg.reader = std::make_unique<QFile>(seg.path);
        if (!seg.reader->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            seg.reader.reset();
            return false;
        }
    }
    if (!seg.reader->seek(loc.offset)) return false;
    out = seg.reader->read(loc.size);
    return out.size() == loc.size;
}

void SpillFile::release(const SpillLocation& loc) {
    auto it = m_segments.find(loc.segment);
    if (!loc.isValid() || it == m_segments.end()) return;
    Segment& seg = it->second;
    seg.live -= loc.size;
    --seg.liveEntries;
    m_liveBytes -= loc.size;
    if (seg.liveEntries <= 0 && loc.segment != m_current) deleteSegment(loc.segment);
}

void SpillFile::deleteSegment(int segment) {
    auto it = m_segments.find(segment);
    if (it == m_segments.end()) return;
    it->second.reader.reset();   // Windows cannot delete an open file
    const QString path = it->second.path;
    m_segments.erase(it);
    if (!QFile::remove(path) && QFileInfo::exists(path)) m_undeleted.append(path);
}

void SpillFile::retryDeletes() {
    for (int i = m_undeleted.size() - 1; i >= 0; --i)
        if (QFile::remove(m_undeleted[i]) || !QFileInfo::exists(m_undeleted[i]))
            m_undeleted.removeAt(i);
}

} // namespace rcx::tl
