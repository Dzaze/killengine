#include "worker_pool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace killcore {

size_t WorkerPool::normalizedThreadCount(size_t requestedThreads, size_t taskCount) {
    if (taskCount == 0) {
        return 0;
    }
    const size_t requested = std::max<size_t>(requestedThreads, 1);
    return std::min(requested, taskCount);
}

void WorkerPool::runBlocking(
    size_t requestedThreads,
    size_t taskCount,
    const std::function<void(size_t taskIndex)>& task) {
    const size_t workerCount = normalizedThreadCount(requestedThreads, taskCount);
    if (workerCount == 0 || !task) {
        return;
    }

    std::atomic_size_t nextTask{0};
    std::exception_ptr firstException;
    std::mutex exceptionMutex;

    auto worker = [&]() {
        while (true) {
            const size_t taskIndex = nextTask.fetch_add(1, std::memory_order_relaxed);
            if (taskIndex >= taskCount) {
                return;
            }

            try {
                task(taskIndex);
            } catch (...) {
                std::lock_guard<std::mutex> lock(exceptionMutex);
                if (!firstException) {
                    firstException = std::current_exception();
                }
                return;
            }
        }
    };

    if (workerCount == 1) {
        worker();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i) {
            workers.emplace_back(worker);
        }
        for (auto& thread : workers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    if (firstException) {
        std::rethrow_exception(firstException);
    }
}

} // namespace killcore
