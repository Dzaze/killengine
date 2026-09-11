#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>

#include "state_machine.h"
#include "llama_runtime.h"
#include "tool_registry.h"
#include "tool_validator.h"

#include <functional>

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

    /// PHASE 120-G : le modèle local propose les hypothèses et le prochain test,
    /// le code conserve seul la pondération numérique.
    QVariantMap proposeInvestigationNotebookPlan(const QString& symptom, const QVariantMap& context = {});

    /// Enregistre le résultat d'une action issue d'un tool call precedent.
    /// L'historique (query + outil + outcome) alimente les prompts suivants
    /// pour que le modele propose une alternative adaptee apres un echec.
    void noteOutcome(const QString& query, const QString& tool, const QString& outcome);

    /// Vide l'historique conversationnel (reset de session).
    void clearHistory();

    /// Demarre le serveur llama.cpp persistant et amorce son cache_prompt
    /// (voir LlamaRuntime::warmup) pour que le premier vrai message
    /// utilisateur n'ait pas a payer le cout de demarrage a froid. Bloquant
    /// (jusqu'a ~90s au tout premier chargement modele) -- l'appelant est
    /// responsable de l'executer hors du thread GUI (voir
    /// ApplicationController::warmupLocalAiModel, fenetre de prechauffage au
    /// demarrage). onStageChanged (optionnel) recoit "initializing",
    /// "loadingModel" puis "warmingPrompt" pour une progression honnete.
    /// NOTE (11/09/2026) : chemin de repli utilise par
    /// warmupLocalModelWithCalibration() quand la calibration elle-meme
    /// echoue -- garde un timeout par defaut plutot fixe. Le chemin nominal
    /// passe desormais par warmupLocalModelWithCalibration ci-dessous.
    LlamaGenerationResult warmupLocalModel(const std::function<void(const QString&)>& onStageChanged = {});

    /// Resultat de warmupLocalModelWithCalibration : soit le prechauffage
    /// s'est termine (succes ou echec, comme warmupLocalModel), soit le debit
    /// mesure indique un temps trop long et une decision utilisateur explicite
    /// est necessaire avant de continuer (voir continueWarmupAfterEstimate).
    struct WarmupCalibrationResult {
        enum class Outcome { Completed, NeedsDecision };
        Outcome outcome{Outcome::Completed};
        LlamaGenerationResult completion;   ///< valide si outcome == Completed
        double estimatedSeconds{0.0};       ///< valide si outcome == NeedsDecision
        int estimatedTokenCount{0};         ///< valide si outcome == NeedsDecision
    };

    /// Mesure reelle du debit de prefill sur cette machine avec ce modele
    /// (echantillon = prefixe du vrai prompt, cache_prompt reutilise ensuite)
    /// avant de lancer le vrai prechauffage -- un timeout fixe (45s) est
    /// structurellement inadapte des que la machine est faible ou le modele
    /// different du defaut recommande (voir docs/PHASE_TRACKER.md, enquete du
    /// 11/09/2026). Si l'estimation depasse le seuil (60s), ne tente RIEN de
    /// plus et retourne NeedsDecision -- c'est a l'appelant (fenetre de
    /// prechauffage) de proposer explicitement d'attendre ou de desactiver le
    /// modele local pour la session plutot que de risquer un nouveau timeout
    /// silencieux.
    WarmupCalibrationResult warmupLocalModelWithCalibration(const std::function<void(const QString&)>& onStageChanged = {});

    /// A appeler apres un WarmupCalibrationResult::NeedsDecision si
    /// l'utilisateur choisit d'attendre : relance le prechauffage complet
    /// avec un budget de temps calcule depuis l'estimation (stockee en
    /// interne lors de l'appel precedent), pas le timeout generique.
    LlamaGenerationResult continueWarmupAfterEstimate(const std::function<void(const QString&)>& onStageChanged = {});

    /// A appeler si l'utilisateur choisit de continuer sans IA locale plutot
    /// que d'attendre : desactive le modele local en memoire UNIQUEMENT (rien
    /// n'est persiste dans QSettings, une nouvelle session repart a zero).
    /// ensureLlamaInitialized() retourne alors immediatement false sans
    /// retenter quoi que ce soit -- reutilise le chemin "modele non pret"
    /// deja gere partout ailleurs (repli deterministe/Claude selon
    /// getActiveAiBackend(), cf. smart_search_manager.cpp).
    void disableForSession();
    bool isSessionDisabled() const;

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
    static QStringList allNumbers(const QString& query);
    static QString firstHexAddress(const QString& query);

    bool           m_ready{false};
    ToolRegistry   m_registry;
    ToolValidator  m_validator;
    StateMachine   m_stateMachine;
    LlamaRuntime   m_llama;
    /// Historique recent {query, tool, outcome} transmis aux prompts.
    QVariantList   m_history;
    /// true tant que l'utilisateur n'a pas explicitement desactive le modele
    /// local pour cette session (voir disableForSession()) -- jamais persiste.
    bool           m_sessionDisabled{false};
    /// Estimation (secondes) calculee par le dernier warmupLocalModelWithCalibration()
    /// ayant retourne NeedsDecision -- lue par continueWarmupAfterEstimate().
    double         m_pendingWarmupEstimateSeconds{0.0};
};

} // namespace killai
