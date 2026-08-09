#include "memory_writer.h"

#include "memory/memory_reader.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

MemoryWriter::MemoryWriter(const ProcessHandle& process)
    : m_process(process) {
}

MemoryWriteResult MemoryWriter::write(uint64_t address, const QByteArray& data, bool verify) const {
    MemoryWriteResult result;
    result.address = address;
    result.requestedBytes = static_cast<size_t>(data.size());

    if (data.isEmpty()) {
        result.errorMessage = "No data to write.";
        return result;
    }

#ifdef Q_OS_WIN
    if (!m_process.isValid()) {
        result.errorMessage = "Process handle is not valid.";
        return result;
    }

    MemoryReader reader(m_process);
    const auto previous = reader.read(address, static_cast<size_t>(data.size()));
    if (previous.success || previous.partial) {
        result.previousValue = previous.data;
    }

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(
        m_process.rawHandle(),
        reinterpret_cast<LPVOID>(address),
        data.constData(),
        static_cast<SIZE_T>(data.size()),
        &bytesWritten);

    result.bytesWritten = static_cast<size_t>(bytesWritten);
    if (!ok || bytesWritten != static_cast<SIZE_T>(data.size())) {
        result.errorCode = GetLastError();
        result.errorMessage = QString("WriteProcessMemory failed at 0x%1 (error=%2).")
                                  .arg(address, 0, 16)
                                  .arg(result.errorCode);
        return result;
    }

    result.success = true;
    if (!verify) {
        result.verified = true;
        return result;
    }

    const auto after = reader.read(address, static_cast<size_t>(data.size()));
    result.verified = after.success && after.data == data;
    if (!result.verified) {
        result.errorMessage = "Write verification failed.";
    }
    return result;
#else
    result.errorMessage = "Memory writing is only implemented on Windows.";
    return result;
#endif
}

} // namespace killcore
