#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace killcore {

class CancellationToken {
public:
    void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

private:
    std::atomic_bool m_cancelled{false};
};

struct MemoryReadResult {
    bool       success{false};
    bool       partial{false};
    bool       cancelled{false};
    uint64_t   address{0};
    size_t     requestedBytes{0};
    size_t     bytesRead{0};
    uint32_t   errorCode{0};
    QString    errorMessage;
    QByteArray data;
};

class MemoryReader {
public:
    explicit MemoryReader(const ProcessHandle& process);

    MemoryReadResult read(uint64_t address, size_t size) const;
    MemoryReadResult readChunked(
        uint64_t address,
        size_t size,
        size_t chunkSize = 64 * 1024,
        const CancellationToken* cancellation = nullptr) const;

private:
    const ProcessHandle& m_process;
};

} // namespace killcore