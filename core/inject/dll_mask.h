#pragma once

#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat d'une opération de masquage de DLL.
struct DllMaskResult {
    bool success{false};
    QString error;
};

/**
 * @brief Masque une DLL injectée pour éviter la détection par les anti-cheat.
 *
 * Utilise NtSetInformationProcess avec ProcessModuleInformation pour masquer
 * la DLL dans la liste des modules du processus cible.
 *
 * Cette technique est utilisée pour éviter que les anti-cheat (SC2, etc.)
 * détectent les DLL injectées par KillEngine.
 */
class DllMask {
public:
    /// Masque une DLL dans le processus cible.
    static DllMaskResult maskDll(uint32_t pid, const QString& dllName);

    /// Restaure une DLL masquée.
    static DllMaskResult restoreDll(uint32_t pid, const QString& dllName);
};

} // namespace killcore