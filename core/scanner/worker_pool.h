#pragma once

#include <cstddef>
#include <functional>

namespace killcore {

class WorkerPool {
public:
    static size_t normalizedThreadCount(size_t requestedThreads, size_t taskCount);
    static void runBlocking(
        size_t requestedThreads,
        size_t taskCount,
        const std::function<void(size_t taskIndex)>& task);
};

} // namespace killcore
