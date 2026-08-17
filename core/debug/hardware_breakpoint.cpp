#include "hardware_breakpoint.h"

#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "process/process_enumerator.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace killcore {

#ifdef Q_OS_WIN

namespace {

/// Calcule les bits DR7 pour un breakpoint donné.
/// DR7 layout (x64) :
///   bits 0-1:   L0/G0 (local/global enable pour DR0)
///   bits 2-3:   L1/G1
///   bits 4-5:   L2/G2
///   bits 6-7:   L3/G3
///   bits 16-17: R/W0 (00=execute, 01=write, 11=read/write)
///   bits 18-19: LEN0 (00=1byte, 01=2bytes, 11=4bytes, 10=8bytes)
///   ... (décalage de 4 bits pour chaque breakpoint suivant)
uint32_t computeDr7Bits(int slot, const BreakpointConfig& config) {
    const uint32_t slotShift = static_cast<uint32_t>(slot) * 2;

    // Enable local (L) bit pour ce slot
    uint32_t dr7 = 1u << (slotShift * 1); // L0=bit0, L1=bit2, L2=bit4, L3=bit6

    // R/W bits (bits 16+ pour DR0, 20+ pour DR1, etc.)
    const uint32_t rwShift = 16 + slot * 4;
    dr7 |= (static_cast<uint32_t>(config.type) & 0x3) << rwShift;

    // LEN bits (bits 18+ pour DR0, etc.)
    const uint32_t lenShift = 18 + slot * 4;
    dr7 |= (static_cast<uint32_t>(config.size) & 0x3) << lenShift;

    return dr7;
}

/// Trouve le module qui contient une adresse.
bool findModuleForAddress(uint32_t pid, uint64_t address, QString* moduleName, uint64_t* moduleOffset) {
    const auto modules = ProcessEnumerator::enumerateModules(pid);
    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            if (moduleName) *moduleName = mod.name;
            if (moduleOffset) *moduleOffset = address - mod.baseAddress;
            return true;
        }
    }
    return false;
}

/// Énumère tous les thread IDs d'un processus.
QList<uint32_t> enumerateThreadIds(uint32_t pid) {
    QList<uint32_t> threadIds;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return threadIds;
    }

    THREADENTRY32 te32{};
    te32.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(snapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == pid) {
                threadIds.append(te32.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &te32));
    }

    CloseHandle(snapshot);
    return threadIds;
}

} // namespace

#endif // Q_OS_WIN

HardwareBreakpointSession::HardwareBreakpointSession(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<BreakpointHit>("killcore::BreakpointHit");
    qRegisterMetaType<DebugSessionState>("killcore::DebugSessionState");
}

HardwareBreakpointSession::~HardwareBreakpointSession() {
    stopMonitoring();
    detach();
}

bool HardwareBreakpointSession::attach(uint32_t pid) {
#ifdef Q_OS_WIN
    if (m_state == DebugSessionState::Active) {
        detach();
    }

    setState(DebugSessionState::Attaching);

    // Activer SeDebugPrivilege
    enableDebugPrivilege();

    m_pid = pid;

    // Attacher comme debugger
    if (!DebugActiveProcess(pid)) {
        const DWORD err = GetLastError();
        KE_LOG_ERROR() << "HardwareBreakpoint: DebugActiveProcess failed for PID " << pid
                       << " (error=" << err << ")";
        setState(DebugSessionState::Error);
        return false;
    }

    // Ouvrir un handle pour lire la mémoire pendant le debug
    m_processHandle = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE,
        pid);
    if (!m_processHandle) {
        KE_LOG_ERROR() << "HardwareBreakpoint: OpenProcess failed (error=" << GetLastError() << ")";
        // Tenter de détacher quand même
        DebugActiveProcessStop(pid);
        setState(DebugSessionState::Error);
        return false;
    }

    setState(DebugSessionState::Active);
    KE_LOG_INFO() << "HardwareBreakpoint: attached to PID " << pid << " as debugger";
    return true;
#else
    (void)pid;
    setState(DebugSessionState::Error);
    return false;
#endif
}

void HardwareBreakpointSession::detach() {
#ifdef Q_OS_WIN
    if (m_state != DebugSessionState::Active && m_state != DebugSessionState::Error) {
        return;
    }

    setState(DebugSessionState::Detaching);

    // Retirer tous les breakpoints avant de détacher
    clearBreakpoints();

    if (m_pid > 0) {
        DebugActiveProcessStop(m_pid);
    }

    if (m_processHandle) {
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
    }

    m_pid = 0;
    setState(DebugSessionState::Idle);
    KE_LOG_INFO() << "HardwareBreakpoint: detached";
#endif
}

