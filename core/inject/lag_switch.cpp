#include "lag_switch.h"

#include "dll_injector.h"
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
        if (error) *error = "Un lag switch est déjà actif sur cette session.";
        return false;
    }
    if (handlerPath.isEmpty()) {
        if (error) *error = "Chemin du handler requis.";
        return false;
    }

    const uint32_t pid = process.pid();
    wchar_t mappingName[64];
    buildLagSwitchMappingName(pid, mappingName, 64);

    // Un mapping déjà existant = handler déjà injecté (une seule injection par PID).
    HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (existing) {
        CloseHandle(existing);
        if (error) *error = "Un lag switch a déjà été injecté dans cette cible "
                            "(une session par lancement de la cible).";
        return false;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(LagSwitchIpcState), mappingName);
    if (!mapping) {
        if (error) *error = "CreateFileMapping a échoué (IPC lag switch).";
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        if (error) *error = "Un lag switch a déjà été injecté dans cette cible.";
        return false;
    }

    auto* state = static_cast<LagSwitchIpcState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LagSwitchIpcState)));
    if (!state) {
        CloseHandle(mapping);
        if (error) *error = "MapViewOfFile a échoué (IPC lag switch).";
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
        if (error) *error = "Injection du handler lag switch échouée: " + injected.error;
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
        if (error) *error = "Le handler lag switch n'a pas pu poser les hooks (ws2_32.dll introuvable ou MinHook échoué).";
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
    if (error) *error = "Fonctionnalité Windows uniquement.";
    return false;
}

void LagSwitchSession::stop() {}
void LagSwitchSession::setDelayMs(int) {}
LagSwitchStats LagSwitchSession::stats() const { return {}; }

#endif

} // namespace killcore
