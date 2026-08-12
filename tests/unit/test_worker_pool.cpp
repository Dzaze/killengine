#include "scanner/worker_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>

using namespace killcore;

TEST(WorkerPool, NormalizesThreadCount) {
    EXPECT_EQ(WorkerPool::normalizedThreadCount(8, 0), 0u);
    EXPECT_EQ(WorkerPool::normalizedThreadCount(0, 4), 1u);
    EXPECT_EQ(WorkerPool::normalizedThreadCount(8, 4), 4u);
    EXPECT_EQ(WorkerPool::normalizedThreadCount(2, 4), 2u);
}

TEST(WorkerPool, RunsEveryTaskOnce) {
    std::atomic_int total{0};
    WorkerPool::runBlocking(4, 100, [&](size_t) {
        total.fetch_add(1, std::memory_order_relaxed);
    });

    EXPECT_EQ(total.load(std::memory_order_relaxed), 100);
}

TEST(WorkerPool, PropagatesTaskException) {
    EXPECT_THROW(
        WorkerPool::runBlocking(4, 8, [&](size_t taskIndex) {
            if (taskIndex == 3) {
                throw std::runtime_error("boom");
            }
        }),
        std::runtime_error);
}
