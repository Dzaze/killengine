#include "candidate_confidence.h"

#include <algorithm>
#include <cmath>

namespace killcore {

const MemoryRegion* findRegionForAddress(
    const QList<MemoryRegion>& regions,
    uint64_t address) {
    // regions DOIT être trié par baseAddress croissant, sans chevauchement
    // (voir le contrat dans le .h) : recherche binaire de la dernière région
    // dont baseAddress <= address, puis vérification de la borne haute.
    auto it = std::upper_bound(
        regions.begin(), regions.end(), address,
        [](uint64_t value, const MemoryRegion& region) { return value < region.baseAddress; });
    if (it == regions.begin()) return nullptr;
    --it;
    if (address >= it->baseAddress && address < it->baseAddress + it->size) {
        return &(*it);
    }
    return nullptr;
}

namespace {

constexpr double kClampMin = 0.0;
constexpr double kClampMax = 1.0;

// Pondération de base (historique disponible et mature, ex. 5+ tours de
// next_scan) : stabilité = signal le plus fort, puis région, puis variante.
// Partagée par computeCandidateConfidence() et computeScanTimeConfidence()
// pour qu'un candidat sans historique et un candidat avec un historique
// encore jeune restent comparables (voir les deux fonctions ci-dessous).
constexpr double kStabilityWeight = 0.45;
constexpr double kRegionWeightBase = 0.40;
constexpr double kVariantWeightBase = 0.15;
constexpr int kRoundsForMaxStabilityWeight = 5;

double clamp01(double v) {
    return std::clamp(v, kClampMin, kClampMax);
}

double regionScoreAtAddress(
    uint64_t address,
    const CandidateConfidenceContext& context) {
    if (!context.regions) {
        return 0.6; // Sans carte mémoire, score neutre.
    }

    const MemoryRegion* region = findRegionForAddress(*context.regions, address);
    if (!region) {
        return 0.1; // Région disparue = très suspect.
    }

    return computeRegionScore(*region, context);
}

double computeVariantScore(const CandidateConfidenceContext& context) {
    return context.secondaryVariant ? 0.85 : 1.0;
}

bool hasCorruptedObservation(const QList<ConfidenceObservation>& observations) {
    return std::any_of(observations.begin(), observations.end(), [](const ConfidenceObservation& obs) {
        return !std::isfinite(obs.previousValue) || !std::isfinite(obs.currentValue);
    });
}

} // namespace

double computeRegionScore(
    const MemoryRegion& region,
    const CandidateConfidenceContext& context) {
    if (!region.readable || region.guarded) {
        return 0.1;
    }

    double score = 0.5;

    if (context.preferWritable && region.writable) {
        score += 0.25;
    } else if (context.preferWritable && !region.writable) {
        score -= 0.1;
    }

    if (region.executable) {
        // Les régions exécutables sont rarement des valeurs de jeu.
        score -= 0.2;
    }

    if (context.preferPrivateOrMapped) {
        if (region.type == MemoryType::Private) {
            score += 0.2;
        } else if (region.type == MemoryType::Mapped) {
            score += 0.1;
        } else if (region.type == MemoryType::Image) {
            score -= 0.15;
        }
    }

    return clamp01(score);
}

double computeCandidateConfidence(
    uint64_t address,
    ValueType /*type*/,
    const CandidateConfidenceContext& context) {
    const double regionScore = regionScoreAtAddress(address, context);

    const QList<ConfidenceObservation>* observations = nullptr;
    if (context.valueHistory) {
        const auto it = context.valueHistory->constFind(address);
        if (it != context.valueHistory->constEnd() && !it.value().isEmpty()) {
            observations = &it.value();
        }
    }

    if (!observations) {
        // Aucun historique réel pour cette adresse (premier next_scan sur ce
        // candidat, ou pas d'historique du tout) : la stabilité n'a rien à
        // apporter. Redistribuer son poids entre région et variante plutôt
        // que de la garder neutre à 0.5 — voir computeScanTimeConfidence().
        return computeScanTimeConfidence(regionScore, context.secondaryVariant);
    }

    if (hasCorruptedObservation(*observations)) {
        // Valeur non finie (NaN/Inf) observée à un moment donné = signal fort
        // de mauvais typage ou de mémoire invalide (ex. octets aléatoires
        // réinterprétés en Float32) — indépendant du nombre de tours ou de
        // la qualité de région : pénalité directe et sévère, pas une simple
        // dilution dans un ratio.
        return clamp01(regionScore * 0.2);
    }

    const double variantScore = computeVariantScore(context);

    // Le poids accordé à la stabilité grandit avec le nombre de tours de
    // next_scan survécus (rendement décroissant, plafond à
    // kRoundsForMaxStabilityWeight tours), au lieu d'être figé à
    // kStabilityWeight dès la première observation : sinon un candidat qui
    // vient tout juste de survivre à UN tour serait jugé aussi stable
    // qu'après cinq, et pire, un candidat SANS AUCUN historique (formule
    // ci-dessus, qui met tout le poids sur région+variante) pourrait
    // paradoxalement scorer plus haut qu'un candidat qui a déjà commencé à
    // faire ses preuves. Le poids retiré à la stabilité est redistribué à
    // région/variante au prorata de leur pondération de base, pour que la
    // formule soit continue avec computeScanTimeConfidence() à 0 tour et
    // avec la pondération classique 0.45/0.40/0.15 au plafond.
    const double roundsRatio = std::min(
        1.0, static_cast<double>(observations->size()) / kRoundsForMaxStabilityWeight);
    const double stabilityWeight = kStabilityWeight * roundsRatio;
    const double remainingWeight = 1.0 - stabilityWeight;
    const double regionWeight = remainingWeight * (kRegionWeightBase / (kRegionWeightBase + kVariantWeightBase));
    const double variantWeight = remainingWeight * (kVariantWeightBase / (kRegionWeightBase + kVariantWeightBase));

    // Stabilité = 1.0 : aucune corruption détectée (sinon retour anticipé
    // ci-dessus), donc l'historique disponible est pleinement en faveur du
    // candidat pour ce que le poids ci-dessus lui accorde.
    const double score = stabilityWeight * 1.0
                       + regionWeight * regionScore
                       + variantWeight * variantScore;

    return clamp01(score);
}

double computeScanTimeConfidence(double regionScore, bool secondaryVariant) {
    // Aucun historique de valeurs n'existe encore au moment du scan lui-même
    // (exact_scan/next_scan) : la composante stabilité de
    // computeCandidateConfidence() n'a donc aucune information réelle à
    // apporter. Plutôt que de la garder à 0.5 et diluer le score (une région
    // parfaite plafonnerait alors à ~0.775, jamais "fiabilité élevée"), on
    // redistribue son poids entre région et variante au prorata de leur
    // pondération de base — équivalent à computeCandidateConfidence() avec
    // stabilityWeight=0 (0 tour survécu).
    const double variantScore = secondaryVariant ? 0.85 : 1.0;
    const double regionWeight = kRegionWeightBase / (kRegionWeightBase + kVariantWeightBase);
    const double variantWeight = kVariantWeightBase / (kRegionWeightBase + kVariantWeightBase);
    const double score = regionWeight * clamp01(regionScore)
                       + variantWeight * variantScore;
    return clamp01(score);
}

} // namespace killcore