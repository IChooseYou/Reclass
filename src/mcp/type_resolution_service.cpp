#include "type_resolution_service.h"
#include "controller.h"
#include "type_resolution.h"
#include "disasm.h"
#include "rtti.h"
#include "symbolstore.h"
#include "imports/pe_debug_info.h"
#include "timeline/frame_provider.h"
#include "timeline/timeline_hub.h"
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <limits>

namespace rcx {
namespace {
constexpr int kMaxBytes = 4096;
constexpr int kMaxJobs = 4;
constexpr int kLifetimeMs = 10 * 60 * 1000;

QJsonObject reply(const QJsonObject& data, bool error = false) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"},
        {"text", QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact))}}}},
        {"structuredContent", data}, {"isError", error}};
}
QJsonObject failure(const QString& message) { return reply({{"error", message}}, true); }
QString hex(uint64_t address) { return QStringLiteral("0x%1").arg(address, 0, 16); }

bool addressValue(const QJsonValue& value, uint64_t& result) {
    if (!value.isString()) return false; // addresses never pass through JSON doubles
    const QString text = value.toString();
    bool ok = false;
    result = text.startsWith("0x", Qt::CaseInsensitive) ? text.mid(2).toULongLong(&ok, 16)
                                                        : text.toULongLong(&ok, 10);
    return ok;
}
bool boundedInt(const QJsonObject& args, const char* key, int fallback, int lo, int hi, int& result) {
    if (!args.contains(key)) { result = fallback; return true; }
    const auto value = args.value(key);
    if (!value.isDouble() || value.toDouble() < lo || value.toDouble() > hi
        || value.toDouble() != double(value.toInt())) return false;
    result = value.toInt(); return true;
}
QString treeRevision(RcxController* controller) {
    QByteArray bytes = QJsonDocument(controller->document()->tree.toJson()).toJson(QJsonDocument::Compact);
    bytes += QByteArray::number(controller->baseAddress());
    bytes += ':' + QByteArray::number(controller->viewRootId());
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Job {
    QString id, revision, state = "collecting", error;
    QPointer<RcxController> controller;
    std::shared_ptr<Provider> provider;
    std::shared_ptr<Provider> boundProvider;
    bool snapshot = false;
    tl::RecordId viewedRecord = tl::kNoRecord;
    std::shared_ptr<tl::CaptureContext> history;
    tl::TimelineHub* historyHub = nullptr; // only used while controller still owns it
    std::weak_ptr<tl::TimelineHub> ownedHub;
    uint64_t historyClass = 0;
    int requester = 0, reusedSamples = 0;
    tl::RecordId lastSeen = tl::kNoRecord, initialLast = tl::kNoRecord;
    QVector<tl::RecordId> pendingRecords;
    qint64 collectingSince = 0;
    int64_t ownedRecordingSince = -1;
    bool captureStarted = false;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    uint64_t base = 0, address = 0, parentId = 0, viewRoot = 0;
    uint64_t treeGeneration = 0;
    int offset = 0, length = 0, pointerSize = 8, targetSamples = 16, intervalMs = 100;
    qint64 created = QDateTime::currentMSecsSinceEpoch(), lastSample = 0;
    QByteArray eligible, knownFloats;
    QVector<resolution::Sample> samples;
    resolution::Analysis analysis;
    QJsonArray code;
    QVector<uint64_t> codeAddresses;
    bool busy = false;
    bool automatic = false;
    QVector<PdbSymbolSet> symbolSets; // immutable snapshot; worker never reads the singleton
    QJsonObject application;
};

resolution::Sample capture(const std::shared_ptr<Job>& job) {
    resolution::Sample sample;
    sample.bytes = QByteArray(job->length, '\0'); sample.valid = QByteArray(job->length, '\0');
    sample.timestampMs = QDateTime::currentMSecsSinceEpoch();
    try {
        for (int off = 0; off < job->length && !job->cancelled->load(); off += 16) {
            const int count = qMin(16, job->length - off);
            if (job->provider->read(job->address + off, sample.bytes.data() + off, count))
                std::fill(sample.valid.begin() + off, sample.valid.begin() + off + count, char(1));
        }
    } catch (...) { sample.error = "Provider failed while collecting evidence"; }
    return sample;
}

bool current(Job& job, RcxController* controller) {
    if (!controller || job.controller != controller || controller->pastRecord() != job.viewedRecord
        || controller->provider() != job.boundProvider || controller->baseAddress() != job.base
        || controller->viewRootId() != job.viewRoot) return false;
    const auto generation = controller->document()->tree.generation();
    if (generation == job.treeGeneration) return true;
    // Value writes also bump NodeTree::generation so display caches refresh,
    // although they do not change the serialized layout. Re-hash only after a
    // generation change, then adopt the new counter when the layout is equal.
    if (treeRevision(controller) != job.revision) return false;
    job.treeGeneration = generation;
    return true;
}

void releaseCapture(const std::shared_ptr<Job>& job) {
    if (job->history && job->requester) job->history->cancelFrames(job->requester);
    auto* ctrl = job->controller.data();
    const auto hub = job->ownedHub.lock();
    if (job->ownedRecordingSince >= 0 && hub && hub->context() == job->history
        && hub->track(job->historyClass).recordingSinceMs == job->ownedRecordingSince
        && hub->mode(job->historyClass) == tl::CaptureMode::Recording) {
        if (ctrl && ctrl->timelineHub() == hub.get() && ctrl->timelineClassId() == job->historyClass)
            ctrl->timelineToggleRecording(); // also ends this producer's coverage immediately
        else {
            hub->stopRecording(job->historyClass, tl::CaptureClock::nowMs());
            if (ctrl) ctrl->reloadTimelineSettings();
        }
    }
    job->ownedRecordingSince = -1;
}

// Bounded wrapper avoids touching a shared provider's mutable module cache.
class EvidenceProvider final : public Provider {
public:
    std::shared_ptr<Provider> source;
    std::shared_ptr<std::atomic_bool> cancelled;
    QVector<ModuleEntry> modules;
    mutable int remaining = 128 * 1024;
    bool read(uint64_t address, void* buffer, int length) const override {
        if (cancelled->load() || length <= 0 || length > remaining
            || address > std::numeric_limits<uint64_t>::max() - uint64_t(length)) return false;
        remaining -= length;
        return source->read(address, buffer, length);
    }
    int size() const override { return source->size(); }
    QVector<ModuleEntry> enumerateModules() const override { return modules; }
};

struct Completed {
    resolution::Analysis analysis;
    QJsonArray code;
    QString error;
};

// Exact global address + matching PE/PDB identity is an independent type source.
// RTTI names or plausible bytes alone never set verified=true.
static QVector<resolution::Candidate> verifiedCandidates(const std::shared_ptr<Job>& job,
                                                         EvidenceProvider& provider) {
    QVector<resolution::Candidate> out;
    const uint64_t objectAddress = job->address - job->offset;
    for (const auto& module : provider.modules) {
        if (objectAddress < module.base || objectAddress - module.base >= module.size
            || objectAddress - module.base > UINT32_MAX) continue;
        const QString moduleName = QFileInfo(module.name).completeBaseName().toLower();
        for (const auto& symbols : job->symbolSets) {
            if (symbols.moduleName != moduleName) continue;
            const auto rva = uint32_t(objectAddress - module.base);
            uint32_t type = 0; QString symbol; bool conflict = false;
            for (auto it = symbols.nameToRva.begin(); it != symbols.nameToRva.end(); ++it) {
                if (it.value() != rva) continue;
                const auto next = symbols.nameToTypeIndex.value(it.key());
                if (!next) continue;
                if (type && type != next) { conflict = true; break; }
                type = next; symbol = symbols.moduleName + '!' + it.key();
            }
            if (!type || conflict) continue;
            const auto identity = extractPdbDebugInfo(provider, module.base);
            if (!identity.valid) continue;
            QString error;
            const auto tree = importVerifiedObjectType(symbols.pdbPath, type, identity.guidString, identity.age, &error);
            if (tree.nodes.isEmpty()) continue;
            const auto root = tree.nodes.first().id;
            // Only simple directly declared scalars are represented exactly here.
            for (int index : tree.childrenOf(root)) {
                const auto& node = tree.nodes[index];
                if (node.kind != NodeKind::Int8 && node.kind != NodeKind::Int16 && node.kind != NodeKind::Int32
                    && node.kind != NodeKind::Int64 && node.kind != NodeKind::UInt8 && node.kind != NodeKind::UInt16
                    && node.kind != NodeKind::UInt32 && node.kind != NodeKind::UInt64 && node.kind != NodeKind::Float
                    && node.kind != NodeKind::Double && node.kind != NodeKind::Bool) continue;
                if (node.refId || !node.enumMembers.isEmpty() || !node.bitfieldMembers.isEmpty()) continue;
                const int off = node.offset - job->offset, size = node.byteSize();
                if (off < 0 || off + size > job->length) continue;
                bool valid = true;
                for (int i = off; i < off + size; ++i) valid &= job->eligible[i] && job->samples.last().valid[i];
                for (int other : tree.childrenOf(root)) {
                    const auto& n = tree.nodes[other];
                    if (other != index && n.offset < node.offset + size && n.offset + n.totalByteSize(tree) > node.offset)
                        valid = false;
                }
                if (!valid) continue;
                resolution::Candidate c;
                c.offset = node.offset; c.size = size; c.kind = node.kind; c.score = 100;
                c.verified = true; c.autoApplicable = true;
                c.evidenceLevel = QStringLiteral("authoritative"); c.declaredName = node.name;
                c.id = QStringLiteral("pdb:%1:%2:%3").arg(c.offset).arg(size).arg(kindToString(c.kind));
                c.observations = job->samples.size();
                c.evidence << "Direct global symbol: " + symbol
                           << "Executable and PDB GUID/age match: " + identity.guidString + '/' + QString::number(identity.age)
                           << "Declared member: " + node.name;
                out.append(c);
            }
        }
    }
    // Conflicting aliases/modules must not produce competing authoritative fields.
    for (int i = 0; i < out.size(); ++i) for (int j = i + 1; j < out.size(); ++j)
        if (out[i].offset < out[j].offset + out[j].size && out[j].offset < out[i].offset + out[i].size) return {};
    return out;
}

Completed finishAnalysis(const std::shared_ptr<Job>& job) {
    Completed completed;
    auto& analysis = completed.analysis;
    analysis.candidates = resolution::infer(job->samples, job->eligible, job->offset, job->pointerSize, job->knownFloats);
    if (job->cancelled->load()) return completed;
    EvidenceProvider provider;
    provider.source = job->provider; provider.cancelled = job->cancelled;
    provider.modules = job->provider->enumerateModules();
    analysis.candidates += verifiedCandidates(job, provider);
    const auto regions = job->provider->enumerateRegions();
    for (const auto& module : provider.modules) {
        if (analysis.modules.size() >= 128) break;
        analysis.modules.append(QJsonObject{{"name", module.name}, {"base", hex(module.base)},
                                           {"size", QString::number(module.size)}});
    }
    QVector<uint64_t> codeAddresses = job->codeAddresses;
    QHash<uint64_t, bool> readableTargets;
    int rttiAttempts = 0;
    for (auto& candidate : analysis.candidates) {
        if (job->cancelled->load()) return completed;
        if (candidate.target.isEmpty()) continue;
        uint64_t target = 0;
        if (!addressValue(candidate.target, target)) continue;
        if (!readableTargets.contains(target) && readableTargets.size() >= 128) {
            candidate.caveats << "Pointer probe budget exhausted"; continue;
        }
        QByteArray preview(32, '\0');
        if (!readableTargets.contains(target)) {
            // A real successful read is stronger than a canonical-looking address.
            char first;
            readableTargets[target] = provider.read(target, &first, 1);
        }
        if (!readableTargets[target]) {
            candidate.score = 0; candidate.caveats << "Target unreadable; conversion disabled"; continue;
        }
        candidate.score = 65;
        candidate.evidence << "Pointer target successfully read";
        QJsonObject pointer{{"candidateId", candidate.id}, {"address", hex(target)}};
        bool executable = false, readableRegion = false, foundRegion = false;
        for (const auto& region : regions) {
            if (target >= region.base && target - region.base < region.size) {
                foundRegion = true; readableRegion = region.readable;
                pointer["module"] = region.moduleName;
                pointer["executable"] = executable = region.executable;
                pointer["writable"] = region.writable;
                candidate.evidence << "Target falls inside an enumerated memory region";
                break;
            }
        }
        const bool previewReadable = provider.read(target, preview.data(), preview.size());
        if (previewReadable) {
            pointer["previewHex"] = QString::fromLatin1(preview.toHex(' '));
            if (!executable && rttiAttempts++ < 16) {
                uint64_t vtable = 0;
                std::memcpy(&vtable, preview.constData(), job->pointerSize);
                // The existing walkers validate their own ABI structures.
                auto rtti = walkRtti(provider, target, job->pointerSize, 8);
                if (!rtti.ok) rtti = walkRttiItanium(provider, target, job->pointerSize, 8);
                const bool isVtable = rtti.ok;
                if (!rtti.ok) rtti = walkRtti(provider, vtable, job->pointerSize, 8);
                if (!rtti.ok) rtti = walkRttiItanium(provider, vtable, job->pointerSize, 8);
                if (rtti.ok) {
                    pointer["rttiName"] = rtti.demangledName;
                    pointer["rttiAbi"] = rtti.abi;
                    pointer["rttiRole"] = isVtable ? "vtable" : "object";
                    candidate.evidence << (isVtable ? "Target is a vtable with RTTI: " : "Pointee has RTTI: ") + rtti.demangledName;
                    candidate.score = 80;
                    for (const auto& method : rtti.vtable) {
                        if (codeAddresses.size() >= 16) break;
                        if (!codeAddresses.contains(method.address)) codeAddresses.append(method.address);
                    }
                }
            }
        }
        if (previewReadable && foundRegion && readableRegion && !executable
            && candidate.observations == job->samples.size() && !job->samples.isEmpty()
            && candidate.distinctValues == 1 && candidate.offset % job->pointerSize == 0
            && target % uint64_t(job->pointerSize) == 0) {
            candidate.autoApplicable = true;
            candidate.evidenceLevel = QStringLiteral("high");
            candidate.score = qMax(candidate.score, 90);
            candidate.evidence << "Stable aligned pointer across all observations"
                               << "Full target preview is readable in a non-executable region";
            candidate.caveats << "Target validity establishes pointer representation, not pointee type";
        }
        if (executable && codeAddresses.size() < 16 && !codeAddresses.contains(target)) codeAddresses.append(target);
        analysis.pointers.append(pointer);
    }
    for (uint64_t address : codeAddresses) {
        if (job->cancelled->load()) return completed;
        if (address > std::numeric_limits<uint64_t>::max() - 256) continue;
        QJsonObject block{{"address", hex(address)},
            {"binding", "Instruction context only; register-to-object binding is not established"}};
        QByteArray code(256, '\0');
        int readable = 0;
        // Only decode the successfully read contiguous prefix.
        for (; readable < code.size(); readable += 16)
            if (!provider.read(address + readable, code.data() + readable, 16)) break;
        code.resize(readable);
        QJsonArray instructions;
        for (const auto& instruction : decodeRange(code, address, job->pointerSize * 8, 256)) {
            if (!instruction.ok) break;
            QJsonArray operands;
            for (const auto& operand : instruction.memoryOperands) {
                operands.append(QJsonObject{{"width", operand.width}, {"baseRegister", operand.baseRegister},
                    {"indexRegister", operand.indexRegister}, {"scale", operand.scale},
                    {"segmentOverride", operand.segment},
                    {"displacement", QString::number(operand.displacement)},
                    {"instructionRelative", operand.instructionRelative}});
            }
            instructions.append(QJsonObject{{"address", hex(address + instruction.offset)},
                {"length", instruction.length}, {"mnemonic", instruction.mnemonic},
                {"text", instruction.text}, {"memoryOperands", operands},
                {"branchTarget", instruction.target ? hex(instruction.target) : QString()}});
        }
        block["instructions"] = instructions;
        block["readableBytes"] = readable;
        completed.code.append(block);
    }
    // Bound client context independently of the observation byte budget.
    if (analysis.candidates.size() > 8192) {
        std::stable_sort(analysis.candidates.begin(), analysis.candidates.end(), [](const auto& a, const auto& b) {
            return a.score > b.score;
        });
        analysis.candidates.resize(8192); analysis.truncated = true;
    }
    std::sort(analysis.candidates.begin(), analysis.candidates.end(), [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });
    resolution::selectLayout(analysis, job->eligible, job->offset, job->automatic);
    return completed;
}

// A single prepared replacement is one undo command, with no intermediate edits.
class ResolutionCommand final : public QUndoCommand {
    QPointer<RcxDocument> document;
    NodeTree before, after;
    void install(const NodeTree& value) {
        if (!document) return;
        const auto generation = document->tree.generation();
        const auto nextId = document->tree.m_nextId;
        document->tree = value;
        document->tree.m_nextId = qMax(nextId, value.m_nextId);
        document->tree.m_generation = generation + 1;
        document->tree.invalidateIdCache();
        document->modified = true;
        emit document->documentChanged();
    }
public:
    ResolutionCommand(RcxDocument* doc, NodeTree replacement)
        : QUndoCommand("Resolve inferred types"), document(doc), before(doc->tree), after(std::move(replacement)) {}
    void undo() override { install(before); }
    void redo() override { install(after); }
};
} // namespace

