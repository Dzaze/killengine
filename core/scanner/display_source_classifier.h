#pragma once

#include "debug/hardware_breakpoint.h"

#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace killcore {

/// Une écriture observée sur l'adresse candidate, réduite au strict
/// nécessaire pour la classification (voir classifyFieldStability) --
/// découplé de BreakpointHit pour rester testable sans capture réelle.
struct WriteObservation {
    uint64_t instructionPointer{0};
    uint32_t captureElapsedMs{0};
};

enum class FieldStabilityVerdict {
    /// Aucune écriture observée pendant la fenêtre de capture. Ne veut PAS
    /// dire "stable a coup sur" -- la fenetre peut juste avoir ete trop
    /// courte, ou le champ ne change que sur une action de gameplay qui ne
    /// s'est pas produite pendant la capture.
    NoWritesObserved,
    /// 1 ou 2 écritures : pas assez de données pour juger d'un motif
    /// régulier vs événementiel.
    InsufficientData,
    /// Plusieurs écritures, intervalle régulier, dominées par la même
    /// instruction -- signature d'un champ recalculé à chaque tick depuis
    /// une autre source (ex. g_counterDisplayed). Geler/patcher ce champ
    /// directement ne tiendra probablement pas ; chercher la source en
    /// amont plutôt.
    LikelyDerivedDisplay,
    /// Plusieurs écritures mais intervalle irrégulier et/ou plusieurs
    /// instructions différentes -- plus cohérent avec des écritures liées à
    /// de vrais événements de gameplay qu'avec un recalcul de tick fixe.
    /// Ne garantit pas que le champ est une "source" sûre, juste que le
    /// motif n'est pas celui d'un champ affiché interpolé.
    LikelyEventDriven,
};

struct FieldStabilityOptions {
    /// En dessous de ce nombre d'écritures, pas assez de données pour
    /// distinguer un motif régulier d'un motif événementiel.
    size_t minHitsForPattern{3};
    /// Coefficient de variation (écart-type / moyenne) des intervalles
    /// en-dessous duquel le rythme est considéré "régulier". Mesuré sur les
    /// fixtures KillEngineTestTarget (tick 50ms, capture réelle incluant la
    /// latence WaitForDebugEvent/ContinueDebugEvent) : garder une marge
    /// généreuse plutôt qu'un seuil serré calé sur une seule mesure.
    double regularIntervalCoefficientOfVariation{0.6};
    /// Part minimale des écritures venant de la même instructionPointer
    /// pour parler d'une instruction "dominante".
    double dominantInstructionShare{0.7};
};

struct FieldStabilityClassification {
    FieldStabilityVerdict verdict{FieldStabilityVerdict::NoWritesObserved};
    size_t writeCount{0};
    size_t distinctInstructionCount{0};
    uint64_t dominantInstructionPointer{0};
    double dominantInstructionShare{0.0};
    double meanIntervalMs{0.0};
    double intervalCoefficientOfVariation{0.0};
    /// Explication humaine (FR), prête à afficher telle quelle dans
    /// l'Assistant/l'UI le jour où ce classifieur y sera branché (pas fait
    /// ici volontairement, voir docs/STRATEGY_ROOM.md).
    QString rationale;
};

/// Logique pure, testable sans process réel : classifie un ensemble
/// d'écritures déjà observées (ordre de capture, pas besoin d'être trié).
FieldStabilityClassification classifyFieldStability(
    const QList<WriteObservation>& observations,
    const FieldStabilityOptions& options = {});

/// Capture réelle via findWhatWrites (core/debug/hardware_breakpoint.*) puis
/// classification. Mêmes précautions que findWhatWrites : attache un
/// debugger à pid pendant la capture, à réserver à une adresse déjà
/// identifiée comme candidate (pas une adresse "chaude" arbitraire -- voir
/// AGENTS.md section "Find What Writes" et le risque documenté de crash sur
/// une adresse à écriture très fréquente/contentionnée).
FieldStabilityClassification classifyFieldStabilityLive(
    uint32_t pid,
    uint64_t address,
    BreakpointSize size = BreakpointSize::DWord,
    int captureWindowMs = 800,
    size_t maxHits = 12,
    const FieldStabilityOptions& options = {});

} // namespace killcore
