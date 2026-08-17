#include "page_guard.h"

#include "logging/logger.h"
#include "process/process_enumerator.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>

namespace killcore {

void* PageGuardSession::s_currentSession = nullptr;

PageGuardSession::PageGuardSession(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<PageGuardHit>("killcore::PageGuardHit");
}

PageGuardSession::~PageGuardSession() {
    stop();
}

#ifdef Q_OS_WIN

LONG WINAPI PageGuardSession::vectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!s_currentSession || !ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* session = static_cast<PageGuardSession*>(s_currentSession);
    const auto& record = ep->ExceptionRecord;

    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && session->m_rearmPending) {
        DWORD oldProtect = 0;
        VirtualProtectEx(session->m_processHandle,
                         reinterpret_cast<LPVOID>(session->m_pageBase),
                         4096,
                         PAGE_READWRITE | PAGE_GUARD,
                         &oldProtect);
        session->m_rearmPending = false;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // STATUS_GUARD_PAGE_VIOLATION = 0x80000001
    if (record->ExceptionCode != static_cast<DWORD>(0x80000001)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint64_t exceptionAddress = static_cast<uint64_t>(record->ExceptionInformation[1]);
    const bool isWrite = (record->ExceptionInformation[0] == 1);
    const uint64_t instructionPointer = reinterpret_cast<uint64_t>(record->ExceptionAddress);

    const bool inTargetRange =
        exceptionAddress >= session->m_targetAddress &&
        exceptionAddress < session->m_targetAddress + session->m_targetSize;

    if (inTargetRange &&
        ((isWrite && session->m_captureWrites) || (!isWrite && session->m_captureReads))) {
        session->handleViolation(exceptionAddress, instructionPointer, isWrite,
                                 static_cast<uint32_t>(GetCurrentThreadId()));
    }

    // La garde est levée par Windows avant de rejouer l'instruction. On pose le
    // trap flag pour recevoir un single-step après l'instruction, puis seulement
    // là on réarme PAGE_GUARD. Sinon on boucle sur la même instruction.
    ep->ContextRecord->EFlags |= 0x100;
    session->m_rearmPending = true;

    return EXCEPTION_CONTINUE_EXECUTION;
}

void PageGuardSession::handleViolation(uint64_t exceptionAddress, uint64_t instructionPointer,
                                        bool isWrite, uint32_t threadId) {
    PageGuardHit hit;
    hit.monitoredAddress = m_targetAddress;
    hit.instructionPointer = instructionPointer;
    hit.accessAddress = exceptionAddress;
    hit.threadId = threadId;
    hit.isWrite = isWrite;

    const auto modules = ProcessEnumerator::enumerateModules(static_cast<uint32_t>(
        GetProcessId(static_cast<HANDLE>(m_processHandle))));
    for (const auto& mod : modules) {
        if (instructionPointer >= mod.baseAddress && instructionPointer < mod.baseAddress + mod.size) {
            hit.module = mod.name;
            hit.moduleOffset = instructionPointer - mod.baseAddress;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_hitsMutex);
        m_pendingHits.append(hit);
    }
    emit hitCaptured(hit);
}

#endif

PageGuardResult PageGuardSession::monitor(const ProcessHandle& process, const PageGuardConfig& config) {
    PageGuardResult result;

#ifdef Q_OS_WIN
    if (s_currentSession) {
        result.error = "Another PageGuardSession is already active (single-session limit)";
        return result;
    }

    m_targetAddress = config.address;
    m_targetSize = config.size;
    m_processHandle = process.rawHandle();
    m_captureWrites = config.captureWrites;
    m_captureReads = config.captureReads;
    m_pageBase = config.address & ~0xFFFULL;

    if (GetProcessId(static_cast<HANDLE>(m_processHandle)) != GetCurrentProcessId()) {
        result.error = "Out-of-process PAGE_GUARD monitoring requires an injected VEH in the target process";
        s_currentSession = nullptr;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(m_hitsMutex);
        m_pendingHits.clear();
    }

    s_currentSession = this;

    void* vehHandle = AddVectoredExceptionHandler(1, vectoredHandler);
    if (!vehHandle) {
        result.error = "AddVectoredExceptionHandler failed";
        s_currentSession = nullptr;
        return result;
    }

    DWORD oldProtect = 0;
    const DWORD targetProtect = PAGE_READWRITE | PAGE_GUARD;
    if (!VirtualProtectEx(m_processHandle, reinterpret_cast<LPVOID>(m_pageBase), 4096,
                          targetProtect, &oldProtect)) {
        result.error = "VirtualProtectEx failed to set page guard";
        RemoveVectoredExceptionHandler(vehHandle);
        s_currentSession = nullptr;
        return result;
    }
    m_originalProtection = oldProtect;

    m_monitoring.store(true);
    m_stopRequested.store(false);

    const auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config.timeoutMs);

    while (!m_stopRequested.load()) {
        if (std::chrono::steady_clock::now() - startTime >= timeout) {
            result.timedOut = true;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(m_hitsMutex);
            if (static_cast<size_t>(m_pendingHits.size()) >= config.maxHits) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    DWORD tempProtect = 0;
    VirtualProtectEx(m_processHandle, reinterpret_cast<LPVOID>(m_pageBase), 4096,
                     m_originalProtection, &tempProtect);

    RemoveVectoredExceptionHandler(vehHandle);
    s_currentSession = nullptr;

    result.success = true;
    {
        std::lock_guard<std::mutex> lock(m_hitsMutex);
        result.hits = m_pendingHits;
    }
    m_monitoring.store(false);
    emit monitoringFinished(result.hits.size());

    KE_LOG_INFO() << "PageGuard: captured " << result.hits.size() << " hits";
#else
    (void)process;
    (void)config;
    result.error = "Page guards are Windows-only";
#endif

    return result;
}

void PageGuardSession::startAsync(const ProcessHandle& process, const PageGuardConfig& config) {
    if (m_monitoring.load()) {
        return;
    }

    const uint32_t pid = process.pid();
    const ProcessAccess access = process.access();
    m_monitorThread = std::thread([this, pid, access, config]() {
        ProcessHandle ownedProcess(pid, access);
        [[maybe_unused]] auto res = monitor(ownedProcess, config);
    });
}

void PageGuardSession::stop() {
    m_stopRequested.store(true);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
}

QList<PageGuardHit> PageGuardSession::takeHits() {
    QList<PageGuardHit> result;
    std::lock_guard<std::mutex> lock(m_hitsMutex);
    result.swap(m_pendingHits);
    return result;
}

} // namespace killcore
