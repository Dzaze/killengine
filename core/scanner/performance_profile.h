#pragma once

#include <cstddef>
#include <cstdint>

namespace killcore {

enum class PerformanceMode {
    Auto,
    Eco,
    Normal,
    Performance,
    Max,
};

struct SystemPerformanceInfo {
    size_t logicalProcessors{1};
    uint64_t availableMemoryBytes{0};
};

struct PerformanceProfile {
    PerformanceMode requestedMode{PerformanceMode::Auto};
    PerformanceMode resolvedMode{PerformanceMode::Normal};
    size_t workerThreads{1};
    size_t reserveThreads{1};
    size_t chunkSize{1024 * 1024};
    uint64_t maxInFlightBytes{16ull * 1024ull * 1024ull};
};

SystemPerformanceInfo detectSystemPerformanceInfo();
PerformanceMode parsePerformanceMode(const char* text, PerformanceMode fallback = PerformanceMode::Auto);
const char* performanceModeToString(PerformanceMode mode);
PerformanceProfile makePerformanceProfile(
    PerformanceMode mode,
    const SystemPerformanceInfo& info = detectSystemPerformanceInfo());

} // namespace killcore
