#include "scanner.h"
#include <QtConcurrent>
#include <QMetaObject>
#include <cstring>
#include <cmath>

namespace rcx {

// ── Pattern parsing ──

static int hexVal(QChar c) {
    ushort u = c.unicode();
    if (u >= '0' && u <= '9') return u - '0';
    if (u >= 'a' && u <= 'f') return u - 'a' + 10;
    if (u >= 'A' && u <= 'F') return u - 'A' + 10;
    return -1;
}

bool parseSignature(const QString& input, QByteArray& pattern, QByteArray& mask,
                    QString* errorMsg)
{
    pattern.clear();
    mask.clear();

    QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("Empty pattern");
        return false;
    }

    // Check for C-style: \xAB\xCD
    if (trimmed.startsWith(QStringLiteral("\\x"))) {
        QStringList parts = trimmed.split(QStringLiteral("\\x"), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            if (part.size() != 2) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid C-style byte: \\x%1").arg(part);
                return false;
            }
            int hi = hexVal(part[0]);
            int lo = hexVal(part[1]);
            if (hi < 0 || lo < 0) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid hex char in: \\x%1").arg(part);
                return false;
            }
            pattern.append(char((hi << 4) | lo));
            mask.append(char(0xFF));
        }
        return !pattern.isEmpty();
    }

    // Space-separated or packed hex
    bool hasSpaces = trimmed.contains(' ');

    if (hasSpaces) {
        QStringList tokens = trimmed.split(' ', Qt::SkipEmptyParts);
        for (const QString& tok : tokens) {
            if (tok == QStringLiteral("??") || tok == QStringLiteral("?")) {
                pattern.append(char(0));
                mask.append(char(0));
            } else if (tok.size() == 2) {
                int hi = hexVal(tok[0]);
                int lo = hexVal(tok[1]);
                if (hi < 0 || lo < 0) {
                    if (errorMsg) *errorMsg = QStringLiteral("Invalid hex byte: %1").arg(tok);
                    return false;
                }
                pattern.append(char((hi << 4) | lo));
                mask.append(char(0xFF));
            } else {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid token: %1 (expected 2 hex chars or wildcards)").arg(tok);
                return false;
            }
        }
    } else {
        // Packed: "488B??05"
        if (trimmed.size() % 2 != 0) {
            if (errorMsg) *errorMsg = QStringLiteral("Odd number of characters in packed pattern");
            return false;
        }
        for (int i = 0; i < trimmed.size(); i += 2) {
            QChar c0 = trimmed[i], c1 = trimmed[i + 1];
            if ((c0 == '?' && c1 == '?')) {
                pattern.append(char(0));
                mask.append(char(0));
            } else {
                int hi = hexVal(c0);
                int lo = hexVal(c1);
                if (hi < 0 || lo < 0) {
                    if (errorMsg) *errorMsg = QStringLiteral("Invalid hex chars at position %1: %2%3")
                        .arg(i).arg(c0).arg(c1);
                    return false;
                }
                pattern.append(char((hi << 4) | lo));
                mask.append(char(0xFF));
            }
        }
    }

    if (pattern.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("Empty pattern after parsing");
        return false;
    }

    return true;
}

// ── Value serialization ──

template<typename T>
static void appendLE(QByteArray& out, T val) {
    out.append(reinterpret_cast<const char*>(&val), sizeof(T));
}

