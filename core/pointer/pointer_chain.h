#pragma once

#include "process/process_handle.h"

#include <QString>
#include <QList>
#include <cstdint>

namespace killcore {

/**
 * @brief Décrit une chaîne de pointeurs multi-niveau.
 *
 * Une chaîne part d'une base de module stable (module + offset de base),
 * puis suit une série de déréférencements + offsets jusqu'à l'adresse finale.
 *
 * Exemple pour StarCraft 2 :
 *   module = "SC2.exe"
 *   baseOffset = 0x0123ABC
 *   offsets = [0x50, 0x10, 0x20]
 *
 * Résolution :
 *   addr = base(SC2.exe) + 0x0123ABC
 *   addr = read<uint64_t>(addr) + 0x50
 *   addr = read<uint64_t>(addr) + 0x10
 *   finalAddr = read<uint64_t>(addr) + 0x20
 *
 * Ce locator survit à l'ASLR et aux réallocations de tas entre redémarrages,
 * tant que la structure interne du jeu ne change pas (patch/mise à jour).
 */
struct PointerChain {
    QString       module;         ///< Nom du module de base (ex: "SC2.exe").
    uint64_t      baseOffset{0};  ///< Offset depuis la base du module.
    QList<uint64_t> offsets;      ///< Offsets de chaque niveau (le dernier est appliqué sans déréférencement).

    /// Retourne le nombre de niveaux de déréférencement (= offsets.size()).
    int depth() const { return offsets.size(); }

    /// Sérialise la chaîne en notation lisible.
    /// Ex: "SC2.exe+0x0123ABC->0x50->0x10->0x20"
    QString toString() const;

    /// Retourne true si la chaîne est suffisamment définie pour être résolue.
    bool isValid() const;
};

/**
 * @brief Résultat de résolution d'une chaîne de pointeurs.
 */
struct PointerChainResolveResult {
    bool    success{false};
    uint64_t finalAddress{0};
    QString errorMessage;
    /// Adresses intermédiaires (pour debug UI). base, puis après chaque niveau.
    QList<uint64_t> intermediateAddresses;
};

/**
 * @brief Résout une chaîne de pointeurs en adresse absolue pour un processus donné.
 *
 * @param handle Le processus attaché.
 * @param chain La chaîne à résoudre.
 * @return Résultat avec l'adresse finale et les étapes intermédiaires.
 */
PointerChainResolveResult resolvePointerChain(
    const ProcessHandle& handle,
    const PointerChain& chain);

/**
 * @brief Vérifie qu'une chaîne résout bien vers une adresse attendue.
 *
 * Utile pour valider une chaîne candidat après sauvegarde.
 */
bool pointerChainResolvesTo(
    const ProcessHandle& handle,
    const PointerChain& chain,
    uint64_t expectedAddress);

} // namespace killcore
