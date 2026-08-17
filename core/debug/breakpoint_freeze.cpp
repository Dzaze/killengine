#include "breakpoint_freeze.h"

#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace killcore {

#ifdef Q_OS_WIN
namespace {

uint32_t computeFreezeDr7Bits(int slot, const BreakpointFreezeConfig& config) {
    const uint32_t enableShift = static_cast<uint32_t>(slot) * 2;
    uint32_t dr7 = 1u << enableShift;

    const uint32_t rwShift = 16 + slot * 4;
    dr7 |= (static_cast<uint32_t>(BreakpointType::Write) & 0x3) << rwShift;

    const uint32_t lenShift = 18 + slot * 4;
    dr7 |= (static_cast<uint32_t>(config.size) & 0x3) << lenShift;
    return dr7;
}

bool applyFreezeBreakpointsToThread(uint32_t threadId, const QList<BreakpointFreezeConfig>& configs) {
    HANDLE hThread = OpenThread(
        THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
        FALSE,
        threadId);
    if (!hThread) {
        return false;
    }

    // SetThreadContext n'est fiable que sur un thread suspendu (documentation
    // Win32) : appele sur un thread qui tourne activement (ex: boucle de jeu
    // sans yield), l'ecriture de DR0-DR7 peut silencieusement ne pas
    // s'appliquer. Ce n'est pas necessaire pendant un evenement de debug
    // (le process est deja globalement gele), mais Suspend/Resume est
    // idempotent dans ce cas et reste sans danger a y appeler systematiquement.
    const DWORD suspendCount = SuspendThread(hThread);
    const bool suspended = suspendCount != static_cast<DWORD>(-1);

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(hThread, &ctx)) {
        if (suspended) ResumeThread(hThread);
        CloseHandle(hThread);
        return false;
    }

    ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = 0;
    ctx.Dr6 = 0;
    ctx.Dr7 = 0;

    uint32_t dr7 = 0;
    for (int i = 0; i < configs.size() && i < 4; ++i) {
        switch (i) {
            case 0: ctx.Dr0 = configs[i].address; break;
            case 1: ctx.Dr1 = configs[i].address; break;
            case 2: ctx.Dr2 = configs[i].address; break;
            case 3: ctx.Dr3 = configs[i].address; break;
        }
        dr7 |= computeFreezeDr7Bits(i, configs[i]);
    }
    ctx.Dr7 = dr7;

    const BOOL ok = SetThreadContext(hThread, &ctx);
    if (suspended) ResumeThread(hThread);
    CloseHandle(hThread);
    return ok != FALSE;
}

void clearFreezeBreakpointsForProcess(uint32_t pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 te32{};
    te32.dwSize = sizeof(THREADENTRY32);
    if (Thread32First(snapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == pid) {
                HANDLE hThread = OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                    FALSE,
                    te32.th32ThreadID);
                if (hThread) {
                    const DWORD suspendCount = SuspendThread(hThread);
                    const bool suspended = suspendCount != static_cast<DWORD>(-1);
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(hThread, &ctx)) {
                        ctx.Dr0 = ctx.Dr1 = ctx.Dr2 = ctx.Dr3 = ctx.Dr6 = ctx.Dr7 = 0;
                        SetThreadContext(hThread, &ctx);
                    }
                    if (suspended) ResumeThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(snapshot, &te32));
    }
    CloseHandle(snapshot);
}

void applyFreezeBreakpointsToExistingThreads(uint32_t pid, const QList<BreakpointFreezeConfig>& configs) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 te32{};
    te32.dwSize = sizeof(THREADENTRY32);
    if (Thread32First(snapshot, &te32)) {
        do {
            if (te32.th32OwnerProcessID == pid) {
                applyFreezeBreakpointsToThread(te32.th32ThreadID, configs);
            }
        } while (Thread32Next(snapshot, &te32));
    }
    CloseHandle(snapshot);
}

} // namespace
#endif

BreakpointFreezeManager::BreakpointFreezeManager(QObject* parent)
    : QObject(parent) {
}

BreakpointFreezeManager::~BreakpointFreezeManager() {
    stop();
}

bool BreakpointFreezeManager::start(uint32_t pid, const BreakpointFreezeConfig& config) {
    QList<BreakpointFreezeConfig> configs;
    configs.append(config);
    return startMulti(pid, configs);
}

