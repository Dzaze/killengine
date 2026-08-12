#include "memory_reader.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <algorithm>
#include <limits>

namespace killcore {

void CancellationToken::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

bool CancellationToken::isCancelled() const {
    return m_cancelled.load(std::memory_order_relaxed);
}

MemoryReader::MemoryReader(const ProcessHandle& process)
    : m_process(process) {
}

MemoryReadResult MemoryReader::read(uint64_t address, size_t size) const {
    return readChunked(address, size, size, nullptr);
}

MemoryReadResult MemoryReader::readChunked(
    uint64_t address,
    size_t size,
    size_t chunkSize,
    const CancellationToken* cancellation) const {
    MemoryReadResult result;
    result.address = address;
    result.requestedBytes = size;

    if (size == 0) {
        result.success = true;
        return result;
    }

    if (chunkSize == 0) {
        result.errorMessage = "Chunk size must be greater than zero.";
        return result;
    }

    if (address > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(size)) {
        result.errorMessage = "Read range overflows address space.";
        return result;
    }

#ifdef Q_OS_WIN
    if (!m_process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    result.data.reserve(static_cast<qsizetype>(std::min<size_t>(size, 1024 * 1024)));

    size_t offset = 0;
    while (offset < size) {
        if (cancellation && cancellation->isCancelled()) {
            result.cancelled = true;
            result.partial = result.bytesRead > 0;
            result.success = false;
            result.errorMessage = "Read cancelled.";
            return result;
        }

        const size_t plannedBytesToRead = std::min(chunkSize, size - offset);
        const size_t minimumAttemptSize = std::max<size_t>(
            1,
            std::min<size_t>(64 * 1024, plannedBytesToRead));
        size_t bytesToRead = plannedBytesToRead;
        QByteArray buffer;
        SIZE_T bytesRead = 0;
        const auto currentAddress = address + static_cast<uint64_t>(offset);
        BOOL ok = FALSE;

        while (true) {
            buffer.resize(static_cast<qsizetype>(bytesToRead));
            bytesRead = 0;
            ok = ReadProcessMemory(
                m_process.rawHandle(),
                reinterpret_cast<LPCVOID>(currentAddress),
                buffer.data(),
                bytesToRead,
                &bytesRead);

            if (ok && bytesRead > 0) {
                break;
            }

            if (bytesToRead <= minimumAttemptSize) {
                break;
            }

            bytesToRead = std::max(minimumAttemptSize, bytesToRead / 2);
        }

        if (!ok || bytesRead == 0) {
            result.errorCode = GetLastError();
            result.partial = result.bytesRead > 0;
            result.success = result.partial;
            result.errorMessage = QString("ReadProcessMemory failed at 0x%1 (error=%2).")
                                      .arg(currentAddress, 0, 16)
                                      .arg(result.errorCode);
            KE_LOG_DEBUG() << result.errorMessage.toStdString();
            return result;
        }

        buffer.resize(static_cast<qsizetype>(bytesRead));
        result.data.append(buffer);
        result.bytesRead += static_cast<size_t>(bytesRead);
        offset += static_cast<size_t>(bytesRead);

        if (bytesRead < bytesToRead) {
            result.partial = true;
            break;
        }
    }

    result.success = result.bytesRead == result.requestedBytes;
    result.partial = result.bytesRead > 0 && result.bytesRead < result.requestedBytes;
    return result;
#else
    Q_UNUSED(cancellation);
    result.errorMessage = "Memory reading is only implemented on Windows.";
    return result;
#endif
}

} // namespace killcore
