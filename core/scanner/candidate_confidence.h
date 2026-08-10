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
 * @brief Recherche la région contenant une adresse.
 *
 * @return Pointeur vers la région (nullptr si introuvable).
 */
const MemoryRegion* findRegionForAddress(
    const QList<MemoryRegion>& regions,
    uint64_t address);

} // namespace killcore