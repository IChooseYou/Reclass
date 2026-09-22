#pragma once

// ── Spill: a recording's history on disk ──
//
// Rolling history lives in RAM and ages out. A RECORDING keeps everything, so
// while one runs the store writes its sealed chunks — and, past a RAM
// threshold, its page images — to append-only segment files. Nothing here is
// meant to outlive the app (the user chose session-only history): the session
// directory is removed on exit, and a session that crashed is swept by a
// later launch because nobody holds its LOCK any more.
//
//   <root>/s<pid>-<startMsHex>/LOCK
//   <root>/s<pid>-<startMsHex>/c<stream>-<segment>.rtc   sealed chunks
//   <root>/s<pid>-<startMsHex>/b<stream>-<segment>.rtb   page images
//
// Every segment starts with a 16-byte header: "RTLF", u16 version, u16 kind,
// u32 session tag, u32 stream. Entries carry no framing — the store keeps
// each one's location, size and checksum, and verifies what it reads back.
//
// A SpillFile belongs to one store and is used only from that store's strand.

#include <QByteArray>
#include <QFile>
#include <QLockFile>
#include <QString>
#include <QVector>
#include <map>
#include <memory>

namespace rcx::tl {

struct SpillLocation {
    int    segment = -1;
    qint64 offset = 0;
    int    size = 0;
    bool isValid() const { return segment >= 0; }
};

class SpillDir {
public:
    // Creates <root>/s<pid>-<startMsHex>/ and takes its LOCK; nullptr when
    // either fails (the caller keeps everything in RAM).
    static std::shared_ptr<SpillDir> create(const QString& root);
    ~SpillDir();   // unlocks and removes the directory with whatever is left

    QString path() const { return m_path; }
    QString name() const;
    quint32 sessionTag() const { return m_tag; }

    // Sweep the sessions of processes that are gone: a directory whose LOCK
    // can be taken (its owner died), or one with no LOCK at all that is at
    // least `orphanAgeMs` old. Only names shaped like a session are touched,
    // links are never followed, and `keepName` is skipped. Returns the number
    // removed.
    static int removeStaleSessions(const QString& root, qint64 orphanAgeMs,
                                   const QString& keepName = {});

private:
    SpillDir() = default;
    QString m_path;
    quint32 m_tag = 0;
    std::unique_ptr<QLockFile> m_lock;
};

class SpillFile {
public:
    static constexpr qint64 kDefaultSegmentBytes = 64LL << 20;
    static constexpr int    kHeaderBytes = 16;

    // `kind` is 'c' (chunks) or 'b' (images). Writes stop — failed() — when
    // one fails or when the volume would drop below `minFreeBytes`.
    SpillFile(std::shared_ptr<SpillDir> dir, quint32 stream, char kind,
              qint64 segmentBytes = kDefaultSegmentBytes, qint64 minFreeBytes = 0);
    ~SpillFile();   // closes and deletes its segments
    SpillFile(const SpillFile&) = delete;
    SpillFile& operator=(const SpillFile&) = delete;

    // Append. An invalid location means nothing was stored: keep the bytes.
    SpillLocation write(const QByteArray& bytes);
    // Exactly what was written, or false (deleted, short read, I/O error).
    bool read(const SpillLocation& loc, QByteArray& out) const;
    // Nothing references these bytes any more. A finished segment with
    // nothing left in it is deleted.
    void release(const SpillLocation& loc);

    bool    failed() const { return m_failed; }
    qint64  liveBytes() const { return m_liveBytes; }
    int     segmentCount() const { return int(m_segments.size()); }
    QString segmentPath(int segment) const;

    // Test seam: make the next writes fail as a full or vanished disk would.
    void failWritesForTest(bool on) { m_failWritesForTest = on; }

private:
    struct Segment {
        QString path;
        qint64  size = 0;         // bytes in the file, header included
        qint64  live = 0;
        int     liveEntries = 0;
        mutable std::unique_ptr<QFile> reader;
    };
    bool openSegment();
    void finishWriter();
    void deleteSegment(int segment);
    void retryDeletes();
    bool roomFor(qint64 bytes);
    void fail();

    std::shared_ptr<SpillDir> m_dir;
    quint32 m_stream = 0;
    char    m_kind = 'c';
    qint64  m_segmentBytes = kDefaultSegmentBytes;
    qint64  m_minFree = 0;
    std::map<int, Segment> m_segments;
    std::unique_ptr<QFile> m_writer;
    int     m_current = -1;
    int     m_nextSegment = 0;
    qint64  m_liveBytes = 0;
    int     m_writesSinceRoomCheck = 0;
    bool    m_failed = false;
    bool    m_failWritesForTest = false;
    QVector<QString> m_undeleted;   // Windows: a file still open elsewhere
};

} // namespace rcx::tl
