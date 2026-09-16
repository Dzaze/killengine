#pragma once

#include "effect_proof_ledger.h"

#include <QString>
#include <QVariantMap>

namespace killengine {

/// Bridge Q_INVOKABLE mince pour killai::EffectProofLedger (PRODUIT-R, section R1).
///
/// Même esprit que InvestigationNotebookManager : ne dépend d'aucun état
/// process/mémoire de ApplicationController -- le registre de preuves est une
/// structure de raisonnement pure, indépendante de l'attache courante.
/// Volontairement sans référence vers ApplicationController tant qu'aucun
/// besoin réel (ex. capture automatique après une écriture) n'est câblé.
class EffectProofManager {
public:
    QVariantMap recordProof(const QString& targetLabel, const QString& address, const QString& level,
                             const QString& source, const QString& conditions, const QString& sessionId,
                             const QString& note, const QString& executableHash = QString());
    /// `currentExecutableHash` (AM-5, optionnel) : voir EffectProofLedger::synthesis.
    /// Reflété dans le résultat (`currentExecutableHash`) pour que l'appelant
    /// puisse comparer la version d'une preuve donnée à la version courante
    /// sans dupliquer le calcul d'identité de process.
    QVariantMap getSynthesis(const QString& currentExecutableHash = QString()) const;
    QVariantMap resetLedger();

private:
    killai::EffectProofLedger m_ledger;
};

} // namespace killengine