int HardwareBreakpointSession::setBreakpoint(const BreakpointConfig& config) {
#ifdef Q_OS_WIN
    if (m_state != DebugSessionState::Active) {
        KE_LOG_ERROR() << "HardwareBreakpoint: cannot set breakpoint, session not active";
        return -1;
    }

    // Trouver un slot libre (0-3)
    int slot = -1;
    for (int i = 0; i < 4; ++i) {
        if (!m_breakpointActive[i]) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        KE_LOG_WARN() << "HardwareBreakpoint: all 4 breakpoint slots are occupied";
        return -1;
    }

    m_breakpoints[slot] = config;
    m_breakpointActive[slot] = true;

    // Appliquer le breakpoint à tous les threads existants
    const auto threadIds = enumerateThreadIds(m_pid);
    for (uint32_t tid : threadIds) {
        applyBreakpointsToThread(tid);
    }

    KE_LOG_INFO() << "HardwareBreakpoint: set DR" << slot
                  << " on 0x" << std::hex << config.address
                  << " type=" << static_cast<int>(config.type)
                  << " size=" << static_cast<int>(config.size)
                  << " threads=" << std::dec << threadIds.size();

    return slot;
#else
    (void)config;
    return -1;
#endif
}

bool HardwareBreakpointSession::removeBreakpoint(int index) {
#ifdef Q_OS_WIN
    if (index < 0 || index >= 4 || !m_breakpointActive[index]) {
        return false;
    }

    m_breakpointActive[index] = false;
    m_breakpoints[index] = {};

    // Réappliquer les breakpoints restants à tous les threads
    const auto threadIds = enumerateThreadIds(m_pid);
    for (uint32_t tid : threadIds) {
        applyBreakpointsToThread(tid);
    }

    KE_LOG_INFO() << "HardwareBreakpoint: removed DR" << index;
    return true;
#else
    (void)index;
    return false;
#endif
}

void HardwareBreakpointSession::clearBreakpoints() {
#ifdef Q_OS_WIN
    for (int i = 0; i < 4; ++i) {
        m_breakpointActive[i] = false;
        m_breakpoints[i] = {};
    }

    if (m_pid > 0 && m_processHandle) {
        const auto threadIds = enumerateThreadIds(m_pid);
        for (uint32_t tid : threadIds) {
            applyBreakpointsToThread(tid);
        }
    }
#endif
}

QList<BreakpointHit> HardwareBreakpointSession::takeHits() {
    QList<BreakpointHit> result;
    result.swap(m_pendingHits);
    return result;
}

void HardwareBreakpointSession::startMonitoring(size_t maxHits, int timeoutMs) {
    if (m_monitoring.load()) {
        return;
    }

    m_monitoring.store(true);
    m_stopRequested.store(false);
    m_monitorThread = std::thread([this, maxHits, timeoutMs]() {
        monitorLoop(maxHits, timeoutMs);
    });
}

void HardwareBreakpointSession::monitorBlocking(size_t maxHits, int timeoutMs) {
    if (m_monitoring.load()) {
        return;
    }

    m_monitoring.store(true);
    m_stopRequested.store(false);
    monitorLoop(maxHits, timeoutMs);
}

void HardwareBreakpointSession::stopMonitoring() {
    m_stopRequested.store(true);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
}

bool HardwareBreakpointSession::enableDebugPrivilege() {
#ifdef Q_OS_WIN
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }

    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);

    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        KE_LOG_WARN() << "HardwareBreakpoint: SeDebugPrivilege not granted (error=" << err << ")";
        return false;
    }

    KE_LOG_INFO() << "HardwareBreakpoint: SeDebugPrivilege enabled";
    return true;
#else
    return false;
#endif
}

void HardwareBreakpointSession::setState(DebugSessionState state) {
    m_state = state;
    emit stateChanged(state);
}

#ifdef Q_OS_WIN

