# Patch ai_engine.cpp: processQuery avec contexte (ancres mono-ligne, header deja patche)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

$path = 'ai\ai_engine.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

# 1. processQuery(query) delegue vers version avec contexte
$old = 'QVariantMap AIEngine::processQuery(const QString& query) {'
$new = "QVariantMap AIEngine::processQuery(const QString& query) {`n    return processQuery(query, {});`n}`n`nQVariantMap AIEngine::processQuery(const QString& query, const QVariantMap& context) {"
if (-not $content.Contains($old)) { Write-Host 'CPP processQuery anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

# 2. llama avec contexte
$old = 'const auto generated = m_llama.planToolCall(query, m_registry);'
$new = 'const auto generated = m_llama.planToolCall(query, m_registry, context);'
if (-not $content.Contains($old)) { Write-Host 'CPP planToolCall call anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

# 3. fallback deterministe avec contexte
$old = 'auto fallback = deterministicPlan(query);'
$new = 'auto fallback = deterministicPlanWithContext(query, context);'
if (-not $content.Contains($old)) { Write-Host 'CPP fallback anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

# 4. Inserer deterministicPlanWithContext avant makeToolCall
$anchor = 'QVariantMap AIEngine::makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale) {'
$idx = $content.IndexOf($anchor)
if ($idx -lt 0) { Write-Host 'CPP makeToolCall anchor not found'; exit 1 }

$body = @'
QVariantMap AIEngine::deterministicPlanWithContext(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();
    const bool processAttached = context.value("processAttached", true).toBool();
    const bool scanActive = context.value("scanActive").toBool();
    const auto candidateCount = context.value("candidateCount").toULongLong();
    const QString value = firstNumber(query);

    // Garde-fou : sans processus attache, aucun scan n'a de sens.
    if (!processAttached) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = "Attache d'abord un processus dans l'onglet Processus, puis relance ta recherche.";
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    // Recherche active + nouvelle valeur observee => reduction plutot que nouveau scan.
    if (scanActive && !value.isEmpty()) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, "Une recherche est deja active: je reduis les candidats avec la nouvelle valeur observee.");
    }

    // Intentions speciales valorisees avant le scan brut.
    if (q.contains("freeze") || q.contains("geler")) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
            {"enabled", true},
        }, "Freeze demande par l'utilisateur.");
    }
    if ((q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, "Ecriture memoire demandee.");
    }
    if (q.contains("unknown") || q.contains("inconnue")
        || (q.contains("sais pas") && (q.contains("valeur") || q.contains("vaut")))
        || (q.contains("augmente") && value.isEmpty() && !scanActive)
        || (q.contains("diminue") && value.isEmpty() && !scanActive)) {
        if (q.contains("capture") || q.contains("initial") || value.isEmpty()) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, "Capture initiale pour valeur inconnue.");
        }
        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", "Auto"}}, "Comparaison unknown initial value.");
    }
    // Valeur affichee a l'ecran introuvable en numerique.
    if ((q.contains("affich") || q.contains("texte")) && !value.isEmpty()) {
        return makeToolCall("trace_ui_string", {{"value", value}}, "Valeur affichee a l'ecran: je cherche la string UI puis ses sources.");
    }
    // Valeur potentiellement chiffree/obfusquee.
    if ((q.contains("chiffr") || q.contains("obfusqu") || q.contains("crypt") || q.contains("xor")) && !value.isEmpty()) {
        QVariantMap args;
        args["value"] = value;
        args["valueType"] = inferValueType(query);
        args["mode"] = "xor";
        args["keySearchBits"] = 16;
        return makeToolCall("encrypted_scan", args, "Valeur possiblement chiffree: scan XOR/Add/Sub borne.");
    }

    if (!value.isEmpty()) {
        if (scanActive && candidateCount > 0) {
            m_stateMachine.setState(AIState::Refining);
            return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, "Recherche active avec candidats: reduction avec la nouvelle valeur.");
        }
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, "Premier scan exact depuis une valeur detectee.");
    }

    // Aucune valeur: objectifs complets ou guidance plutot que message brut.
    if (q.contains("trouve") || q.contains("cherche") || q.contains("objectif") || q.contains("guide")) {
        return makeToolCall("auto_resolve", {{"query", query}}, "Objectif complet sans valeur directe: mini-boucle safe Auto.");
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = "Je n'ai pas trouve de valeur ou d'action claire. Donne-moi la valeur affichee (ex: 41250), decris ce que tu cherches (ca augmente quand...), ou colle une adresse 0x....";
    result["state"] = m_stateMachine.currentStateName();
    result["availableTools"] = m_registry.availableTools();
    return result;
}
'@
$body = $body.Replace("`r`n", "`n")
$content = $content.Substring(0, $idx) + $body + "`n" + $content.Substring($idx)
[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'ai_engine.cpp patched'

# 5. ApplicationController: passer le contexte (ancre mono-ligne)
$path = 'apps\desktop\application_controller.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

$old = 'result = m_ai.processQuery(query);'
$new = @'
QVariantMap aiContext;
        aiContext["processAttached"] = m_handle.isValid();
        aiContext["processName"] = processName();
        aiContext["scanActive"] = m_smartSearchActive;
        aiContext["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
        aiContext["initialValue"] = m_smartSearchInitialValue;
        aiContext["targetValue"] = m_smartSearchTargetValue;
        aiContext["activeTargetCount"] = static_cast<qulonglong>(m_chatMemoryTargets.size());
        result = m_ai.processQuery(query, aiContext);
'@
$new = $new.Replace("`r`n", "`n").TrimEnd("`n")
if (-not $content.Contains($old)) { Write-Host 'APP processQuery anchor not found'; exit 1 }
$content = $content.Replace($old, $new)
[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'application_controller.cpp patched'