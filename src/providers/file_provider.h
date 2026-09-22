#pragma once
#include "provider.h"
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <climits>
#include <memory>

namespace rcx {

// Flat-file offsets, not dump virtual addresses. Keep the original read-only;
// edits use sparse copy-on-write pages, matching BufferProvider's session edits.
class FileProvider final : public Provider {
public:
    static std::shared_ptr<FileProvider> open(const QString& path, QString* error = nullptr) {
        if (!QFileInfo(path).isFile()) {
            if (error) *error = QStringLiteral("Not a regular file");
            return {};
        }
        auto provider = std::shared_ptr<FileProvider>(new FileProvider(path));
        if (!provider->m_file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            if (error) *error = provider->m_file.errorString();
            return {};
        }
        const qint64 length = provider->m_file.size();
        if (length < 0) {
            if (error) *error = provider->m_file.errorString();
            return {};
        }
        provider->m_size = static_cast<uint64_t>(length);
        return provider;
    }

    // Preserve the plugin ABI's legacy int size; all address/range operations
    // below use the full 64-bit extent, including scanner regions.
    int size() const override { return static_cast<int>(qMin(m_size, uint64_t(INT_MAX))); }
    uint64_t byteSize() const { return m_size; }
    QString name() const override { return m_name; }
    QString kind() const override { return QStringLiteral("File"); }
    bool isWritable() const override { return true; }

    bool isReadable(uint64_t addr, int len) const override {
        return len >= 0 && addr <= m_size && uint64_t(len) <= m_size - addr;
    }

    bool read(uint64_t addr, void* buf, int len) const override {
        if (!isReadable(addr, len) || (!buf && len)) return false;
        if (!len) return true;
        QMutexLocker lock(&m_mutex);
        if (!readFile(addr, static_cast<char*>(buf), len)) return false;
        const uint64_t end = addr + uint64_t(len);
        for (auto it = m_edits.lowerBound(addr & ~(kPageSize - 1));
             it != m_edits.end() && it.key() < end; ++it) {
            const uint64_t start = qMax(addr, it.key());
            const uint64_t stop = qMin(end, it.key() + uint64_t(it->size()));
            if (stop > start)
                std::memcpy(static_cast<char*>(buf) + (start - addr),
                            it->constData() + (start - it.key()), size_t(stop - start));
        }
        return true;
    }

    bool write(uint64_t addr, const void* buf, int len) override {
        if (!isReadable(addr, len) || (!buf && len)) return false;
        if (!len) return true;
        QMutexLocker lock(&m_mutex);
        QMap<uint64_t, QByteArray> pending;
        const uint64_t end = addr + uint64_t(len);
        for (uint64_t page = addr & ~(kPageSize - 1); page < end; page += kPageSize) {
            QByteArray bytes = m_edits.value(page);
            if (bytes.isEmpty()) {
                bytes.resize(static_cast<int>(qMin(kPageSize, m_size - page)));
                if (!readFile(page, bytes.data(), bytes.size())) return false;
            }
            const uint64_t start = qMax(addr, page);
            const uint64_t stop = qMin(end, page + uint64_t(bytes.size()));
            std::memcpy(bytes.data() + (start - page),
                        static_cast<const char*>(buf) + (start - addr), size_t(stop - start));
            pending.insert(page, std::move(bytes));
        }
        for (auto it = pending.begin(); it != pending.end(); ++it)
            m_edits.insert(it.key(), std::move(it.value()));
        return true;
    }

    QVector<MemoryRegion> enumerateRegions() const override {
        if (!m_size) return {};
        return {{0, m_size, true, true, false, m_name, RegionType::Mapped}};
    }

    int cachedBytes() const {
        QMutexLocker lock(&m_mutex);
        return m_cache.size();
    }

private:
    explicit FileProvider(const QString& path) : m_file(path), m_name(QFileInfo(path).fileName()) {}
    static constexpr uint64_t kPageSize = 4096;
    static constexpr uint64_t kCacheSize = 64 * 1024;
    mutable QFile m_file;
    QString m_name;
    uint64_t m_size = 0;
    mutable QMutex m_mutex;
    mutable QByteArray m_cache;
    mutable uint64_t m_cacheStart = 0;
    QMap<uint64_t, QByteArray> m_edits;

    // Caller holds the mutex: QFile's seek position and this cache are shared
    // by editor panes and background scans. Large scan reads bypass the cache.
    bool readFile(uint64_t addr, char* buf, int len) const {
        if (len >= int(kCacheSize)) {
            if (!m_file.seek(qint64(addr))) return false;
            return m_file.read(buf, len) == len;
        }
        while (len > 0) {
            if (m_cache.isEmpty() || addr < m_cacheStart
                || addr - m_cacheStart >= uint64_t(m_cache.size())) {
                m_cacheStart = addr & ~(kCacheSize - 1);
                m_cache.clear();
                if (!m_file.seek(qint64(m_cacheStart))) return false;
                m_cache = m_file.read(qint64(qMin(kCacheSize, m_size - m_cacheStart)));
                if (addr - m_cacheStart >= uint64_t(m_cache.size())) return false;
            }
            const int offset = int(addr - m_cacheStart);
            const int count = qMin(len, int(m_cache.size()) - offset);
            std::memcpy(buf, m_cache.constData() + offset, size_t(count));
            buf += count;
            addr += uint64_t(count);
            len -= count;
        }
        return true;
    }
};

} // namespace rcx
