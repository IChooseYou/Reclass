#pragma once
#include "provider.h"
#include <QFile>
#include <QFileInfo>

namespace rcx {

class BufferProvider : public Provider {
    QByteArray m_data;
    QString    m_name;

public:
    explicit BufferProvider(QByteArray data, const QString& name = {})
        : m_data(std::move(data))
        , m_name(name) {}

    static BufferProvider fromFile(const QString& path) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            return BufferProvider(f.readAll(), QFileInfo(path).fileName());
        return BufferProvider({});
    }

    int size() const override { return m_data.size(); }

    bool read(uint64_t addr, void* buf, int len) const override {
        if (!isReadable(addr, len)) return false;
        std::memcpy(buf, m_data.constData() + addr, len);
        return true;
    }

    bool isWritable() const override { return true; }

    bool write(uint64_t addr, const void* buf, int len) override {
        if (!isReadable(addr, len)) return false;
        std::memcpy(m_data.data() + addr, buf, len);
        return true;
    }

    QString name() const override { return m_name; }
    QString kind() const override { return QStringLiteral("File"); }

    // Expose the buffer as a single synthetic region named after the
    // file (or "[buffer]" for in-memory data with no name) so the
    // scanner's Module column shows e.g. "issue.png+0x6A0A" instead of
    // an empty cell. moduleName is the source-of-truth for the formatter
    // in scanner.cpp::formatRegionContext.
    QVector<MemoryRegion> enumerateRegions() const override {
        if (m_data.isEmpty()) return {};
        MemoryRegion r;
        r.base       = 0;
        r.size       = (uint64_t)m_data.size();
        r.readable   = true;
        r.writable   = true;
        r.executable = false;
        r.moduleName = m_name.isEmpty() ? QStringLiteral("[buffer]") : m_name;
        r.type       = RegionType::Mapped;
        return { r };
    }

    const QByteArray& data() const { return m_data; }
    QByteArray& data() { return m_data; }
};

} // namespace rcx
