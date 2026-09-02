#include "external_tool_profiler.h"

#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "process/process_enumerator.h"

#include <QDateTime>

#include <algorithm>

namespace killengine {

namespace {

uint64_t profilerHash(const QByteArray& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

ExternalToolProfiler::ExternalToolProfiler(
    const killcore::ProcessHandle& handle,
    TelemetryCallback telemetry,
    PidCallback pid)
    : m_handle(handle)
    , m_appendScanTelemetry(std::move(telemetry))
    , m_pid(std::move(pid)) {
}

void ExternalToolProfiler::clearSessionState() {
    m_checkpoints.clear();
    m_checkpointOrder.clear();
}

QVariantMap ExternalToolProfiler::clearProfilerSession() {
    clearSessionState();
    QVariantMap result;
    result["success"] = true;
    return result;
}

QVariantMap ExternalToolProfiler::listProfilerCheckpoints() const {
    QVariantMap result;
    QVariantList labels;
    for (const auto& label : m_checkpointOrder) {
        const auto& checkpoint = m_checkpoints.value(label);
        QVariantMap entry;
        entry["label"] = label;
        entry["capturedAtMs"] = checkpoint.capturedAtMs;
        entry["pid"] = checkpoint.pid;
        entry["moduleCount"] = checkpoint.modules.size();
        entry["regionCount"] = checkpoint.regions.size();
        labels.append(entry);
    }
    result["success"] = true;
    result["checkpoints"] = labels;
    return result;
}

QVariantMap ExternalToolProfiler::captureProfilerCheckpoint(const QString& label, const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    const QString trimmedLabel = label.trimmed();
    if (trimmedLabel.isEmpty()) {
        result["error"] = "Label de checkpoint requis.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const int pid = m_pid ? m_pid() : 0;

    ProfilerCheckpoint checkpoint;
    checkpoint.label = trimmedLabel;
    checkpoint.capturedAtMs = QDateTime::currentMSecsSinceEpoch();
    checkpoint.pid = pid;

    const auto modules = killcore::ProcessEnumerator::enumerateModules(static_cast<uint32_t>(pid));
    checkpoint.modules.reserve(modules.size());
    for (const auto& module : modules) {
        checkpoint.modules.append({module.name, module.path, module.baseAddress, module.size});
    }

    QString moduleFilter = options.value("moduleName").toString().trimmed();
    uint64_t filterBase = 0;
    uint64_t filterEnd = 0;
    bool hasFilter = false;
    if (!moduleFilter.isEmpty()) {
        for (const auto& module : checkpoint.modules) {
            if (module.name.compare(moduleFilter, Qt::CaseInsensitive) == 0
                || module.name.contains(moduleFilter, Qt::CaseInsensitive)) {
                filterBase = module.baseAddress;
                filterEnd = module.baseAddress + module.size;
                hasFilter = true;
                break;
            }
        }
        if (!hasFilter) {
            result["error"] = QString("Module '%1' introuvable dans la liste courante — aucun filtre appliqué, capture élargie à tout le process.").arg(moduleFilter);
        }
    }

    const bool hashContent = options.value("hashContent", true).toBool();
    const uint64_t maxHashBytes = static_cast<uint64_t>(
        std::clamp(options.value("maxHashBytesMb", 64).toInt(), 8, 512)) * 1024ull * 1024ull;

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    killcore::MemoryReader reader(m_handle);
    uint64_t hashBudgetUsed = 0;

    checkpoint.regions.reserve(regions.size());
    for (const auto& region : regions) {
        if (!region.readable || region.guarded || region.size == 0) {
            continue;
        }
        if (hasFilter) {
            const uint64_t regionEnd = region.baseAddress + region.size;
            const bool overlaps = region.baseAddress < filterEnd && regionEnd > filterBase;
            if (!overlaps) {
                continue;
            }
        }

        ProfilerRegionState state;
        state.baseAddress = region.baseAddress;
        state.size = region.size;
        state.protection = killcore::protectionToString(region.protection);
        state.memoryType = killcore::memoryTypeToString(region.type);
        state.executable = region.executable;
        state.writable = region.writable;

        if (hashContent && hashBudgetUsed < maxHashBytes) {
            const uint64_t toRead = std::min<uint64_t>(region.size, maxHashBytes - hashBudgetUsed);
            const auto read = reader.read(region.baseAddress, static_cast<size_t>(toRead));
            if (read.bytesRead > 0) {
                const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
                state.contentHash = profilerHash(data);
                state.hashed = true;
                hashBudgetUsed += read.bytesRead;
            }
        }

        checkpoint.regions.append(state);
    }

    if (!m_checkpoints.contains(trimmedLabel)) {
        m_checkpointOrder.append(trimmedLabel);
    }
    m_checkpoints.insert(trimmedLabel, checkpoint);

    result["success"] = true;
    result["label"] = trimmedLabel;
    result["pid"] = pid;
    result["moduleCount"] = checkpoint.modules.size();
    result["regionCount"] = checkpoint.regions.size();
    result["regionsHashed"] = std::count_if(
        checkpoint.regions.begin(), checkpoint.regions.end(),
        [](const ProfilerRegionState& r) { return r.hashed; });
    result["hashBytesUsed"] = static_cast<qulonglong>(hashBudgetUsed);
    result["moduleFilterApplied"] = hasFilter;
    if (!result.contains("error")) {
        result["error"] = QString();
    }
    m_appendScanTelemetry("external_tool_profiler_checkpoint", result);
    return result;
}

QVariantMap ExternalToolProfiler::moduleStateToVariant(const ProfilerModuleState& module) const {
    QVariantMap variant;
    variant["name"] = module.name;
    variant["path"] = module.path;
    variant["baseAddress"] = QString::number(module.baseAddress, 16);
    variant["size"] = static_cast<qulonglong>(module.size);
    return variant;
}

QVariantMap ExternalToolProfiler::regionStateToVariant(const ProfilerRegionState& region) const {
    QVariantMap variant;
    variant["baseAddress"] = QString::number(region.baseAddress, 16);
    variant["size"] = static_cast<qulonglong>(region.size);
    variant["protection"] = region.protection;
    variant["memoryType"] = region.memoryType;
    variant["executable"] = region.executable;
    variant["writable"] = region.writable;
    variant["hashed"] = region.hashed;
    return variant;
}

QString ExternalToolProfiler::classifyRegion(
    const ProfilerRegionState& region,
    const QList<ProfilerModuleState>& newlyAddedModules) const {
    for (const auto& module : newlyAddedModules) {
        const uint64_t moduleEnd = module.baseAddress + module.size;
        if (region.baseAddress >= module.baseAddress && region.baseAddress < moduleEnd) {
            return "injected_module_page";
        }
    }
    if (region.executable && region.writable) {
        return "new_executable_writable_page";
    }
    if (region.memoryType == "Image" && region.executable) {
        return "code_page_changed";
    }
    if (region.memoryType == "Image") {
        return "image_data_page_changed";
    }
    if (region.memoryType == "Private") {
        return "private_page_changed";
    }
    return "other";
}

QVariantMap ExternalToolProfiler::getProfilerDiff(const QString& labelA, const QString& labelB, const QVariantMap& options) const {
    QVariantMap result;
    result["success"] = false;

    if (!m_checkpoints.contains(labelA) || !m_checkpoints.contains(labelB)) {
        result["error"] = "Les deux labels doivent correspondre à des checkpoints déjà capturés.";
        return result;
    }

    const auto& a = m_checkpoints.value(labelA);
    const auto& b = m_checkpoints.value(labelB);

    QHash<QString, ProfilerModuleState> modulesA;
    for (const auto& module : a.modules) {
        modulesA.insert(module.name, module);
    }
    QHash<QString, ProfilerModuleState> modulesB;
    for (const auto& module : b.modules) {
        modulesB.insert(module.name, module);
    }

    QVariantList modulesAdded;
    QList<ProfilerModuleState> newlyAddedModules;
    for (auto it = modulesB.constBegin(); it != modulesB.constEnd(); ++it) {
        if (!modulesA.contains(it.key())) {
            modulesAdded.append(moduleStateToVariant(it.value()));
            newlyAddedModules.append(it.value());
        }
    }
    QVariantList modulesRemoved;
    for (auto it = modulesA.constBegin(); it != modulesA.constEnd(); ++it) {
        if (!modulesB.contains(it.key())) {
            modulesRemoved.append(moduleStateToVariant(it.value()));
        }
    }

    QHash<uint64_t, ProfilerRegionState> regionsA;
    for (const auto& region : a.regions) {
        regionsA.insert(region.baseAddress, region);
    }
    QHash<uint64_t, ProfilerRegionState> regionsB;
    for (const auto& region : b.regions) {
        regionsB.insert(region.baseAddress, region);
    }

    const int maxHits = std::clamp(options.value("maxHits", 500).toInt(), 1, 5000);

    QVariantList regionsAdded;
    QVariantList regionsRemoved;
    QVariantList regionsChanged;

    for (auto it = regionsB.constBegin(); it != regionsB.constEnd() && regionsAdded.size() < maxHits; ++it) {
        if (!regionsA.contains(it.key())) {
            QVariantMap entry = regionStateToVariant(it.value());
            entry["classification"] = classifyRegion(it.value(), newlyAddedModules);
            regionsAdded.append(entry);
        }
    }
    for (auto it = regionsA.constBegin(); it != regionsA.constEnd() && regionsRemoved.size() < maxHits; ++it) {
        if (!regionsB.contains(it.key())) {
            regionsRemoved.append(regionStateToVariant(it.value()));
        }
    }
    for (auto it = regionsB.constBegin(); it != regionsB.constEnd() && regionsChanged.size() < maxHits; ++it) {
        if (!regionsA.contains(it.key())) {
            continue;
        }
        const auto& before = regionsA.value(it.key());
        const auto& after = it.value();
        const bool protectionChanged = before.protection != after.protection;
        const bool typeChanged = before.memoryType != after.memoryType;
        const bool contentChanged = before.hashed && after.hashed && before.contentHash != after.contentHash;
        if (!protectionChanged && !typeChanged && !contentChanged) {
            continue;
        }
        QVariantMap entry = regionStateToVariant(after);
        entry["classification"] = classifyRegion(after, newlyAddedModules);
        entry["protectionBefore"] = before.protection;
        entry["protectionAfter"] = after.protection;
        entry["memoryTypeBefore"] = before.memoryType;
        entry["memoryTypeAfter"] = after.memoryType;
        entry["protectionChanged"] = protectionChanged;
        entry["memoryTypeChanged"] = typeChanged;
        entry["contentChanged"] = contentChanged;
        regionsChanged.append(entry);
    }

    result["success"] = true;
    result["labelA"] = labelA;
    result["labelB"] = labelB;
    result["modulesAdded"] = modulesAdded;
    result["modulesRemoved"] = modulesRemoved;
    result["regionsAdded"] = regionsAdded;
    result["regionsRemoved"] = regionsRemoved;
    result["regionsChanged"] = regionsChanged;
    result["modulesAddedCount"] = modulesAdded.size();
    result["modulesRemovedCount"] = modulesRemoved.size();
    result["regionsAddedCount"] = regionsAdded.size();
    result["regionsRemovedCount"] = regionsRemoved.size();
    result["regionsChangedCount"] = regionsChanged.size();
    result["error"] = QString();
    return result;
}

} // namespace killengine
