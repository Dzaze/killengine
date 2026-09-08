#pragma once

#include "model_locator.h"
#include "tool_registry.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

namespace killai {

struct LlamaRuntimeInfo {
    bool available{false};
    QString executablePath;
    QString modelPath;
    QString errorMessage;
};

struct LlamaGenerationResult {
    bool success{false};
    QString output;
    QString errorMessage;
    /// "llama-server" si servi par le serveur persistant, "llama-cli" sinon.
    QString backend;
};

class LlamaRuntime {
public:
    bool init();
    bool isAvailable() const;
    LlamaRuntimeInfo info() const;
    /// Contexte d'etat transmis au prompt pour choisir le bon outil
    /// (processus attache, scan actif, candidats restants, valeurs initiale/cible...).
    /// Champs optionnels supplementaires:
    /// - "history": QVariantList de {query, tool, outcome} (conversation recente)
    /// - "unknownSnapshotActive": bool (snapshot unknown capture)
    /// - "freezeCount": int (adresses gelees)
    /// - "valueType": dernier type de valeur utilise
    LlamaGenerationResult planToolCall(
        const QString& query,
        const ToolRegistry& registry,
        const QVariantMap& context = {}) const;
    LlamaGenerationResult planIntent(const QString& query) const;
    LlamaGenerationResult planInvestigationNotebook(const QString& symptom, const QVariantMap& context = {}) const;

    static QVariantMap extractToolCallJson(const QString& text, QString* error = nullptr);
    static QVariantMap extractIntentJson(const QString& text, QString* error = nullptr);

    /// PHASE 140 : rendu public (etait prive) uniquement pour permettre un
    /// test de non-regression qui verifie que la ligne "Schema obligatoire"
    /// codee en dur (ai/llama_runtime.cpp) reste synchronisee avec
    /// ToolRegistry::availableTools() -- cette ligne ne se genere PAS
    /// automatiquement depuis le registre (contrairement au bloc "Outils
    /// disponibles" plus bas dans le meme prompt), et un outil ajoute au
    /// registre sans etre ajoute ici devient invisible pour le modele local
    /// meme si son dispatch existe par ailleurs (bug reel trouve et corrige
    /// en PHASE 140 : get_auto_report/disassemble_backward/
    /// test_candidate_fields manquaient a cette ligne). Fonction pure
    /// (aucun etat d'instance), donc sans risque a exposer.
    static QString buildPrompt(const QString& query, const ToolRegistry& registry, const QVariantMap& context);

private:
    /// Completion serveur persistant si dispo, sinon llama-cli one-shot.
    LlamaGenerationResult generate(const QString& prompt, int nPredict) const;
    static QString findExecutable();
    static QString buildContextBlock(const QVariantMap& context);
    static QString buildHistoryBlock(const QVariantMap& context);
    static QString buildDynamicHints(const QVariantMap& context);
    static QString buildIntentPrompt(const QString& query);

    LlamaRuntimeInfo m_info;
    QString m_serverExecutablePath;
    /// true tant que le serveur persistant reste utilisable (retombe sur llama-cli sinon).
    /// PHASE (08/09/2026) : avant, un seul echec (ex: lenteur passagere sous
    /// charge, cf. docs/PHASE_TRACKER.md) desactivait le serveur persistant
    /// pour le reste de la session sans jamais reessayer -- toute requete
    /// suivante retombait alors sur llama-cli (processus a froid, sans cache
    /// de prompt, structurellement plus lent) meme une fois la machine
    /// redevenue disponible. m_serverRetryAfterMs porte le prochain instant
    /// (QDateTime::currentMSecsSinceEpoch()) ou reessayer le serveur.
    mutable bool m_serverUsable{true};
    mutable qint64 m_serverRetryAfterMs{0};
};

} // namespace killai
