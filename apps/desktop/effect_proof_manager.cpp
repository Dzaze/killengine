#include "effect_proof_manager.h"

#include "localization/localization.h"

namespace killengine {

QVariantMap EffectProofManager::recordProof(const QString& targetLabel, const QString& address, const QString& level,
                                             const QString& source, const QString& conditions, const QString& sessionId,
                                             const QString& note) {
    QVariantMap result;

    if (targetLabel.trimmed().isEmpty() && address.trimmed().isEmpty()) {
        result["success"] = false;
        result["error"] = KE_TXT("Cible vide : renseigner un libellé ou une adresse.",
                                  "Empty target: provide a label or an address.");
        return result;
    }

    if (!killai::effectProofLevelIsKnownString(level)) {
        result["success"] = false;
        result["error"] = KE_TXT("Niveau de preuve inconnu.", "Unknown proof level.");
        return result;
    }

    const killai::EffectProofLevel parsedLevel = killai::effectProofLevelFromString(level);
    const QString id = m_ledger.addRecord(targetLabel, address, parsedLevel, source, conditions, sessionId, note);

    const QString targetKey = killai::EffectProofLedger::targetKeyFor(targetLabel, address);
    const killai::EffectProofTargetStatus status = m_ledger.statusForTarget(targetKey);
    QVariantMap targetStatus;
    targetStatus["targetKey"] = status.targetKey;
    targetStatus["targetLabel"] = status.targetLabel;
    targetStatus["address"] = status.address;
    targetStatus["bestLevel"] = killai::effectProofLevelToString(status.bestLevel);
    targetStatus["nextAction"] = status.nextAction;

    result["success"] = true;
    result["recordId"] = id;
    result["targetStatus"] = targetStatus;
    return result;
}

QVariantMap EffectProofManager::getSynthesis() const {
    QVariantMap synthesis = m_ledger.synthesis();
    QVariantMap result;
    result["success"] = true;
    result["known"] = synthesis["known"];
    result["uncertain"] = synthesis["uncertain"];
    result["overallNextAction"] = synthesis["overallNextAction"];
    return result;
}

QVariantMap EffectProofManager::resetLedger() {
    m_ledger.clear();
    QVariantMap result;
    result["success"] = true;
    return result;
}

} // namespace killengine
