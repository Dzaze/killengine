#include "investigation_notebook_planner.h"

#include "localization/localization.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariantList>

namespace killai {

namespace {

constexpr int kMaxHypotheses = 5;
constexpr int kMaxHypothesisLength = 220;
constexpr int kMaxTextLength = 260;
constexpr int kMaxPreconditions = 4;

QString compactText(QString text, int maxLength = kMaxTextLength) {
    text = text.trimmed().replace(QRegularExpression(R"(\s+)"), " ");
    if (text.size() > maxLength) {
        text = text.left(maxLength - 1).trimmed() + QLatin1Char('.');
    }
    return text;
}

QStringList compactStringList(const QVariant& value, int maxItems, int maxLength) {
    QStringList result;
    const QVariantList list = value.toList();
    for (const QVariant& item : list) {
        QString text;
        if (item.typeId() == QMetaType::QVariantMap) {
            const QVariantMap map = item.toMap();
            text = map.value("description").toString();
            if (text.isEmpty()) text = map.value("text").toString();
            if (text.isEmpty()) text = map.value("label").toString();
        } else {
            text = item.toString();
        }

        text = compactText(text, maxLength);
        if (!text.isEmpty() && !result.contains(text)) {
            result.append(text);
        }
        if (result.size() >= maxItems) {
            break;
        }
    }
    return result;
}

int findJsonObjectEnd(const QString& text, int start) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (int i = start; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == QLatin1Char('\\')) {
                escaped = true;
            } else if (ch == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }

