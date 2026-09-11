#include "lag_switch.h"

#include "dll_injector.h"
#include "localization/localization.h"
#include "logging/logger.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <chrono>
#include <thread>

namespace killcore {

LagSwitchSession::~LagSwitchSession() {
    stop();
}

#ifdef Q_OS_WIN

bool LagSwitchSession::start(const ProcessHandle& process, int delayMs,
                             const QString& handlerPath, QString* error) {
    if (m_active) {
        if (error) *error = KE_TXT("Un lag switch est déjà actif sur cette session.", "A lag switch is already active on this session.");
        return false;
    }
    if (handlerPath.isEmpty()) {
        if (error) *error = KE_TXT("Chemin du handler requis.", "Handler path required.");
        return false;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildLagSwitchMappingName(pid, mappingName, 64);

    // Un mapping déjà existant = handler déjà injecté (une seule injection par PID).
    HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (existing) {
        CloseHandle(existing);
        if (error) *error = KE_TXT("Un lag switch a déjà été injecté dans cette cible "
                            "(une session par lancement de la cible).",
                            "A lag switch has already been injected into this target "
                            "(one session per target launch).");
        return false;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(LagSwitchIpcState), mappingName);
    if (!mapping) {
        if (error) *error = KE_TXT("CreateFileMapping a échoué (IPC lag switch).", "CreateFileMapping failed (lag switch IPC).");
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        if (error) *error = KE_TXT("Un lag switch a déjà été injecté dans cette cible.", "A lag switch has already been injected into this target.");
        return false;
    }

    auto* state = static_cast<LagSwitchIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LagSwitchIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = KE_TXT("MapViewOfFile a échoué (IPC lag switch).", "MapViewOfFile failed (lag switch IPC).");
        return false;
    }

    // Config initiale
    ZeroMemory(state, sizeof(LagSwitchIpcState));
    state->delayMs = static_cast<int32_t>(delayMs);
    state->active = 0;
    state->installError = 0;
    state->totalCalls = 0;
    state->totalDelayed = 0;
    state->removeRequested = 0;

    const auto injected = killcore::injectDll(process, handlerPath);
    if (!injected.success) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = KE_TXT("Injection du handler lag switch échouée : %1", "Lag switch handler injection failed: %1").arg(injected.error);
        return false;
    }

    // Attend que le handler signale active/installError
    const auto installStart = std::chrono::steady_clock::now();
    while (!state->active && !state->installError) {
        if (std::chrono::steady_clock::now() - installStart > std::chrono::milliseconds(6000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (state->installError) {
        UnmapViewOfFile(state);
        CloseHandle(mapping);
        if (error) *error = KE_TXT("Le handler lag switch n'a pas pu poser les hooks (ws2_32.dll introuvable ou MinHook échoué).",
            "The lag switch handler was unable to set the hooks (ws2_32.dll not found or MinHook failed).");
        return false;
    }

    m_mapping = mapping;
    m_state = state;
    m_pid = pid;
    m_active = true;

    KE_LOG_INFO() << "LagSwitchSession: started pid=" << pid << " delayMs=" << delayMs;
    return true;
}

void LagSwitchSession::stop() {
    if (!m_active || !m_state) return;

    m_state->removeRequested = 1;

    // Attend que le handler confirme le retrait des hooks
    const auto removeStart = std::chrono::steady_clock::now();
    while (m_state->active) {
        if (std::chrono::steady_clock::now() - removeStart > std::chrono::milliseconds(3000)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    UnmapViewOfFile(m_state);
    CloseHandle(m_mapping);
    m_mapping = nullptr;
    m_state = nullptr;
    m_active = false;

    KE_LOG_INFO() << "LagSwitchSession: stopped pid=" << m_pid;
}

void LagSwitchSession::setDelayMs(int delayMs) {
    if (m_state) {
        m_state->delayMs = static_cast<int32_t>(delayMs);
    }
}

LagSwitchStats LagSwitchSession::stats() const {
    LagSwitchStats s;
    if (m_state) {
        s.active = m_state->active && !m_state->removeRequested;
        s.installError = m_state->installError != 0;
        s.delayMs = m_state->delayMs;
        s.totalCalls = m_state->totalCalls;
        s.totalDelayed = m_state->totalDelayed;
    }
    return s;
}

#else // non-Windows

bool LagSwitchSession::start(const ProcessHandle&, int, const QString&, QString* error) {
    if (error) *error = KE_TXT("Fonctionnalité réservée à Windows.", "Windows-only feature.");
    return false;
}

void LagSwitchSession::stop() {}
void LagSwitchSession::setDelayMs(int) {}
LagSwitchStats LagSwitchSession::stats() const { return {}; }

#endif

} // namespace killcore
