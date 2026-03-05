#pragma once
#include "providers/provider.h"
#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVector>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

namespace rcx {

// ── Value scan types ──

enum class ValueType {
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float, Double,
    Vec2, Vec3, Vec4,
    UTF8, UTF16,
    HexBytes
};

// ── Scan condition (Cheat Engine-style) ──

enum class ScanCondition {
    ExactValue,      // first scan + rescan: match specific bytes
    UnknownValue,    // first scan only: capture all aligned addresses
    Changed,         // rescan: current != previous
    Unchanged,       // rescan: current == previous
    Increased,       // rescan: current > previous (numeric)
    Decreased        // rescan: current < previous (numeric)
};

// ── Scan request / result ──

struct AddressRange {
    uint64_t start = 0;
    uint64_t end   = 0;   // exclusive
};

struct ScanRequest {
    QByteArray pattern;             // literal bytes to match (empty for UnknownValue)
    QByteArray mask;                // 0xFF = must match, 0x00 = wildcard

    bool filterExecutable = false;  // only scan +x regions
    bool filterWritable   = false;  // only scan +w regions

    int alignment  = 1;             // 1 = every byte, 4 = dword, 8 = qword
    int maxResults = 50000;

    ScanCondition condition = ScanCondition::ExactValue;
    int valueSize = 4;              // bytes per value (for unknown scans)

    uint64_t startAddress = 0;      // 0 = no limit (scan all regions)
    uint64_t endAddress   = 0;      // 0 = no limit (scan all regions)

    // If non-empty, only scan within these address ranges (intersected with provider regions).
    QVector<AddressRange> constrainRegions;
};

struct ScanResult {
    uint64_t   address;
    QString    regionModule;
    QByteArray scanValue;       // cached bytes at scan/update time
    QByteArray previousValue;   // value before last update
};

// ── Pattern parsing ──

// Parse IDA-style signature string ("48 8B ?? 05") into pattern + mask.
// Returns true on success. On failure, sets errorMsg.
bool parseSignature(const QString& input, QByteArray& pattern, QByteArray& mask,
                    QString* errorMsg = nullptr);

// Serialize a typed value into raw bytes for exact-match scanning.
// Returns true on success. On failure, sets errorMsg.
bool serializeValue(ValueType type, const QString& input,
                    QByteArray& pattern, QByteArray& mask,
                    QString* errorMsg = nullptr);

// Natural alignment for a value type (used as default alignment for value scans).
int naturalAlignment(ValueType type);

// Byte-size for a value type (used for unknown scans and rescan read size).
int valueSizeForType(ValueType type);

// ── Scan engine ──

class ScanEngine : public QObject {
    Q_OBJECT
public:
    explicit ScanEngine(QObject* parent = nullptr);

    void start(std::shared_ptr<Provider> provider, const ScanRequest& req);
    void startRescan(std::shared_ptr<Provider> provider,
                     QVector<ScanResult> results, int readSize,
                     ScanCondition condition = ScanCondition::ExactValue,
                     ValueType valueType = ValueType::Int32,
                     const QByteArray& filterPattern = {},
                     const QByteArray& filterMask = {});
    void abort();
    bool isRunning() const;

signals:
    void progress(int percent);
    void finished(QVector<ScanResult> results);
    void rescanFinished(QVector<ScanResult> results);
    void error(QString message);

private:
    QVector<ScanResult> runScan(std::shared_ptr<Provider> prov, const ScanRequest& req);
    QVector<ScanResult> runRescan(std::shared_ptr<Provider> prov,
                                   QVector<ScanResult> results, int readSize,
                                   ScanCondition condition, ValueType valueType,
                                   const QByteArray& filterPattern,
                                   const QByteArray& filterMask);

    std::atomic<bool> m_abort{false};
    QFutureWatcher<QVector<ScanResult>>* m_watcher = nullptr;
};

} // namespace rcx

Q_DECLARE_METATYPE(QVector<rcx::ScanResult>)
