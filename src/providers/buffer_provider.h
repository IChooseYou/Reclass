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

    const QByteArray& data() const { return m_data; }
    QByteArray& data() { return m_data; }
};

} // namespace rcx