        if (ch == QLatin1Char('"')) {
            inString = true;
        } else if (ch == QLatin1Char('{')) {
            ++depth;
        } else if (ch == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return -1;
}

QString normalizedRisk(QString risk) {
    risk = risk.trimmed().toLower();
    if (risk == "safe" || risk == "read" || risk == "readonly") return "safe";
    if (risk == "debug" || risk == "debugger") return "debug";
    if (risk == "write" || risk == "confirmation") return "confirmation";
    if (risk == "patch" || risk == "inject") return "confirmation";
    return "safe";
}

QVariantMap fallbackNextTest(const QString& symptom) {
    const QString q = symptom.toLower();
    QVariantMap next;
    next["risk"] = "safe";
    next["preconditions"] = QStringList{
        KE_TXT("Processus cible attaché", "Target process attached"),
        KE_TXT("Valeur affichée actuelle connue", "Current displayed value known"),
    };

    if ((q.contains("affich") || q.contains("display") || q.contains("écran") || q.contains("ecran"))
        && (q.contains("trouve pas") || q.contains("introuvable") || q.contains("rien") || q.contains("not found"))) {
        next["title"] = KE_TXT("Tracer la valeur affichée puis analyser ses sources numériques",
                                "Trace the displayed value then analyze its numeric sources");
        next["tool"] = "scan_ui_strings";
        next["expectedIfTrue"] = KE_TXT("Une chaîne affichée ou une source numérique proche suit la valeur observée.",
                                         "A displayed string or a nearby numeric source follows the observed value.");
        next["expectedIfFalse"] = KE_TXT("La valeur n'est probablement pas stockée sous forme texte lisible dans les pages scannées.",
                                          "The value is probably not stored as readable text in the scanned pages.");
        next["rationale"] = KE_TXT("Le symptôme parle d'une valeur visible que les scans numériques classiques ne retrouvent pas.",
                                    "The symptom describes a visible value that classic numeric scans don't find.");
        return next;
    }

    if (q.contains("freeze") || q.contains("fige") || q.contains("gèle") || q.contains("gele")
        || q.contains("réécrit") || q.contains("reecrit") || q.contains("revient") || q.contains("clignote")) {
        next["title"] = KE_TXT("Observer le rythme d'écriture du champ avant tout freeze plus agressif",
                                "Observe the field's write rate before any more aggressive freeze");
        next["tool"] = "analyze_field_stability";
        next["risk"] = "debug";
        next["preconditions"] = QStringList{
            KE_TXT("Adresse candidate sélectionnée", "Candidate address selected"),
            KE_TXT("Cible autorisée à être attachée en debugger", "Target allowed to be attached as a debugger"),
            KE_TXT("Fenêtre courte pendant laquelle la valeur varie", "Short window during which the value changes"),
        };
        next["expectedIfTrue"] = KE_TXT("Une instruction dominante réécrit vite le champ, ce qui oriente vers Freeze BP ou recherche de source.",
                                         "A dominant instruction rewrites the field quickly, pointing toward Freeze BP or source search.");
        next["expectedIfFalse"] = KE_TXT("Le champ semble événementiel ou l'adresse testée n'est pas celle qui se fait réécrire.",
                                          "The field looks event-driven, or the tested address isn't the one being rewritten.");
        next["rationale"] = KE_TXT("Le symptôme indique une valeur qui ne tient pas après écriture.",
                                    "The symptom indicates a value that doesn't stick after being written.");
        return next;
    }

    if (q.contains("sauvegarde") || q.contains("save file") || q.contains("disque") || q.contains("uwp")
        || q.contains("localstate") || q.contains("cache")) {
        next["title"] = KE_TXT("Découvrir les fichiers d'état probables avant de surveiller les changements",
                                "Discover likely state files before monitoring changes");
        next["tool"] = "discover_save_files";
        next["expectedIfTrue"] = KE_TXT("Un fichier récent ou LocalSettings bouge avec la valeur observée.",
                                         "A recent file or LocalSettings changes along with the observed value.");
        next["expectedIfFalse"] = KE_TXT("Le problème est plus probablement en mémoire vive ou dans un cache interne.",
                                          "The issue is more likely in live memory or an internal cache.");
        next["rationale"] = KE_TXT("Le symptôme mentionne une persistance ou un cache hors mémoire.",
                                    "The symptom mentions persistence or an off-memory cache.");
        return next;
    }

    next["title"] = KE_TXT("Démarrer par un scan exact multi-type puis réduire après variation contrôlée",
                            "Start with a multi-type exact scan then narrow after a controlled change");
    next["tool"] = "exact_scan_multi_type";
    next["expectedIfTrue"] = KE_TXT("Les candidats se réduisent quand la valeur change de manière contrôlée.",
                                     "Candidates narrow down when the value changes in a controlled way.");
    next["expectedIfFalse"] = KE_TXT("Basculer vers Unknown initial value ou Trace UI string selon ce qui est observable.",
                                      "Switch to Unknown initial value or Trace UI string depending on what's observable.");
    next["rationale"] = KE_TXT("Le symptôme ne donne pas encore assez d'indice pour privilégier une enquête spécialisée.",
                                "The symptom doesn't yet give enough clues to favor a specialized investigation.");
    return next;
}

} // namespace

QString buildInvestigationNotebookPlanPrompt(const QString& symptom, const QVariantMap& context) {
    QString contextLines;
    for (auto it = context.cbegin(); it != context.cend(); ++it) {
        if (it.key() == "useModel" || it.key() == "baselineScore") {
            continue;
        }
        contextLines += QString("- %1: %2\n").arg(it.key(), it.value().toString());
    }
    if (contextLines.isEmpty()) {
        contextLines = "- aucun contexte runtime supplémentaire\n";
    }

    return QStringLiteral(
        "Tu es l'assistant local de KillEngine en mode enquête.\n"
        "Objectif: proposer plusieurs hypothèses concurrentes et un seul prochain test.\n"
        "Règles strictes:\n"
        "- Réponds uniquement par un objet JSON, sans texte autour.\n"
        "- Ne fournis jamais de score, confiance, probabilité ou poids numérique.\n"
        "- Sépare hypothèses, préconditions, prédiction si vrai, prédiction si faux.\n"
        "- Ne propose aucune écriture, patch, injection ou destruction automatique.\n"
        "- Le prochain test doit être une observation ou une étape explicitement gated.\n"
        "Schéma obligatoire:\n"
        "{"
        "\"hypotheses\":[{\"description\":\"...\"}],"
        "\"nextTest\":{\"title\":\"...\",\"tool\":\"scan_ui_strings|analyze_field_stability|discover_save_files|exact_scan_multi_type|unknown_capture|manual_observation\","
        "\"risk\":\"safe|debug|confirmation\",\"preconditions\":[\"...\"],"
        "\"expectedIfTrue\":\"...\",\"expectedIfFalse\":\"...\",\"rationale\":\"...\"}"
        "}\n"
        "Symptôme utilisateur:\n%1\n"
        "Contexte:\n%2").arg(symptom.trimmed(), contextLines);
}

QVariantMap extractInvestigationNotebookPlanJson(const QString& text, QString* error) {
    int start = text.lastIndexOf('{');
    QString lastError;
    while (start >= 0) {
        const int end = findJsonObjectEnd(text, start);
        if (end > start) {
            QJsonParseError parseError;
            const QByteArray jsonBytes = text.mid(start, end - start + 1).toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QVariantMap parsed = document.object().toVariantMap();
                if (parsed.contains("hypotheses") && parsed.contains("nextTest")) {
                    if (error) error->clear();
                    return parsed;
                }
                lastError = "JSON object did not contain hypotheses and nextTest.";
            } else {
                lastError = QString("Model output JSON parse failed: %1.").arg(parseError.errorString());
            }
        }
        if (start == 0) {
            break;
        }
        start = text.lastIndexOf('{', start - 1);
    }

