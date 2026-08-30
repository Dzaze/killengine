#include "investigation_notebook.h"

#include <algorithm>

namespace killai {

QString hypothesisStatusToString(HypothesisStatus status) {
    switch (status) {
        case HypothesisStatus::Active:
            return QStringLiteral("active");
        case HypothesisStatus::Confirmed:
            return QStringLiteral("confirmed");
        case HypothesisStatus::Refuted:
            return QStringLiteral("refuted");
    }
    return QStringLiteral("active");
}

QString InvestigationNotebook::addHypothesis(const QString& description, int baselineScore) {
    Hypothesis hypothesis;
    hypothesis.id = QStringLiteral("H%1").arg(m_nextId++);
    hypothesis.description = description;
    hypothesis.confidenceScore = std::clamp(baselineScore, kMinScore, kMaxScore);
    hypothesis.status = HypothesisStatus::Active;
    m_hypotheses.append(hypothesis);
    return hypothesis.id;
}

bool InvestigationNotebook::recordTestResult(const QString& hypothesisId, bool confirmed, const QString& evidenceNote) {
    for (Hypothesis& hypothesis : m_hypotheses) {
        if (hypothesis.id != hypothesisId) {
            continue;
        }
        if (hypothesis.status != HypothesisStatus::Active) {
            return false;
        }

        int delta = confirmed ? kConfirmDelta : kContradictDelta;
        hypothesis.confidenceScore = std::clamp(hypothesis.confidenceScore + delta, kMinScore, kMaxScore);
        hypothesis.evidenceLog.append(evidenceNote);

        if (hypothesis.confidenceScore <= kEliminationThreshold) {
            hypothesis.status = HypothesisStatus::Refuted;
        } else if (hypothesis.confidenceScore >= kConfirmationThreshold) {
            hypothesis.status = HypothesisStatus::Confirmed;
        }
        return true;
    }
    return false;
}

const Hypothesis* InvestigationNotebook::findHypothesis(const QString& hypothesisId) const {
    for (const Hypothesis& hypothesis : m_hypotheses) {
        if (hypothesis.id == hypothesisId) {
            return &hypothesis;
        }
    }
    return nullptr;
}

QList<Hypothesis> InvestigationNotebook::hypotheses() const {
    return m_hypotheses;
}

QList<Hypothesis> InvestigationNotebook::activeHypotheses() const {
    QList<Hypothesis> active;
    for (const Hypothesis& hypothesis : m_hypotheses) {
        if (hypothesis.status == HypothesisStatus::Active) {
            active.append(hypothesis);
        }
    }
    return active;
}

namespace {
QVariantList hypothesesToVariantList(QList<Hypothesis> list) {
    std::sort(list.begin(), list.end(), [](const Hypothesis& a, const Hypothesis& b) {
        return a.confidenceScore > b.confidenceScore;
    });
    QVariantList result;
    for (const Hypothesis& hypothesis : list) {
        QVariantMap entry;
        entry["id"] = hypothesis.id;
        entry["description"] = hypothesis.description;
        entry["confidenceScore"] = hypothesis.confidenceScore;
        entry["status"] = hypothesisStatusToString(hypothesis.status);
        entry["evidenceLog"] = hypothesis.evidenceLog;
        result.append(entry);
    }
    return result;
}
} // namespace

QVariantMap InvestigationNotebook::synthesis() const {
    QList<Hypothesis> confirmed, active, refuted;
    for (const Hypothesis& hypothesis : m_hypotheses) {
        switch (hypothesis.status) {
            case HypothesisStatus::Confirmed:
                confirmed.append(hypothesis);
                break;
            case HypothesisStatus::Refuted:
                refuted.append(hypothesis);
                break;
            case HypothesisStatus::Active:
                active.append(hypothesis);
                break;
        }
    }

    QVariantMap result;
    result["confirmed"] = hypothesesToVariantList(confirmed);
    result["active"] = hypothesesToVariantList(active);
    result["refuted"] = hypothesesToVariantList(refuted);
    return result;
}

void InvestigationNotebook::reset() {
    m_hypotheses.clear();
    m_nextId = 1;
}

} // namespace killai
