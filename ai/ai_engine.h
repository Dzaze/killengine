#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>

#include "state_machine.h"
#include "llama_runtime.h"
#include "tool_registry.h"
#include "tool_validator.h"

namespace killai {

/**
 * @brief Moteur IA de KillEngine.
 *
 * Phase 9: socle tool-calling local, prêt pour branchement llama.cpp.
 * Phase 14: serveur llama persistant, retry correctif, historique conversationnel.
 */
class AIEngine : public QObject {
    Q_OBJECT
public:
    explicit AIEngine(QObject* parent = nullptr);
    ~AIEngine() override;

    /// Initialise le runtime IA (chargement différé du modèle).
    bool init();

    /// Le moteur IA est-il prêt ?
    bool isReady() const;

    /// Traite une requête utilisateur et retourne une décision JSON.
    QVariantMap processQuery(const QString& query);

    /// Surcharge avec contexte de session (processus, scan actif, candidats,
    /// valeurs initiale/cible, adresses actives) pour un choix d'outil pertinent.
    QVariantMap processQuery(const QString& query, const QVariantMap& context);

    /// Interprète une requête utilisateur en intention structurée validée.
    QVariantMap processIntent(const QString& query);

    /// Enregistre le résultat d'une action issue d'un tool call precedent.
    /// L'historique (query + outil + outcome) alimente les prompts suivants
    /// pour que le modele propose une alternative adaptee apres un echec.
    void noteOutcome(const QString& query, const QString& tool, const QString& outcome);

    /// Vide l'historique conversationnel (reset de session).
    void clearHistory();

private:
    QVariantMap deterministicIntent(const QString& query);
    QVariantMap makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale);
    QVariantMap deterministicPlan(const QString& query);
    QVariantMap deterministicPlanWithContext(const QString& query, const QVariantMap& context);
    bool ensureLlamaInitialized();
    /// Invoque le modele avec retry correctif borne si le JSON est invalide.
    QVariantMap modelToolCallWithRetry(const QString& query, const QVariantMap& context, QString* backend);
    QVariantMap modelIntentWithRetry(const QString& query, QString* backend);
    QVariantMap lastHistoryTurn() const;
    static QString inferValueType(const QString& query);
    static QString firstNumber(const QString& query);
    static QString firstHexAddress(const QString& query);

    bool           m_ready{false};
    ToolRegistry   m_registry;
    ToolValidator  m_validator;
    StateMachine   m_stateMachine;
    LlamaRuntime   m_llama;
    /// Historique recent {query, tool, outcome} transmis aux prompts.
    QVariantList   m_history;
};

} // namespace killai