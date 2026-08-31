#pragma once

#include "process/process_handle.h"

#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat d'une opération anti-anti-debug.
struct AntiDebugResult {
    bool success{false};
    QString error;
    int hooksInstalled{0};
};

/**
 * @brief Contourne les mécanismes anti-debug de SC2 et autres jeux.
 *
 * Hook les fonctions suivantes dans le processus cible :
 *   - IsDebuggerPresent → retourne FALSE
 *   - CheckRemoteDebuggerPresent → retourne FALSE
 *   - NtQueryInformationProcess → retourne FALSE pour ProcessDebugPort
 *   - NtSetInformationProcess → empêche la désactivation du debug
 *
 * Utilise un VEH (Vectored Exception Handler) dans le processus cible
 * pour intercepter les appels, sans nécessiter de DLL injectée.
 */
class AntiDebugSession {
public:
    AntiDebugSession() = default;
    ~AntiDebugSession();

    AntiDebugSession(const AntiDebugSession&) = delete;
    AntiDebugSession& operator=(const AntiDebugSession&) = delete;

    /// Installe les hooks anti-anti-debug dans le processus cible.
    AntiDebugResult start(const ProcessHandle& process);

    /// Retire les hooks anti-anti-debug.
    void stop();

    bool isActive() const { return m_active; }

private:
    bool m_active{false};
    uint32_t m_pid{0};
    HANDLE m_hProcess{nullptr};
    uint64_t m_isDebuggerPresentAddr{0};
    uint64_t m_checkRemoteDebuggerPresentAddr{0};
    uint64_t m_ntQueryInformationProcessAddr{0};
    uint64_t m_ntSetInformationProcessAddr{0};
    BYTE m_originalByte1{0};
    BYTE m_originalByte2{0};
    BYTE m_originalByte3{0};
    BYTE m_originalByte4{0};
};

} // namespace killcore