#pragma once

#include "memory/memory_region.h"
#include "scanner/scan_types.h"

#include <QHash>
#include <QList>
#include <QString>

#include <cstdint>

namespace killcore {

/**
 * @brief Entrées d'historique d'observation par adresse.
 *
 * Phase 13 - Précision de recherche : le scoring de confiance utilise
 * l'historique des valeurs observées pour mesurer la stabilité d'un candidat.
 */
struct ConfidenceObservation {
    uint64_t address{0};
    double   previousValue{0.0};
    double   currentValue{0.0};
    bool     kept{true};
};

/**
 * @brief Décrit le contexte de scoring pour un candidat.
 *
 * Permet de calculer un score de confiance [0.0, 1.0] qui reflète
 * la vraisemblance qu'une adresse soit bien la cible recherchée.
 */
struct CandidateConfidenceContext {
    const QHash<uint64_t, QList<ConfidenceObservation>>* valueHistory{nullptr}; ///< Historique par adresse.
    const QList<MemoryRegion>* regions{nullptr};        ///< Carte mémoire (pour le filtrage région).
    const QStringList* knownModules{nullptr};           ///< Noms de modules connus (pour le bonus module).
    bool   preferWritable{true};        ///< Les régions writables sont plus pertinentes.
    bool   preferPrivateOrMapped{true}; ///< Privilégie Private/Mapped sur Image.
    bool   secondaryVariant{false};     ///< Variante de moindre priorité (×100, ×1000...).
};

/**
 * @brief Calcule un score de confiance pour un candidat donné.
 *
 * Facteurs pris en compte :
 *   - région writable (bonus)
 *   - type de région Private/Mapped (bonus) vs Image (malus)
 *   - stabilité historique (un candidat qui reste cohérent à travers les
 *     observations gagne en confiance)
 *   - variante secondaire (malus léger pour ×100, ×1000, unsigned...)
 *
 * @return Score dans [0.0, 1.0].
 */
double computeCandidateConfidence(
    uint64_t address,
    ValueType type,
    const CandidateConfidenceContext& context);

/**
 * @brief Score de pertinence [0.0, 1.0] d'une région mémoire déjà connue.
 *
 * Variante de computeCandidateConfidence() pour l'appelant qui a déjà la
 * MemoryRegion sous la main (typiquement : le scan lui-même, une fois par
 * région balayée, pas une fois par candidat) — évite une recherche
 * (même en O(log R)) là où elle n'apporte rien.
 */
double computeRegionScore(
    const MemoryRegion& region,
    const CandidateConfidenceContext& context);

/**
 * @brief Score de confiance calculable au moment du scan, avant tout
 * historique de valeurs (exact_scan / next_scan sur le pipeline normal).
 *
 * Aucune observation n'existe encore à ce stade : la composante stabilité de
 * computeCandidateConfidence() n'a rien à apporter, donc son poids (0.45)
 * est redistribué au prorata entre région et variante plutôt que de diluer
 * le score avec une valeur neutre — une région idéale (writable/private)
 * avec une variante primaire doit pouvoir atteindre une confiance élevée dès
 * le premier scan, pas seulement après plusieurs next_scan.
 */
double computeScanTimeConfidence(double regionScore, bool secondaryVariant);

/**
 * @brief Recherche la région contenant une adresse.
 *
 * `regions` DOIT être trié par baseAddress croissant et ne pas contenir de
 * chevauchement (recherche binaire, pas un scan linéaire) — voir
 * auto_dissect.h::isPointerValid() pour le même contrat.
 *
 * Utilisée en interne par computeCandidateConfidence() quand seule l'adresse
 * est connue. Le scan classique (exact_scan/next_scan) a lui déjà la
 * MemoryRegion exacte sous la main pour chaque région balayée et appelle
 * directement computeRegionScore() une fois par région, pas par candidat —
 * pas de lookup ici sur le chemin chaud.
 *
 * @return Pointeur vers la région (nullptr si introuvable).
 */
const MemoryRegion* findRegionForAddress(
    const QList<MemoryRegion>& regions,
    uint64_t address);

} // namespace killcore