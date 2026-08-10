#include "candidate_confidence.h"

#include <algorithm>
#include <cmath>

namespace killcore {

const MemoryRegion* findRegionForAddress(
    const QList<MemoryRegion>& regions,
    uint64_t address) {
    for (const auto& region : regions) {
        const uint64_t end = region.baseAddress + region.size;
        if (address >= region.baseAddress && address < end) {
            return &region;
        }
    }
    return nullptr;
}

namespace {

constexpr double kClampMin = 0.0;
constexpr double kClampMax = 1.0;

double clamp01(double v) {
    return std::clamp(v, kClampMin, kClampMax);
}

double computeRegionScore(
    uint64_t address,
    const CandidateConfidenceContext& context) {
    if (!context.regions) {
        return 0.6; // Sans carte mémoire, score neutre.
    }

    const MemoryRegion* region = findRegionForAddress(*context.regions, address);
    if (!region) {
        return 0.1; // Région disparue = très suspect.
    }

    if (!region->readable || region->guarded) {
        return 0.1;
    }

    double score = 0.5;

    if (context.preferWritable && region->writable) {
        score += 0.25;
    } else if (context.preferWritable && !region->writable) {
        score -= 0.1;
    }

    if (region->executable) {
        // Les régions exécutables sont rarement des valeurs de jeu.
        score -= 0.2;
    }

    if (context.preferPrivateOrMapped) {
        if (region->type == MemoryType::Private) {
            score += 0.2;
        } else if (region->type == MemoryType::Mapped) {
            score += 0.1;
        } else if (region->type == MemoryType::Image) {
            score -= 0.15;
        }
    }

    return clamp01(score);
}

double computeStabilityScore(
    uint64_t address,
    const CandidateConfidenceContext& context) {
    if (!context.valueHistory) {
        return 0.5;
    }

    const auto it = context.valueHistory->constFind(address);
    if (it == context.valueHistory->constEnd() || it.value().isEmpty()) {
        return 0.5; // Pas d'historique = neutre.
    }

    const auto& observations = it.value();
    int keptCount = 0;
    int consistentCount = 0;

    for (const auto& obs : observations) {
        if (obs.kept) {
            ++keptCount;
        }
        // Une variation cohérente = la valeur précédente correspond à l'attendu.
        if (obs.kept && std::isfinite(obs.previousValue) && std::isfinite(obs.currentValue)) {
            ++consistentCount;
        }
    }

    const double keptRatio = observations.size() > 0
        ? static_cast<double>(keptCount) / static_cast<double>(observations.size())
        : 0.0;
    const double consistentRatio = observations.size() > 0
        ? static_cast<double>(consistentCount) / static_cast<double>(observations.size())
        : 0.0;

    // Plus l'adresse a survécu aux réductions, plus elle est digne de confiance.
    return clamp01(0.4 + 0.4 * keptRatio + 0.2 * consistentRatio);
}

double computeVariantScore(const CandidateConfidenceContext& context) {
    return context.secondaryVariant ? 0.85 : 1.0;
}

} // namespace

double computeCandidateConfidence(
    uint64_t address,
    ValueType /*type*/,
    const CandidateConfidenceContext& context) {
    const double regionScore = computeRegionScore(address, context);
    const double stabilityScore = computeStabilityScore(address, context);
    const double variantScore = computeVariantScore(context);

    // Pondération : la stabilité historique est le signal le plus fort,
    // suivie de la pertinence de la région, puis de la variante.
    const double score = 0.45 * stabilityScore
                       + 0.40 * regionScore
                       + 0.15 * variantScore;

    return clamp01(score);
}

} // namespace killcore