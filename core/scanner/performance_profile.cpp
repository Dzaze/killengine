#include "performance_profile.h"

#include <algorithm>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace killcore {

namespace {

constexpr uint64_t MiB = 1024ull * 1024ull;
constexpr uint64_t GiB = 1024ull * MiB;

size_t clampWorkerCount(size_t wanted, size_t logicalProcessors, size_t reserveThreads) {
    const size_t processors = std::max<size_t>(logicalProcessors, 1);
    const size_t usable = processors > reserveThreads ? processors - reserveThreads : 1;
    return std::clamp(wanted, static_cast<size_t>(1), usable);
}

uint64_t memoryBudget(uint64_t availableMemoryBytes, uint64_t numerator, uint64_t denominator, uint64_t minimum, uint64_t maximum) {
    if (availableMemoryBytes == 0 || denominator == 0) {
        return minimum;
    }
    const uint64_t budget = (availableMemoryBytes / denominator) * numerator;
    return std::clamp(budget, minimum, maximum);
}

PerformanceMode resolveAutoMode(const SystemPerformanceInfo& info) {
    const size_t processors = std::max<size_t>(info.logicalProcessors, 1);
    if (processors <= 2 || (info.availableMemoryBytes != 0 && info.availableMemoryBytes < 3ull * GiB)) {
        return PerformanceMode::Eco;
    }
    if (processors >= 12 && (info.availableMemoryBytes == 0 || info.availableMemoryBytes >= 12ull * GiB)) {
        return PerformanceMode::Performance;
    }
    return PerformanceMode::Normal;
}

} // namespace

SystemPerformanceInfo detectSystemPerformanceInfo() {
    SystemPerformanceInfo info;
    info.logicalProcessors = std::max<size_t>(std::thread::hardware_concurrency(), 1);

#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        info.availableMemoryBytes = status.ullAvailPhys;
    }
#endif

    return info;
}

PerformanceMode parsePerformanceMode(const char* text, PerformanceMode fallback) {
    if (!text) {
        return fallback;
    }
    if (_stricmp(text, "auto") == 0) return PerformanceMode::Auto;
    if (_stricmp(text, "eco") == 0) return PerformanceMode::Eco;
    if (_stricmp(text, "normal") == 0) return PerformanceMode::Normal;
    if (_stricmp(text, "performance") == 0) return PerformanceMode::Performance;
    if (_stricmp(text, "max") == 0) return PerformanceMode::Max;
    return fallback;
}

const char* performanceModeToString(PerformanceMode mode) {
    switch (mode) {
    case PerformanceMode::Auto: return "Auto";
    case PerformanceMode::Eco: return "Eco";
    case PerformanceMode::Normal: return "Normal";
    case PerformanceMode::Performance: return "Performance";
    case PerformanceMode::Max: return "Max";
    }
    return "Auto";
}

PerformanceProfile makePerformanceProfile(PerformanceMode mode, const SystemPerformanceInfo& info) {
    const PerformanceMode resolvedMode = mode == PerformanceMode::Auto ? resolveAutoMode(info) : mode;
    const size_t processors = std::max<size_t>(info.logicalProcessors, 1);

    PerformanceProfile profile;
    profile.requestedMode = mode;
    profile.resolvedMode = resolvedMode;

    switch (resolvedMode) {
    case PerformanceMode::Eco:
        profile.reserveThreads = processors >= 4 ? 2 : 1;
        profile.workerThreads = clampWorkerCount(1, processors, profile.reserveThreads);
        profile.chunkSize = 512 * 1024;
        profile.maxInFlightBytes = memoryBudget(info.availableMemoryBytes, 1, 64, 8ull * MiB, 64ull * MiB);
        break;
    case PerformanceMode::Normal:
        profile.reserveThreads = processors >= 6 ? 2 : 1;
        profile.workerThreads = clampWorkerCount((processors + 1) / 2, processors, profile.reserveThreads);
        profile.chunkSize = 1024 * 1024;
        profile.maxInFlightBytes = memoryBudget(info.availableMemoryBytes, 1, 32, 16ull * MiB, 256ull * MiB);
        break;
    case PerformanceMode::Performance:
        profile.reserveThreads = processors >= 8 ? 2 : 1;
        profile.workerThreads = clampWorkerCount((processors * 3) / 4, processors, profile.reserveThreads);
        profile.chunkSize = 2 * 1024 * 1024;
        profile.maxInFlightBytes = memoryBudget(info.availableMemoryBytes, 1, 16, 32ull * MiB, 512ull * MiB);
        break;
    case PerformanceMode::Max:
        profile.reserveThreads = 1;
        profile.workerThreads = clampWorkerCount(processors, processors, profile.reserveThreads);
        profile.chunkSize = 4 * 1024 * 1024;
        profile.maxInFlightBytes = memoryBudget(info.availableMemoryBytes, 1, 8, 64ull * MiB, 1024ull * MiB);
        break;
    case PerformanceMode::Auto:
        break;
    }

    return profile;
}

} // namespace killcore
