#pragma once

#include "scanner/scan_types.h"

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

} // namespace killcore
