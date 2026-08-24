#include "inprocess_breakpoint.h"

#include "breakpoint_arbiter.h"
#include "inject/dll_injector.h"
#include "logging/logger.h"
#include "process/process_enumerator.h"
#include "process/process_suspend.h"

#ifdef Q_OS_WIN
#include <sddl.h>
#include <windows.h>
#endif

#include <chrono>

namespace killcore {

InProcessBreakpointSession::InProcessBreakpointSession(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<InProcessBreakpointHit>("killcore::InProcessBreakpointHit");
}

InProcessBreakpointSession::~InProcessBreakpointSession() {
    stop();
}

#ifdef Q_OS_WIN

namespace {
struct AppContainerMappingSecurity {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor{nullptr};

    AppContainerMappingSecurity() {
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = FALSE;
    }

    ~AppContainerMappingSecurity() {
        if (descriptor) {
            LocalFree(descriptor);
        }
    }
};

bool buildAppContainerMappingSecurity(AppContainerMappingSecurity* security, QString* error) {
    if (!security) return false;

    constexpr const wchar_t* kSddl =
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GRGW;;;AC)S:(ML;;NW;;;LW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &security->descriptor, nullptr)) {
        if (error) {
            *error = QStringLiteral("ConvertStringSecurityDescriptorToSecurityDescriptorW(IPC breakpoint in-process AppContainer) a échoué (error=%1).")
                         .arg(GetLastError());
        }
        return false;
    }
    security->attributes.lpSecurityDescriptor = security->descriptor;
    return true;
}

// Même encodage que hardware_breakpoint.cpp::computeDr7Bits, mais seul DR0/
// slot 0 nous intéresse ici (une session = une adresse surveillée) : size 1/2/4/8
// octets -> LEN0 (0/1/3/2), écriture seule ou lecture/écriture -> R/W0 (1/3).
uint32_t sizeCodeFor(size_t size) {
    switch (size) {
        case 1: return 0;
        case 2: return 1;
        case 8: return 2;
        default: return 3; // 4 octets, valeur par défaut raisonnable
    }
}

/// Arme DR0 sur toutes les threads DEJA EXISTANTES de la cible (sauf la
/// thread d'installation, deja armee par ses propres moyens), depuis
/// KillEngine.exe -- PAS depuis la DLL injectee. Suspend TOUT le process
/// cible d'un coup via ProcessThreadsSuspendGuard avant d'ecrire les
/// registres de debug, ce qui evite precisement la course qui avait fait
/// planter une cible reelle le 19/08/2026 (une premiere version suspendait
/// les threads UNE A LA FOIS depuis l'interieur de la cible pendant que la
/// thread d'installation continuait a faire des appels Win32 -- risque reel
/// de corruption si une thread suspendue tenait un verrou OS critique
/// pendant que d'autre code du meme process continuait de tourner). Ici,
/// rien dans la cible ne tourne pendant l'armement : tout est fige par un
/// seul controleur externe, puis tout reprend d'un coup.
int armExistingThreadsSafely(uint32_t pid, uint32_t installThreadId,
                              const InProcessBreakpointConfig& config,
                              InProcessBreakpointIpcState* state) {
    ProcessThreadsSuspendGuard suspendGuard(pid, installThreadId);

    const uint32_t rwCode = config.captureExecute ? 0u : (config.captureWrites ? 1u : 3u);
    const uint32_t sizeCode = sizeCodeFor(config.size);
    uint32_t dr7 = 1u; // L0 (bit 0) : active DR0
    dr7 |= (rwCode & 0x3u) << 16;
    dr7 |= (sizeCode & 0x3u) << 18;

    int armed = 0;
    for (const auto& thread : suspendGuard.suspendedThreads()) {
        const HANDLE threadHandle = static_cast<HANDLE>(thread.handle);
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(threadHandle, &ctx)) {
            continue;
        }
        ctx.Dr0 = config.address;
        ctx.Dr6 = 0;
        ctx.Dr7 = dr7;
        if (!SetThreadContext(threadHandle, &ctx)) {
            continue;
        }
        ++armed;
        const long idx = InterlockedIncrement(&state->armedThreadIdCount) - 1;
        if (idx >= 0 && idx < 8) {
            state->armedThreadIds[idx] = thread.threadId;
        }
    }
    state->armedThreadCount += static_cast<uint32_t>(armed);
    return armed;
    // suspendGuard sort de portee ici -> ResumeThread sur toutes les threads
    // suspendues, d'un coup (destructeur RAII).
}

