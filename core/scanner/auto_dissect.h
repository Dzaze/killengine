#pragma once

#include "scanner/structure_analyzer.h"
#include "memory/memory_reader.h"
#include "memory/memory_region.h"

#include <QList>
#include <QString>
#include <QVariant>

#include <cstdint>

namespace killcore {

/// Une instance de structure découverte par le scan auto.
struct DiscoveredInstance {
    uint64_t baseAddress{0};
    QList<QVariant> fieldValues;  ///< Valeurs décodées des champs du template
    double confidence{0.0};       ///< Score de confiance [0..1]
};

/// Résultat du scan auto-dissect.
struct AutoDissectResult {
    bool success{false};
    QString error;
    QList<DiscoveredInstance> instances;
    int scannedRegions{0};
    int totalCandidates{0};
};

/// Options du scan auto-dissect.
struct AutoDissectOptions {
    int maxResults{128};          ///< Nombre max d'instances à retourner
    bool requirePointerValidity{true}; ///< Vérifier que les champs pointeurs pointent vers des pages valides
    double minConfidence{0.5};    ///< Seuil de confiance minimum
};

/// Vérifie si une adresse de pointeur pointe vers une page valide (committed
/// + readable). `sortedRegions` DOIT être trié par baseAddress croissant et
/// ne pas contenir de chevauchement (recherche binaire, pas un scan
/// linéaire) — voir auto_dissect.cpp pour le détail. Exposée ici (plutôt que
/// `static` dans le .cpp) pour être testée directement en isolation :
/// c'est le seul endroit où une régression réintroduirait silencieusement
/// des faux négatifs (une structure valide ignorée) dans le scan complet.
bool isPointerValid(const QList<MemoryRegion>& sortedRegions, uint64_t addr);

/// Scanne la mémoire du process pour trouver toutes les instances d'un template de structure.
AutoDissectResult findStructureInstances(
    const ProcessHandle& process,
    const StructureTemplate& tmpl,
    const AutoDissectOptions& options = {});

} // namespace killcore
