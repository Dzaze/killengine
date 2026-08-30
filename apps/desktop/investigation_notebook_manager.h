#pragma once

#include "investigation_notebook.h"

#include <QString>
#include <QVariantMap>

namespace killengine {

/// Bridge Q_INVOKABLE mince pour killai::InvestigationNotebook (PHASE 120-F).
///
/// Ne dépend d'aucun état process/mémoire de ApplicationController -- le carnet
/// est une structure de raisonnement pure, indépendante de l'attache courante.
/// Volontairement sans référence vers ApplicationController (contrairement aux
/// autres managers) : rien ici n'a besoin d'y accéder, l'ajouter serait un état
/// mort tant qu'un besoin réel ne se présente pas.
class InvestigationNotebookManager {
public:
    QVariantMap addHypothesis(const QString& description, int baselineScore);
    QVariantMap recordTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote);
    QVariantMap getSynthesis() const;
    QVariantMap resetNotebook();

private:
    killai::InvestigationNotebook m_notebook;

    static QVariantMap hypothesisToVariantMap(const killai::Hypothesis& hypothesis);
};

} // namespace killengine
