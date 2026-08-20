#include "page_guard.h"

#include "inject/dll_injector.h"
#include "logging/logger.h"
#include "page_guard_ipc.h"
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
        // Un AddVectoredExceptionHandler posé ici (dans KillEngine.exe) ne
        // reçoit que les exceptions de KillEngine lui-même — jamais celles du
        // processus cible. Le cas réel (surveiller un jeu externe) passe donc
        // par un handler injecté + IPC mémoire partagée, pas par ce chemin.
        s_currentSession = nullptr;
        return monitorRemote(process, config);
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

#ifdef Q_OS_WIN
PageGuardResult PageGuardSession::monitorRemote(const ProcessHandle& process, const PageGuardConfig& config) {
    PageGuardResult result;

    if (config.injectedHandlerPath.isEmpty()) {
        result.error = "Chemin de KillEnginePageGuardHandler.dll manquant.";
        return result;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildPageGuardMappingName(pid, mappingName, 64);

    // Cree AVANT l'injection : la DLL, une fois chargee, ouvre ce mapping par
    // son nom (derive du PID qu'elle lit via GetCurrentProcessId() — les deux
    // cotes n'ont donc besoin d'aucun echange prealable).
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(PageGuardIpcState), mappingName);
    if (!mapping) {
        result.error = "CreateFileMapping a échoué (IPC page guard).";
        return result;
    }
    auto* state = static_cast<PageGuardIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PageGuardIpcState)));
    if (!state) {
        CloseHandle(mapping);
        result.error = "MapViewOfFile a échoué (IPC page guard).";
        return result;
    }

    state->active = 0;
    state->installError = 0;
    state->stopRequested = 0;
    state->hitCount = 0;
    state->watchAddress = config.address;
    state->watchSize = static_cast<uint64_t>(config.size);
    state->captureWrites = config.captureWrites ? 1u : 0u;
    state->captureReads = config.captureReads ? 1u : 0u;

    const auto injected = killcore::injectDll(process, config.injectedHandlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        result.error = "Injection du handler PAGE_GUARD échouée: " + injected.error;
        return result;
    }

    m_monitoring.store(true);
    m_stopRequested.store(false);

    // Attend l'installation cote cible (thread InstallThread de la DLL
    // injectee) avant de commencer a sonder les hits, pour ne pas rater les
    // tout premiers si une ecriture survient immediatement.
    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(2000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError || !state->active) {
        state->stopRequested = 1;
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        m_monitoring.store(false);
        result.error = state->installError
            ? "Le handler injecté n'a pas pu poser la garde (VirtualProtect/VEH échoué dans la cible)."
            : "Timeout: le handler injecté ne s'est pas installé.";
        return result;
    }

    const auto modules = ProcessEnumerator::enumerateModules(pid);
    long lastSeenHitCount = 0;
    const auto startTime = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(config.timeoutMs);

    while (!m_stopRequested.load()) {
        if (std::chrono::steady_clock::now() - startTime >= timeout) {
            result.timedOut = true;
            break;
        }

        // Sondage best-effort (50ms) : si plusieurs hits surviennent entre
        // deux sondages, seul le plus recent est visible (lastHitRip/etc.
        // n'est pas une file). Coherent avec le reste de la classe, deja
        // documentee "moins precis" que le hardware breakpoint — un plafond
        // maxHits/timeout court reste le bon usage (capture ponctuelle, pas
        // un flux exhaustif).
        const long currentHitCount = state->hitCount;
        if (currentHitCount != lastSeenHitCount) {
            lastSeenHitCount = currentHitCount;
            PageGuardHit hit;
            hit.monitoredAddress = config.address;
            hit.instructionPointer = state->lastHitRip;
            hit.accessAddress = state->lastHitAccessAddress;
            hit.threadId = state->lastHitThreadId;
            hit.isWrite = state->lastHitIsWrite != 0;
            for (const auto& mod : modules) {
                if (hit.instructionPointer >= mod.baseAddress && hit.instructionPointer < mod.baseAddress + mod.size) {
                    hit.module = mod.name;
                    hit.moduleOffset = hit.instructionPointer - mod.baseAddress;
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lock(m_hitsMutex);
                m_pendingHits.append(hit);
            }
            emit hitCaptured(hit);
            if (static_cast<size_t>(currentHitCount) >= config.maxHits) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Best-effort : demande l'arret cote cible (le VEH injecte laisse la
    // garde levee au prochain cycle plutot que de re-armer, voir
    // page_guard_handler.cpp). La DLL reste chargee dans la cible — la
    // retirer proprement demanderait FreeLibrary a distance, plus risque
    // qu'utile pour une session de capture ponctuelle.
    state->stopRequested = 1;
    UnmapViewOfFile(state);
    CloseHandle(mapping);

    result.success = true;
    {
        std::lock_guard<std::mutex> lock(m_hitsMutex);
        result.hits = m_pendingHits;
    }
    m_monitoring.store(false);
    emit monitoringFinished(result.hits.size());
    KE_LOG_INFO() << "PageGuard(remote): captured " << result.hits.size() << " hits";
    return result;
}
#else
PageGuardResult PageGuardSession::monitorRemote(const ProcessHandle&, const PageGuardConfig&) {
    PageGuardResult result;
    result.error = "Page guards are Windows-only";
    return result;
}
#endif

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
