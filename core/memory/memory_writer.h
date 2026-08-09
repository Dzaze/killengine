#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace killcore {

struct MemoryWriteResult {
    bool success{false};
    bool verified{false};
    uint64_t address{0};
    size_t requestedBytes{0};
    size_t bytesWritten{0};
    uint32_t errorCode{0};
    QString errorMessage;
    QByteArray previousValue;
};

class MemoryWriter {
public:
    explicit MemoryWriter(const ProcessHandle& process);

    MemoryWriteResult write(uint64_t address, const QByteArray& data, bool verify = true) const;

private:
    const ProcessHandle& m_process;
};

} // namespace killcore
