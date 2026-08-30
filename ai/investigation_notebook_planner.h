#pragma once

#include <QString>
#include <QVariantMap>

namespace killai {

/// Construit le prompt JSON strict demandé au modèle local pour PHASE 120-G.
QString buildInvestigationNotebookPlanPrompt(const QString& symptom, const QVariantMap& context = {});

/// Extrait le premier objet JSON exploitable dans une sortie de modèle.
QVariantMap extractInvestigationNotebookPlanJson(const QString& text, QString* error = nullptr);

/// Nettoie une proposition de modèle : pas de score LLM, pas d'action destructrice,
/// listes bornées et chaînes compactées.
QVariantMap normalizeInvestigationNotebookPlan(
    const QVariantMap& proposal,
    const QString& symptom,
    const QString& source);

/// Repli déterministe quand le modèle est désactivé ou indisponible.
QVariantMap makeFallbackInvestigationNotebookPlan(const QString& symptom, const QString& source = "deterministic_fallback");

} // namespace killai
