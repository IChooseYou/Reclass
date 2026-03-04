#include "scanner.h"
#include <QtConcurrent>
#include <QMetaObject>
#include <QElapsedTimer>
#include <QDebug>
#include <cstring>
#include <cmath>
#include <algorithm>

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

int valueSizeForType(ValueType type) {
    switch (type) {
    case ValueType::Int8:  case ValueType::UInt8:  return 1;
    case ValueType::Int16: case ValueType::UInt16: return 2;
    case ValueType::Int32: case ValueType::UInt32: case ValueType::Float: return 4;
    case ValueType::Int64: case ValueType::UInt64: case ValueType::Double: return 8;
    case ValueType::Vec2: return 8;
    case ValueType::Vec3: return 12;
    case ValueType::Vec4: return 16;
    default: return 4;
    }
}

// ── Typed comparison for rescan conditions ──

static int compareTyped(const QByteArray& a, const QByteArray& b, ValueType vt) {
    const char* da = a.constData();
    const char* db = b.constData();
    int sz = qMin(a.size(), b.size());

    switch (vt) {
    case ValueType::Int8:
        if (sz >= 1) { int8_t va, vb; memcpy(&va, da, 1); memcpy(&vb, db, 1); return (va > vb) - (va < vb); }
        break;
    case ValueType::UInt8:
        if (sz >= 1) { uint8_t va, vb; memcpy(&va, da, 1); memcpy(&vb, db, 1); return (va > vb) - (va < vb); }
        break;
    case ValueType::Int16:
        if (sz >= 2) { int16_t va, vb; memcpy(&va, da, 2); memcpy(&vb, db, 2); return (va > vb) - (va < vb); }
        break;
    case ValueType::UInt16:
        if (sz >= 2) { uint16_t va, vb; memcpy(&va, da, 2); memcpy(&vb, db, 2); return (va > vb) - (va < vb); }
        break;
    case ValueType::Int32:
        if (sz >= 4) { int32_t va, vb; memcpy(&va, da, 4); memcpy(&vb, db, 4); return (va > vb) - (va < vb); }
        break;
    case ValueType::UInt32:
        if (sz >= 4) { uint32_t va, vb; memcpy(&va, da, 4); memcpy(&vb, db, 4); return (va > vb) - (va < vb); }
        break;
    case ValueType::Int64:
        if (sz >= 8) { int64_t va, vb; memcpy(&va, da, 8); memcpy(&vb, db, 8); return (va > vb) - (va < vb); }
        break;
    case ValueType::UInt64:
        if (sz >= 8) { uint64_t va, vb; memcpy(&va, da, 8); memcpy(&vb, db, 8); return (va > vb) - (va < vb); }
        break;
    case ValueType::Float:
        if (sz >= 4) { float va, vb; memcpy(&va, da, 4); memcpy(&vb, db, 4); return (va > vb) - (va < vb); }
        break;
    case ValueType::Double:
        if (sz >= 8) { double va, vb; memcpy(&va, da, 8); memcpy(&vb, db, 8); return (va > vb) - (va < vb); }
        break;
    default:
        break;
    }
    // Fallback: byte comparison
    return memcmp(da, db, sz);
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

    if (req.condition != ScanCondition::UnknownValue) {
        if (req.pattern.isEmpty()) {
            emit error(QStringLiteral("Empty pattern"));
            return;
        }
        if (req.pattern.size() != req.mask.size()) {
            emit error(QStringLiteral("Pattern and mask size mismatch"));
            return;
        }
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
    QElapsedTimer timer;
    timer.start();

    QVector<ScanResult> results;
    const bool isUnknown = (req.condition == ScanCondition::UnknownValue);

    if (!prov || (!isUnknown && req.pattern.isEmpty()))
        return results;

    auto regions = prov->enumerateRegions();
    qDebug() << "[scan] regions:" << regions.size()
             << " pattern:" << req.pattern.size() << "bytes"
             << " align:" << req.alignment
             << " condition:" << (int)req.condition
             << " filterExec:" << req.filterExecutable
             << " filterWrite:" << req.filterWritable;

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

    const int patternLen = isUnknown ? req.valueSize : req.pattern.size();
    const char* pat = isUnknown ? nullptr : req.pattern.constData();
    const char* msk = isUnknown ? nullptr : req.mask.constData();
    const int alignment = qMax(1, req.alignment);
    const int valSize = isUnknown ? req.valueSize : patternLen;
    const bool hasRange = (req.startAddress != 0 || req.endAddress != 0) &&
                           req.endAddress > req.startAddress;

    // Pre-compute total bytes for progress
    uint64_t totalBytes = 0;
    for (const auto& r : regions) {
        if (req.filterExecutable && !r.executable) continue;
        if (req.filterWritable && !r.writable) continue;
        uint64_t rStart = r.base, rEnd = r.base + r.size;
        if (hasRange) {
            if (rEnd <= req.startAddress || rStart >= req.endAddress) continue;
            rStart = qMax(rStart, req.startAddress);
            rEnd   = qMin(rEnd, req.endAddress);
        }
        totalBytes += rEnd - rStart;
    }

    qDebug() << "[scan] total scannable:" << (totalBytes / 1024) << "KB across filtered regions";

    if (totalBytes == 0) return results;

    uint64_t scannedBytes = 0;
    int lastPct = -1;

    constexpr int kChunk = 256 * 1024;

    for (const auto& region : regions) {
        if (m_abort.load()) break;

        if (req.filterExecutable && !region.executable) continue;
        if (req.filterWritable && !region.writable) continue;

        // Clip region to requested address range
        uint64_t regStart = region.base;
        uint64_t regEnd   = region.base + region.size;
        if (hasRange) {
            if (regEnd <= req.startAddress || regStart >= req.endAddress) {
                // Entirely outside range — skip
                continue;
            }
            regStart = qMax(regStart, req.startAddress);
            regEnd   = qMin(regEnd, req.endAddress);
        }
        uint64_t regSize = regEnd - regStart;

        if ((uint64_t)patternLen > regSize) {
            scannedBytes += regSize;
            continue;
        }

        const int overlap = patternLen - 1;
        QByteArray chunk(qMin((uint64_t)kChunk, regSize), Qt::Uninitialized);
        uint64_t regOffset = regStart - region.base; // offset within provider region

        for (uint64_t off = 0; off < regSize; ) {
            if (m_abort.load()) break;

            uint64_t remaining = regSize - off;
            int readLen = (int)qMin((uint64_t)chunk.size(), remaining);

            if (!prov->read(regStart + off, chunk.data(), readLen)) {
                // Skip unreadable chunk
                off += readLen;
                scannedBytes += readLen;
                continue;
            }

            int scanEnd = readLen - patternLen;
            const char* data = chunk.constData();

            if (isUnknown) {
                // Unknown value: capture every aligned address
                for (int i = 0; i <= scanEnd; i += alignment) {
                    ScanResult r;
                    r.address = regStart + off + (uint64_t)i;
                    r.scanValue = QByteArray(data + i, valSize);
                    results.append(r);

                    if (results.size() >= req.maxResults)
                        goto done;
                }
            } else {
                // Exact pattern match
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
                        r.address = regStart + off + (uint64_t)i;
                        r.regionModule = region.moduleName;
                        r.scanValue = QByteArray(data + i, qMin(16, readLen - i));
                        results.append(r);

                        if (results.size() >= req.maxResults)
                            goto done;
                    }
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
    qDebug() << "[scan] done:" << results.size() << "results in" << timer.elapsed() << "ms"
             << " scanned:" << (scannedBytes / 1024) << "KB";
    return results;
}

void ScanEngine::startRescan(std::shared_ptr<Provider> provider,
                              QVector<ScanResult> results, int readSize,
                              ScanCondition condition, ValueType valueType,
                              const QByteArray& filterPattern,
                              const QByteArray& filterMask) {
    if (isRunning()) return;

    m_abort.store(false);

    auto* watcher = new QFutureWatcher<QVector<ScanResult>>(this);
    m_watcher = watcher;

    connect(watcher, &QFutureWatcher<QVector<ScanResult>>::finished, this, [this, watcher]() {
        auto results = watcher->result();
        watcher->deleteLater();
        if (m_watcher == watcher)
            m_watcher = nullptr;
        emit rescanFinished(results);
    });

    watcher->setFuture(QtConcurrent::run(
        [this, provider, results = std::move(results), readSize,
         condition, valueType, filterPattern, filterMask]() mutable {
            return runRescan(provider, std::move(results), readSize,
                             condition, valueType, filterPattern, filterMask);
        }));
}

QVector<ScanResult> ScanEngine::runRescan(std::shared_ptr<Provider> prov,
                                           QVector<ScanResult> results, int readSize,
                                           ScanCondition condition, ValueType valueType,
                                           const QByteArray& filterPattern,
                                           const QByteArray& filterMask) {
    QElapsedTimer timer;
    timer.start();

    int total = results.size();
    if (total == 0 || !prov) return results;

    bool hasExactFilter = !filterPattern.isEmpty() && condition == ScanCondition::ExactValue;
    bool hasComparison = (condition == ScanCondition::Changed ||
                          condition == ScanCondition::Unchanged ||
                          condition == ScanCondition::Increased ||
                          condition == ScanCondition::Decreased);
    bool needsFilter = hasExactFilter || hasComparison;

    qDebug() << "[rescan] start:" << total << "results, readSize:" << readSize
             << "condition:" << (int)condition
             << "exactFilter:" << (hasExactFilter ? "yes" : "no")
             << "comparison:" << (hasComparison ? "yes" : "no");

    // Save previous values
    for (auto& r : results)
        r.previousValue = r.scanValue;

    // Sort indices by address for sequential chunked reads
    QVector<int> order(total);
    for (int i = 0; i < total; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&results](int a, int b) {
        return results[a].address < results[b].address;
    });

    constexpr int kChunk = 256 * 1024;
    int updated = 0;
    int lastPct = -1;
    int chunks = 0;
    uint64_t totalBytesRead = 0;
    int i = 0;

    // Track which results matched (by original index)
    QVector<bool> matched(total, !needsFilter); // if no filter, all match

    while (i < total && !m_abort.load()) {
        uint64_t spanBase = results[order[i]].address;
        int spanEnd = i;

        // Extend span while next result fits in the same chunk
        while (spanEnd + 1 < total) {
            uint64_t endAddr = results[order[spanEnd + 1]].address + readSize;
            if (endAddr - spanBase > (uint64_t)kChunk) break;
            spanEnd++;
        }

        uint64_t spanLast = results[order[spanEnd]].address;
        int chunkLen = (int)(spanLast + readSize - spanBase);
        QByteArray chunk(chunkLen, '\0');
        prov->read(spanBase, chunk.data(), chunkLen);

        for (int j = i; j <= spanEnd; j++) {
            int idx = order[j];
            auto& r = results[idx];
            int off = (int)(r.address - spanBase);
            r.scanValue = chunk.mid(off, readSize);

            // Apply exact-value filter
            if (hasExactFilter) {
                int patLen = filterPattern.size();
                if (r.scanValue.size() >= patLen) {
                    bool ok = true;
                    const char* data = r.scanValue.constData();
                    const char* pat  = filterPattern.constData();
                    const char* msk  = filterMask.constData();
                    for (int k = 0; k < patLen; k++) {
                        if ((data[k] & msk[k]) != (pat[k] & msk[k])) {
                            ok = false;
                            break;
                        }
                    }
                    matched[idx] = ok;
                }
            }

            // Apply comparison-based filter
            if (hasComparison && !r.previousValue.isEmpty()) {
                int cmp = compareTyped(r.scanValue, r.previousValue, valueType);
                switch (condition) {
                case ScanCondition::Changed:   matched[idx] = (cmp != 0); break;
                case ScanCondition::Unchanged: matched[idx] = (cmp == 0); break;
                case ScanCondition::Increased: matched[idx] = (cmp > 0);  break;
                case ScanCondition::Decreased: matched[idx] = (cmp < 0);  break;
                default: break;
                }
            }
        }

        chunks++;
        totalBytesRead += chunkLen;
        updated += (spanEnd - i + 1);
        i = spanEnd + 1;

        int pct = updated * 100 / total;
        if (pct != lastPct) {
            lastPct = pct;
            QMetaObject::invokeMethod(this, "progress",
                Qt::QueuedConnection, Q_ARG(int, pct));
        }
    }

    // Filter out non-matching results
    if (needsFilter) {
        QVector<ScanResult> filtered;
        filtered.reserve(total);
        for (int k = 0; k < total; k++) {
            if (matched[k])
                filtered.append(std::move(results[k]));
        }
        qDebug() << "[rescan] done:" << filtered.size() << "/" << total
                 << "matched in" << timer.elapsed() << "ms |" << chunks
                 << "chunks," << (totalBytesRead / 1024) << "KB read";
        return filtered;
    }

    qDebug() << "[rescan] done:" << updated << "/" << total << "results in"
             << timer.elapsed() << "ms |" << chunks << "chunks,"
             << (totalBytesRead / 1024) << "KB read";
    return results;
}

} // namespace rcx
