#include "speedhack.h"

#include "inject/dll_injector.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <thread>

namespace killcore {

SpeedhackSession::~SpeedhackSession() {
    stop();
}

#ifdef Q_OS_WIN

bool SpeedhackSession::start(const ProcessHandle& process, double factor, const QString& injectedHandlerPath, QString* error) {
    if (m_active) {
        if (error) *error = "Un speedhack est déjà actif sur cette session.";
        return false;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildSpeedhackMappingName(pid, mappingName, 64);

    // Réutilise un mapping déjà installé pour ce PID s'il existe (le composant
    // reste installé indéfiniment une fois injecté, voir commentaire de
    // classe) : évite une réinjection inutile pour simplement réactiver.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    const bool foundExisting = (mapping != nullptr);

    if (!foundExisting) {
        if (injectedHandlerPath.isEmpty()) {
            if (error) *error = "Chemin de KillEngineSpeedhackHandler.dll manquant.";
            return false;
        }
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                      0, sizeof(SpeedhackIpcState), mappingName);
        if (!mapping) {
            if (error) *error = "CreateFileMapping a échoué (IPC speedhack).";
            return false;
        }
    }
    // Course possible entre notre OpenFileMappingW et ce CreateFileMappingW
    // (une autre instance KillEngine vient de créer le mapping entretemps) :
    // ERROR_ALREADY_EXISTS remonte le même cas que foundExisting ci-dessus.
    const bool mappingAlreadyExisted = foundExisting || (GetLastError() == ERROR_ALREADY_EXISTS);

    auto* state = static_cast<SpeedhackIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SpeedhackIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = "MapViewOfFile a échoué (IPC speedhack).";
        return false;
    }

    if (mappingAlreadyExisted) {
        if (!state->active) {
            // Mapping existant mais composant jamais devenu actif (installError,
            // ou état orphelin) : rien à réinstaller (LoadLibraryW sur un module
            // déjà chargé ne relance pas DllMain, même limitation documentée
            // dans inprocess_breakpoint.h) — refus propre plutôt qu'un état
            // silencieusement faux.
            UnmapViewOfFile(state);
            CloseHandle(mapping);
            if (error) *error = "Un composant speedhack existe déjà pour cette cible mais n'a jamais démarré "
                                 "correctement (installError). Redémarre la cible pour réessayer.";
            return false;
        }
        // Composant déjà installé et fonctionnel pour ce PID : juste régler
        // le facteur, pas besoin de réinjecter.
        state->factor = factor;
        m_mapping = mapping;
        m_state = state;
        m_pid = pid;
        m_active = true;
        KE_LOG_INFO() << "Speedhack: session réactivée sur un composant déjà installé (pid=" << pid << ")";
        return true;
    }

    state->active = 0;
    state->installError = 0;
    state->hooksInstalledMask = 0;
    state->factor = factor;

    const auto injected = killcore::injectDll(process, injectedHandlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = "Injection du composant speedhack échouée: " + injected.error;
        return false;
    }

    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(2000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError || !state->active) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = state->installError
            ? "Le composant injecté n'a trouvé aucune fonction de temps à hooker dans cette cible."
            : "Timeout: le composant speedhack ne s'est pas installé.";
        return false;
    }
    state->factor = factor;

    m_mapping = mapping;
    m_state = state;
    m_pid = pid;
    m_active = true;
    KE_LOG_INFO() << "Speedhack: session démarrée (pid=" << pid << ", factor=" << factor
                  << ", hooksInstalledMask=" << state->hooksInstalledMask << ")";
    return true;
}

bool SpeedhackSession::setFactor(double factor) {
    if (!m_active || !m_state) {
        return false;
    }
    m_state->factor = factor;
    return true;
}

void SpeedhackSession::stop() {
    if (!m_active) {
        return;
    }
    if (m_state) {
        m_state->factor = 1.0; // passthrough transparent, jamais de dé-injection (voir commentaire de classe)
        UnmapViewOfFile(m_state);
    }
    if (m_mapping) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
    }
    KE_LOG_INFO() << "Speedhack: session arrêtée (pid=" << m_pid << "), facteur remis à 1.0";
    m_mapping = nullptr;
    m_state = nullptr;
    m_pid = 0;
    m_active = false;
}

SpeedhackStats SpeedhackSession::stats() const {
    SpeedhackStats stats;
    if (!m_state) {
        return stats;
    }
    stats.active = m_state->active != 0;
    stats.installError = m_state->installError != 0;
    stats.hooksInstalledMask = static_cast<uint32_t>(m_state->hooksInstalledMask);
    stats.factor = m_state->factor;
    return stats;
}

#else

bool SpeedhackSession::start(const ProcessHandle&, double, const QString&, QString* error) {
    if (error) *error = "Speedhack is Windows-only";
    return false;
}

bool SpeedhackSession::setFactor(double) {
    return false;
}

void SpeedhackSession::stop() {}

SpeedhackStats SpeedhackSession::stats() const {
    return {};
}

#endif

} // namespace killcore