bool HardwareBreakpointSession::applyBreakpointsToThread(uint32_t threadId) {
    HANDLE hThread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
        FALSE,
        threadId);
    if (!hThread) {
        return false;
    }

    // SetThreadContext n'est fiable que sur un thread suspendu (doc Win32) :
    // appele hors d'un evenement de debug (ex: setBreakpoint() applique aux
    // threads existants au moment de l'attache) sur un thread qui tourne
    // activement, l'ecriture de DR0-DR7 peut silencieusement ne pas
    // s'appliquer. Meme bug corrige dans core/debug/breakpoint_freeze.cpp.
    const DWORD suspendCount = SuspendThread(hThread);
    const bool suspended = suspendCount != static_cast<DWORD>(-1);

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    if (!GetThreadContext(hThread, &ctx)) {
        if (suspended) ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }

    // Construire DR7 à partir des breakpoints actifs
    uint32_t dr7 = 0;

    // D'abord tout effacer
    ctx.Dr0 = 0;
    ctx.Dr1 = 0;
    ctx.Dr2 = 0;
    ctx.Dr3 = 0;
    ctx.Dr7 = 0;

    // Puis appliquer chaque breakpoint actif
    for (int i = 0; i < 4; ++i) {
        if (!m_breakpointActive[i]) {
            continue;
        }

        const auto& bp = m_breakpoints[i];
        const uint64_t addr = bp.address;

        switch (i) {
            case 0: ctx.Dr0 = addr; break;
            case 1: ctx.Dr1 = addr; break;
            case 2: ctx.Dr2 = addr; break;
            case 3: ctx.Dr3 = addr; break;
        }

        dr7 |= computeDr7Bits(i, bp);
    }

    ctx.Dr7 = dr7;

    const BOOL ok = SetThreadContext(hThread, &ctx);
    if (suspended) ResumeThread(hThread);
    CloseHandle(hThread);

    if (!ok) {
        KE_LOG_WARN() << "HardwareBreakpoint: SetThreadContext failed for TID " << threadId
                      << " (error=" << GetLastError() << ")";
    }

    return ok != FALSE;
}

void HardwareBreakpointSession::monitorLoop(size_t maxHits, int timeoutMs) {
#ifdef Q_OS_WIN
    size_t captured = 0;
    const DWORD startTime = GetTickCount();
    const DWORD timeoutDw = static_cast<DWORD>(timeoutMs);

    while (!m_stopRequested.load() && captured < maxHits) {
        // Timeout
        if (GetTickCount() - startTime > timeoutDw) {
            break;
        }

        DEBUG_EVENT debugEvent{};
        const DWORD waitResult = WaitForDebugEvent(&debugEvent, 100); // 100ms timeout

        if (waitResult == 0) {
            continue; // Timeout, réessayer
        }

        DWORD continueStatus = DBG_CONTINUE;

        switch (debugEvent.dwDebugEventCode) {
            case EXCEPTION_DEBUG_EVENT: {
                const auto& exception = debugEvent.u.Exception;

                // Hardware breakpoint = EXCEPTION_SINGLE_STEP (0x80000003)
                if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP ||
                    exception.ExceptionRecord.ExceptionCode == static_cast<DWORD>(0x80000004)) {

                    const uint64_t instructionAddress = reinterpret_cast<uint64_t>(
                        exception.ExceptionRecord.ExceptionAddress);

                    BreakpointHit hit;
                    if (captureHitFromDebugEvent(
                            exception,
                            instructionAddress,
                            debugEvent.dwThreadId,
                            &hit)) {
                        m_pendingHits.append(hit);
                        m_hitCount.fetch_add(1);
                        ++captured;
                        emit breakpointHit(hit);
                    }

                    // Re-poser le breakpoint (les hardware breakpoints restent actifs,
                    // mais il faut remettre le flag RF dans le contexte pour continuer)
                    applyBreakpointsToThread(debugEvent.dwThreadId);
                } else {
                    // Exception non-gérée par nous, la passer au jeu
                    continueStatus = DBG_EXCEPTION_NOT_HANDLED;
                }
                break;
            }

            case CREATE_THREAD_DEBUG_EVENT:
                // Appliquer les breakpoints au nouveau thread
                applyBreakpointsToThread(debugEvent.dwThreadId);
                break;

            case EXIT_THREAD_DEBUG_EVENT:
            case CREATE_PROCESS_DEBUG_EVENT:
            case EXIT_PROCESS_DEBUG_EVENT:
            case LOAD_DLL_DEBUG_EVENT:
            case UNLOAD_DLL_DEBUG_EVENT:
            case OUTPUT_DEBUG_STRING_EVENT:
                // Événements système, continuer normalement
                break;

            default:
                break;
        }

        ContinueDebugEvent(
            debugEvent.dwProcessId,
            debugEvent.dwThreadId,
            continueStatus);
    }

    m_monitoring.store(false);
    emit monitoringFinished(captured);
    KE_LOG_INFO() << "HardwareBreakpoint: monitoring finished, captured " << captured << " hits";
