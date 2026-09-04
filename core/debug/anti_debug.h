#pragma once

#include "process/process_handle.h"

#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat d'une opération anti-anti-debug.
struct AntiDebugResult {
    bool success{false};
    QString error;
    /// Nombre de champs PEB effectivement patchés (0-3).
    int fieldsPatched{0};
    /// Adresse du PEB de la cible (0 si inconnue).
    uint64_t pebAddress{0};
};

/**
 * @brief Contourne les mécanismes anti-debug de SC2 et autres jeux.
 *
 * Patche directement le PEB (Process Environment Block) du processus cible :
 *   - BeingDebugged = 0          (PEB+0x2, x64)
 *   - NtGlobalFlag &= ~0x70      (PEB+0xBC, x64 — bits de check heap)
 *   - DebugObjectHandle = 0      (PEB+0x1C, x64)
 *
 * C'est la technique classique anti-anti-debug : elle couvre à la fois les
 * chemins qui appellent IsDebuggerPresent/CheckRemoteDebuggerPresent (qui
 * lisent BeingDebugged) et ceux qui lisent le PEB directement.
 *
 * Remplace l'ancienne approche (INT3 écrits sur les fonctions anti-debug
 * sans aucun handler enregistré dans la cible — le module rapportait succès
 * mais le jeu crashait au premier check anti-debug, voir STEALTH-Q dans
 * docs/PHASE_TRACKER.md).
 *
 * stop() restaure les valeurs originales.
 */
class AntiDebugSession {
public:
    AntiDebugSession() = default;
    ~AntiDebugSession();

    AntiDebugSession(const AntiDebugSession&) = delete;
    AntiDebugSession& operator=(const AntiDebugSession&) = delete;

    /// Patche le PEB du processus cible.
    AntiDebugResult start(const ProcessHandle& process);

    /// Restaure les valeurs PEB originales.
    void stop();

    bool isActive() const { return m_active; }

private:
    bool m_active{false};
    uint32_t m_pid{0};
    HANDLE m_hProcess{nullptr};
    uint64_t m_pebAddress{0};
    BYTE m_originalBeingDebugged{0};
    DWORD m_originalNtGlobalFlag{0};
    uint64_t m_originalDebugObjectHandle{0};
};

} // namespace killcore
