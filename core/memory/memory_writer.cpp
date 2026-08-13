#include "memory_writer.h"

#include "memory/memory_reader.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <limits>

namespace killcore {

namespace {

#ifdef Q_OS_WIN

/// Tente de rendre une plage d'adresses accessible en écriture via VirtualProtectEx.
/// Retourne true si la protection a été changée (et remplit oldProtect).
/// En cas d'échec, oldProtect reste inchangé et la fonction retourne false.
bool makeWritable(const ProcessHandle& process, uint64_t address, size_t size, DWORD& oldProtect) {
    if (size == 0) {
        return false;
    }

    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uint64_t pageSize = info.dwPageSize > 0 ? static_cast<uint64_t>(info.dwPageSize) : 4096ull;
    if (address > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(size)) {
        return false;
    }

    // VirtualProtectEx travaille sur des pages entières.
    // On arrondit l'adresse au début de la page et la taille pour couvrir toute la plage.
    const uint64_t pageStart = (address / pageSize) * pageSize;
    const uint64_t endAddress = address + static_cast<uint64_t>(size);
    const uint64_t pageEnd = ((endAddress + pageSize - 1) / pageSize) * pageSize;
    const SIZE_T protectSize = static_cast<SIZE_T>(pageEnd - pageStart);

    DWORD protect = 0;
    if (!VirtualProtectEx(
            process.rawHandle(),
            reinterpret_cast<LPVOID>(pageStart),
            protectSize,
            PAGE_EXECUTE_READWRITE,
            &protect)) {
        const DWORD err = GetLastError();
        KE_LOG_DEBUG() << "MemoryWriter: VirtualProtectEx(PAGE_EXECUTE_READWRITE) failed at 0x"
                       << std::hex << pageStart << " (error=" << std::dec << err << ")";
        return false;
    }

    oldProtect = protect;
    return true;
}

/// Restaure la protection mémoire originale.
bool restoreProtection(const ProcessHandle& process, uint64_t address, size_t size, DWORD targetProtect) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uint64_t pageSize = info.dwPageSize > 0 ? static_cast<uint64_t>(info.dwPageSize) : 4096ull;
    if (address > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(size)) {
        return false;
    }

    const uint64_t pageStart = (address / pageSize) * pageSize;
    const uint64_t endAddress = address + static_cast<uint64_t>(size);
    const uint64_t pageEnd = ((endAddress + pageSize - 1) / pageSize) * pageSize;
    const SIZE_T protectSize = static_cast<SIZE_T>(pageEnd - pageStart);

    DWORD dummy = 0;
    return VirtualProtectEx(
        process.rawHandle(),
        reinterpret_cast<LPVOID>(pageStart),
        protectSize,
        targetProtect,
        &dummy);
}

#endif // Q_OS_WIN

} // namespace

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

    // Tentative 1 : WriteProcessMemory direct (cas normal, page writable).
    SIZE_T bytesWritten = 0;
    BOOL ok = WriteProcessMemory(
        m_process.rawHandle(),
        reinterpret_cast<LPVOID>(address),
        data.constData(),
        static_cast<SIZE_T>(data.size()),
        &bytesWritten);

    result.bytesWritten = static_cast<size_t>(bytesWritten);

    // Tentative 2 : si l'écriture échoue (page protégée), on change la protection
    // mémoire le temps de l'écriture, puis on restaure.
    DWORD originalProtect = 0;
    bool protectionWasChanged = false;

    DWORD lastWriteError = ERROR_SUCCESS;
    if (!ok || bytesWritten != static_cast<SIZE_T>(data.size())) {
        lastWriteError = GetLastError();
        KE_LOG_DEBUG() << "MemoryWriter: WriteProcessMemory failed at 0x" << std::hex << address
                       << " (error=" << std::dec << lastWriteError << "), retrying with VirtualProtectEx...";

        if (makeWritable(m_process, address, static_cast<size_t>(data.size()), originalProtect)) {
            protectionWasChanged = true;

            // Flush instruction cache si la page était exécutable (bonne pratique).
            bytesWritten = 0;
            ok = WriteProcessMemory(
                m_process.rawHandle(),
                reinterpret_cast<LPVOID>(address),
                data.constData(),
                static_cast<SIZE_T>(data.size()),
                &bytesWritten);
            if (!ok || bytesWritten != static_cast<SIZE_T>(data.size())) {
                lastWriteError = GetLastError();
            }
            result.bytesWritten = static_cast<size_t>(bytesWritten);

            // Restaurer la protection originale immédiatement après l'écriture.
            if (!restoreProtection(m_process, address, static_cast<size_t>(data.size()), originalProtect)) {
                KE_LOG_WARN() << "MemoryWriter: failed to restore memory protection at 0x"
                                 << std::hex << address;
            }
        }
    }

    if (!ok || bytesWritten != static_cast<SIZE_T>(data.size())) {
        result.errorCode = lastWriteError == ERROR_SUCCESS ? GetLastError() : lastWriteError;
        result.protectionChanged = protectionWasChanged;
        result.errorMessage = QString("WriteProcessMemory failed at 0x%1 (error=%2)%3.")
                                  .arg(address, 0, 16)
                                  .arg(result.errorCode)
                                  .arg(protectionWasChanged ? " even after VirtualProtectEx" : "");
        return result;
    }

    result.success = true;
    result.protectionChanged = protectionWasChanged;

    if (protectionWasChanged) {
        FlushInstructionCache(
            m_process.rawHandle(),
            reinterpret_cast<LPVOID>(address),
            static_cast<SIZE_T>(data.size()));
    }

    if (!verify) {
        result.verified = true;
        return result;
    }

    const auto after = reader.read(address, static_cast<size_t>(data.size()));
    result.verified = after.success && after.data == data;
    if (!result.verified) {
        result.errorMessage = "Write verification failed (value was overwritten or unreadable).";
    }
    return result;
#else
    result.errorMessage = "Memory writing is only implemented on Windows.";
    return result;
#endif
}

} // namespace killcore