bool serializeValue(ValueType type, const QString& input,
                    QByteArray& pattern, QByteArray& mask,
                    QString* errorMsg)
{
    pattern.clear();
    mask.clear();

    QString trimmed = input.trimmed();
    if (trimmed.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("Empty value");
        return false;
    }

    bool ok = false;

    switch (type) {
    case ValueType::Int8: {
        int v = trimmed.toInt(&ok);
        if (!ok || v < -128 || v > 127) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid int8 value");
            return false;
        }
        appendLE<int8_t>(pattern, (int8_t)v);
        break;
    }
    case ValueType::Int16: {
        int v = trimmed.toInt(&ok);
        if (!ok || v < -32768 || v > 32767) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid int16 value");
            return false;
        }
        appendLE<int16_t>(pattern, (int16_t)v);
        break;
    }
    case ValueType::Int32: {
        int v = trimmed.toInt(&ok);
        if (!ok) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid int32 value");
            return false;
        }
        appendLE<int32_t>(pattern, (int32_t)v);
        break;
    }
    case ValueType::Int64: {
        qlonglong v = trimmed.toLongLong(&ok);
        if (!ok) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid int64 value");
            return false;
        }
        appendLE<int64_t>(pattern, (int64_t)v);
        break;
    }
    case ValueType::UInt8: {
        uint v = trimmed.toUInt(&ok);
        if (!ok || v > 255) {
            // Try hex
            if (trimmed.startsWith("0x", Qt::CaseInsensitive))
                v = trimmed.toUInt(&ok, 16);
            if (!ok || v > 255) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid uint8 value");
                return false;
            }
        }
        appendLE<uint8_t>(pattern, (uint8_t)v);
        break;
    }
    case ValueType::UInt16: {
        uint v = trimmed.toUInt(&ok);
        if (!ok || v > 65535) {
            if (trimmed.startsWith("0x", Qt::CaseInsensitive))
                v = trimmed.toUInt(&ok, 16);
            if (!ok || v > 65535) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid uint16 value");
                return false;
            }
        }
        appendLE<uint16_t>(pattern, (uint16_t)v);
        break;
    }
    case ValueType::UInt32: {
        quint32 v = trimmed.toULong(&ok);
        if (!ok) {
            if (trimmed.startsWith("0x", Qt::CaseInsensitive))
                v = trimmed.toULong(&ok, 16);
            if (!ok) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid uint32 value");
                return false;
            }
        }
        appendLE<uint32_t>(pattern, v);
        break;
    }
    case ValueType::UInt64: {
        quint64 v = trimmed.toULongLong(&ok);
        if (!ok) {
            if (trimmed.startsWith("0x", Qt::CaseInsensitive))
                v = trimmed.toULongLong(&ok, 16);
            if (!ok) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid uint64 value");
                return false;
            }
        }
        appendLE<uint64_t>(pattern, v);
        break;
    }
    case ValueType::Float: {
        float v = trimmed.toFloat(&ok);
        if (!ok) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid float value");
            return false;
        }
        appendLE<float>(pattern, v);
        break;
    }
    case ValueType::Double: {
        double v = trimmed.toDouble(&ok);
        if (!ok) {
            if (errorMsg) *errorMsg = QStringLiteral("Invalid double value");
            return false;
        }
        appendLE<double>(pattern, v);
        break;
    }
    case ValueType::Vec2: {
        QStringList parts = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            if (errorMsg) *errorMsg = QStringLiteral("Vec2 requires 2 space-separated floats");
            return false;
        }
        for (const QString& p : parts) {
            float v = p.toFloat(&ok);
            if (!ok) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid float in vec2: %1").arg(p);
                return false;
            }
            appendLE<float>(pattern, v);
        }
        break;
    }
    case ValueType::Vec3: {
        QStringList parts = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() != 3) {
            if (errorMsg) *errorMsg = QStringLiteral("Vec3 requires 3 space-separated floats");
            return false;
        }
        for (const QString& p : parts) {
            float v = p.toFloat(&ok);
            if (!ok) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid float in vec3: %1").arg(p);
                return false;
            }
            appendLE<float>(pattern, v);
        }
        break;
    }
    case ValueType::Vec4: {
        QStringList parts = trimmed.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() != 4) {
            if (errorMsg) *errorMsg = QStringLiteral("Vec4 requires 4 space-separated floats");
            return false;
        }
        for (const QString& p : parts) {
            float v = p.toFloat(&ok);
            if (!ok) {
                if (errorMsg) *errorMsg = QStringLiteral("Invalid float in vec4: %1").arg(p);
                return false;
            }
            appendLE<float>(pattern, v);
        }
        break;
    }
    case ValueType::UTF8: {
        QByteArray encoded = trimmed.toUtf8();
        if (encoded.isEmpty()) {
            if (errorMsg) *errorMsg = QStringLiteral("Empty UTF-8 string");
            return false;
        }
        pattern = encoded;
        break;
    }
    case ValueType::UTF16: {
        // UTF-16LE encoding
        for (int i = 0; i < trimmed.size(); i++) {
            ushort u = trimmed[i].unicode();
            appendLE<uint16_t>(pattern, u);
        }
        if (pattern.isEmpty()) {
            if (errorMsg) *errorMsg = QStringLiteral("Empty UTF-16 string");
            return false;
        }
        break;
    }
    case ValueType::HexBytes: {
        // Parse hex bytes (like signature but no wildcards)
        QByteArray dummyMask;
        if (!parseSignature(trimmed, pattern, dummyMask, errorMsg))
            return false;
        // HexBytes = exact match, no wildcards
        break;
    }
    }

    // Set mask to all 0xFF (exact match) for value scans
    mask.fill(char(0xFF), pattern.size());
    return true;
}

int naturalAlignment(ValueType type) {
    switch (type) {
    case ValueType::Int8:
    case ValueType::UInt8:
    case ValueType::UTF8:
    case ValueType::HexBytes:
        return 1;
    case ValueType::Int16:
    case ValueType::UInt16:
    case ValueType::UTF16:
        return 2;
    case ValueType::Int32:
    case ValueType::UInt32:
    case ValueType::Float:
    case ValueType::Vec2:
    case ValueType::Vec3:
    case ValueType::Vec4:
        return 4;
    case ValueType::Int64:
    case ValueType::UInt64:
    case ValueType::Double:
        return 8;
    }
    return 1;
}