/// Attend jusqu'a 2s que la cible confirme state->disarmed apres une demande
/// d'arret -- desarmement deterministe (piege corrige le 20/08/2026, voir
/// docs/STRATEGY_ROOM.md) : avant cette correction, un timeout de capture
/// sans aucun hit laissait DR7 arme indefiniment dans la cible (le VEH ne le
/// desarmait que "au prochain hit", jamais garanti) pendant que KillEngine
/// considerait deja la ressource libre -- l'arbitre pouvait donc autoriser un
/// autre mecanisme a poser un breakpoint concurrent sur la meme cible.
bool waitForDeterministicDisarm(InProcessBreakpointIpcState* state, uint32_t pid) {
    state->stopRequested = 1;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000)) {
        if (state->disarmed) {
            KE_LOG_INFO() << "InProcessBreakpoint: disarm confirmed pid=" << pid;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    KE_LOG_ERROR() << "InProcessBreakpoint: disarm TIMED OUT pid=" << pid
                   << " -- DR7 peut rester arme dans la cible, PID marque Error dans l'arbitre "
                   << "(tout futur breakpoint materiel sur ce PID sera refuse jusqu'a un changement "
                   << "de cible/PID)";
    return false;
}
} // namespace

InProcessBreakpointResult InProcessBreakpointSession::monitor(const ProcessHandle& process, const InProcessBreakpointConfig& config) {
    InProcessBreakpointResult result;

    if (config.injectedHandlerPath.isEmpty()) {
        result.error = "Chemin de KillEngineInProcessBreakpointHandler.dll manquant.";
        return result;
    }

    const uint32_t pid = process.pid();

    // Proprietaire unique des registres de debug pour ce PID (incident du
    // 19-20/08/2026, voir docs/STRATEGY_ROOM.md et breakpoint_arbiter.h) :
    // refuse proprement plutot que de risquer un conflit avec un breakpoint
    // externe (findWhatWrites/freezeWithBreakpoint) deja actif sur la meme
    // cible. RAII : release() garanti sur CHAQUE retour anticipe ci-dessous,
    // par defaut avec confirmDisarmed(true) (rien n'a encore ete arme).
    HwBreakpointOwnershipGuard breakpointOwnership(
        pid, HwBreakpointOwner::InProcess, "in-process capture", config.address, /*drSlot=*/0);
    if (!breakpointOwnership.acquired()) {
        result.error = breakpointOwnership.error();
        return result;
    }

    wchar_t mappingName[64];
    buildInProcessBreakpointMappingName(pid, mappingName, 64);

    AppContainerMappingSecurity security;
    QString securityError;
    SECURITY_ATTRIBUTES* securityAttributes = nullptr;
    if (buildAppContainerMappingSecurity(&security, &securityError)) {
        securityAttributes = &security.attributes;
    } else {
        KE_LOG_WARN() << "InProcessBreakpoint: AppContainer IPC security unavailable: " << securityError.toStdString();
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, securityAttributes, PAGE_READWRITE,
                                        0, sizeof(InProcessBreakpointIpcState), mappingName);
    if (!mapping) {
        result.error = "CreateFileMapping a échoué (IPC breakpoint in-process).";
        return result;
    }
    // Piege trouve le 19/08/2026 (voir docs/STRATEGY_ROOM.md) : si un mapping du
    // meme nom existe deja (composant deja injecte lors d'un appel precedent sur
    // la meme cible), CreateFileMappingW retourne un handle vers l'objet EXISTANT
    // plutot que d'en creer un nouveau — mais LoadLibraryW sur un module deja
    // charge n'appelle PAS DllMain(DLL_PROCESS_ATTACH) une seconde fois, donc le
    // thread d'installation cote cible ne se relance jamais. Ecraser active=0
    // ci-dessous sans qu'aucun code cote cible ne le remette a 1 laissait l'etat
    // partage dans une configuration incoherente (constate : plantage de
    // KillEngine.exe en conditions reelles). Refuse proprement plutot que
    // d'ecraser un etat deja actif.
    const bool mappingAlreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);
    auto* state = static_cast<InProcessBreakpointIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(InProcessBreakpointIpcState)));
    if (!state) {
        CloseHandle(mapping);
        result.error = "MapViewOfFile a échoué (IPC breakpoint in-process).";
        return result;
    }
    if (mappingAlreadyExisted && state->active) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        result.error = "Un composant breakpoint in-process est déjà actif sur cette cible "
                        "(injecté lors d'un appel précédent). Arrête-le (stop) avant d'en "
                        "démarrer un autre, ou redémarre la cible — la réutilisation sans "
                        "redémarrage a été tentée et retirée après un crash reproductible de "
                        "la cible en test (voir docs/PHASE_TRACKER.md).";
        return result;
    }

    state->active = 0;
    state->installError = 0;
    state->stopRequested = 0;
    state->hitCount = 0;
    state->watchAddress = config.address;
    state->sizeCode = sizeCodeFor(config.size);
    state->rwCode = config.captureExecute ? 0u : (config.captureWrites ? 1u : 3u);
    state->mode = 0; // Capture

    const auto injected = killcore::injectDll(process, config.injectedHandlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        result.error = "Injection du handler breakpoint in-process échouée: " + injected.error;
        return result;
    }

    m_monitoring.store(true);
    m_stopRequested.store(false);

    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(2000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError || !state->active) {
        const long installError = state->installError;
        breakpointOwnership.markDisarming();
        const bool disarmConfirmed = waitForDeterministicDisarm(state, pid);
        breakpointOwnership.confirmDisarmed(disarmConfirmed);
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        m_monitoring.store(false);
        result.error = installError
            ? "Le composant injecté n'a pas pu poser le breakpoint (SetThreadContext/VEH échoué dans la cible)."
            : "Timeout: le composant injecté ne s'est pas installé.";
        return result;
    }

    breakpointOwnership.markActive();
    breakpointOwnership.confirmDisarmed(false); // vraiment arme desormais, plus "rien a desarmer"

    if (config.armExistingThreads) {
        const uint32_t installThreadId = (state->armedThreadIdCount > 0) ? state->armedThreadIds[0] : 0;
        result.existingThreadsArmed = armExistingThreadsSafely(pid, installThreadId, config, state);
        KE_LOG_INFO() << "InProcessBreakpoint: armed " << result.existingThreadsArmed
                      << " pre-existing thread(s) in addition to the install thread, pid=" << pid;
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

        // Sondage best-effort (50ms), même limite documentée que PageGuardSession::
        // monitorRemote (seul le hit le plus récent entre deux sondages est visible).
        const long currentHitCount = state->hitCount;
        if (currentHitCount != lastSeenHitCount) {
            lastSeenHitCount = currentHitCount;
            InProcessBreakpointHit hit;
            hit.instructionPointer = state->lastHitRip;
            hit.threadId = state->lastHitThreadId;
            hit.rax = state->lastRax;
            hit.rcx = state->lastRcx;
            hit.rdx = state->lastRdx;
            hit.rbp = state->lastRbp;
            hit.rsp = state->lastRsp;
            hit.r8 = state->lastR8;
            hit.r9 = state->lastR9;
            hit.xmm0 = QByteArray(reinterpret_cast<const char*>(state->lastXmm0), sizeof(state->lastXmm0));
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

    // Le composant reste charge (retirer proprement demanderait FreeLibrary a
    // distance, plus risque qu'utile pour une capture ponctuelle) mais DR0
    // est desormais desarme de façon deterministe cote cible avant qu'on ne
    // libere la possession aupres de l'arbitre — plus question d'attendre un
    // hypothetique futur hit (piege corrige le 20/08/2026).
    breakpointOwnership.markDisarming();
    const bool disarmConfirmed = waitForDeterministicDisarm(state, pid);
    breakpointOwnership.confirmDisarmed(disarmConfirmed);
    UnmapViewOfFile(state);
    CloseHandle(mapping);

    result.success = true;
    {
        std::lock_guard<std::mutex> lock(m_hitsMutex);
        result.hits = m_pendingHits;
    }
    m_monitoring.store(false);
    emit monitoringFinished(result.hits.size());
    KE_LOG_INFO() << "InProcessBreakpoint: captured " << result.hits.size() << " hits";
    return result;
}

bool InProcessBreakpointSession::startFreeze(
    const ProcessHandle& process,
    uint64_t address,
    size_t size,
    bool captureWrites,
    const QByteArray& frozenBytes,
    const QString& injectedHandlerPath,
    QString* error) {
    if (m_freezing.load()) {
        if (error) *error = "Un freeze in-process est déjà actif.";
        return false;
    }
    if (injectedHandlerPath.isEmpty()) {
        if (error) *error = "Chemin de KillEngineInProcessBreakpointHandler.dll manquant.";
        return false;
    }
    if (frozenBytes.isEmpty() || frozenBytes.size() > 8) {
        if (error) *error = "Valeur figée invalide (1 à 8 octets attendus).";
        return false;
    }

    const uint32_t pid = process.pid();

    // Meme arbitrage que monitor() — voir son commentaire. Ici le guard doit
    // survivre au-dela du retour de cette fonction (le freeze reste actif
    // jusqu'a stop()), donc il vit en membre plutot que sur la pile.
    auto ownership = std::make_unique<HwBreakpointOwnershipGuard>(
        pid, HwBreakpointOwner::InProcess, "in-process freeze", address, /*drSlot=*/0);
    if (!ownership->acquired()) {
        if (error) *error = ownership->error();
        return false;
    }

    wchar_t mappingName[64];
    buildInProcessBreakpointMappingName(pid, mappingName, 64);

    AppContainerMappingSecurity security;
    QString securityError;
    SECURITY_ATTRIBUTES* securityAttributes = nullptr;
    if (buildAppContainerMappingSecurity(&security, &securityError)) {
        securityAttributes = &security.attributes;
    } else {
        KE_LOG_WARN() << "InProcessBreakpoint: AppContainer IPC security unavailable: " << securityError.toStdString();
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, securityAttributes, PAGE_READWRITE,
                                        0, sizeof(InProcessBreakpointIpcState), mappingName);
    if (!mapping) {
        if (error) *error = "CreateFileMapping a échoué (IPC breakpoint in-process).";
        return false;
    }
    // Meme garde que monitor() ci-dessus — voir son commentaire pour le piege
    // complet (composant deja injecte, DllMain ne se relance pas, l'etat
    // partage ne doit pas etre ecrase sans qu'un vrai reinstall ne suive).
    const bool mappingAlreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);
    auto* state = static_cast<InProcessBreakpointIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(InProcessBreakpointIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = "MapViewOfFile a échoué (IPC breakpoint in-process).";
        return false;
    }
    if (mappingAlreadyExisted && state->active) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Un composant breakpoint in-process est déjà actif sur cette cible "
                             "(injecté lors d'un appel précédent). Arrête-le avant d'en démarrer "
                             "un autre, ou redémarre la cible.";
        return false;
    }

    state->active = 0;
    state->installError = 0;
    state->stopRequested = 0;
    state->hitCount = 0;
    state->watchAddress = address;
    state->sizeCode = sizeCodeFor(size);
    state->rwCode = captureWrites ? 1u : 3u;
    state->mode = 1; // RewriteValue
    state->freezeValueSize = static_cast<uint32_t>(frozenBytes.size());
    memcpy(state->freezeValueBytes, frozenBytes.constData(), static_cast<size_t>(frozenBytes.size()));

    const auto injected = killcore::injectDll(process, injectedHandlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Injection du composant breakpoint in-process échouée: " + injected.error;
        return false;
    }

    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(2000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError || !state->active) {
        const long installError = state->installError;
        ownership->markDisarming();
        const bool disarmConfirmed = waitForDeterministicDisarm(state, pid);
        ownership->confirmDisarmed(disarmConfirmed);
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = installError
            ? "Le composant injecté n'a pas pu poser le breakpoint (SetThreadContext/VEH échoué dans la cible)."
            : "Timeout: le composant injecté ne s'est pas installé.";
        return false;
    }

    ownership->markActive();
    ownership->confirmDisarmed(false); // vraiment arme desormais, jusqu'a stop()

    m_freezeMapping = mapping;
    m_freezeState = state;
    m_freezePid = pid;
    m_freezeOwnership = std::move(ownership);
    m_freezing.store(true);
    m_stopRequested.store(false);
    KE_LOG_INFO() << "InProcessBreakpoint: freeze started, armedThreads=" << state->armedThreadCount;
    return true;
}

