#pragma once

#include "process_enumerator.h"

#include <cstdint>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

/**
 * @brief Niveaux d'accès à un processus (section 8 du cahier des charges).
 *
 * KillEngine demande uniquement les droits nécessaires.
 */
enum class ProcessAccess : uint32_t {
    /// Lecture seule: QUERY_INFORMATION + VM_READ
    ReadOnly,
    /// Modification: ReadOnly + VM_WRITE + VM_OPERATION
    ReadWrite,
    /// Tous les droits (rarement nécessaire)
    AllAccess,
};

/**
 * @brief RAII wrapper autour d'un HANDLE de processus Windows.
 *
 * Garantit que CloseHandle est toujours appelé.
 * Fournit des méthodes pour lire/écrire la mémoire (délégué aux phases suivantes).
 *
 * Section 7-8 du cahier des charges:
 *   OpenProcess(), CloseHandle()
 *   PROCESS_QUERY_INFORMATION, PROCESS_VM_READ
 *   PROCESS_VM_WRITE, PROCESS_VM_OPERATION
 */
class ProcessHandle {
public:
    ProcessHandle();
    explicit ProcessHandle(uint32_t pid, ProcessAccess access = ProcessAccess::ReadOnly);
    ~ProcessHandle();

    // Non-copyable, movable
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;

    /// Ouvre un processus avec le niveau d'accès spécifié.
    bool open(uint32_t pid, ProcessAccess access = ProcessAccess::ReadOnly);

    /// Ferme le handle.
    void close();

    /// Le handle est-il valide ?
    bool isValid() const;

    /// Retourne le HANDLE brut (pour les API Win32).
#ifdef Q_OS_WIN
    HANDLE rawHandle() const { return m_handle; }
#else
    void* rawHandle() const { return nullptr; }
#endif

    /// PID du processus.
    uint32_t pid() const { return m_pid; }

    /// Niveau d'accès actuel.
    ProcessAccess access() const { return m_access; }

    /// Architecture du processus (mis en cache après ouverture).
    Architecture architecture() const { return m_arch; }

    /// Retourne le nom de l'exécutable.
    QString executableName() const { return m_executableName; }

    /// Retourne le chemin complet de l'exécutable.
    QString executablePath() const;

    /// Met à jour les infos (nom exe, architecture) après ouverture.
    void refreshInfo();

private:
#ifdef Q_OS_WIN
    HANDLE          m_handle{nullptr};
#else
    void*           m_handle{nullptr};
#endif
    uint32_t        m_pid{0};
    ProcessAccess   m_access{ProcessAccess::ReadOnly};
    Architecture    m_arch{Architecture::Unknown};
    QString         m_executableName;

#ifdef Q_OS_WIN
    static DWORD    accessFlags(ProcessAccess access);
#endif
};

} // namespace killcore