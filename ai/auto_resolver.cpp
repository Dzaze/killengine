#include "auto_resolver.h"

namespace killai {

namespace {

qulonglong numericEventValue(const QVariantMap& event, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const QVariant value = event.value(QString::fromLatin1(key));
        if (value.isValid() && !value.isNull()) {
            bool ok = false;
            const qulonglong parsed = value.toULongLong(&ok);
            if (ok) {
                return parsed;
            }
        }
    }
    return 0;
}

int nestedSignatureInt(const QVariantMap& event, const QString& key, int fallback = 0) {
    const QVariantMap quality = event.value("signatureQuality").toMap();
    if (quality.contains(key)) {
        bool ok = false;
        const int value = quality.value(key).toInt(&ok);
        if (ok) return value;
    }
    bool ok = false;
    const int direct = event.value(key).toInt(&ok);
    return ok ? direct : fallback;
}

} // namespace

QString stepTypeToString(AutoResolveStepType type) {
    switch (type) {
        case AutoResolveStepType::ScanExact:      return "Scan exact";
        case AutoResolveStepType::UnknownCapture: return "Capture unknown";
        case AutoResolveStepType::UnknownCompare: return "Comparaison unknown";
        case AutoResolveStepType::TestWrite:      return "Test d'écriture";
        case AutoResolveStepType::VerifyFreeze:   return "Vérification freeze";
        case AutoResolveStepType::FindWhatWrites: return "Find What Writes";
        case AutoResolveStepType::GenerateAob:    return "Génération AOB";
        case AutoResolveStepType::SuggestPatch:   return "Suggestion patch";
        case AutoResolveStepType::ApplyPatch:     return "Application patch";
        case AutoResolveStepType::Done:           return "Terminé";
        case AutoResolveStepType::Failed:         return "Échec";
    }
    return "Inconnu";
}

AutoResolver::AutoResolver(QObject* parent)
    : QObject(parent) {
}

QList<AutoResolveStep> AutoResolver::planForGoal(const AutoResolveGoal& goal) {
    QList<AutoResolveStep> plan;

    // Étape 1: Scan exact pour la valeur cible
    AutoResolveStep step1;
    step1.type = AutoResolveStepType::ScanExact;
    step1.description = QStringLiteral("Recherche exacte de la valeur %1 (%2)")
        .arg(goal.targetValue).arg(goal.valueType);
    step1.params["value"] = static_cast<qlonglong>(goal.targetValue);
    step1.params["type"] = goal.valueType;
    plan.append(step1);

    // Étape 2: Si le scan exact ne trouve rien, on passe en unknown
    AutoResolveStep step2;
    step2.type = AutoResolveStepType::UnknownCapture;
    step2.description = "Capture de l'état initial (unknown initial value)";
    step2.params["type"] = goal.valueType;
    plan.append(step2);

    // Étapes 3-5: Comparaisons unknown (3 passes)
    const QStringList comparisons = {"increased", "increased", "changed"};
    for (int i = 0; i < 3; ++i) {
        AutoResolveStep step;
        step.type = AutoResolveStepType::UnknownCompare;
        step.description = QStringLiteral("Comparaison #%1 : la valeur a %2")
            .arg(i + 1).arg(comparisons[i]);
        step.params["mode"] = comparisons[i];
        plan.append(step);
    }

    // Étape 6: Test d'écriture sur les top candidats
    AutoResolveStep step6;
    step6.type = AutoResolveStepType::TestWrite;
    step6.description = QStringLiteral("Test d'écriture de %1 sur les 5 meilleurs candidats")
        .arg(goal.targetValue);
    step6.params["value"] = static_cast<qlonglong>(goal.targetValue);
    step6.params["maxCandidates"] = 5;
    plan.append(step6);

    // Étape 7: Vérifier si le freeze tient
    AutoResolveStep step7;
    step7.type = AutoResolveStepType::VerifyFreeze;
    step7.description = "Vérification de la persistance du freeze (2 secondes)";
    step7.params["durationMs"] = 2000;
    plan.append(step7);

    // Étape 8: Si le freeze ne tient pas, find what writes
    AutoResolveStep step8;
    step8.type = AutoResolveStepType::FindWhatWrites;
    step8.description = "Recherche de l'instruction qui écrit la valeur";
    step8.params["timeoutMs"] = 5000;
    plan.append(step8);

    // Étape 9: Générer l'AOB
    AutoResolveStep step9;
    step9.type = AutoResolveStepType::GenerateAob;
    step9.description = "Génération d'une signature AOB stable";
    plan.append(step9);

    // Étape 10: Suggérer un patch
    AutoResolveStep step10;
    step10.type = AutoResolveStepType::SuggestPatch;
    step10.description = "Suggestion de patch (NOP écriture ou forçage valeur)";
    plan.append(step10);

    return plan;
}

