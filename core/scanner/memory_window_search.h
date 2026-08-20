#pragma once

// Primitive generique de recherche de valeur dans une fenetre de memoire
// autour d'une adresse ancre. Volontairement independante de tout contexte
// "chaine UI" -- voir scanner/display_value_tracker.h::findUiStringSourcesInBuffer
// pour ce cas d'usage precis (different : la aussi on cherche une source
// numerique adjacente a un texte, mais la fonction est nommee/documentee
// autour de ce cas precis).
//
// Cree le 20/08/2026 sur demande explicite de l'utilisateur suite a la
// session de test live sur Solitaire (voir docs/STRATEGY_ROOM.md) : plutot
// que de detourner analyzeUiStringSources() (conçue pour tracer une chaine
// affichee a l'ecran) pour faire une recherche memoire generique autour
// d'une adresse binaire quelconque, cette primitive dediee existe pour
// couvrir ce besoin proprement, avec ses propres bornes/validations.

#include "scanner/value_variants.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace killcore {

struct MemoryWindowMatch {
    uint64_t address{0};
    ValueType type{ValueType::Int32};
    QString variantLabel;     ///< Ex. "Int32", "Float64 x100"
    QByteArray bytes;
    double valueNumber{0.0};  ///< Valeur numerique actuelle a cette adresse
    int64_t offsetFromAnchor{0};
    uint64_t distanceBytes{0};
};

/// Cherche, dans un buffer deja lu (`buffer`, dont le premier octet
/// correspond a l'adresse `windowStart`), toutes les occurrences numeriques
/// de `value` -- tous types/echelles courants confondus (voir
/// generateScanVariants) -- alignees sur `alignment` octets.
///
/// `anchorAddress` sert de centre pour le tri par distance et pour la zone
/// d'exclusion : les `excludeBytes` octets a partir de `anchorAddress` sont
/// ignores (typiquement la taille du champ deja connu/observe, pour ne pas
/// se re-matcher soi-meme).
///
/// Bornes : `maxResults` est force dans [1, 5000], le buffer d'entree doit
/// deja avoir ete borne par l'appelant (voir ApplicationController::
/// scanMemoryWindow qui applique les memes limites de rayon que
/// analyzeUiStringSources).
QList<MemoryWindowMatch> findValuesInMemoryWindow(
    const QByteArray& buffer,
    uint64_t windowStart,
    uint64_t anchorAddress,
    int excludeBytes,
    const QString& value,
    int maxResults,
    int alignment);

} // namespace killcore