struct TypeResolutionService::Impl {
    TypeResolutionService* owner;
    QHash<QString, std::shared_ptr<Job>> jobs;
    QTimer timer;
    explicit Impl(TypeResolutionService* parent) : owner(parent) {
        timer.setInterval(50);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { tick(); });
        timer.start();
    }
    ~Impl() { for (const auto& job : jobs) { job->cancelled->store(true); releaseCapture(job); } }

    void analyzeSamples(const std::shared_ptr<Job>& job) {
        releaseCapture(job);
        if (job->samples.isEmpty()) {
            job->state = "failed"; job->error = "No readable bytes were captured"; return;
        }
        job->state = "analyzing"; job->busy = true;
        auto* analysis = new QFutureWatcher<Completed>(owner);
        QObject::connect(analysis, &QFutureWatcher<Completed>::finished, owner, [analysis, job] {
            const auto completed = analysis->result(); analysis->deleteLater(); job->busy = false;
            if (job->cancelled->load()) return;
            if (!completed.error.isEmpty()) { job->state = "failed"; job->error = completed.error; return; }
            job->analysis = completed.analysis; job->code = completed.code;
            job->state = current(*job, job->controller) ? "ready" : "stale";
        });
        analysis->setFuture(QtConcurrent::run([job] {
            try { return finishAnalysis(job); }
            catch (...) { Completed result; result.error = "Analysis/provider failed"; return result; }
        }));
    }

    void collectHistory(const std::shared_ptr<Job>& job, qint64 now) {
        auto* ctrl = job->controller.data();
        if (!ctrl || ctrl->timelineContext() != job->history.get() || ctrl->timelineHub() != job->historyHub) {
            job->state = "stale"; releaseCapture(job); return;
        }
        const auto& model = job->history->model();
        const auto last = model.lastRecord();
        if (last != tl::kNoRecord && last != job->lastSeen && job->pendingRecords.isEmpty()) {
            const auto begin = job->lastSeen == tl::kNoRecord
                ? qMax(model.firstRecord(), last - qMin(last, tl::RecordId(255)))
                : qMax(model.firstRecord(), job->lastSeen + 1);
            for (auto r = begin; r <= last; ++r) {
                const auto time = model.timeOf(r);
                if (!job->historyHub->isHiddenFor(job->historyClass, time)
                    && job->historyHub->baseAt(time, job->base) == job->base && !model.gapAt(time))
                    job->pendingRecords.append(r);
            }
            job->lastSeen = last;
            if (job->samples.isEmpty() && job->pendingRecords.size() > job->targetSamples)
                job->pendingRecords = job->pendingRecords.mid(job->pendingRecords.size() - job->targetSamples);
        }
        if (!job->pendingRecords.isEmpty()) {
            const auto r = job->pendingRecords.takeFirst();
            QVector<uint64_t> pages;
            const auto lastPage = (job->address + job->length - 1) & tl::kPageMask;
            for (uint64_t page = job->address & tl::kPageMask;; page += tl::kPageSize) {
                pages.append(page);
                if (page == lastPage) break;
            }
            job->busy = true;
            job->history->requestFrame(job->requester, r, pages, owner, [this, job, r](tl::FramePtr frame) {
                job->busy = false;
                if (job->cancelled->load()) return;
                if (!current(*job, job->controller)) { job->state = "stale"; releaseCapture(job); return; }
                if (frame) {
                    resolution::Sample sample;
                    sample.timestampMs = tl::CaptureClock::toEpochMs(frame->timeMs);
                    sample.bytes = QByteArray(job->length, '\0'); sample.valid = QByteArray(job->length, '\0');
                    tl::TimelineFrameProvider source(job->boundProvider, frame, job->length);
                    for (int off = 0; off < job->length;) {
                        const int count = qMin(job->length - off,
                            int(tl::kPageSize - ((job->address + off) & ~tl::kPageMask)));
                        if (source.read(job->address + off, sample.bytes.data() + off, count))
                            std::fill(sample.valid.begin() + off, sample.valid.begin() + off + count, char(1));
                        off += count;
                    }
                    if (sample.valid.contains(char(1))) {
                        job->samples.append(sample);
                        if (job->initialLast != tl::kNoRecord && r <= job->initialLast) ++job->reusedSamples;
                    }
                }
                if (job->samples.size() >= job->targetSamples) analyzeSamples(job);
            });
            return;
        }
        if (!job->captureStarted) {
            job->captureStarted = true; job->collectingSince = now;
            if (!ctrl->timelineRecording()) {
                ctrl->timelineToggleRecording();
                job->ownedRecordingSince = job->historyHub->track(job->historyClass).recordingSinceMs;
            }
        }
        if (!ctrl->timelineRecording() || now - job->collectingSince >= qMax(2500, job->targetSamples * job->intervalMs * 2))
            analyzeSamples(job);
    }

    void tick() {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = jobs.begin(); it != jobs.end();) {
            const auto job = it.value();
            if (now - job->created > kLifetimeMs || !job->controller) {
                job->cancelled->store(true); releaseCapture(job); it = jobs.erase(it); continue;
            }
            ++it;
            if (job->busy || job->state != "collecting" || now - job->lastSample < job->intervalMs) continue;
            if (!current(*job, job->controller)) {
                job->state = "stale"; job->cancelled->store(true); releaseCapture(job); continue;
            }
            if (job->history) { collectHistory(job, now); continue; }
            job->busy = true; job->lastSample = now;
            auto* watcher = new QFutureWatcher<resolution::Sample>(owner);
            QObject::connect(watcher, &QFutureWatcher<resolution::Sample>::finished, owner, [this, watcher, job] {
                const auto sample = watcher->result(); watcher->deleteLater(); job->busy = false;
                if (job->cancelled->load()) return;
                if (!sample.error.isEmpty()) { job->state = "failed"; job->error = sample.error; releaseCapture(job); return; }
                job->samples.append(sample);
                if (job->samples.size() < job->targetSamples) return;
                analyzeSamples(job);
            });
            watcher->setFuture(QtConcurrent::run([job] { return capture(job); }));
        }
    }
};

