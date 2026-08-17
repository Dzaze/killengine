#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QList>

namespace killai {

/// Étape d'un plan d'auto-résolution.
enum class AutoResolveStepType {
    ScanExact,          // Scan exact pour une valeur connue
    UnknownCapture,     // Capture unknown initial value
    UnknownCompare,     // Comparaison unknown (increased/decreased/changed/stable)
    TestWrite,          // Test d'écriture sur les top candidats
    VerifyFreeze,       // Vérifier si le freeze tient
    FindWhatWrites,     // Trouver l'instruction qui écrit
    GenerateAob,        // Générer une signature AOB
    SuggestPatch,       // Suggérer un patch NOP/hook
    ApplyPatch,         // Appliquer le patch
    Done,               // Objectif atteint
    Failed,             // Échec
};

/// Convertit un type d'étape en texte lisible.
QString stepTypeToString(AutoResolveStepType type);

/// Une étape dans le plan d'auto-résolution.
struct AutoResolveStep {
    AutoResolveStepType type{AutoResolveStepType::ScanExact};
    QString description;        // Description lisible ("Scan exact pour 9999...")
    QVariantMap params;         // Paramètres (value, type, address, etc.)
    bool completed{false};
    bool success{false};
    QString result;             // Résultat après exécution
};

/// État de l'objectif utilisateur.
struct AutoResolveGoal {
    QString description;        // "Je veux 9999 minéraux"
    int64_t targetValue{0};     // Valeur cible
    QString valueType{"Int32"}; // Type de valeur
    QString gameContext;        // Nom du processus/profil cible.
};

/**
 * @brief Planificateur d'auto-résolution proactive.
 *
 * NOTE D'ARCHITECTURE (16/08/2026) : cette classe génère un plan d'affichage
 * (`planForGoal`) que l'Assistant montre à l'utilisateur, mais le moteur
 * d'EXÉCUTION réel vit dans `ApplicationController::startAutoResolve`
 * (apps/desktop/application_controller.cpp) et dans le chaînage de
 * `findWhatWritesAsync` (write confirmé -> écriture qui ne tient pas ->
 * find what writes -> AOB -> suggestion de patch, tout sauf l'application
 * du patch qui reste une confirmation explicite). Cette classe avait
 * auparavant des méthodes `executePlan`/`executeStep`/`resolve` qui
 * dupliquaient cette logique avec des noms d'outils abstraits
 * ("exact_scan", "test_write"...) qui ne correspondaient à aucune méthode
 * réelle de `ApplicationController` — elles n'étaient jamais appelées par
 * le produit et ont été retirées pour ne pas induire un futur agent en
 * erreur sur quel code fait réellement quelque chose.
 */
class AutoResolver : public QObject {
    Q_OBJECT
public:
    explicit AutoResolver(QObject* parent = nullptr);

    /// Génère un plan d'action depuis un objectif (affichage seulement,
    /// voir la note d'architecture ci-dessus pour où le plan est exécuté).
    QList<AutoResolveStep> planForGoal(const AutoResolveGoal& goal);
};

/// Un signal actionnable dérivé de la télémétrie récente : ce qui a été
/// observé, pourquoi c'est pertinent, et quelle action concrète suivre.
/// Correspond exactement à ce que `getAutoResolveReport` expose au
/// frontend sous `telemetryInsights` (id/label/reason/nextAction/safe).
struct AutoResolveTelemetryInsight {
    QString id;
    QString label;
    QString reason;
    QString nextAction;
    bool safe{true};
};

/// Rapport dérivé de la télémétrie récente : insights actionnables +
/// synthèse "valeur affichée découplée de la source mémoire" utilisée par
/// le panneau Trace UI string / rapport IA.
struct AutoResolveTelemetryReport {
    QList<AutoResolveTelemetryInsight> insights;
    bool displayValueSignals{false};
    QString displayValuePattern;
    QString displayValueRecommendation;
    int traceUiSourceCount{0};
    int traceUiGlobalHits{0};
    int exactZeroCount{0};
    int aobMultiMatchCount{0};
    int aobWeakQualityCount{0};
    int trainerBlockedCount{0};
    int freezeInstabilityCount{0};
};

/// Au-delà de ce nombre de sources Trace UI trouvées en un seul passage,
/// ce n'est plus un checkpoint exploitable : c'est du bruit qui a besoin
/// d'une deuxième variation observée pour se filtrer.
constexpr int kTraceUiSourceOverflowThreshold = 40;

/// Analyse les événements scan_telemetry/smart_search_debug et produit les
/// insights actionnables affichés dans le chat Assistant et la vue
/// Investigation. Fonction pure, testée indépendamment du contrôleur
/// desktop — c'est la seule implémentation de cette logique (avant le
/// 16/08/2026, une version incompatible et non testée par le produit
/// existait dupliquée en ligne dans `ApplicationController::getAutoResolveReport`).
AutoResolveTelemetryReport computeAutoResolveTelemetryReport(const QList<QVariantMap>& events);

} // namespace killai
