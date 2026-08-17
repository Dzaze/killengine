# Patch llama_runtime + ai_engine: contexte d'etat dans les prompts + fallback deterministe conscient du contexte
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# ---------- 1. llama_runtime.h ----------
$path = 'ai\llama_runtime.h'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

$old = @'
    LlamaGenerationResult planToolCall(const QString& query, const ToolRegistry& registry) const;
'@
$new = @'
    /// Contexte d'etat transmis au prompt pour choisir le bon outil
    /// (processus attache, scan actif, candidats restants, valeurs initiale/cible...).
    LlamaGenerationResult planToolCall(
        const QString& query,
        const ToolRegistry& registry,
        const QVariantMap& context = {}) const;
'@
if (-not $content.Contains($old)) { Write-Host 'H planToolCall anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

$old = @'
    static QString buildPrompt(const QString& query, const ToolRegistry& registry);
'@
$new = @'
    static QString buildPrompt(const QString& query, const ToolRegistry& registry, const QVariantMap& context);
    static QString buildContextBlock(const QVariantMap& context);
'@
if (-not $content.Contains($old)) { Write-Host 'H buildPrompt anchor not found'; exit 1 }
$content = $content.Replace($old, $new)
[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'llama_runtime.h patched'

# ---------- 2. llama_runtime.cpp ----------
$path = 'ai\llama_runtime.cpp'
$content = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)

$old = @'
LlamaGenerationResult LlamaRuntime::planToolCall(const QString& query, const ToolRegistry& registry) const {
'@
$new = @'
LlamaGenerationResult LlamaRuntime::planToolCall(
    const QString& query,
    const ToolRegistry& registry,
    const QVariantMap& context) const {
'@
if (-not $content.Contains($old)) { Write-Host 'CPP planToolCall anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

$old = @'
        "-p", buildPrompt(query, registry),
'@
$new = @'
        "-p", buildPrompt(query, registry, context),
'@
if (-not $content.Contains($old)) { Write-Host 'CPP prompt call anchor not found'; exit 1 }
$content = $content.Replace($old, $new)

# Remplacer buildPrompt complet par la version avec contexte + regles + few-shots
$oldStart = 'QString LlamaRuntime::buildPrompt(const QString& query, const ToolRegistry& registry) {'
$oldEndMarker = '} // namespace killai'
$startIdx = $content.IndexOf($oldStart)
if ($startIdx -lt 0) { Write-Host 'CPP buildPrompt start not found'; exit 1 }
$endIdx = $content.IndexOf($oldEndMarker, $startIdx)
if ($endIdx -lt 0) { Write-Host 'CPP namespace end not found'; exit 1 }

$newFunctions = @'
QString LlamaRuntime::buildContextBlock(const QVariantMap& context) {
    if (context.isEmpty()) return QString();

    QStringList lines;
    lines << "Etat actuel de la session:";
    if (context.contains("processAttached")) {
        lines << QString("- processus attache: %1").arg(context.value("processAttached").toBool() ? "oui" : "non");
    }
    if (context.contains("processName")) {
        lines << QString("- processus: %1").arg(context.value("processName").toString());
    }
    if (context.contains("scanActive")) {
        lines << QString("- recherche guidee active: %1").arg(context.value("scanActive").toBool() ? "oui" : "non");
    }
    if (context.contains("candidateCount")) {
        lines << QString("- candidats restants: %1").arg(context.value("candidateCount").toULongLong());
    }
    if (context.contains("initialValue")) {
        const QString v = context.value("initialValue").toString();
        if (!v.isEmpty()) lines << QString("- valeur initiale cherchee: %1").arg(v);
    }
    if (context.contains("targetValue")) {
        const QString v = context.value("targetValue").toString();
        if (!v.isEmpty()) lines << QString("- valeur cible: %1").arg(v);
    }
    if (context.contains("activeTargetCount")) {
        lines << QString("- adresses actives: %1").arg(context.value("activeTargetCount").toULongLong());
    }
    return lines.join('\n');
}

QString LlamaRuntime::buildPrompt(const QString& query, const ToolRegistry& registry, const QVariantMap& context) {
    QStringList tools;
    for (const auto& item : registry.availableTools()) {
        const auto tool = item.toMap();
        tools << QString("- %1 required=%2 risk=%3 confirmation=%4")
                     .arg(
                         tool.value("name").toString(),
                         tool.value("requiredArgs").toStringList().join(","),
                         tool.value("risk", "safe").toString(),
                         tool.value("requiresConfirmation").toBool() ? "yes" : "no");
    }

    const QString contextBlock = buildContextBlock(context);

    return QString(
        "Tu es le planner local de KillEngine, un moteur de recherche memoire type cheat engine.\n"
        "Reponds uniquement avec un objet JSON compact et rien d'autre.\n"
        "Schema obligatoire: {\"tool\":\"auto_resolve|exact_scan|exact_scan_multi_type|next_scan|encrypted_scan|trace_ui_string|analyze_ui_sources|unknown_capture|unknown_compare|prepare_write_checkpoint|write_value|freeze_value|find_what_writes|generate_aob|suggest_patch\",\"args\":{...}}\n"
        "%1"
        "Regles de choix:\n"
        "- Pour un objectif utilisateur complet, privilegie auto_resolve avec args.query.\n"
        "- Les outils risk=write/debug/patch exigent confirmation explicite; ne les choisis que si l'utilisateur confirme clairement l'action risquee.\n"
        "- Si une action risquee est seulement la prochaine etape logique, choisis prepare_write_checkpoint ou auto_resolve, pas write/freeze/debug/patch direct.\n"
        "- Si la requete contient une valeur numerique actuelle sans adresse, utilise exact_scan.\n"
        "- Pour exact_scan, args doit contenir value en string et valueType.\n"
        "- Si aucun type explicite n'est donne, valueType vaut Int32.\n"
        "- Si exact_scan echoue ou si la representation est incertaine, les alternatives safe sont exact_scan_multi_type, encrypted_scan, trace_ui_string et unknown_capture.\n"
        "- Si une recherche guidee est deja active et que l'utilisateur donne une nouvelle valeur observee, choisis next_scan avec mode=exact.\n"
        "- Si aucun processus n'est attache, ne choisis aucun outil de scan; auto_resolve avec args.query reste acceptable pour planifier.\n"
        "- Si l'utilisateur decrit une valeur chiffree/obfusquee ou que le scan exact echoue, choisis encrypted_scan.\n"
        "- Si l'utilisateur decrit une valeur affichee a l'ecran mais introuvable en numerique, choisis trace_ui_string.\n"
        "- Si l'utilisateur ne connait pas la valeur (juste qu'elle augmente/diminue), choisis unknown_capture.\n"
        "Exemples:\n"
        "Exemple: j'ai 41250 argent => {\"tool\":\"exact_scan\",\"args\":{\"value\":\"41250\",\"valueType\":\"Int32\"}}\n"
        "Exemple: trouve cette valeur et guide-moi => {\"tool\":\"auto_resolve\",\"args\":{\"query\":\"trouve cette valeur et guide-moi\"}}\n"
        "Exemple: maintenant c'est 812 (recherche active) => {\"tool\":\"next_scan\",\"args\":{\"mode\":\"exact\",\"value\":\"812\"}}\n"
        "Exemple: gele l'adresse 0x1a2b3c4d a 100 => {\"tool\":\"freeze_value\",\"args\":{\"address\":\"0x1a2b3c4d\",\"valueType\":\"Int32\",\"value\":\"100\",\"enabled\":true}}\n"
        "Exemple: la valeur est affichee mais le scan ne trouve rien => {\"tool\":\"trace_ui_string\",\"args\":{\"value\":\"60\"}}\n"
        "Exemple: je ne connais pas la valeur, elle augmente quand je gagne => {\"tool\":\"unknown_capture\",\"args\":{}}\n"
        "Outils disponibles:\n%2\n"
        "Requete utilisateur: %3")
        .arg(contextBlock.isEmpty() ? QString() : contextBlock + "\n", tools.join('\n'), query);
}

'@

$content = $content.Substring(0, $startIdx) + $newFunctions + $content.Substring($endIdx)
[System.IO.File]::WriteAllText($path, $content, $utf8)
Write-Host 'llama_runtime.cpp patched'