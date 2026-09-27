#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

namespace killcore {

/**
 * @brief Décrit une variante de représentation pour un scan multi-type.
 *
 * Phase 13 - Précision de recherche : permet de chercher automatiquement
 * les représentations courantes (unsigned, valeur multipliée par 10/100/1000,
 * score interne) quand l'utilisateur ne précise pas le type exact.
 */
struct ValueVariant {
    ScanValue value;        ///< Représentation byte à byte à chercher.
    QString   label;        ///< Libellé lisible (ex. "Int32 unsigned", "Float32 ×100").
    bool      secondary{false}; ///< Variante de moindre priorité (pour le scoring).
};

/**
 * @brief Génère les variantes de représentation courantes pour une valeur numérique donnée.
 *
 * Si `explicitType` est faux (l'utilisateur n'a pas précisé de type), la fonction
 * produit les types natifs courants (Int/UInt 8/16/32/64, Float32, Float64).
 *
 * Pour chaque type, des variantes de scaling sont produites quand c'est pertinent :
 *   - valeur exacte
 *   - valeur ×10, ×100, ×1000, ×4096, ×65536 (fixed-point / scores internes)
 *   - représentations compactes et unsigned
 *
 * @param rawValue  Texte source (ex. "100", "12.5").
 * @param explicitType Type imposé (ValueType::Int32 par défaut si non précisé).
 * @param explicitTypeGiven Vrai si l'utilisateur a explicitement demandé ce type.
 * @return Liste ordonnée des variantes (les principales d'abord, secondaires ensuite).
 */
QList<ValueVariant> generateScanVariants(
    const QString& rawValue,
    ValueType explicitType = ValueType::Int32,
    bool explicitTypeGiven = false);

/// Convertit des octets bruts vers un double lisible, selon le type. Utilise
/// par toute primitive de recherche memoire qui a besoin de rapporter "quelle
/// valeur ces octets representent" (ex: scanner/memory_window_search.*).
/// Retourne 0.0 si `bytes` est trop court pour `type`.
double scanBytesToDouble(const QByteArray& bytes, ValueType type);

/// UX-PRODUIT-16 -- texte base-10 EXACT (pas de conversion double
/// intermediaire, donc pas de perte de precision) pour la valeur decodee
/// depuis `bytes` selon `type`. Les types entiers passent par
/// int64_t/uint64_t natifs (QString::number gere ces largeurs exactement,
/// contrairement a scanBytesToDouble/double qui perd des bits au-dela de
/// 2^53). Float32/64 : texte round-trippable ('g', 17 chiffres significatifs),
/// avec "NaN"/"Inf"/"-Inf" explicites plutot qu'un texte numerique trompeur
/// pour ces cas particuliers. Retourne une chaine vide si `bytes` est trop
/// court pour `type` (jamais une valeur inventee).
QString scanBytesToExactString(const QByteArray& bytes, ValueType type);

/// Variante de delta scalé pour un scan Unknown en mode Delta (SC2-UNKNOWN-1).
struct DeltaVariant {
    double  rawDelta{0.0}; ///< Delta attendu sur la valeur brute stockée (ex. 7 * 4096 = 28672).
    QString label;         ///< Libellé de variante (ex. "Int32 x4096"), même convention que ValueVariant::label.
};

/// Génère les deltas bruts correspondant à un delta affiché, pour les mêmes
/// facteurs d'échelle que generateScanVariants (x1/x10/x100/x1000/x4096/x65536).
///
/// Motivation : un scan Unknown en mode Delta compare `(current - previous)`
/// sur la mémoire brute, mais l'utilisateur ne connaît que le delta affiché
/// à l'écran (ex. "+7"). Si le jeu stocke la valeur en virgule fixe (score
/// interne = affiché * 4096), le delta brut réel est 28672, pas 7 — sans
/// cette fonction, un scan Delta raterait silencieusement ces représentations
/// (même trou que celui déjà comblé pour le scan exact, voir
/// docs/PHASE_TRACKER.md "SC2-UNKNOWN-1"). Les types flottants ne sont pas
/// scalés (le scaling en virgule fixe est une technique réservée aux
/// entiers) : un seul DeltaVariant non scalé est retourné pour eux.
QList<DeltaVariant> generateDeltaVariants(double displayedDelta, ValueType type);

} // namespace killcore