InProcessBreakpointFreezeStats InProcessBreakpointSession::freezeStats() const {
    InProcessBreakpointFreezeStats stats;
    if (!m_freezeState) {
        return stats;
    }
    stats.active = m_freezeState->active != 0;
    stats.installError = m_freezeState->installError != 0;
    stats.hitCount = static_cast<uint64_t>(m_freezeState->hitCount);
    stats.armedThreadCount = m_freezeState->armedThreadCount;
    return stats;
}

#else
InProcessBreakpointResult InProcessBreakpointSession::monitor(const ProcessHandle&, const InProcessBreakpointConfig&) {
    InProcessBreakpointResult result;
    result.error = "In-process breakpoints are Windows-only";
    return result;
}

bool InProcessBreakpointSession::startFreeze(const ProcessHandle&, uint64_t, size_t, bool, const QByteArray&, const QString&, QString* error) {
    if (error) *error = "In-process breakpoints are Windows-only";
    return false;
}

InProcessBreakpointFreezeStats InProcessBreakpointSession::freezeStats() const {
    return {};
}
#endif

void InProcessBreakpointSession::startAsync(const ProcessHandle& process, const InProcessBreakpointConfig& config) {
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

void InProcessBreakpointSession::stop() {
    m_stopRequested.store(true);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
#ifdef Q_OS_WIN
    if (m_freezing.load()) {
        if (m_freezeState) {
            if (m_freezeOwnership) m_freezeOwnership->markDisarming();
            const bool disarmConfirmed = waitForDeterministicDisarm(m_freezeState, m_freezePid);
            if (m_freezeOwnership) {
                m_freezeOwnership->confirmDisarmed(disarmConfirmed);
                m_freezeOwnership.reset(); // libere aupres de l'arbitre ici (destructeur du guard)
            }
            UnmapViewOfFile(m_freezeState);
            m_freezeState = nullptr;
        }
        if (m_freezeMapping) {
            CloseHandle(static_cast<HANDLE>(m_freezeMapping));
            m_freezeMapping = nullptr;
        }
        m_freezePid = 0;
        m_freezing.store(false);
    }
#endif
}

} // namespace killcore
