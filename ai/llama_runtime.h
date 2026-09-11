#pragma once

#include "model_locator.h"
#include "tool_registry.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>

#include <functional>

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
    /// Debit de prefill mesure pour CETTE requete (0 si indisponible, ex.
    /// chemin llama-cli). Utilise pour calibrer le budget de temps du
    /// prechauffage sur la machine/le modele reels.
    double promptTokensPerSecond{0.0};
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

    /// Requete factice (prefixe statique du prompt uniquement, n_predict
    /// volontairement petit) pour amorcer cache_prompt cote llama-server
    /// AVANT le premier vrai message utilisateur -- voir docs/PHASE_TRACKER.md
    /// (goulot d'etranglement "llama.cpp execute", piste 1, 08/09/2026).
    /// onStage (optionnel) : relaye a LlamaServer::complete, voir la-bas.
    /// timeoutMsOverride (optionnel, >0) : budget explicite au lieu du defaut
    /// generique -- calcule par AIEngine a partir d'une calibration reelle
    /// (voir measurePrefillSpeed) plutot qu'une constante fixe inadaptee a la
    /// machine/au modele charge.
    LlamaGenerationResult warmup(const ToolRegistry& registry,
                                 const std::function<void(const QString&)>& onStage = {},
                                 int timeoutMsOverride = 0) const;

    /// Calibration reelle du debit de prefill sur CETTE machine avec CE
    /// modele : lance une petite completion sur `sampleText` (idealement un
    /// prefixe du vrai prompt, cache_prompt actif donc reutilise ensuite par
    /// le vrai prechauffage) et renvoie `promptTokensPerSecond` mesure par le
    /// serveur. success=false si indisponible (ex. serveur down, fallback
    /// llama-cli sans mesure) -- l'appelant doit alors se rabattre sur un
    /// timeout par defaut plutot que d'echouer tout le flux.
    LlamaGenerationResult measurePrefillSpeed(const QString& sampleText,
                                               const std::function<void(const QString&)>& onStage = {}) const;

    /// Nombre de tokens de `text` selon le tokenizer du modele charge (pur,
    /// aucune inference). -1 si indisponible (best-effort).
    int tokenCount(const QString& text) const;

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
    /// onStage (optionnel) : relaye a LlamaServer::complete sur le chemin
    /// serveur ; sur le fallback llama-cli, un seul "loadingModel" avant de
    /// lancer le process (pas de decoupage plus fin sur ce chemin rare).
    /// timeoutMsOverride : voir warmup().
    LlamaGenerationResult generate(const QString& prompt, int nPredict,
                                    const std::function<void(const QString&)>& onStage = {},
                                    int timeoutMsOverride = 0) const;
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
