#include "auto_resolver.h"

#include "localization/localization.h"

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
        case AutoResolveStepType::ScanExact:      return KE_TXT("Scan exact", "Exact scan");
        case AutoResolveStepType::UnknownCapture: return KE_TXT("Capture unknown", "Unknown capture");
        case AutoResolveStepType::UnknownCompare: return KE_TXT("Comparaison unknown", "Unknown comparison");
        case AutoResolveStepType::TestWrite:      return KE_TXT("Test d'écriture", "Write test");
        case AutoResolveStepType::VerifyFreeze:   return KE_TXT("Vérification freeze", "Freeze verification");
        case AutoResolveStepType::FindWhatWrites: return "Find What Writes";
        case AutoResolveStepType::GenerateAob:    return KE_TXT("Génération AOB", "AOB generation");
        case AutoResolveStepType::SuggestPatch:   return KE_TXT("Suggestion patch", "Patch suggestion");
        case AutoResolveStepType::ApplyPatch:     return KE_TXT("Application patch", "Patch application");
        case AutoResolveStepType::Done:           return KE_TXT("Terminé", "Done");
        case AutoResolveStepType::Failed:         return KE_TXT("Échec", "Failed");
    }
    return KE_TXT("Inconnu", "Unknown");
}

AutoResolver::AutoResolver(QObject* parent)
    : QObject(parent) {
}

