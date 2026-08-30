#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat d'une injection.
struct InjectionResult {
    bool success{false};
    QString error;
    uint64_t remoteThreadHandle{0}; ///< Handle du thread distant (à fermer)
    uint64_t moduleBase{0};          ///< Base de la DLL injectée (si applicable)
};

struct InjectDllOptions {
    /// Charge une copie au nom unique même si la DLL originale est déjà
    /// lisible. Utile pour les handlers dont DllMain doit se rejouer à
    /// chaque capture dans un même PID.
    bool forceUniqueLoad{false};
};

/**
 * @brief Injecte une DLL dans un processus distant via CreateRemoteThread + LoadLibraryW.
 *
 * Technique classique :
 *   1. VirtualAllocEx — alloue de la mémoire dans le processus cible pour le chemin DLL
 *   2. WriteProcessMemory — écrit le chemin unicode de la DLL
 *   3. GetProcAddress(LoadLibraryW) — trouve l'adresse de LoadLibraryW dans kernel32
 *   4. CreateRemoteThread — lance LoadLibraryW dans le processus cible
 *
 * @param process Handle du processus cible (nécessite PROCESS_ALL_ACCESS)
 * @param dllPath Chemin absolu de la DLL à injecter
 * @return InjectionResult
 */
InjectionResult injectDll(const ProcessHandle& process, const QString& dllPath);
InjectionResult injectDll(const ProcessHandle& process, const QString& dllPath, const InjectDllOptions& options);

/**
 * @brief Exécute du shellcode dans un processus distant (sans DLL sur disque).
 *
 * Alloue de la mémoire, écrit le shellcode, crée un thread qui exécute le shellcode.
 * Plus discret que l'injection DLL car aucun fichier sur disque.
 *
 * @param process Handle du processus cible
 * @param shellcode Bytes du shellcode à exécuter
 * @return InjectionResult
 */
InjectionResult injectShellcode(const ProcessHandle& process, const QByteArray& shellcode);

/**
 * @brief Récupère l'adresse d'une fonction dans un module du processus distant.
 *
 * Utilise l'invariance de l'adresse de kernel32 (même adresse dans tous les processus
 * sur la même session Windows).
 *
 * @param moduleName Nom du module (ex: "kernel32.dll")
 * @param functionName Nom de la fonction (ex: "LoadLibraryW")
 * @return Adresse de la fonction, ou 0 si échec
 */
uint64_t getRemoteProcAddress(const QString& moduleName, const QString& functionName);

} // namespace killcore
