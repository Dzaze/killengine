#include "effect_proof_ledger.h"

#include "localization/localization.h"

#include <QDateTime>

namespace killai {

namespace {

int effectProofLevelRank(EffectProofLevel level) {
    switch (level) {
        case EffectProofLevel::Unverified:      return 0;
        case EffectProofLevel::Inconclusive:    return 1;
        case EffectProofLevel::WriteConfirmed:  return 2;
        case EffectProofLevel::EffectConfirmed: return 3;
        case EffectProofLevel::DurableSolution: return 4;
    }
    return 0;
}

QString nextActionForLevel(EffectProofLevel bestLevel) {
    switch (bestLevel) {
        case EffectProofLevel::Unverified:
            return KE_TXT("Aucune preuve enregistrée : confirmer d'abord l'écriture (relecture).",
                           "No proof recorded yet: confirm the write first (reread).");
        case EffectProofLevel::Inconclusive:
            return KE_TXT("Résultat inconclusif : réessayer avec une preuve plus fiable ou observer l'effet manuellement.",
                           "Inconclusive result: retry with a more reliable proof or observe the effect manually.");
        case EffectProofLevel::WriteConfirmed:
            return KE_TXT("Écriture confirmée par relecture, mais effet non vérifié : observer le comportement attendu (effet immédiat, transition).",
                           "Write confirmed by reread, but effect not verified: observe the expected behavior (immediate effect, transition).");
        case EffectProofLevel::EffectConfirmed:
            return KE_TXT("Effet confirmé, mais persistance non testée : vérifier après redémarrage ou fin de partie.",
                           "Effect confirmed, but persistence not tested: verify after a restart or end of session.");
        case EffectProofLevel::DurableSolution:
            return KE_TXT("Solution durable confirmée : aucune action supplémentaire nécessaire.",
                           "Durable solution confirmed: no further action needed.");
    }
    return QString();
}

QVariantMap recordToVariantMap(const EffectProofRecord& record) {
    QVariantMap map;
    map["id"] = record.id;
    map["targetLabel"] = record.targetLabel;
    map["address"] = record.address;
    map["level"] = effectProofLevelToString(record.level);
    map["source"] = record.source;
    map["conditions"] = record.conditions;
    map["sessionId"] = record.sessionId;
    map["executableHash"] = record.executableHash;
    map["recordedAt"] = record.recordedAt;
    map["note"] = record.note;
    return map;
}

// AM-5 : une adresse brute qui coïncide entre deux exécutables différents ne
// désigne pas la même cible -- un enregistrement versionné (executableHash non
// vide) qui ne correspond pas à la version courante est étranger à la cible
// affichée maintenant, jamais inclus dans son meilleur niveau/historique.
bool recordBelongsToCurrentContext(const EffectProofRecord& record, const QString& currentExecutableHash) {
    if (currentExecutableHash.isEmpty() || record.executableHash.isEmpty()) {
        return true;
    }
    return record.executableHash.compare(currentExecutableHash, Qt::CaseInsensitive) == 0;
}

QVariantMap targetStatusToVariantMap(const EffectProofTargetStatus& status) {
    QVariantMap map;
    map["targetKey"] = status.targetKey;
    map["targetLabel"] = status.targetLabel;
    map["address"] = status.address;
    map["bestLevel"] = effectProofLevelToString(status.bestLevel);
    map["nextAction"] = status.nextAction;
    QVariantList history;
    for (const EffectProofRecord& record : status.history) {
        history.append(recordToVariantMap(record));
    }
    map["history"] = history;
    return map;
}

} // namespace

QString effectProofLevelToString(EffectProofLevel level) {
    switch (level) {
        case EffectProofLevel::Unverified:      return QStringLiteral("unverified");
        case EffectProofLevel::Inconclusive:    return QStringLiteral("inconclusive");
        case EffectProofLevel::WriteConfirmed:  return QStringLiteral("write_confirmed");
        case EffectProofLevel::EffectConfirmed: return QStringLiteral("effect_confirmed");
        case EffectProofLevel::DurableSolution: return QStringLiteral("durable_solution");
    }
    return QStringLiteral("unverified");
}

bool effectProofLevelIsKnownString(const QString& text) {
    static const QStringList kKnown = {
        QStringLiteral("unverified"),
        QStringLiteral("inconclusive"),
        QStringLiteral("write_confirmed"),
        QStringLiteral("effect_confirmed"),
        QStringLiteral("durable_solution"),
    };
    return kKnown.contains(text.trimmed().toLower());
}

EffectProofLevel effectProofLevelFromString(const QString& text) {
    const QString normalized = text.trimmed().toLower();
    if (normalized == QStringLiteral("inconclusive")) return EffectProofLevel::Inconclusive;
    if (normalized == QStringLiteral("write_confirmed")) return EffectProofLevel::WriteConfirmed;
    if (normalized == QStringLiteral("effect_confirmed")) return EffectProofLevel::EffectConfirmed;
    if (normalized == QStringLiteral("durable_solution")) return EffectProofLevel::DurableSolution;
    return EffectProofLevel::Unverified;
}

QString EffectProofLedger::targetKeyFor(const QString& targetLabel, const QString& address) {
    const QString trimmedAddress = address.trimmed();
    if (!trimmedAddress.isEmpty()) {
        return QStringLiteral("addr:") + trimmedAddress.toLower();
    }
    return QStringLiteral("label:") + targetLabel.trimmed().toLower();
}

QString EffectProofLedger::addRecord(const QString& targetLabel, const QString& address, EffectProofLevel level,
                                      const QString& source, const QString& conditions, const QString& sessionId,
                                      const QString& note, const QString& executableHash) {
    EffectProofRecord record;
    record.id = QStringLiteral("P%1").arg(m_nextId++);
    record.targetLabel = targetLabel.trimmed();
    record.address = address.trimmed();
    record.level = level;
    record.source = source.trimmed();
    record.conditions = conditions.trimmed();
    record.sessionId = sessionId.trimmed();
    record.executableHash = executableHash.trimmed();
    record.recordedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record.note = note.trimmed();
    m_records.append(record);
    return record.id;
}

void EffectProofLedger::clear() {
    m_records.clear();
    m_nextId = 1;
}

QList<QString> EffectProofLedger::orderedTargetKeys() const {
    QList<QString> keys;
    for (const EffectProofRecord& record : m_records) {
        const QString key = targetKeyFor(record.targetLabel, record.address);
        if (!keys.contains(key)) {
            keys.append(key);
        }
    }
    return keys;
}

EffectProofTargetStatus EffectProofLedger::statusForTarget(const QString& targetKey, const QString& currentExecutableHash) const {
    EffectProofTargetStatus status;
    status.targetKey = targetKey;

    // Le plus récent en tête, mais le "meilleur niveau atteint" retient le rang
    // maximum jamais observé -- une relance inconclusive après un effet déjà
    // confirmé ne doit pas faire régresser le statut affiché. Les
    // enregistrements d'une autre version/exécutable (AM-5) sont ignorés ici :
    // ils ne comptent ni pour l'historique affiché ni pour le meilleur niveau.
    for (auto it = m_records.crbegin(); it != m_records.crend(); ++it) {
        const EffectProofRecord& record = *it;
        if (targetKeyFor(record.targetLabel, record.address) != targetKey) {
            continue;
        }
        if (!recordBelongsToCurrentContext(record, currentExecutableHash)) {
            continue;
        }
        status.history.append(record);
        if (status.targetLabel.isEmpty()) status.targetLabel = record.targetLabel;
        if (status.address.isEmpty()) status.address = record.address;
        if (effectProofLevelRank(record.level) > effectProofLevelRank(status.bestLevel)) {
            status.bestLevel = record.level;
        }
    }
    status.nextAction = nextActionForLevel(status.bestLevel);
    return status;
}

QVariantMap EffectProofLedger::synthesis(const QString& currentExecutableHash) const {
    QVariantList known;
    QVariantList uncertain;
    EffectProofTargetStatus mostUrgent;
    bool hasUncertain = false;

    for (const QString& key : orderedTargetKeys()) {
        EffectProofTargetStatus status = statusForTarget(key, currentExecutableHash);
        if (status.history.isEmpty()) {
            // Tous les enregistrements de cette clé appartiennent à une autre
            // version/exécutable (AM-5) -- rien à afficher pour le contexte
            // courant, ne pas synthétiser une entrée vide.
            continue;
        }
        const bool isKnown = effectProofLevelRank(status.bestLevel) >= effectProofLevelRank(EffectProofLevel::EffectConfirmed);
        if (isKnown) {
            known.append(targetStatusToVariantMap(status));
        } else {
            uncertain.append(targetStatusToVariantMap(status));
            if (!hasUncertain || effectProofLevelRank(status.bestLevel) < effectProofLevelRank(mostUrgent.bestLevel)) {
                mostUrgent = status;
                hasUncertain = true;
            }
        }
    }

    QString overallNextAction;
    if (known.isEmpty() && uncertain.isEmpty()) {
        // AM-5 : un registre global non vide peut n'avoir aucune cible pour le
        // contexte courant (tout appartient à un autre exécutable/version,
        // filtré ci-dessus) -- même message que "registre vraiment vide",
        // jamais le message "tout est déjà prouvé" qui serait trompeur ici.
        overallNextAction = KE_TXT("Aucun objectif suivi pour le moment.", "No tracked goal yet.");
    } else if (hasUncertain) {
        overallNextAction = mostUrgent.nextAction;
    } else {
        overallNextAction = KE_TXT("Toutes les cibles suivies ont un effet confirmé ou une solution durable.",
                                    "All tracked targets have a confirmed effect or a durable solution.");
    }

    QVariantMap result;
    result["known"] = known;
    result["uncertain"] = uncertain;
    result["overallNextAction"] = overallNextAction;
    return result;
}

} // namespace killai
