#include "investigation_notebook_manager.h"

#include "localization/localization.h"

namespace killengine {

QVariantMap InvestigationNotebookManager::hypothesisToVariantMap(const killai::Hypothesis& hypothesis) {
    QVariantMap map;
    map["id"] = hypothesis.id;
    map["description"] = hypothesis.description;
    map["confidenceScore"] = hypothesis.confidenceScore;
    map["status"] = killai::hypothesisStatusToString(hypothesis.status);
    map["evidenceLog"] = hypothesis.evidenceLog;
    return map;
}

QVariantMap InvestigationNotebookManager::addHypothesis(const QString& description, int baselineScore) {
    QVariantMap result;
    if (description.trimmed().isEmpty()) {
        result["success"] = false;
        result["error"] = KE_TXT("Description vide.", "Empty description.");
        return result;
    }

    QString id = m_notebook.addHypothesis(description, baselineScore);
    const killai::Hypothesis* hypothesis = m_notebook.findHypothesis(id);
    result["success"] = true;
    result["hypothesis"] = hypothesisToVariantMap(*hypothesis);
    return result;
}

QVariantMap InvestigationNotebookManager::recordTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote) {
    QVariantMap result;
    bool accepted = m_notebook.recordTestResult(hypothesisId, confirmed, evidenceNote);
    const killai::Hypothesis* hypothesis = m_notebook.findHypothesis(hypothesisId);

    if (!accepted) {
        result["success"] = false;
        result["error"] = hypothesis
            ? KE_TXT("Hypothèse déjà dans un état terminal (confirmée ou réfutée).", "Hypothesis already in a terminal state (confirmed or refuted).")
            : KE_TXT("Hypothèse introuvable.", "Hypothesis not found.");
        return result;
    }

    result["success"] = true;
    result["hypothesis"] = hypothesisToVariantMap(*hypothesis);
    return result;
}

QVariantMap InvestigationNotebookManager::getSynthesis() const {
    QVariantMap synthesis = m_notebook.synthesis();
    QVariantMap result;
    result["success"] = true;
    result["confirmed"] = synthesis["confirmed"];
    result["active"] = synthesis["active"];
    result["refuted"] = synthesis["refuted"];
    return result;
}

QVariantMap InvestigationNotebookManager::resetNotebook() {
    m_notebook.reset();
    QVariantMap result;
    result["success"] = true;
    return result;
}

} // namespace killengine
