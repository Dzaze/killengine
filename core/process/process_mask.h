#pragma once

#include <QString>

#include <cstdint>

namespace killcore {

/// Résultat d'une opération de masquage de processus.
struct ProcessMaskResult {
    bool success{false};
    QString error;
};

/**
 * @brief Masque le nom du processus KillEngine pour éviter la détection par nom.
 *
 * Utilise NtSetInformationProcess avec ProcessBasicInformation pour changer
 * le nom du processus dans le PEB (Process Environment Block).
 *
 * Cette technique est utilisée pour éviter que les anti-cheat (SC2, etc.)
 * détectent KillEngine par son nom de processus.
 */
class ProcessMask {
public:
    /// Masque le nom du processus actuel.
    static ProcessMaskResult maskCurrentProcess(const QString& newName);

    /// Restaure le nom original du processus.
    static ProcessMaskResult restoreOriginalName();
};

} // namespace killcore