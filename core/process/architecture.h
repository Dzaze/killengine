#pragma once

#include "process_enumerator.h"

#include <cstdint>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

/**
 * @brief Détecte l'architecture d'un processus Windows.
 *
 * Utilise IsWow64Process2 pour déterminer si un processus est x86 ou x64.
 *
 * Section 7 du cahier des charges:
 *   IsWow64Process2()
 */
class ArchitectureDetector {
public:
    /**
     * @brief Détecte l'architecture d'un processus à partir de son HANDLE.
     * @param processHandle Handle ouvert sur le processus (PROCESS_QUERY_LIMITED_INFORMATION minimum)
     * @return Architecture détectée
     */
    static Architecture detect(HANDLE processHandle);

    /**
     * @brief Détecte l'architecture d'un processus à partir de son PID.
     * Ouvre temporairement le processus, détecte, puis ferme le handle.
     * @param pid PID du processus
     * @return Architecture détectée
     */
    static Architecture detectByPid(uint32_t pid);

    /**
     * @brief Retourne l'architecture du processus courant (KillEngine lui-même).
     */
    static Architecture current();
};

} // namespace killcore