bool BreakpointFreezeManager::startMulti(uint32_t pid, const QList<BreakpointFreezeConfig>& configs) {
#ifdef Q_OS_WIN
    if (m_active.load()) {
        stop();
    }

    if (configs.isEmpty() || configs.size() > 4) {
        emit freezeError("Invalid freeze config: need 1-4 addresses");
        return false;
    }

    m_pid = pid;
    m_configs = configs;
    m_stats = {};

    m_stopRequested.store(false);
    auto startSignal = std::make_shared<std::promise<bool>>();
    auto startFuture = startSignal->get_future();
    m_freezeThread = std::thread([this, startSignal]() { freezeLoop(startSignal); });

    const bool started = startFuture.get();
    if (!started) {
        if (m_freezeThread.joinable()) {
            m_freezeThread.join();
        }
        return false;
    }

    KE_LOG_INFO() << "BreakpointFreeze: started for PID " << pid
                  << " with " << configs.size() << " addresses";
    return true;
#else
    (void)pid;
    (void)configs;
    emit freezeError("Hardware breakpoint freeze is Windows-only");
    return false;
#endif
}

void BreakpointFreezeManager::stop() {
#ifdef Q_OS_WIN
    if (!m_active.load() && !m_freezeThread.joinable()) return;

    m_stopRequested.store(true);

    if (m_freezeThread.joinable()) {
        m_freezeThread.join();
    }
    BreakpointFreezeStats statsSnapshot = stats();
    KE_LOG_INFO() << "BreakpointFreeze: stopped. Stats: hits=" << statsSnapshot.totalHits
                  << " rewrites=" << statsSnapshot.rewrites << " blocks=" << statsSnapshot.blocks
                  << " errors=" << statsSnapshot.errors;
#endif
}

BreakpointFreezeStats BreakpointFreezeManager::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void BreakpointFreezeManager::freezeLoop(std::shared_ptr<std::promise<bool>> startSignal) {
#ifdef Q_OS_WIN
    bool attached = false;
    bool signalled = false;

    auto failStart = [&](const QString& error) {
        emit freezeError(error);
        if (!signalled) {
            startSignal->set_value(false);
            signalled = true;
        }
    };

    HardwareBreakpointSession::enableDebugPrivilege();

    m_processHandle = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE,
        m_pid);
    if (!m_processHandle) {
        failStart("OpenProcess failed for breakpoint freeze");
        return;
    }

    if (!DebugActiveProcess(m_pid)) {
        const DWORD err = GetLastError();
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
        failStart(QStringLiteral("DebugActiveProcess failed for breakpoint freeze (error=%1)").arg(err));
        return;
    }
    attached = true;

    applyFreezeBreakpointsToExistingThreads(m_pid, m_configs);
    m_active.store(true);
    startSignal->set_value(true);
    signalled = true;

    KE_LOG_INFO() << "BreakpointFreeze: monitoring loop started";

    while (!m_stopRequested.load()) {
        DEBUG_EVENT debugEvent{};
        const DWORD waitResult = WaitForDebugEvent(&debugEvent, 100);

        if (waitResult == 0) {
            continue;
        }

        DWORD continueStatus = DBG_CONTINUE;
        handleDebugEvent(debugEvent, &continueStatus);

        ContinueDebugEvent(
            debugEvent.dwProcessId,
            debugEvent.dwThreadId,
            continueStatus);
    }

    clearFreezeBreakpointsForProcess(m_pid);
    if (attached) {
        DebugActiveProcessStop(m_pid);
    }
    if (m_processHandle) {
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
    }
    m_active.store(false);
    KE_LOG_INFO() << "BreakpointFreeze: monitoring loop ended";
#endif
}