TypeResolutionService::TypeResolutionService(QObject* parent) : QObject(parent), d(new Impl(this)) {}
TypeResolutionService::~TypeResolutionService() = default;

QJsonObject TypeResolutionService::analyze(const QJsonObject& args, RcxController* controller) {
    const QString action = args.value("action").toString("start");
    if (action == "list") {
        QJsonArray jobs;
        for (const auto& job : d->jobs) if (job->controller == controller)
            jobs.append(QJsonObject{{"jobId", job->id}, {"state", job->state}, {"parentId", QString::number(job->parentId)},
                {"offset", job->offset}, {"length", job->length}, {"samples", job->samples.size()},
                {"revision", job->revision}, {"expiresAtMs", double(job->created + kLifetimeMs)}});
        return reply({{"jobs", jobs}});
    }
    if (action == "status" || action == "cancel") {
        const auto job = d->jobs.value(args.value("jobId").toString());
        if (!job || job->controller != controller) return failure("Unknown job for this tab; use action:list to resume");
        if (action == "cancel") { job->cancelled->store(true); job->state = "cancelled"; releaseCapture(job); }
        else if (job->state != "applied" && job->state != "cancelled" && !current(*job, controller)) {
            job->cancelled->store(true); job->state = "stale"; releaseCapture(job);
        }
        int cursor, limit;
        if (!boundedInt(args, "cursor", 0, 0, 8192, cursor) || !boundedInt(args, "limit", 64, 1, 256, limit))
            return failure("Invalid cursor or limit");
        QJsonObject report{{"jobId", job->id}, {"state", job->state}, {"revision", job->revision},
            {"parentId", QString::number(job->parentId)}, {"address", hex(job->address)},
            {"offset", job->offset}, {"length", job->length}, {"samples", job->samples.size()},
            {"targetSamples", job->targetSamples}, {"expiresAtMs", double(job->created + kLifetimeMs)},
            {"scoreMeaning", "Rank only, not a calibrated probability. autoApplicable is a separate conservative validation gate."}};
        report["snapshotAtomic"] = false;
        report["automatic"] = job->automatic;
        report["evidenceSource"] = job->snapshot ? "snapshot" : job->history ? "recording" : "observations";
        report["reusedSamples"] = job->reusedSamples;
        report["autoStartedRecording"] = job->ownedRecordingSince >= 0;
        if (!job->error.isEmpty()) report["error"] = job->error;
        if (!job->application.isEmpty()) report["application"] = job->application;
        if (job->state == "ready") {
            QJsonArray candidates;
            const int end = qMin(cursor + limit, int(job->analysis.candidates.size()));
            for (int i = cursor; i < end; ++i) candidates.append(job->analysis.candidates[i].toJson());
            report["candidates"] = candidates;
            report["candidateCount"] = job->analysis.candidates.size();
            report["nextCursor"] = end < job->analysis.candidates.size() ? end : -1;
            report["truncated"] = job->analysis.truncated;
            if (cursor == 0) {
                report["suggestedFields"] = job->analysis.suggestedFields;
                QJsonArray automaticCandidates;
                for (const auto& candidate : job->analysis.candidates) {
                    if (!candidate.autoApplicable) continue;
                    if (automaticCandidates.size() == 512) break;
                    automaticCandidates.append(candidate.toJson());
                }
                report["automaticCandidates"] = automaticCandidates;
                QVector<const resolution::Candidate*> review;
                for (const auto& candidate : job->analysis.candidates)
                    if (candidate.score >= 45) review.append(&candidate);
                std::stable_sort(review.begin(), review.end(), [](const auto* a, const auto* b) {
                    return a->score != b->score ? a->score > b->score : a->offset < b->offset;
                });
                QJsonArray reviewCandidates;
                for (int i = 0; i < qMin(128, int(review.size())); ++i)
                    reviewCandidates.append(review[i]->toJson());
                report["reviewCandidates"] = reviewCandidates;
                report["unresolved"] = job->analysis.unresolved;
                report["pointers"] = job->analysis.pointers;
                report["modules"] = job->analysis.modules;
                report["code"] = job->code;
                int valid = 0;
                if (!job->samples.isEmpty()) for (char byte : job->samples.last().valid) valid += byte != 0;
                report["latestReadableBytes"] = valid;
                report["nextSteps"] = QJsonArray{
                    "Inspect candidates, contradictions, pointer/RTTI and instruction context. Preserve unknown names.",
                    "Use symbols.lookup/importType for authoritative layouts; codeAddresses on a new job for relevant functions.",
                    "Submit tree.apply resolution:{jobId,revision,fields,validateOnly:true}; poll status for application.valid. Then validateOnly:false and poll for application.applied.",
                    "Each leaf is {candidateId,name?}; Struct:{kind,offset,size,fields}; Array:{kind,offset,size,elements:[candidateId,...]}. ALL offsets, including nested groups, are relative to the originally selected parent.",
                    "Stop when no additional evidence is available. Repeated identical observations do not prove a type."};
            }
        } else if (job->state == "collecting" || job->state == "analyzing" || job->state == "verifying") report["pollAfterMs"] = 500;
        return reply(report);
    }
    if (action != "start") return failure("Unknown action; use start, status, cancel, or list");
    if (!controller || !controller->provider()
        || (!controller->isViewingPast() && !controller->provider()->isValid())) return failure("No readable source");
    if (d->jobs.size() >= kMaxJobs) {
        auto oldest = d->jobs.end();
        for (auto it = d->jobs.begin(); it != d->jobs.end(); ++it)
            if (!it.value()->busy && it.value()->state != "collecting"
                && (oldest == d->jobs.end() || it.value()->created < oldest.value()->created)) oldest = it;
        if (oldest == d->jobs.end()) return failure("Four observation jobs are active; cancel one first");
        d->jobs.erase(oldest);
    }
    const auto& tree = controller->document()->tree;
    uint64_t parentId = controller->viewRootId();
    if (!parentId && !args.contains("parentId"))
        for (const auto& node : tree.nodes) if (!node.parentId && node.kind == NodeKind::Struct) { parentId = node.id; break; }
    if (args.contains("parentId") && !addressValue(args.value("parentId"), parentId)) return failure("Invalid parentId");
    const int index = tree.indexOfId(parentId);
    if (index < 0 || tree.nodes[index].kind != NodeKind::Struct || tree.nodes[index].classKeyword == "union")
        return failure("parentId must identify a non-union struct");
    // Bind only embedded ancestors of the current view root; never guess a dereference.
    int64_t relative = 0;
    uint64_t ancestor = parentId;
    QSet<uint64_t> visited;
    while (ancestor != controller->viewRootId()) {
        const int ai = tree.indexOfId(ancestor);
        if (ai < 0 || visited.contains(ancestor) || tree.nodes[ai].kind != NodeKind::Struct
            || tree.nodes[ai].classKeyword == "union") return failure("Open this struct as the view root before resolving it");
        visited.insert(ancestor); relative += tree.nodes[ai].offset; ancestor = tree.nodes[ai].parentId;
    }
    if (relative < 0) return failure("Negative object offset is unsupported");
    auto job = std::make_shared<Job>();
    int offset, length, samples, interval;
    const int span = tree.structSpan(parentId);
    if (!boundedInt(args, "offset", 0, 0, qMax(0, span - 1), offset)
        || !boundedInt(args, "length", qMin(kMaxBytes, span - offset), 1, kMaxBytes, length)
        || int64_t(offset) + length > span || length <= 0
        || !boundedInt(args, "samples", 16, 1, 64, samples)
        || !boundedInt(args, "intervalMs", 100, 50, 2000, interval)) return failure("Invalid observation range, sample count, or interval");
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    if (controller->baseAddress() > max - uint64_t(relative) - uint64_t(offset) - uint64_t(length))
        return failure("Address range overflows");
    job->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->controller = controller; job->provider = controller->provider();
    job->boundProvider = controller->provider();
    job->snapshot = !job->provider->isLive() || controller->isViewingPast();
    job->viewedRecord = controller->pastRecord();
    job->base = controller->baseAddress(); job->parentId = parentId;
    job->viewRoot = controller->viewRootId();
    job->address = job->base + uint64_t(relative) + offset;
    if (controller->isViewingPast()) {
        const auto* displayed = dynamic_cast<const tl::TimelineFrameProvider*>(controller->displayedProvider());
        if (!displayed || !displayed->frame()) return failure("Wait for the recorded frame to finish loading");
        job->provider = std::make_shared<tl::TimelineFrameProvider>(job->boundProvider, displayed->frame(), length);
        const auto thenBase = controller->timelineHub()->baseAt(displayed->frame()->timeMs, job->base);
        if (thenBase > max - uint64_t(relative) - uint64_t(offset) - uint64_t(length)) return failure("Address range overflows");
        job->address = thenBase + uint64_t(relative) + offset;
    }
    job->offset = offset; job->length = length; job->pointerSize = tree.pointerSize;
    if (job->pointerSize != 4 && job->pointerSize != 8) return failure("Unsupported pointer size");
    job->targetSamples = job->snapshot ? 1 : samples;
    job->intervalMs = interval; job->revision = treeRevision(controller);
    job->treeGeneration = tree.generation();
    job->automatic = args.value("automatic").toBool(false);
    for (const auto& name : SymbolStore::instance().loadedModules())
        if (const auto* symbols = SymbolStore::instance().moduleData(name)) job->symbolSets.append(*symbols);
    job->eligible = resolution::eligibleBytes(tree, parentId, offset, length);
    job->knownFloats = resolution::knownFloatBytes(tree, parentId, offset, length);
    if (!job->eligible.contains(char(1))) return failure("No eligible hex bytes in this region; typed, annotated and overlapping fields are protected");
    const auto code = args.value("codeAddresses").toArray();
    if (code.size() > 16) return failure("At most 16 code addresses per job");
    for (const auto& value : code) {
        uint64_t address;
        if (!addressValue(value, address) || address > max - 256) return failure("Invalid code address");
        job->codeAddresses.append(address);
    }
    if (job->automatic && !job->snapshot) {
        if (!controller->timelineEnabled()) controller->setTimelineEnabled(true);
        if (auto* hub = controller->timelineHub(); hub && hub->context()) {
            job->historyHub = hub; job->history = hub->context();
            job->ownedHub = controller->timelineHubHandle();
            job->historyClass = controller->timelineClassId();
            job->requester = job->history->addProducer();
            job->initialLast = job->history->model().lastRecord();
            if (!args.value("reuseHistory").toBool(true)) job->lastSeen = job->initialLast;
        }
    }
    d->jobs.insert(job->id, job);
    return reply({{"jobId", job->id}, {"state", job->state}, {"revision", job->revision},
        {"address", hex(job->address)}, {"length", length}, {"targetSamples", job->targetSamples},
        {"evidenceSource", job->snapshot ? "snapshot" : job->history ? "recording" : "observations"},
        {"pollAfterMs", 500}, {"expiresAtMs", double(job->created + kLifetimeMs)}});
}