AutoResolveTelemetryReport computeAutoResolveTelemetryReport(const QList<QVariantMap>& events) {
    AutoResolveTelemetryReport report;

    QVariantMap eventCounts;
    int unknownTooLargeCount = 0;
    int writeOrFreezeCount = 0;

    for (const QVariantMap& event : events) {
        const QString name = event.value("event").toString();
        if (!name.isEmpty()) {
            eventCounts[name] = eventCounts.value(name).toInt() + 1;
        }

        const qulonglong matches = numericEventValue(
            event,
            {"matchesFound", "matchesReturned", "matchCount", "candidateCount", "stored", "remaining"});

        if (name.startsWith("exact_scan") && matches == 0) {
            ++report.exactZeroCount;
        }
        if (name.contains("unknown") && numericEventValue(event, {"remaining", "stored", "candidateCount", "matchesFound"}) > 100000) {
            ++unknownTooLargeCount;
        }
        if (name == "ui_string_sources_analyze") {
            report.traceUiSourceCount += static_cast<int>(numericEventValue(
                event,
                {"matchesFound", "matchesReturned", "sourceCount", "candidateCount"}));
        }
        if (name == "ui_string_investigation_finish") {
            report.traceUiGlobalHits += static_cast<int>(numericEventValue(event, {"globalValueHits"}));
        }
        if ((name.contains("aob") || name.contains("patch")) && numericEventValue(event, {"matchesFound", "matchesReturned", "matchCount"}) > 1) {
            ++report.aobMultiMatchCount;
        }
        if (name.contains("aob") || name.contains("patch") || name.contains("trainer")) {
            const QString level = event.value("signatureRisk", event.value("signatureLevel")).toString().toLower();
            const int score = nestedSignatureInt(event, "score", nestedSignatureInt(event, "signatureScore", 100));
            const int fixedBytes = nestedSignatureInt(event, "fixedBytes", nestedSignatureInt(event, "signatureFixedBytes", 99));
            const QString error = event.value("error").toString().toLower();
            if (level == "weak" || score < 35 || fixedBytes < 3) {
                ++report.aobWeakQualityCount;
            }
            if (error.contains("bloqu") || error.contains("trop faible") || error.contains("non unique")) {
                ++report.trainerBlockedCount;
            }
        }
        if (name.contains("write") || name.contains("freeze")) {
            ++writeOrFreezeCount;
        }
        if (name == "freeze_poll_instability") {
            ++report.freezeInstabilityCount;
        }
    }

    auto addInsight = [&report](const QString& id, const QString& label, const QString& reason, const QString& nextAction, bool safe) {
        report.insights.append(AutoResolveTelemetryInsight{id, label, reason, nextAction, safe});
    };

    if (report.exactZeroCount > 0) {
        addInsight(
            "exact_zero_fallback",
            "Scan exact sans candidat",
            QString("%1 scan(s) exact/multi-type recent(s) ont retourne 0 candidat.").arg(report.exactZeroCount),
            "Lancer Trace UI string ou scan chiffre borne avant de tenter un debugger.",
            true);
    }
    if (unknownTooLargeCount > 0) {
        addInsight(
            "unknown_too_large",
            "Unknown trop large",
            QString("%1 passe(s) Unknown recentes restent trop bruyantes.").arg(unknownTooLargeCount),
            "Demander une variation plus nette puis utiliser increased/decreased/changed plutot que stable.",
            true);
    }
    if (report.traceUiSourceCount > kTraceUiSourceOverflowThreshold) {
        // Un seul passage Analyser sources retourne souvent des centaines de
        // coincidences autour des strings suivies : ce n'est pas un checkpoint
        // exploitable, c'est du bruit qui a besoin d'une deuxieme variation
        // pour se filtrer.
        addInsight(
            "trace_ui_sources_overflow",
            "Trop de sources Trace UI",
            QString("%1 source(s) proches trouvees en un seul passage : trop pour un checkpoint fiable.").arg(report.traceUiSourceCount),
            "Change encore la valeur dans le jeu puis reclique Scan suivant (sources) pour ne garder que celles qui suivent vraiment, ou lance Auto origine qui enchaine plusieurs rayons automatiquement.",
            true);
    } else if (report.traceUiSourceCount > 0 || report.traceUiGlobalHits > 0) {
        addInsight(
            "trace_ui_sources_ready",
            "Trace UI exploitable",
            QString("%1 source(s) proche(s) et %2 hit(s) globalValueHits observes.").arg(report.traceUiSourceCount).arg(report.traceUiGlobalHits),
            "Promouvoir les meilleures sources en checkpoints, tester par petit lot, puis freeze confirme.",
            false);
    }
    if (report.aobMultiMatchCount > 0) {
        addInsight(
            "aob_multimatch_guard",
            "AOB multi-match",
            QString("%1 signature(s)/patch(s) recents matchent plusieurs sites.").arg(report.aobMultiMatchCount),
            "Bloquer l'application directe et regenerer une signature plus specifique autour du RIP.",
            false);
    }
    if (report.aobWeakQualityCount > 0 || report.trainerBlockedCount > 0) {
        addInsight(
            "aob_quality_guard",
            "AOB faible / Trainer bloque",
            QString("%1 signature(s) faible(s), %2 blocage(s) Trainer recents.").arg(report.aobWeakQualityCount).arg(report.trainerBlockedCount),
            "Stabiliser l'AOB: fenetre plus longue, plus d'octets fixes, verification unicite avant sauvegarde/applique.",
            false);
    }
    if (writeOrFreezeCount > 0) {
        addInsight(
            "audit_risky_actions",
            "Actions risquees detectees",
            QString("%1 evenement(s) write/freeze recents sont presents dans la telemetry.").arg(writeOrFreezeCount),
            "Conserver l'audit Investigation et sauvegarder uniquement les checkpoints verifies dans Trainer.",
            false);
    }
    if (report.freezeInstabilityCount > 0) {
        addInsight(
            "freeze_instability_detected",
            "Freeze qui ne tient pas",
            QString("%1 freeze(s) polling recent(s) ont derive au-dela du seuil de tenue.").arg(report.freezeInstabilityCount),
            "Passer en Freeze BP (bloque l'ecriture a la source) pour cette adresse.",
            false);
    }

    report.displayValueSignals =
        report.exactZeroCount > 0 ||
        report.traceUiSourceCount > 0 ||
        report.traceUiGlobalHits > 0 ||
        eventCounts.value("ui_string_scan").toInt() > 0 ||
        eventCounts.value("ui_string_sources_analyze").toInt() > 0;

    report.displayValuePattern = report.displayValueSignals
        ? QString("Valeur affichee decouplee de la source memoire possible")
        : QString();

    report.displayValueRecommendation = report.displayValueSignals
        ? (report.traceUiSourceCount > 0 || report.traceUiGlobalHits > 0
            ? QString("Priorite: tester les sources Trace UI par petits lots, puis breakpoint/debug seulement sur candidat confirme.")
            : QString("Priorite: Trace UI string, sources x100/x65536, puis find-what-writes confirme si la source suit l'affichage."))
        : QString();

    return report;
}

} // namespace killai
