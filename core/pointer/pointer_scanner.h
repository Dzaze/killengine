#pragma once

#include "pointer/pointer_chain.h"
#include "process/process_handle.h"
#include "memory/memory_reader.h"

#include <QList>
#include <QString>
#include <functional>
#include <cstddef>
#include <cstdint>

namespace killcore {

/**
 * @brief Options de configuration pour le scan de chaînes de pointeurs.
 */
struct PointerScanOptions {
    /// Profondeur maximale de la chaîne (nombre de niveaux de déréférencement).
    /// 3 est un bon compromis pour la plupart des jeux. Au-delà, l'explosion
    /// combinatoire rend le scan très lent.
    int maxDepth{3};

    /// Offset maximal accepté entre un pointeur et sa cible à chaque niveau.
    /// Les structures de jeu ont généralement des offsets < 0x2000.
    /// Une valeur plus petite = scan plus rapide + moins de faux positifs.
    uint64_t maxOffset{0x1000};

    /// Nombre maximum de chaînes à retourner. Le scan s'arrête dès qu'atteint.
    size_t maxResults{100};

    /// Si vrai, seules les chaînes dont la base est dans un module statique
    /// (survit à l'ASLR) sont acceptées. Recommandé pour StarCraft 2.
    bool onlyModuleBase{true};

    /// Si non vide, restreint la base aux modules listés (ex: ["SC2.exe"]).
    /// Si vide, tous les modules sont acceptés comme base potentielle.
    QStringList baseModules;

    /// Alignement des pointeurs candidats (8 = pointeurs x64 alignés).
    size_t alignment{8};

    /// Taille des chunks de lecture mémoire.
    size_t chunkSize{1024 * 1024};

    /// Callback de progression (optionnel).
    std::function<void(const struct PointerScanProgress&)> progressCallback;

    /// Jeton d'annulation (optionnel).
    const CancellationToken* cancellation{nullptr};
};

/**
 * @brief Progression du scan de pointeurs.
 */
struct PointerScanProgress {
    int     currentLevel{0};
    int     maxLevel{0};
    size_t  pointersScanned{0};
    size_t  candidatesFound{0};
    size_t  chainsFound{0};
    size_t  bytesScanned{0};
};

/**
 * @brief Résultat d'un scan de chaînes de pointeurs.
 */
struct PointerScanResult {
    bool                 success{false};
    bool                 partial{false};
    bool                 cancelled{false};
    QList<PointerChain>  chains;
    size_t               pointersScanned{0};
    size_t               bytesScanned{0};
    QString              errorMessage;
};

/**
 * @brief Scanne la mémoire pour trouver des chaînes de pointeurs menant à une adresse cible.
 *
 * Algorithme BFS par niveaux :
 *   - Niveau 1 : trouve toutes les adresses A où memory[A] pointe vers [target, target + maxOffset].
 *   - Niveau 2 : pour chaque A, trouve les adresses B où memory[B] pointe vers [A, A + maxOffset].
 *   - ...jusqu'à maxDepth.
 *   - Niveau final : si l'adresse retombe dans un module statique → chaîne valide.
 *
 * @param handle Le processus attaché.
 * @param targetAddress L'adresse absolue de la valeur trouvée (ex: minéraux).
 * @param options Options de configuration.
 * @return Les chaînes de pointeurs candidates, triées par profondeur croissante.
 */
PointerScanResult scanForPointerChains(
    const ProcessHandle& handle,
    uint64_t targetAddress,
    const PointerScanOptions& options = {});

} // namespace killcore