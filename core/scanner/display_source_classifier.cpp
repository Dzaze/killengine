#include "display_source_classifier.h"

#include "localization/localization.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace killcore {

namespace {

struct InstructionTally {
    uint64_t instructionPointer{0};
    size_t count{0};
};

InstructionTally dominantInstruction(const QList<WriteObservation>& observations, size_t* distinctCount) {
    QHash<uint64_t, size_t> counts;
    for (const auto& obs : observations) {
        counts[obs.instructionPointer] += 1;
    }
    *distinctCount = static_cast<size_t>(counts.size());

    InstructionTally best;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > best.count) {
            best.instructionPointer = it.key();
            best.count = it.value();
        }
    }
    return best;
}

} // namespace

FieldStabilityClassification classifyFieldStability(
    const QList<WriteObservation>& observations,
    const FieldStabilityOptions& options) {
    FieldStabilityClassification result;
    result.writeCount = static_cast<size_t>(observations.size());

    if (observations.isEmpty()) {
        result.verdict = FieldStabilityVerdict::NoWritesObserved;
        result.rationale = KE_TXT(
                            "Aucune écriture observée pendant la fenêtre de capture -- "
                            "ne veut pas forcément dire que le champ est stable, la fenêtre "
                            "était peut-être trop courte ou l'action qui le modifie ne s'est pas produite.",
                            "No write observed during the capture window -- this doesn't necessarily "
                            "mean the field is stable, the window may have been too short or the "
                            "action that modifies it may not have happened.");
        return result;
    }

    size_t distinctCount = 0;
    const InstructionTally dominant = dominantInstruction(observations, &distinctCount);
    result.distinctInstructionCount = distinctCount;
    result.dominantInstructionPointer = dominant.instructionPointer;
    result.dominantInstructionShare = result.writeCount > 0
        ? static_cast<double>(dominant.count) / static_cast<double>(result.writeCount)
        : 0.0;

    if (result.writeCount < options.minHitsForPattern) {
        result.verdict = FieldStabilityVerdict::InsufficientData;
        result.rationale = KE_TXT(
            "Seulement %1 écriture(s) observée(s) -- pas assez pour distinguer un motif "
            "régulier (champ affiché recalculé) d'une écriture événementielle isolée.",
            "Only %1 write(s) observed -- not enough to distinguish a regular pattern "
            "(recalculated displayed field) from an isolated event-driven write.")
            .arg(result.writeCount);
        return result;
    }

    QList<uint32_t> elapsed;
    elapsed.reserve(observations.size());
    for (const auto& obs : observations) {
        elapsed.append(obs.captureElapsedMs);
    }
    std::sort(elapsed.begin(), elapsed.end());

    QList<double> intervals;
    intervals.reserve(elapsed.size() - 1);
    for (int i = 1; i < elapsed.size(); ++i) {
        intervals.append(static_cast<double>(elapsed[i]) - static_cast<double>(elapsed[i - 1]));
    }

    double meanIntervalMs = 0.0;
    for (double v : intervals) {
        meanIntervalMs += v;
    }
    meanIntervalMs = intervals.isEmpty() ? 0.0 : meanIntervalMs / intervals.size();

    double variance = 0.0;
    for (double v : intervals) {
        variance += (v - meanIntervalMs) * (v - meanIntervalMs);
    }
    variance = intervals.isEmpty() ? 0.0 : variance / intervals.size();
    const double stddev = std::sqrt(variance);
    const double coefficientOfVariation = meanIntervalMs > 0.0 ? stddev / meanIntervalMs : 0.0;

    result.meanIntervalMs = meanIntervalMs;
    result.intervalCoefficientOfVariation = coefficientOfVariation;

    const bool regularInterval = coefficientOfVariation <= options.regularIntervalCoefficientOfVariation;
    const bool dominantInstructionFound = result.dominantInstructionShare >= options.dominantInstructionShare;

    if (regularInterval && dominantInstructionFound) {
        result.verdict = FieldStabilityVerdict::LikelyDerivedDisplay;
        result.rationale = KE_TXT(
            "%1 écritures régulières (~%2 ms d'intervalle, coefficient de variation %3), "
            "%4%% venant de la même instruction (0x%5) -- motif cohérent avec un champ "
            "affiché recalculé à chaque tick depuis une autre source. Geler/patcher cette "
            "adresse directement risque de ne pas tenir ; chercher la source en amont plutôt.",
            "%1 regular writes (~%2 ms interval, coefficient of variation %3), "
            "%4%% coming from the same instruction (0x%5) -- pattern consistent with a "
            "displayed field recalculated on every tick from another source. Freezing/patching "
            "this address directly risks not holding; look for the source upstream instead.")
            .arg(result.writeCount)
            .arg(meanIntervalMs, 0, 'f', 0)
            .arg(coefficientOfVariation, 0, 'f', 2)
            .arg(result.dominantInstructionShare * 100.0, 0, 'f', 0)
            .arg(QString::number(dominant.instructionPointer, 16).toUpper());
    } else {
        result.verdict = FieldStabilityVerdict::LikelyEventDriven;
        result.rationale = KE_TXT(
            "%1 écritures observées mais motif pas régulier et/ou pas dominé par une seule "
            "instruction (%2 instruction(s) distincte(s), intervalle ~%3 ms, coefficient de "
            "variation %4) -- plus cohérent avec des écritures liées à de vrais événements "
            "qu'avec un recalcul de tick fixe. Ne garantit pas que ce champ est une source "
            "sûre, juste que ce n'est pas la signature d'un champ affiché interpolé.",
            "%1 writes observed but the pattern is not regular and/or not dominated by a "
            "single instruction (%2 distinct instruction(s), ~%3 ms interval, coefficient of "
            "variation %4) -- more consistent with writes tied to real events than a fixed "
            "tick recalculation. This doesn't guarantee this field is a safe source, only "
            "that it's not the signature of an interpolated displayed field.")
            .arg(result.writeCount)
            .arg(distinctCount)
            .arg(meanIntervalMs, 0, 'f', 0)
            .arg(coefficientOfVariation, 0, 'f', 2);
    }

    return result;
}

FieldStabilityClassification classifyFieldStabilityLive(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size,
    int captureWindowMs,
    size_t maxHits,
    const FieldStabilityOptions& options) {
    const QList<BreakpointHit> hits = findWhatWrites(pid, address, size, captureWindowMs, maxHits);

    QList<WriteObservation> observations;
    observations.reserve(hits.size());
    for (const auto& hit : hits) {
        observations.append({hit.instructionPointer, hit.captureElapsedMs});
    }

    return classifyFieldStability(observations, options);
}

} // namespace killcore