    if (error) {
        *error = lastError.isEmpty() ? "Model output did not contain an investigation plan JSON object." : lastError;
    }
    return {};
}

QVariantMap normalizeInvestigationNotebookPlan(const QVariantMap& proposal, const QString& symptom, const QString& source) {
    const QStringList hypotheses = compactStringList(proposal.value("hypotheses"), kMaxHypotheses, kMaxHypothesisLength);
    if (hypotheses.isEmpty()) {
        return makeFallbackInvestigationNotebookPlan(symptom, source + "_empty");
    }

    QVariantMap nextTest = proposal.value("nextTest").toMap();
    QVariantMap normalizedNext;
    normalizedNext["title"] = compactText(nextTest.value("title").toString());
    normalizedNext["tool"] = compactText(nextTest.value("tool").toString(), 80);
    normalizedNext["risk"] = normalizedRisk(nextTest.value("risk").toString());
    normalizedNext["preconditions"] = compactStringList(nextTest.value("preconditions"), kMaxPreconditions, 120);
    normalizedNext["expectedIfTrue"] = compactText(nextTest.value("expectedIfTrue").toString());
    normalizedNext["expectedIfFalse"] = compactText(nextTest.value("expectedIfFalse").toString());
    normalizedNext["rationale"] = compactText(nextTest.value("rationale").toString());

    const QVariantMap fallbackTest = fallbackNextTest(symptom);
    for (auto it = fallbackTest.cbegin(); it != fallbackTest.cend(); ++it) {
        if (normalizedNext.value(it.key()).toString().isEmpty()
            && normalizedNext.value(it.key()).toStringList().isEmpty()) {
            normalizedNext[it.key()] = it.value();
        }
    }

    QVariantMap result;
    result["success"] = true;
    result["source"] = source;
    result["modelUsed"] = !source.startsWith("deterministic");
    result["hypotheses"] = hypotheses;
    result["nextTest"] = normalizedNext;
    result["summary"] = compactText(proposal.value("summary").toString());
    return result;
}

QVariantMap makeFallbackInvestigationNotebookPlan(const QString& symptom, const QString& source) {
    const QString q = symptom.toLower();
    QStringList hypotheses;
    hypotheses << KE_TXT("La valeur affichée est une copie UI recalculée depuis une source interne plus stable.",
                          "The displayed value is a UI copy recomputed from a more stable internal source.");
    hypotheses << KE_TXT("La vraie source numérique existe ailleurs en mémoire, avec une représentation différente ou proche d'une chaîne affichée.",
                          "The real numeric source exists elsewhere in memory, with a different representation or near a displayed string.");

    if (q.contains("sauvegarde") || q.contains("save") || q.contains("disque") || q.contains("cache") || q.contains("uwp")) {
        hypotheses << KE_TXT("La valeur persistante est restaurée depuis un fichier d'état, LocalSettings ou un cache hors mémoire.",
                              "The persistent value is restored from a state file, LocalSettings, or an off-memory cache.");
    } else if (q.contains("freeze") || q.contains("fige") || q.contains("revient") || q.contains("clignote")) {
        hypotheses << KE_TXT("Une instruction de la cible réécrit périodiquement le champ après l'écriture externe.",
                              "An instruction in the target periodically rewrites the field after the external write.");
    } else {
        hypotheses << KE_TXT("Le scan initial est trop étroit et doit être élargi en multi-type ou Unknown initial value.",
                              "The initial scan is too narrow and should be widened to multi-type or Unknown initial value.");
    }

    QVariantMap result;
    result["success"] = true;
    result["source"] = source;
    result["modelUsed"] = false;
    result["hypotheses"] = hypotheses;
    result["nextTest"] = fallbackNextTest(symptom);
    result["summary"] = KE_TXT("Plan de repli déterministe, utilisé quand le modèle local n'est pas disponible.",
                                "Deterministic fallback plan, used when the local model is unavailable.");
    return result;
}

} // namespace killai