// ── Scan engine ──

ScanEngine::ScanEngine(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<ScanResult>>("QVector<rcx::ScanResult>");
}

bool ScanEngine::isRunning() const {
    return m_watcher && m_watcher->isRunning();
}

void ScanEngine::abort() {
    m_abort.store(true);
}

void ScanEngine::start(std::shared_ptr<Provider> provider, const ScanRequest& req) {
    if (isRunning()) return;

    if (req.pattern.isEmpty()) {
        emit error(QStringLiteral("Empty pattern"));
        return;
    }
    if (req.pattern.size() != req.mask.size()) {
        emit error(QStringLiteral("Pattern and mask size mismatch"));
        return;
    }

    m_abort.store(false);

    auto* watcher = new QFutureWatcher<QVector<ScanResult>>(this);
    m_watcher = watcher;

    connect(watcher, &QFutureWatcher<QVector<ScanResult>>::finished, this, [this, watcher]() {
        auto results = watcher->result();
        watcher->deleteLater();
        if (m_watcher == watcher)
            m_watcher = nullptr;
        emit finished(results);
    });

    watcher->setFuture(QtConcurrent::run([this, provider, req]() {
        return runScan(provider, req);
    }));
}

QVector<ScanResult> ScanEngine::runScan(std::shared_ptr<Provider> prov,
                                         const ScanRequest& req)
{
    QVector<ScanResult> results;

    if (!prov || req.pattern.isEmpty())
        return results;

    auto regions = prov->enumerateRegions();

    // Fallback for providers that don't enumerate regions (file/buffer)
    if (regions.isEmpty()) {
        MemoryRegion fallback;
        fallback.base = 0;
        fallback.size = (uint64_t)prov->size();
        fallback.readable = true;
        fallback.writable = true;
        fallback.executable = false;
        regions.append(fallback);
    }

    const int patternLen = req.pattern.size();
    const char* pat = req.pattern.constData();
    const char* msk = req.mask.constData();
    const int alignment = qMax(1, req.alignment);

    // Pre-compute total bytes for progress
    uint64_t totalBytes = 0;
    for (const auto& r : regions) {
        if (req.filterExecutable && !r.executable) continue;
        if (req.filterWritable && !r.writable) continue;
        totalBytes += r.size;
    }

    if (totalBytes == 0) return results;

    uint64_t scannedBytes = 0;
    int lastPct = -1;

    constexpr int kChunk = 256 * 1024;

    for (const auto& region : regions) {
        if (m_abort.load()) break;

        if (req.filterExecutable && !region.executable) continue;
        if (req.filterWritable && !region.writable) continue;

        if ((uint64_t)patternLen > region.size) {
            scannedBytes += region.size;
            continue;
        }

        const int overlap = patternLen - 1;
        QByteArray chunk(qMin((uint64_t)kChunk, region.size), Qt::Uninitialized);

        for (uint64_t off = 0; off < region.size; ) {
            if (m_abort.load()) break;

            uint64_t remaining = region.size - off;
            int readLen = (int)qMin((uint64_t)chunk.size(), remaining);

            if (!prov->read(region.base + off, chunk.data(), readLen)) {
                // Skip unreadable chunk
                off += readLen;
                scannedBytes += readLen;
                continue;
            }

            int scanEnd = readLen - patternLen;
            const char* data = chunk.constData();

            for (int i = 0; i <= scanEnd; i += alignment) {
                bool match = true;
                for (int j = 0; j < patternLen; j++) {
                    if ((data[i + j] & msk[j]) != (pat[j] & msk[j])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    ScanResult r;
                    r.address = region.base + off + (uint64_t)i;
                    r.regionModule = region.moduleName;
                    results.append(r);

                    if (results.size() >= req.maxResults)
                        goto done;
                }
            }

            // Advance with overlap to catch patterns that straddle chunks
            uint64_t advance;
            if (readLen > overlap)
                advance = (uint64_t)(readLen - overlap);
            else
                advance = 1; // prevent infinite loop on tiny regions
            scannedBytes += advance;
            off += advance;

            // Throttled progress
            int pct = (int)(scannedBytes * 100 / totalBytes);
            if (pct > 100) pct = 100;
            if (pct != lastPct) {
                lastPct = pct;
                QMetaObject::invokeMethod(this, "progress",
                    Qt::QueuedConnection, Q_ARG(int, pct));
            }
        }
    }

done:
    return results;
}

} // namespace rcx
