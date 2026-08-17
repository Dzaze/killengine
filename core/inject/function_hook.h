#pragma once

#include "process/process_handle.h"
#include "inject/dll_injector.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat de l'installation d'un hook (detour).
struct HookResult {
    bool success{false};
    QString error;
    uint64_t trampolineAddress{0};   ///< Adresse du trampoline (appel original)
    uint64_t hookFunctionAddress{0}; ///< Adresse de la fonction de remplacement
    QByteArray originalBytes;        ///< Bytes originaux sauvegardés pour restore
};

/**
 * @brief Installe un inline hook (detour) sur une fonction distante.
 *
 * Technique : on remplace les premiers bytes de la fonction cible par un JMP vers
 * notre fonction de hook. Un trampoline contient les bytes originaux + un JMP retour
 * pour pouvoir appeler la fonction originale.
 *
 * Layout mémoire dans le processus cible :
 *   [Fonction cible] : JMP hookFunction  (5 ou 14 bytes selon la distance)
 *   [Trampoline]     : <bytes originaux> + JMP (retour vers fonction cible + N)
 *
 * Avantages :
 *   - Intercepte TOUS les appels à la fonction
 *   - Permet de modifier les arguments ou le résultat
 *   - Permet un "freeze invincible" en hookant l'écriture d'une valeur
 *
 * Inconvénients :
 *   - Nécessite du code injecté (DLL ou shellcode)
 *   - Détectable par intégrity checks
 *
 * @param process Processus cible
 * @param targetFunction Adresse de la fonction à hooker
 * @param hookFunction Adresse de la fonction de remplacement (dans le processus cible)
 * @return HookResult
 */
HookResult installInlineHook(const ProcessHandle& process,
                              uint64_t targetFunction,
                              uint64_t hookFunction);

/**
 * @brief Retire un hook et restaure les bytes originaux.
 */
HookResult removeInlineHook(const ProcessHandle& process,
                             uint64_t targetFunction,
                             const QByteArray& originalBytes);

/**
 * @brief Génère un shellcode de JMP vers une adresse distante.
 *
 * Pour les jumps courts (< 2 GB) : JMP rel32 (5 bytes)
 * Pour les jumps longs (>= 2 GB) : MOV RAX, addr64 + JMP RAX (12 bytes)
 */
QByteArray generateJumpShellcode(uint64_t fromAddress, uint64_t toAddress);

/**
 * @brief Calcule la taille minimale à copier pour le trampoline.
 *
 * Décode les instructions à l'adresse cible pour trouver combien de bytes copier
 * sans casser une instruction (au moins 5 ou 14 bytes pour le JMP).
 */
int calculateTrampolineSize(const ProcessHandle& process, uint64_t address, int minBytes);

} // namespace killcore