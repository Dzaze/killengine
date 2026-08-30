#pragma once

#include <QString>
#include <QStringList>
#include <QList>
#include <QVariantMap>

namespace killai {

enum class HypothesisStatus { Active, Confirmed, Refuted };

QString hypothesisStatusToString(HypothesisStatus status);

struct Hypothesis {
    QString id;
    QString description;
    int confidenceScore{0};
    HypothesisStatus status{HypothesisStatus::Active};
    QStringList evidenceLog;
};

/// Moteur de pondération déterministe pour le carnet d'hypothèses (PHASE 120-E).
///
/// Portée volontairement limitée : ce moteur ne génère PAS d'hypothèses et ne
/// propose PAS de prochain test — ça reste le rôle du modèle local (PHASE 120-G).
/// Il se contente de stocker des hypothèses concurrentes et d'appliquer une règle
/// de mise à jour de confiance fixe et testable après chaque résultat de test,
/// pour que le poids ne dépende jamais d'un calcul demandé au LLM (décision
/// propriétaire du 30/08/2026, voir docs/PHASE_TRACKER.md).
///
/// Le delta de contradiction est délibérément plus fort que le delta de
/// confirmation : la motivation d'origine (cas Bulles Solitaire) est qu'une
/// première hypothèse fausse peut s'ancrer et faire perdre du temps si on ne
/// l'abandonne pas assez vite face à des preuves contraires.
class InvestigationNotebook {
public:
    static constexpr int kDefaultBaselineScore = 50;
    static constexpr int kMinScore = 0;
    static constexpr int kMaxScore = 100;
    static constexpr int kConfirmDelta = 20;
    static constexpr int kContradictDelta = -30;
    static constexpr int kEliminationThreshold = 10;
    static constexpr int kConfirmationThreshold = 90;

    /// Ajoute une hypothèse active et retourne son id généré (ex. "H1").
    QString addHypothesis(const QString& description, int baselineScore = kDefaultBaselineScore);

    /// Applique le résultat d'un test à une hypothèse encore active : `confirmed`
    /// indique si le test a confirmé (true) ou contredit (false) sa prédiction.
    /// `evidenceNote` est ajouté tel quel au journal de preuves de l'hypothèse.
    /// Retourne false si l'id est inconnu ou si l'hypothèse est déjà dans un état
    /// terminal (Confirmed/Refuted, verrouillé — pas de mise à jour rétroactive).
    bool recordTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote);

    const Hypothesis* findHypothesis(const QString& hypothesisId) const;
    QList<Hypothesis> hypotheses() const;
    QList<Hypothesis> activeHypotheses() const;

    /// Structure factuelle {confirmed, active, refuted}, chacune triée par
    /// confiance décroissante. Ne contient aucun texte de "prochaine expérience"
    /// généré — cette partie narrative reste le rôle du modèle (PHASE 120-G).
    QVariantMap synthesis() const;

    void reset();

private:
    QList<Hypothesis> m_hypotheses;
    int m_nextId{1};
};

} // namespace killai