QJsonObject TypeResolutionService::apply(const QJsonObject& args, RcxController* controller) {
    const auto job = d->jobs.value(args.value("jobId").toString());
    if (!job || job->controller != controller) return failure("Unknown resolution job for this tab");
    if (job->state == "applied") return reply(job->application);
    if (job->state != "ready") return failure("Resolution job must be ready before validation/application; poll analysis.infer_types status");
    if (args.value("revision").toString() != job->revision || !current(*job, controller))
        return failure("Stale evidence: source, base, view, or tree changed; start a fresh job");
    if (QDateTime::currentMSecsSinceEpoch() - job->created > kLifetimeMs)
        return failure("Observation expired; collect fresh evidence");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "jobId" && it.key() != "revision" && it.key() != "fields" && it.key() != "validateOnly")
            return failure("Unsupported resolution property: " + it.key());
    NodeTree replacement;
    QJsonArray applied;
    QString error;
    QJsonArray fields = args.value("fields").toArray();
    if (job->automatic) {
        QHash<QString, resolution::Candidate> allowed;
        for (const auto& c : job->analysis.candidates) if (c.autoApplicable) allowed.insert(c.id, c);
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            auto field = (*it).toObject();
            const QString id = field.value("candidateId").toString();
            if (!allowed.contains(id) || field.size() != 1)
                return failure("AUTO accepts only candidates that pass RC's automatic validation gate");
            const auto candidate = allowed.value(id);
            if (candidate.verified && !candidate.declaredName.isEmpty()) {
                QString existingName;
                for (int index : controller->document()->tree.childrenOf(job->parentId)) {
                    const auto& existing = controller->document()->tree.nodes[index];
                    if (existing.offset == candidate.offset) { existingName = existing.name; break; }
                }
                // A custom user label remains user-owned. PDB names replace
                // only empty/generated raw-field labels.
                QString suffix = existingName.startsWith("field_") ? existingName.mid(6)
                               : existingName.startsWith("pad_") ? existingName.mid(4) : QString();
                bool generated = false;
                if (!suffix.isEmpty()) suffix.toULongLong(&generated, 16);
                field["name"] = (!existingName.isEmpty() && !generated)
                    ? existingName : candidate.declaredName;
            }
            *it = field;
        }
    }
    if (!resolution::prepare(controller->document()->tree, job->parentId, job->offset, job->length,
        job->analysis.candidates, fields, replacement, applied, error))
        return failure(error.isEmpty() ? "Invalid field proposal" : error);
    const bool validateOnly = args.value("validateOnly").toBool(true);
    if (args.contains("validateOnly") && !args.value("validateOnly").isBool()) return failure("validateOnly must be boolean");
    // Even the apply call performs a fresh read, outside the UI thread. A
    // validated proposal is not permission to reuse old bytes indefinitely.
    QSet<QString> selected;
    std::function<void(const QJsonArray&)> gather = [&](const QJsonArray& fields) {
        for (const auto& value : fields) {
            const auto field = value.toObject();
            if (field.contains("candidateId")) selected.insert(field.value("candidateId").toString());
            for (const auto& element : field.value("elements").toArray()) selected.insert(element.toString());
            gather(field.value("fields").toArray());
        }
    };
    gather(args.value("fields").toArray());
    job->state = "verifying"; job->busy = true;
    job->application = {{"state", "verifying"}, {"validateOnly", validateOnly}};
    struct Verification { QString error; QSet<QString> rejected; };
    auto* watcher = new QFutureWatcher<Verification>(this);
    connect(watcher, &QFutureWatcher<Verification>::finished, this,
        [watcher, job, replacement = std::move(replacement), applied, fields, validateOnly]() mutable {
            const auto verified = watcher->result(); watcher->deleteLater(); job->busy = false;
            if (job->cancelled->load()) return;
            QString error = verified.error;
            if (!current(*job, job->controller)) error = "Source or tree changed during verification";
            QJsonArray skipped;
            if (error.isEmpty() && !verified.rejected.isEmpty()) {
                QJsonArray compatible;
                for (const auto& field : fields) {
                    const QString id = field.toObject().value("candidateId").toString();
                    if (verified.rejected.contains(id)) skipped.append(id);
                    else compatible.append(field);
                }
                if (compatible.isEmpty()) {
                    job->state = "abstained";
                    job->application = {{"valid", true}, {"applied", false}, {"fields", QJsonArray{}}, {"skippedFields", skipped}};
                    return;
                }
                if (!resolution::prepare(job->controller->document()->tree, job->parentId, job->offset,
                        job->length, job->analysis.candidates, compatible, replacement, applied, error)
                    && error.isEmpty()) error = "Remaining fields failed validation";
            }
            if (!error.isEmpty()) {
                job->state = "stale";
                job->application = {{"valid", false}, {"applied", false}, {"error", error}};
                return;
            }
            if (!validateOnly)
                job->controller->document()->undoStack.push(new ResolutionCommand(job->controller->document(), std::move(replacement)));
            job->state = validateOnly ? "ready" : "applied";
            job->application = {{"valid", true}, {"applied", !validateOnly}, {"jobId", job->id}, {"fields", applied},
                {"skippedFields", skipped},
                {"revision", treeRevision(job->controller)},
                {"validationScope", "Fresh reads, evidence references, source/tree revision, protected fields, bounds and overlaps. Compatibility is not proof of the original type."}};
        });
    watcher->setFuture(QtConcurrent::run([job, selected] {
        Verification result;
        try {
            const auto fresh = capture(job);
            if (!fresh.error.isEmpty()) { result.error = fresh.error; return result; }
            auto observations = job->samples; observations.append(fresh);
            const auto now = resolution::infer(observations, job->eligible, job->offset, job->pointerSize, job->knownFloats);
            QHash<QString, resolution::Candidate> candidates;
            for (const auto& c : now) candidates.insert(c.id, c);
            EvidenceProvider source; source.source = job->provider; source.cancelled = job->cancelled;
            source.modules = job->provider->enumerateModules();
            for (const auto& c : verifiedCandidates(job, source)) candidates.insert(c.id, c);
            const auto regions = job->provider->enumerateRegions();
            for (const auto& c : job->analysis.candidates) {
                if (!selected.contains(c.id)) continue;
                const auto check = [&]() -> QString {
                    for (int i = c.offset - job->offset; i < c.offset - job->offset + c.size; ++i)
                        if (!fresh.valid[i]) { return "Selected field became unreadable: " + c.id; }
                    if (!candidates.contains(c.id)) { return "Fresh bytes contradict candidate " + c.id; }
                    const auto currentCandidate = candidates.value(c.id);
                    if (job->automatic && !c.verified && c.kind != NodeKind::Pointer32
                        && c.kind != NodeKind::Pointer64 && !currentCandidate.autoApplicable) {
                        return "Candidate no longer passes the automatic validation gate: " + c.id;
                    }
                    if (!c.target.isEmpty()) {
                        uint64_t target = 0;
                        QByteArray preview(32, '\0');
                        bool readableDataRegion = false;
                        if (addressValue(c.target, target)) for (const auto& region : regions) {
                            if (target >= region.base && target - region.base < region.size) {
                                readableDataRegion = region.readable && !region.executable;
                                break;
                            }
                        }
                        if (currentCandidate.target != c.target || !addressValue(c.target, target)
                            || currentCandidate.observations != observations.size()
                            || currentCandidate.distinctValues != 1
                            || c.offset % job->pointerSize != 0 || target % uint64_t(job->pointerSize) != 0
                            || !readableDataRegion || !job->provider->read(target, preview.data(), preview.size())) {
                            return "Pointer changed or became unreadable: " + c.id;
                        }
                    }
                    return {};
                };
                const QString error = check();
                if (!error.isEmpty()) {
                    if (job->automatic) result.rejected.insert(c.id);
                    else { result.error = error; return result; }
                }
            }
        } catch (...) { result.error = "Provider failed during fresh verification"; }
        return result;
    }));
    return reply({{"state", "verifying"}, {"jobId", job->id}, {"pollAfterMs", 500},
        {"next", "Poll analysis.infer_types action:status; application contains the validation/application outcome"}});
}
} // namespace rcx
