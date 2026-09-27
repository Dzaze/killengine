#pragma once

#include "scanner/scan_types.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace killcore {

/**
 * @brief UX-PRODUIT-16 (16A) -- décodage typé d'un point de comparaison.
 *
 * Le collecteur (candidate_comparison_collector.*) stocke des octets bruts,
 * comme MemoryTimelineCollector. Ce module fait le pont vers les définitions
 * canoniques de type déjà en place (killcore::ValueType,
 * scanner/value_variants.h::scanBytesToDouble/scanBytesToExactString) plutôt
 * que de réinventer un décodage numérique -- confirmé absent ailleurs dans la
 * chaîne Timeline existante (collecteur/analyzer/export ne manipulent que des
 * octets/valueHex, jamais un type natif).
 */
struct DecodedComparisonValue {
    /// false si les octets sont trop courts pour `type` -- un trou de courbe
    /// explicite, jamais une valeur 0 inventée silencieusement.
    bool ok{false};
    /// Texte base-10 exact (scanBytesToExactString) -- utilisé pour l'affichage
    /// exact et pour formatScaledValueText ci-dessous, jamais reconstruit
    /// depuis numericValue (qui peut avoir perdu de la précision).
    QString exactValueText;
    /// Valeur décodée en double (scanBytesToDouble) -- utilisée uniquement
    /// pour le tracé graphique, jamais pour un texte exact affiché.
    double numericValue{0.0};
    bool isNaN{false};
    bool isInfinite{false};
};

/// Décode `bytes` selon `type`. `ok=false` si `bytes.size()` est inférieur à
/// la taille attendue par `type` (valueTypeSize).
DecodedComparisonValue decodeComparisonValue(const std::vector<uint8_t>& bytes, ValueType type);

/// "Valeur affichée = stockée / facteur". Division EXACTE (déplacement de la
/// virgule en texte) uniquement pour les facteurs décimaux usuels (1, 10,
/// 100, 1000 -- cf. les échelles fixes déjà utilisées par
/// generateScanVariants) appliqués à un type entier ; `*outExact` est mis à
/// true dans ce cas. Pour tout autre facteur (ex. 4096/65536, facteurs non
/// entiers, type flottant) ou factor <= 0/NaN/Inf, repli sur
/// numericValue/factor formaté en double -- `*outExact` est mis à false :
/// c'est une approximation documentée, pas une exactitude perdue par erreur
/// (même limite déjà acceptée ailleurs pour ces échelles binaires, ex. le
/// Plan d'écriture de Trace UI string).
QString formatScaledValueText(const DecodedComparisonValue& decoded, ValueType type, double factor, bool* outExact);

} // namespace killcore