QList<AutoResolveStep> AutoResolver::planForGoal(const AutoResolveGoal& goal) {
    QList<AutoResolveStep> plan;

    // Étape 1: Scan exact pour la valeur cible
    AutoResolveStep step1;
    step1.type = AutoResolveStepType::ScanExact;
    step1.description = KE_TXT("Recherche exacte de la valeur %1 (%2)", "Exact search for value %1 (%2)")
        .arg(goal.targetValue).arg(goal.valueType);
    step1.params["value"] = static_cast<qlonglong>(goal.targetValue);
    step1.params["type"] = goal.valueType;
    plan.append(step1);

    // Étape 2: Si le scan exact ne trouve rien, on passe en unknown
    AutoResolveStep step2;
    step2.type = AutoResolveStepType::UnknownCapture;
    step2.description = KE_TXT("Capture de l'état initial (unknown initial value)", "Capture of the initial state (unknown initial value)");
    step2.params["type"] = goal.valueType;
    plan.append(step2);

    // Étapes 3-5: Comparaisons unknown (3 passes)
    const QStringList comparisons = {"increased", "increased", "changed"};
    for (int i = 0; i < 3; ++i) {
        AutoResolveStep step;
        step.type = AutoResolveStepType::UnknownCompare;
        step.description = KE_TXT("Comparaison #%1 : la valeur a %2", "Comparison #%1: the value has %2")
            .arg(i + 1).arg(comparisons[i]);
        step.params["mode"] = comparisons[i];
        plan.append(step);
    }

    // Étape 6: Test d'écriture sur les top candidats
    AutoResolveStep step6;
    step6.type = AutoResolveStepType::TestWrite;
    step6.description = KE_TXT("Test d'écriture de %1 sur les 5 meilleurs candidats", "Write test of %1 on the top 5 candidates")
        .arg(goal.targetValue);
    step6.params["value"] = static_cast<qlonglong>(goal.targetValue);
    step6.params["maxCandidates"] = 5;
    plan.append(step6);

    // Étape 7: Vérifier si le freeze tient
    AutoResolveStep step7;
    step7.type = AutoResolveStepType::VerifyFreeze;
    step7.description = KE_TXT("Vérification de la persistance du freeze (2 secondes)", "Checking freeze persistence (2 seconds)");
    step7.params["durationMs"] = 2000;
    plan.append(step7);

    // Étape 8: Si le freeze ne tient pas, find what writes
    AutoResolveStep step8;
    step8.type = AutoResolveStepType::FindWhatWrites;
    step8.description = KE_TXT("Recherche de l'instruction qui écrit la valeur", "Looking for the instruction that writes the value");
    step8.params["timeoutMs"] = 5000;
    plan.append(step8);

    // Étape 9: Générer l'AOB
    AutoResolveStep step9;
    step9.type = AutoResolveStepType::GenerateAob;
    step9.description = KE_TXT("Génération d'une signature AOB stable", "Generating a stable AOB signature");
    plan.append(step9);

    // Étape 10: Suggérer un patch
    AutoResolveStep step10;
    step10.type = AutoResolveStepType::SuggestPatch;
    step10.description = KE_TXT("Suggestion de patch (NOP écriture ou forçage valeur)", "Patch suggestion (NOP the write or force the value)");
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
            const QString errorCode = event.value("errorCode").toString();
            if (level == "weak" || score < 35 || fixedBytes < 3) {
                ++report.aobWeakQualityCount;
            }
            // Code stable plutot que du pattern-matching sur le texte affiche
            // (traduit via KE_TXT depuis le 11/09/2026, docs/BACKEND_UI_LOCALIZATION_ROADMAP.md
            // B3) -- sinon ce compteur casse silencieusement pour un utilisateur
            // en anglais. Meme patron que trainerDependencies.ts/resolveOrderErrorMessage
            // cote frontend (chantier de localisation frontend, round U27).
            if (errorCode == "aob_signature_too_weak" || errorCode == "aob_signature_not_unique") {
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
            KE_TXT("Scan exact sans candidat", "Exact scan with no candidate"),
            KE_TXT("%1 scan(s) exact/multi-type récent(s) ont retourné 0 candidat.",
                "%1 recent exact/multi-type scan(s) returned 0 candidates.").arg(report.exactZeroCount),
            KE_TXT("Lancer Trace UI string ou scan chiffré borné avant de tenter un debugger.",
                "Run Trace UI string or a bounded encrypted scan before attempting a debugger."),
            true);
    }
    if (unknownTooLargeCount > 0) {
        addInsight(
            "unknown_too_large",
            KE_TXT("Unknown trop large", "Unknown too large"),
            KE_TXT("%1 passe(s) Unknown récente(s) restent trop bruyantes.",
                "%1 recent Unknown pass(es) remain too noisy.").arg(unknownTooLargeCount),
            KE_TXT("Demander une variation plus nette puis utiliser increased/decreased/changed plutôt que stable.",
                "Ask for a clearer variation then use increased/decreased/changed instead of stable."),
            true);
    }
    if (report.traceUiSourceCount > kTraceUiSourceOverflowThreshold) {
        // Un seul passage Analyser sources retourne souvent des centaines de
        // coincidences autour des strings suivies : ce n'est pas un checkpoint
        // exploitable, c'est du bruit qui a besoin d'une deuxieme variation
        // pour se filtrer.
        addInsight(
            "trace_ui_sources_overflow",
            KE_TXT("Trop de sources Trace UI", "Too many Trace UI sources"),
            KE_TXT("%1 source(s) proches trouvées en un seul passage : trop pour un checkpoint fiable.",
                "%1 close source(s) found in a single pass: too many for a reliable checkpoint.").arg(report.traceUiSourceCount),
            KE_TXT("Change encore la valeur dans le jeu puis reclique Scan suivant (sources) pour ne garder que celles qui suivent vraiment, ou lance Auto origine qui enchaine plusieurs rayons automatiquement.",
                "Change the value in the game again then click Next scan (sources) to keep only the ones that truly follow it, or run Auto origin which chains several rounds automatically."),
            true);
    } else if (report.traceUiSourceCount > 0 || report.traceUiGlobalHits > 0) {
        addInsight(
            "trace_ui_sources_ready",
            KE_TXT("Trace UI exploitable", "Trace UI usable"),
            KE_TXT("%1 source(s) proche(s) et %2 hit(s) globalValueHits observés.",
                "%1 close source(s) and %2 globalValueHits hit(s) observed.").arg(report.traceUiSourceCount).arg(report.traceUiGlobalHits),
            KE_TXT("Promouvoir les meilleures sources en checkpoints, tester par petit lot, puis freeze confirmé.",
                "Promote the best sources to checkpoints, test in small batches, then confirm the freeze."),
            false);
    }
    if (report.aobMultiMatchCount > 0) {
        addInsight(
            "aob_multimatch_guard",
            KE_TXT("AOB multi-match", "AOB multi-match"),
            KE_TXT("%1 signature(s)/patch(s) récent(s) matchent plusieurs sites.",
                "%1 recent signature(s)/patch(es) match multiple sites.").arg(report.aobMultiMatchCount),
            KE_TXT("Bloquer l'application directe et regénérer une signature plus spécifique autour du RIP.",
                "Block direct application and regenerate a more specific signature around the RIP."),
            false);
    }
    if (report.aobWeakQualityCount > 0 || report.trainerBlockedCount > 0) {
        addInsight(
            "aob_quality_guard",
            KE_TXT("AOB faible / Trainer bloqué", "Weak AOB / Trainer blocked"),
            KE_TXT("%1 signature(s) faible(s), %2 blocage(s) Trainer récent(s).",
                "%1 weak signature(s), %2 recent Trainer block(s).").arg(report.aobWeakQualityCount).arg(report.trainerBlockedCount),
            KE_TXT("Stabiliser l'AOB : fenêtre plus longue, plus d'octets fixes, vérification unicité avant sauvegarde/application.",
                "Stabilize the AOB: longer window, more fixed bytes, uniqueness check before saving/applying."),
            false);
    }
    if (writeOrFreezeCount > 0) {
        addInsight(
            "audit_risky_actions",
            KE_TXT("Actions risquées détectées", "Risky actions detected"),
            KE_TXT("%1 événement(s) write/freeze récent(s) sont présents dans la télémetrie.",
                "%1 recent write/freeze event(s) are present in the telemetry.").arg(writeOrFreezeCount),
            KE_TXT("Conserver l'audit Investigation et sauvegarder uniquement les checkpoints vérifiés dans Trainer.",
                "Keep the Investigation audit and only save verified checkpoints in the Trainer."),
            false);
    }
    if (report.freezeInstabilityCount > 0) {
        addInsight(
            "freeze_instability_detected",
            KE_TXT("Freeze qui ne tient pas", "Freeze that doesn't hold"),
            KE_TXT("%1 freeze(s) polling récent(s) ont dérivé au-delà du seuil de tenue.",
                "%1 recent polling freeze(s) drifted beyond the holding threshold.").arg(report.freezeInstabilityCount),
            KE_TXT("Passer en Freeze BP (bloque l'écriture à la source) pour cette adresse.",
                "Switch to Freeze BP (blocks the write at the source) for this address."),
            false);
    }

    report.displayValueSignals =
        report.exactZeroCount > 0 ||
        report.traceUiSourceCount > 0 ||
        report.traceUiGlobalHits > 0 ||
        eventCounts.value("ui_string_scan").toInt() > 0 ||
        eventCounts.value("ui_string_sources_analyze").toInt() > 0;

    report.displayValuePattern = report.displayValueSignals
        ? KE_TXT("Valeur affichée possiblement découplée de la source mémoire", "Displayed value possibly decoupled from the memory source")
        : QString();

    report.displayValueRecommendation = report.displayValueSignals
        ? (report.traceUiSourceCount > 0 || report.traceUiGlobalHits > 0
            ? KE_TXT("Priorité : tester les sources Trace UI par petits lots, puis breakpoint/debug seulement sur candidat confirmé.",
                "Priority: test the Trace UI sources in small batches, then breakpoint/debug only on a confirmed candidate.")
            : KE_TXT("Priorité : Trace UI string, sources x100/x65536, puis find-what-writes confirmé si la source suit l'affichage.",
                "Priority: Trace UI string, x100/x65536 sources, then confirmed find-what-writes if the source follows the display."))
        : QString();

    return report;
}

} // namespace killai
