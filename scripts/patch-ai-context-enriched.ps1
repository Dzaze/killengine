# Patch ApplicationController: contexte IA enrichi + dispatch prepare_write_checkpoint + noteOutcome
# Tolerant aux fins de ligne mixtes (CRLF/LF) du fichier.
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

$path = 'apps\desktop\application_controller.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

# Remplace la 1re occurrence de $old par $new, en tolerant CRLF ou LF.
# $new est ecrit en CRLF (dominant dans le fichier).
# Idempotent: si le marqueur du nouveau contenu est deja present, on passe.
function Replace-Anchor([string]$content, [string]$old, [string]$new, [string]$label, [string]$marker) {
    if ($content.Contains($marker)) { Write-Host "$label already applied"; return $content }
    $oldLf = $old -replace "`r`n", "`n"
    $newLf = $new -replace "`r`n", "`n"
    # [regex]::Escape transforme un saut de ligne reel en suite de 2 caracteres '\n'.
    # On convertit cette suite en pattern CRLF-optionnel pour tolerer les fins mixtes.
    $pattern = [regex]::Escape($oldLf) -replace '\\n', '\r?\n'
    $regex = New-Object System.Text.RegularExpressions.Regex($pattern)
    if (-not $regex.IsMatch($content)) { Write-Host "$label anchor not found"; exit 1 }
    $replacement = $newLf -replace "`n", "`r`n"
    return $regex.Replace($content, $replacement.Replace('$', '$$'), 1)
}

# ---------- 1. Contexte IA enrichi ----------
$old = @'
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
$new = @'
        QVariantMap aiContext;
        aiContext["processAttached"] = m_handle.isValid();
        aiContext["processName"] = processName();
        aiContext["scanActive"] = m_smartSearchActive;
        aiContext["candidateCount"] = static_cast<qulonglong>(m_candidates.size());
        aiContext["initialValue"] = m_smartSearchInitialValue;
        aiContext["targetValue"] = m_smartSearchTargetValue;
        aiContext["activeTargetCount"] = static_cast<qulonglong>(m_chatMemoryTargets.size());
        aiContext["unknownSnapshotActive"] = !m_snapshot.isEmpty();
        aiContext["freezeCount"] = static_cast<qulonglong>(m_freeze.entries().size());
        aiContext["valueType"] = m_smartSearchValueType;
        result = m_ai.processQuery(query, aiContext);
'@
$content = Replace-Anchor $content $old $new 'aiContext' 'aiContext["unknownSnapshotActive"]'

# ---------- 2. noteOutcome apres execution d'outil ----------
$old = @'
    const QString tool = result.value("tool").toString();
    const QVariantMap args = result.value("args").toMap();
    QVariantMap actionResult;

    if (tool == "exact_scan") {
'@
$new = @'
    const QString tool = result.value("tool").toString();
    const QVariantMap args = result.value("args").toMap();
    QVariantMap actionResult;

    // Le modele connait l'issue des actions precedentes: apres un echec il
    // proposera une alternative (multi_type, encrypted, trace UI, unknown).
    const auto noteAiOutcome = [this, &query, &tool](bool success, const QString& detail) {
        m_ai.noteOutcome(query, tool, success ? QStringLiteral("success") : QStringLiteral("failed"));
        Q_UNUSED(detail);
    };

    if (tool == "exact_scan") {
'@
$content = Replace-Anchor $content $old $new 'noteOutcome' 'noteAiOutcome'

# ---------- 3. Dispatch prepare_write_checkpoint ----------
$old = @'
    } else if (tool == "trace_ui_string") {
'@
$new = @'
    } else if (tool == "prepare_write_checkpoint") {
        // Checkpoint safe: prepare les suggestions d'ecriture sans ecrire.
        const QString checkpointValue = args.value("value", m_smartSearchTargetValue).toString();
        auto suggestions = suggestedWritesForCandidates(m_candidates, checkpointValue, kAutoWriteCandidateLimit);
        enrichSuggestedWritesWithHistory(&suggestions);
        actionResult["success"] = !suggestions.isEmpty();
        actionResult["suggestedWrites"] = suggestions;
        result["suggestedWrites"] = suggestions;
        result["suggestedWrite"] = suggestions.isEmpty() ? QVariantMap{} : suggestions.first().toMap();
        result["workflowStatus"] = suggestions.isEmpty() ? "no_candidate" : "awaiting_write_confirmation";
        result["message"] = suggestions.isEmpty()
            ? QString("Aucun candidat fiable a preparer. Continue a reduire la liste avec de nouvelles valeurs observees.")
            : QString("Checkpoint pret: %1 adresse(s) candidate(s) pour ecrire %2. Confirme l'ecriture pour appliquer.")
                  .arg(suggestions.size())
                  .arg(checkpointValue);
    } else if (tool == "trace_ui_string") {
'@
$content = Replace-Anchor $content $old $new 'prepare_write_checkpoint' 'tool == "prepare_write_checkpoint"'

# ---------- 4. Enregistrer l'outcome apres execution ----------
$old = @'
    result["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
    result["actionResult"] = actionResult;

    if (!actionResult.value("success").toBool()) {
'@
$new = @'
    result["actionStatus"] = actionResult.value("success").toBool() ? "executed" : "failed";
    result["actionResult"] = actionResult;
    noteAiOutcome(actionResult.value("success").toBool(), QString());

    if (!actionResult.value("success").toBool()) {
'@
$content = Replace-Anchor $content $old $new 'outcome recording' 'noteAiOutcome(actionResult.value("success").toBool(), QString());'

[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'application_controller.cpp patched'