void BreakpointFreezeManager::handleDebugEvent(const DEBUG_EVENT& event, unsigned long* continueStatus) {
#ifdef Q_OS_WIN
    switch (event.dwDebugEventCode) {
        case EXCEPTION_DEBUG_EVENT: {
            const auto& exception = event.u.Exception;

            if (exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP ||
                exception.ExceptionRecord.ExceptionCode == static_cast<DWORD>(0x80000004)) {

                std::vector<int> triggeredSlots;
                HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, event.dwThreadId);
                if (hThread) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_CONTROL;
                    if (GetThreadContext(hThread, &ctx)) {
                        for (int i = 0; i < m_configs.size() && i < 4; ++i) {
                            if ((ctx.Dr6 & (1ULL << i)) != 0) {
                                triggeredSlots.push_back(i);
                            }
                        }
                        ctx.Dr6 = 0;
                        ctx.EFlags |= 0x10000; // RF (Resume Flag)
                        SetThreadContext(hThread, &ctx);
                    }
                    CloseHandle(hThread);
                }

                if (triggeredSlots.empty() && m_configs.size() == 1) {
                    triggeredSlots.push_back(0);
                }

                {
                    std::lock_guard<std::mutex> lock(m_statsMutex);
                    m_stats.totalHits += triggeredSlots.size();
                }

                for (int slot : triggeredSlots) {
                    if (slot < 0 || slot >= m_configs.size()) {
                        continue;
                    }
                    const auto& config = m_configs[slot];
                    if (config.mode == BreakpointFreezeMode::Capture) {
                        continue;
                    }

                    if (config.mode == BreakpointFreezeMode::RewriteValue) {
                        uint64_t gameValue = 0;
                        SIZE_T bytesRead = 0;
                        if (ReadProcessMemory(m_processHandle,
                                              reinterpret_cast<LPCVOID>(config.address),
                                              &gameValue, sizeof(gameValue), &bytesRead)) {
                            if (rewriteValue(config.address, config.frozenValue)) {
                                {
                                    std::lock_guard<std::mutex> lock(m_statsMutex);
                                    m_stats.rewrites++;
                                }

                                uint64_t rewrittenValue = 0;
                                std::memcpy(&rewrittenValue, config.frozenValue.constData(),
                                            std::min<size_t>(config.frozenValue.size(), sizeof(rewrittenValue)));

                                emit writeIntercepted(config.address, gameValue, rewrittenValue);
                            } else {
                                std::lock_guard<std::mutex> lock(m_statsMutex);
                                m_stats.errors++;
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(m_statsMutex);
                            m_stats.errors++;
                        }
                    } else if (config.mode == BreakpointFreezeMode::BlockWrite) {
                        if (blockWriteByContext(event.dwThreadId, config.address)) {
                            std::lock_guard<std::mutex> lock(m_statsMutex);
                            m_stats.blocks++;
                        } else {
                            std::lock_guard<std::mutex> lock(m_statsMutex);
                            m_stats.errors++;
                        }
                    }
                }

                *continueStatus = DBG_CONTINUE;
            } else {
                *continueStatus = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }

        case CREATE_THREAD_DEBUG_EVENT: {
            applyFreezeBreakpointsToThread(event.dwThreadId, m_configs);
            *continueStatus = DBG_CONTINUE;
            break;
        }

        case EXIT_PROCESS_DEBUG_EVENT:
            m_stopRequested.store(true);
            emit freezeError("Target process exited");
            *continueStatus = DBG_CONTINUE;
            break;

        default:
            *continueStatus = DBG_CONTINUE;
            break;
    }
#else
    (void)event;
    (void)continueStatus;
#endif
}

bool BreakpointFreezeManager::rewriteValue(uint64_t address, const QByteArray& value) {
#ifdef Q_OS_WIN
    if (!m_processHandle || value.isEmpty()) return false;

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(
        m_processHandle,
        reinterpret_cast<LPVOID>(address),
        value.constData(),
        static_cast<SIZE_T>(value.size()),
        &bytesWritten);

    return ok && bytesWritten == static_cast<SIZE_T>(value.size());
#else
    (void)address;
    (void)value;
    return false;
#endif
}

bool BreakpointFreezeManager::blockWriteByContext(uint32_t threadId, uint64_t address) {
#ifdef Q_OS_WIN
    // Mode BlockWrite avancé : nécessiterait de décoder l'instruction pour identifier
    // le registre destination. Pour la v1, on fallback sur RewriteValue.
    (void)threadId;
    (void)address;

    for (const auto& config : m_configs) {
        if (config.mode == BreakpointFreezeMode::BlockWrite && config.address == address) {
            return rewriteValue(config.address, config.frozenValue);
        }
    }
    return false;
#else
    (void)threadId;
    (void)address;
    return false;
#endif
}

} // namespace killcore
