#pragma once

#include "process/process_handle.h"
#include "pointer/pointer_chain.h"

#include <QString>
#include <QByteArray>
#include <functional>
#include <cstdint>

namespace killcore {

/**
 * @brief Type de locator pour retrouver une adresse après redémarrage.
 */
enum class LocatorKind {
    Absolute,      // Adresse absolue (non stable, debug uniquement)
    ModuleOffset,  // Module + offset (stable via ASLR)
    PointerChain,  // Chaîne de pointeurs multi-niveau (stable, requis pour les jeux modernes)
    ClrField,      // Objet CLR retrouvé par type + champ d'identité + valeur
};

struct ClrFieldLocator {
    QString typeSubstring;
    QString identityField;
    QString identityValue;
    QString targetField;

    bool isValid() const {
        return !typeSubstring.isEmpty()
            && !identityField.isEmpty()
            && !identityValue.isEmpty()
            && !targetField.isEmpty();
    }
};

/**
 * @brief Décrit comment retrouver une adresse mémoire après redémarrage.
 *
 * Le locator module_offset est privilégié car il survit à l'ASLR.
 */
struct Locator {
    LocatorKind kind{LocatorKind::ModuleOffset};
    QString     module;     // ex: "KillEngineTestTarget.exe"
    uint64_t    offset{0};  // ex: 0x3F2A0
    uint64_t    lastAddress{0}; // dernière adresse absolue connue (debug)

    /// Chaîne de pointeurs (utilisé quand kind == PointerChain).
    /// Pour les jeux modernes où la cible est allouée dynamiquement sur le tas.
    PointerChain pointerChain;

    /// Locator CLR (utilisé quand kind == ClrField).
    /// L'adresse du tas managé peut changer après GC : on retrouve l'objet par
    /// identité logique avant d'écrire targetField.
    ClrFieldLocator clrField;

    /// Sérialise le locator en chaîne lisible.
    QString toString() const;

    /// Retourne true si le locator est valide.
    bool isValid() const;
};

/**
 * @brief Résout un locator en adresse absolue pour un processus donné.
 *
 * @param handle Le processus attaché.
 * @param locator Le locator à résoudre.
 * @param[out] address L'adresse absolue calculée.
 * @return true si la résolution a réussi.
 */
bool resolveLocatorAddress(const ProcessHandle& handle, const Locator& locator, uint64_t* address);

/// Read-only diagnostic resolver. Does not fall back to lastAddress. The injected
/// reader allows deterministic relocation tests without attaching a process.
struct LocatorProbe {
    bool resolved{false};
    bool readable{false};
    uint64_t address{0};
    QByteArray bytes;
    QString errorCode;
};

using LocatorReadBytes = std::function<QByteArray(uint64_t, size_t)>;
LocatorProbe probeLocator(const Locator& locator,
                          const QList<ProcessModuleInfo>& modules,
                          size_t pointerBytes, size_t readBytes,
                          const LocatorReadBytes& read);

} // namespace killcore