#endif // Q_OS_WIN
}

bool HardwareBreakpointSession::captureHitFromDebugEvent(
    const EXCEPTION_DEBUG_INFO& exceptionInfo,
    uint64_t instructionAddress,
    uint32_t threadId,
    BreakpointHit* outHit) {
#ifdef Q_OS_WIN
    if (!outHit) return false;

    outHit->instructionPointer = instructionAddress;
    outHit->threadId = threadId;

    // Déterminer quel breakpoint a déclenché (DR6)
    // Pour simplifier, on utilise le premier breakpoint actif qui correspond
    for (int i = 0; i < 4; ++i) {
        if (m_breakpointActive[i]) {
            outHit->address = m_breakpoints[i].address;
            break;
        }
    }

    // Lire la valeur actuelle à l'adresse (après l'écriture)
    if (m_processHandle && outHit->address) {
        uint64_t value = 0;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(m_processHandle,
                              reinterpret_cast<LPCVOID>(outHit->address),
                              &value, sizeof(value), &bytesRead)) {
            outHit->valueAfter = value;
        }
    }

    // Identifier le module de l'instruction
    findModuleForAddress(m_pid, instructionAddress, &outHit->module, &outHit->moduleOffset);

    return true;
#else
    (void)exceptionInfo;
    (void)instructionAddress;
    (void)threadId;
    (void)outHit;
    return false;
#endif
}

#else // Non-Windows stub

void HardwareBreakpointSession::monitorLoop(size_t, int) {}

#endif

QList<BreakpointHit> findWhatWrites(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits) {
    return findWhatWrites(pid, address, size, timeoutMs, maxHits, nullptr);
}

QList<BreakpointHit> findWhatWrites(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation) {

    QList<BreakpointHit> hits;

#ifdef Q_OS_WIN
    HardwareBreakpointSession session;

    if (!session.attach(pid)) {
        return hits;
    }

    BreakpointConfig config;
    config.address = address;
    config.type = BreakpointType::Write;
    config.size = size;

    const int slot = session.setBreakpoint(config);
    if (slot < 0) {
        session.detach();
        return hits;
    }

    // Monitoring bloquant dans le même thread que DebugActiveProcess.
    std::atomic_bool watcherDone{false};
    std::thread cancellationWatcher;
    if (cancellation) {
        cancellationWatcher = std::thread([&session, cancellation, &watcherDone]() {
            while (!watcherDone.load()) {
                if (cancellation->isCancelled()) {
                    session.stopMonitoring();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    session.monitorBlocking(maxHits, timeoutMs);
    watcherDone.store(true);
    if (cancellationWatcher.joinable()) {
        cancellationWatcher.join();
    }
    hits = session.takeHits();
    session.detach();
#endif

    return hits;
}

namespace {

QList<BreakpointHit> findWithBreakpointType(
    uint32_t pid,
    uint64_t address,
    BreakpointType breakpointType,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation) {

    QList<BreakpointHit> hits;

#ifdef Q_OS_WIN
    HardwareBreakpointSession session;

    if (!session.attach(pid)) {
        return hits;
    }

    BreakpointConfig config;
    config.address = address;
    config.type = breakpointType;
    config.size = size;

    const int slot = session.setBreakpoint(config);
    if (slot < 0) {
        session.detach();
        return hits;
    }

    std::atomic_bool watcherDone{false};
    std::thread cancellationWatcher;
    if (cancellation) {
        cancellationWatcher = std::thread([&session, cancellation, &watcherDone]() {
            while (!watcherDone.load()) {
                if (cancellation->isCancelled()) {
                    session.stopMonitoring();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    session.monitorBlocking(maxHits, timeoutMs);
    watcherDone.store(true);
    if (cancellationWatcher.joinable()) {
        cancellationWatcher.join();
    }
    hits = session.takeHits();
    session.detach();
#else
    (void)pid;
    (void)address;
    (void)breakpointType;
    (void)size;
    (void)timeoutMs;
    (void)maxHits;
    (void)cancellation;
#endif

    return hits;
}

} // namespace

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits) {
    return findWhatAccesses(pid, address, size, timeoutMs, maxHits, nullptr);
}

QList<BreakpointHit> findWhatAccesses(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int timeoutMs,
    size_t maxHits,
    const CancellationToken* cancellation) {
    return findWithBreakpointType(
        pid,
        address,
        BreakpointType::Access,
        size,
        timeoutMs,
        maxHits,
        cancellation);
}

} // namespace killcore
