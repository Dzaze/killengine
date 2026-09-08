#include "llama_runtime.h"

#include "investigation_notebook_planner.h"
#include "llama_server.h"
#include "logging/logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace killai {

namespace {

/// Delai avant de retenter le serveur persistant apres un echec (voir
/// m_serverRetryAfterMs, ai/llama_runtime.h) -- assez long pour ne pas
/// marteler un serveur reellement casse a chaque message, assez court pour
/// se retablir dans la meme session apres un ralentissement passager.
constexpr qint64 kServerRetryCooldownMs = 120000; // 2 minutes

/// Retire les blocs de raisonnement <think>...</think> emis par Qwen3.5
/// (le champ content de /completion peut en contenir si le mode reasoning
/// n'est pas desactive cote serveur) et retourne le texte utile.
QString stripThinkingBlocks(const QString& text) {
    static const QRegularExpression thinkRe(
        "<think>.*?</think>", QRegularExpression::DotMatchesEverythingOption);
    QString cleaned = text;
    cleaned.remove(thinkRe);
    const int openIndex = cleaned.indexOf("<think>");
    const int closeIndex = cleaned.lastIndexOf("</think>");
    if (openIndex >= 0) {
        // Bloc non ferme: budget de tokens epuise pendant le raisonnement.
        cleaned.truncate(openIndex);
    } else if (closeIndex >= 0) {
        // Fermeture orpheline: le serveur peut omettre le tag d'ouverture,
        // on garde uniquement ce qui suit la fin du raisonnement.
        cleaned = cleaned.mid(closeIndex + QString("</think>").size());
    }
    return cleaned.trimmed();
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
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return -1;
}

} // namespace

bool LlamaRuntime::init() {
    const auto model = ModelLocator::findQwenGguf();
    const QString executable = findExecutable();
    const QString serverExecutable = LlamaServer::locateExecutable();

    m_info.modelPath = model.path;
    m_info.executablePath = executable;
    m_serverExecutablePath = serverExecutable;
    m_serverUsable = !serverExecutable.isEmpty();

    // Le serveur persistant suffit meme sans llama-cli (il embarque le meme moteur).
    const bool hasAnyBackend = !executable.isEmpty() || !serverExecutable.isEmpty();
    if (!hasAnyBackend) {
        m_info.available = false;
        m_info.errorMessage = "No llama.cpp backend found. Set KILLENGINE_LLAMA_CLI or KILLENGINE_LLAMA_SERVER.";
        return false;
    }

    if (!model.found) {
        m_info.available = false;
        m_info.errorMessage = model.errorMessage;
        return false;
    }

    // Configure le serveur persistant des maintenant (demarrage paresseux au
    // premier appel de completion, pas ici).
    if (!serverExecutable.isEmpty()) {
        LlamaServer::instance().setModel(model.path, serverExecutable);
    }

    m_info.available = true;
    m_info.errorMessage.clear();
    return true;
}

bool LlamaRuntime::isAvailable() const {
    return m_info.available;
}

LlamaRuntimeInfo LlamaRuntime::info() const {
    return m_info;
}

LlamaGenerationResult LlamaRuntime::generate(const QString& prompt, int nPredict) const {
    LlamaGenerationResult result;

    // 1) Serveur persistant: modele deja charge en RAM, prefixe cache.
    // PHASE (08/09/2026) : reessaie apres un cooldown plutot que d'abandonner
    // le serveur pour le reste de la session sur un seul echec (voir
    // m_serverRetryAfterMs, ai/llama_runtime.h) -- un echec passager
    // (machine chargee au moment precis de l'appel) ne doit pas condamner
    // toute la session au fallback llama-cli, plus lent (aucun cache de
    // prompt, processus a froid a chaque appel).
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool serverDueForRetry = !m_serverUsable && nowMs >= m_serverRetryAfterMs;
    if ((m_serverUsable || serverDueForRetry) && !m_serverExecutablePath.isEmpty()) {
        const auto completion = LlamaServer::instance().complete(
            prompt, nPredict, QStringList{"\nRequete utilisateur:", "Requete:"});
        if (completion.success) {
            if (!m_serverUsable) {
                KE_LOG_INFO() << "llama-server recovered after cooldown, resuming persistent server.";
            }
            m_serverUsable = true;
            result.success = true;
            result.output = completion.content;
            result.backend = "llama-server";
            return result;
        }
        // Echec (premier ou apres cooldown) : retombe sur llama-cli pour ce
        // message, mais reessaiera le serveur automatiquement apres
        // kServerRetryCooldownMs plutot que jamais.
        m_serverUsable = false;
        m_serverRetryAfterMs = nowMs + kServerRetryCooldownMs;
        KE_LOG_INFO() << "llama-server unavailable, falling back to llama-cli (retry in "
                      << (kServerRetryCooldownMs / 1000) << "s): "
                      << completion.errorMessage.toStdString();
    }

    // 2) Fallback historique: processus llama-cli one-shot.
    if (m_info.executablePath.isEmpty()) {
        result.errorMessage = "llama-cli executable not found.";
        return result;
    }

    QProcess process;
    process.setProgram(m_info.executablePath);
    process.setArguments({
        "-m", m_info.modelPath,
        "-p", prompt,
        "-n", QString::number(nPredict),
        "--temp", "0",
        "--no-display-prompt",
        "--single-turn",
        "--reasoning", "off",
        "--no-warmup",
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(5000)) {
        result.errorMessage = "llama-cli failed to start.";
        return result;
    }

    // PHASE (08/09/2026) : 60s s'est revele insuffisant en conditions reelles
    // pour un prompt de cette taille (~58 outils) traite a froid (aucun cache,
    // contrairement au serveur persistant) sur un CPU portable modeste --
    // remonte a 90s, meme budget que le chargement modele du serveur
    // (startupTimeoutMs, ai/llama_server.cpp).
    if (!process.waitForFinished(90000)) {
        process.kill();
        process.waitForFinished(3000);
        result.errorMessage = "llama-cli timed out.";
        return result;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.errorMessage = QString("llama-cli failed: %1").arg(output);
        return result;
    }

    result.success = true;
    result.output = output;
    result.backend = "llama-cli";
    return result;
}

LlamaGenerationResult LlamaRuntime::planToolCall(
    const QString& query,
    const ToolRegistry& registry,
    const QVariantMap& context) const {
    LlamaGenerationResult result;
    if (!m_info.available) {
        result.errorMessage = m_info.errorMessage;
        return result;
    }
    // Budget genereux: le bloc <think> eventuel est stripte ensuite, le JSON
    // doit rester dans le budget.
    auto generated = generate(buildPrompt(query, registry, context), 384);
    if (generated.success) {
        generated.output = stripThinkingBlocks(generated.output);
    }
    return generated;
}

LlamaGenerationResult LlamaRuntime::planIntent(const QString& query) const {
    LlamaGenerationResult result;
    if (!m_info.available) {
        result.errorMessage = m_info.errorMessage;
        return result;
    }
    auto generated = generate(buildIntentPrompt(query), 256);
    if (generated.success) {
        generated.output = stripThinkingBlocks(generated.output);
    }
    return generated;
}

LlamaGenerationResult LlamaRuntime::planInvestigationNotebook(const QString& symptom, const QVariantMap& context) const {
    LlamaGenerationResult result;
    if (!m_info.available) {
        result.errorMessage = m_info.errorMessage;
        return result;
    }
    auto generated = generate(buildInvestigationNotebookPlanPrompt(symptom, context), 512);
    if (generated.success) {
        generated.output = stripThinkingBlocks(generated.output);
    }
    return generated;
}

QVariantMap LlamaRuntime::extractToolCallJson(const QString& text, QString* error) {
    int start = text.lastIndexOf('{');
    if (start < 0) {
        if (error) *error = "Model output did not contain a JSON object.";
        return {};
    }

    QString lastError;
    while (start >= 0) {
        const int end = findJsonObjectEnd(text, start);
        if (end > start) {
            QJsonParseError parseError;
            const QByteArray jsonBytes = text.mid(start, end - start + 1).toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QVariantMap parsed = document.object().toVariantMap();
                if (parsed.contains("tool") && parsed.contains("args")) {
                    QVariantMap call;
                    call["tool"] = parsed.value("tool").toString();
                    call["args"] = parsed.value("args").toMap();
                    if (error) error->clear();
                    return call;
                }
                lastError = "JSON object did not contain a tool call.";
            } else if (parseError.error != QJsonParseError::NoError) {
                lastError = QString("Model output JSON parse failed: %1.").arg(parseError.errorString());
            }
        }

        start = text.lastIndexOf('{', start - 1);
    }

    if (error) *error = lastError.isEmpty() ? "Model output did not contain a tool call JSON object." : lastError;
    return {};
}

QVariantMap LlamaRuntime::extractIntentJson(const QString& text, QString* error) {
    int start = text.lastIndexOf('{');
    if (start < 0) {
        if (error) *error = "Model output did not contain a JSON object.";
        return {};
    }

    QString lastError;
    while (start >= 0) {
        const int end = findJsonObjectEnd(text, start);
        if (end > start) {
            QJsonParseError parseError;
            const QByteArray jsonBytes = text.mid(start, end - start + 1).toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QVariantMap parsed = document.object().toVariantMap();
                if (parsed.contains("intent")) {
                    if (error) error->clear();
                    return parsed;
                }
                lastError = "JSON object did not contain an intent.";
            } else if (parseError.error != QJsonParseError::NoError) {
                lastError = QString("Model output JSON parse failed: %1.").arg(parseError.errorString());
            }
        }

        start = text.lastIndexOf('{', start - 1);
    }

    if (error) *error = lastError.isEmpty() ? "Model output did not contain an intent JSON object." : lastError;
    return {};
}

QString LlamaRuntime::findExecutable() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString envExe = env.value("KILLENGINE_LLAMA_CLI").trimmed();
    if (!envExe.isEmpty() && QFileInfo::exists(envExe)) {
        return QFileInfo(envExe).absoluteFilePath();
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("llama-cli.exe"),
        appDir.filePath("llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/llama-cli.exe"),
    };

    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return {};
}

QString LlamaRuntime::buildIntentPrompt(const QString& query) {
    return QString(
        "Tu es l'interpreteur d'intention de KillEngine.\n"
        "Reponds uniquement avec un objet JSON compact et rien d'autre.\n"
        "Schema: {\"intent\":\"Unknown|ResetContext|ExactScan|GuidedScan|RefineScan|ActivateMemoryTargets|WriteMemoryTargets|RewriteLastTargets|WriteProfileTargets|ReportBadTargets\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[],\"confidence\":0.0,\"missing\":\"\"}\n"
        "Regles:\n"
        "- ExactScan: l'utilisateur donne une valeur actuelle a chercher.\n"
        "- GuidedScan: l'utilisateur donne une valeur actuelle et une valeur cible.\n"
        "- RefineScan: l'utilisateur donne une nouvelle valeur observee pour reduire une recherche deja active.\n"
        "- ActivateMemoryTargets: l'utilisateur donne une ou plusieurs adresses 0x sans valeur a ecrire.\n"
        "- WriteMemoryTargets: l'utilisateur donne adresse(s) 0x et valeur a ecrire, ou demande d'ecrire sur adresses actives.\n"
        "- RewriteLastTargets: l'utilisateur demande de repasser/modifier les dernieres adresses trouvees.\n"
        "- WriteProfileTargets: l'utilisateur demande explicitement d'ecrire sur une cible nommee/profil.\n"
        "- ReportBadTargets: l'utilisateur signale que les dernieres adresses/ecritures ne marchent pas.\n"
        "- ResetContext: l'utilisateur veut une autre recherche ou repartir de zero.\n"
        "- Si une valeur requise manque, mets intent Unknown et missing avec la question courte a poser.\n"
        "Exemples:\n"
        "j'ai 900 en score je veux le passer a 1000 => {\"intent\":\"GuidedScan\",\"value\":\"900\",\"targetValue\":\"1000\",\"addresses\":[],\"confidence\":0.95,\"missing\":\"\"}\n"
        "0x2d3a80afb0c et 0x2d3f7e25594 => {\"intent\":\"ActivateMemoryTargets\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[\"0x2d3a80afb0c\",\"0x2d3f7e25594\"],\"confidence\":0.95,\"missing\":\"\"}\n"
        "passe le score => {\"intent\":\"Unknown\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[],\"confidence\":0.4,\"missing\":\"Tu veux le passer a quelle valeur ?\"}\n"
        "Requete utilisateur: %1")
        .arg(query);
}

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
    if (context.contains("unknownSnapshotActive")) {
        lines << QString("- snapshot unknown capture: %1")
                     .arg(context.value("unknownSnapshotActive").toBool() ? "oui (unknown_compare disponible)" : "non");
    }
    if (context.contains("freezeCount")) {
        const auto freezeCount = context.value("freezeCount").toULongLong();
        if (freezeCount > 0) lines << QString("- adresses gelees (freeze): %1").arg(freezeCount);
    }
    if (context.contains("valueType")) {
        const QString v = context.value("valueType").toString();
        if (!v.isEmpty()) lines << QString("- dernier type de valeur: %1").arg(v);
    }
    return lines.join('\n');
}

QString LlamaRuntime::buildHistoryBlock(const QVariantMap& context) {
    const QVariantList history = context.value("history").toList();
    if (history.isEmpty()) return QString();

    QStringList lines;
    lines << "Conversation recente (plus ancien d'abord):";
    // On garde les 4 derniers tours pour limiter le prompt.
    const int start = history.size() > 4 ? history.size() - 4 : 0;
    for (int i = start; i < history.size(); ++i) {
        const auto turn = history.at(i).toMap();
        const QString query = turn.value("query").toString();
        const QString tool = turn.value("tool").toString();
        const QString outcome = turn.value("outcome").toString();
        if (query.isEmpty()) continue;
        QString line = QString("- utilisateur: \"%1\"").arg(query.left(120));
        if (!tool.isEmpty()) line += QString(" -> outil: %1").arg(tool);
        if (!outcome.isEmpty()) line += QString(" (%1)").arg(outcome.left(40));
        lines << line;
    }
    return lines.join('\n');
}

QString LlamaRuntime::buildDynamicHints(const QVariantMap& context) {
    QStringList hints;

    const bool scanActive = context.value("scanActive").toBool();
    const auto candidateCount = context.value("candidateCount").toULongLong();
    const QString targetValue = context.value("targetValue").toString();
    const bool unknownSnapshot = context.value("unknownSnapshotActive").toBool();

    // Indications prioritaires deduites de l'etat: elles guident le modele
    // vers l'outil pertinent sans lui imposer une reponse.
    if (unknownSnapshot) {
        hints << "- Un snapshot unknown est capture: si l'utilisateur decrit une variation (augmente/diminue/change/stable), choisis unknown_compare.";
    }
    if (scanActive && !targetValue.isEmpty() && candidateCount > 0 && candidateCount <= 10) {
        hints << "- Peu de candidats restants avec valeur cible connue: prepare_write_checkpoint est la prochaine etape logique (safe, sans ecrire).";
    }
    if (scanActive && candidateCount > 10000) {
        hints << "- Tres nombreux candidats: propose a l'utilisateur de refaire varier la valeur (next_scan) plutot que d'ecrire.";
    }
    if (context.contains("history")) {
        const QVariantList history = context.value("history").toList();
        if (!history.isEmpty()) {
            const auto lastTurn = history.last().toMap();
            if (lastTurn.value("outcome").toString() == "failed") {
                hints << "- La derniere action a echoue: propose une alternative differente (multi_type, encrypted_scan, trace_ui_string ou unknown_capture).";
            }
        }
    }
    return hints.isEmpty() ? QString() : hints.join('\n');
}

QString LlamaRuntime::buildPrompt(const QString& query, const ToolRegistry& registry, const QVariantMap& context) {
    // IMPORTANT: le prefixe (systeme + regles + exemples + outils) est
    // volontairement STATIQUE et place en tete: le serveur persistant active
    // cache_prompt, donc ce prefixe n'est re-encode qu'une seule fois pour
    // toutes les requetes. Seuls le contexte/historique/requete changent.
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
    const QString historyBlock = buildHistoryBlock(context);
    const QString hintsBlock = buildDynamicHints(context);

    return QString(
        "Tu es le planner local de KillEngine, en mode Inspecteur Codex: tu raisonnes comme un enqueteur prudent de recherche memoire.\n"
        "Reponds uniquement avec un objet JSON compact et rien d'autre. Ne raisonnes pas, pas de bloc <think>, pas d'explication: uniquement le JSON final.\n"
        // PHASE 140 : get_auto_report/disassemble_backward/test_candidate_fields
        // manquaient ici (presents dans le registre et donc dans le bloc
        // "Outils disponibles" genere plus bas, mais absents de CETTE ligne
        // schema codee en dur) -- le modele local ne pouvait donc jamais les
        // choisir, meme une fois leur dispatch cote startSmartSearch branche.
        // Cette ligne ne se genere pas automatiquement depuis le registre :
        // verifier les deux restent synchronises si un outil est ajoute/retire.
        //
        // PHASE 148 : audit complet de ce qui manquait encore, decide outil
        // par outil (voir docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md pour le
        // detail) :
        //   - AJOUTES ici (lecture seule ou attente bornee courte, deja
        //     dispatches cote startSmartSearch) : discover_save_files,
        //     inspect_local_settings, read_save_file_text, watch_save_file.
        //   - VOLONTAIREMENT EXCLUS (write/injection reel) : kernel_write,
        //     speedhack_set, block_process_network, patch_file_bytes,
        //     apply_stealth_mode, restore_stealth_mode (risk=injection,
        //     requiresConfirm=true, ai/tool_registry.cpp). Ces 5 restent
        //     joignables UNIQUEMENT sur mot-cle explicite
        //     (ai/ai_engine.cpp, deterministicPlan/deterministicPlanWithContext)
        //     -- jamais un choix libre du modele, leur propre description dit
        //     deja "a utiliser seulement si l'utilisateur le demande
        //     explicitement". patch_file_bytes redirige toujours vers l'UI/pipe
        //     sans jamais executer depuis le chat (voir son dispatch,
        //     apps/desktop/application_controller.cpp) -- pas encore de
        //     fast-path explicite non plus, laisse hors schema par prudence
        //     tant que ce chemin n'est pas davantage exerce.
        //
        // PHASE 271 : tous les outils exposes au LLM, y compris ceux a risque.
        // L'Assistant doit avoir acces a toutes les capacites sans restriction.
        "Schema obligatoire: {\"tool\":\"auto_resolve|get_auto_report|exact_scan|exact_scan_module|exact_scan_multi_type|next_scan|get_candidates|encrypted_scan|trace_ui_string|analyze_ui_sources|read_window_text|list_process_modules|start_changed_pages_diff|finish_changed_pages_diff|unknown_capture|unknown_compare|prepare_write_checkpoint|write_value|freeze_value|find_what_writes|analyze_field_stability|generate_aob|suggest_patch|disassemble_backward|test_candidate_fields|discover_save_files|inspect_local_settings|read_save_file_text|watch_save_file|trainer_list_features|trainer_create_write|trainer_delete_feature|trainer_apply_request|trainer_restore_request|get_stealth_status|apply_stealth_mode|restore_stealth_mode|getWebView2InspectorStatus|listWebView2CdpTargets|connectWebView2Inspector|disconnectWebView2Inspector|evaluateWebView2JavaScript|findWebView2DisplayedValues|findWebView2DisplayedText|probeWebView2GlobalScope|kernel_write|speedhack_set|block_process_network|get_process_network_connections|get_process_network_modules|start_http_proxy|stop_http_proxy|get_http_proxy_requests|modify_http_request|spoof_dns|restore_dns|set_lag_switch|patch_file_bytes\",\"args\":{...}}\n"
        "Posture Inspecteur:\n"
        "- Observe avant d'ecrire: une adresse n'est fiable que si elle suit plusieurs variations et si l'hypothese explique les echecs precedents.\n"
        "- Distingue source gameplay, copie d'affichage, buffer UI recycle, table de sequence et pointeur intermediaire.\n"
        "- En cas de crash ou de cible fragile, evite les breakpoints externes/debug; privilegie les outils lecture seule et les snapshots bornes.\n"
        "- Quand une piste echoue, ne repete pas le meme scan: change d'hypothese et choisis l'outil le moins invasif qui produit une preuve nouvelle.\n"
        "Regles de choix:\n"
        "- Pour un objectif utilisateur complet, privilegie auto_resolve avec args.query.\n"
        "- Les outils risk=write/debug/patch exigent confirmation explicite; ne les choisis que si l'utilisateur confirme clairement l'action risquee.\n"
        "- Si une action risquee est seulement la prochaine etape logique, choisis prepare_write_checkpoint ou auto_resolve, pas write/freeze/debug/patch direct.\n"
        "- Si l'utilisateur demande le mode inspecteur, une enquete prudente, ou veut comprendre avant d'ecrire, choisis start_changed_pages_diff si aucune capture diff n'est active; choisis finish_changed_pages_diff si l'utilisateur donne valeur precedente et valeur actuelle.\n"
        "- Si la requete contient une valeur numerique actuelle sans adresse, utilise exact_scan (ou exact_scan_multi_type si le type est incertain).\n"
        "- Pour exact_scan, args doit contenir value en string et valueType.\n"
        "- Si l'utilisateur nomme un module/DLL concret et une valeur, choisis exact_scan_module avec args.module, args.value et args.valueType; cela borne le scan a ce module charge.\n"
        "- Si aucun type explicite n'est donne, valueType vaut Int32.\n"
        "- Si exact_scan echoue ou si la representation est incertaine, les alternatives safe sont exact_scan_multi_type, encrypted_scan, trace_ui_string et unknown_capture.\n"
        "- Si Trace UI string trouve des copies instables, buffers recycles, ou que l'adresse de string n'est pas ecrite sur place, choisis start_changed_pages_diff avant la prochaine variation, puis finish_changed_pages_diff apres la variation.\n"
        "- Si la cible est une app UWP/Store ou que la synchronisation utilisateur est floue, choisis read_window_text avant de lancer une observation memoire.\n"
        "- Si l'utilisateur demande les DLL/modules, ou dit que exact/increased/decreased ne convergent pas et qu'il faut chercher dans les DLL/modules, choisis list_process_modules; lecture seule, aucun argument.\n"
        "- Si une recherche guidee est deja active et que l'utilisateur donne une nouvelle valeur observee, choisis next_scan avec mode=exact.\n"
        "- Si une recherche est active et que l'utilisateur decrit une variation sans valeur (augmente/diminue/change), choisis next_scan avec mode=increased|decreased|changed.\n"
        "- Si l'utilisateur demande l'adresse exacte des candidats restants, choisis get_candidates (aucun argument, lecture seule) -- ne devine JAMAIS une adresse, next_scan/exact_scan ne renvoient qu'un nombre de candidats, jamais leur adresse. Si le resultat revient tronque (displaySuppressed), il reste trop de candidats: continue a reduire avec next_scan avant de rappeler get_candidates. Plusieurs adresses reelles peuvent legitimement correspondre a la meme valeur logique (copies redondantes, checksums) -- ne suppose pas qu'une seule est la bonne.\n"
        "- Si aucun processus n'est attache, ne choisis aucun outil de scan; auto_resolve avec args.query reste acceptable pour planifier.\n"
        "- Si l'utilisateur decrit une valeur chiffree/obfusquee ou que le scan exact echoue, choisis encrypted_scan.\n"
        "- Si l'utilisateur decrit une valeur affichee a l'ecran mais introuvable en numerique, choisis trace_ui_string.\n"
        "- Si l'utilisateur ne connait pas la valeur (juste qu'elle augmente/diminue), choisis unknown_capture.\n"
        "- Si l'utilisateur signale que les dernieres adresses ne marchent pas, propose une alternative (ReportBadTargets implicite): multi_type, encrypted_scan, trace_ui_string ou unknown_capture.\n"
        "- Pour le Trainer en langage naturel: liste via trainer_list_features; cree seulement une feature write si adresse 0x... et valeur sont explicites; pour activer/restaurer, choisis trainer_apply_request/trainer_restore_request afin de demander confirmation UI, jamais une activation autonome.\n"
        "- Si l'utilisateur demande si une adresse est un champ affiche/derive ou une vraie source (avant de figer/patcher), choisis analyze_field_stability avec args.address; c'est une observation passive (aucune ecriture), pas besoin d'une confirmation d'ecriture, mais ca attache un debugger comme find_what_writes.\n"
        "- Si l'utilisateur demande un bilan/rapport de l'auto-resolution (contexte, strategie recommandee, evenements recents), choisis get_auto_report; aucun argument requis, lecture seule.\n"
        "- Si l'utilisateur demande d'analyser les sources numeriques d'une string UI deja localisee (apres trace_ui_string), choisis analyze_ui_sources avec args.value (la valeur affichee actuelle); l'adresse de la string est reprise automatiquement de la derniere recherche si non fournie.\n"
        "- Si l'utilisateur demande de generer une signature AOB ou de suggerer un patch de code pour une adresse, choisis generate_aob ou suggest_patch avec args.address; lecture seule, ne modifie jamais la memoire (l'application reelle d'un patch reste un geste UI/pipe distinct).\n"
        "- Si l'utilisateur demande de desassembler en arriere pour trouver les champs sources d'une instruction connue (apres analyze_field_stability=likely_derived_display par exemple), choisis disassemble_backward avec args.address; lecture seule.\n"
        "- Si l'utilisateur demande explicitement de TESTER quels champs candidats tiennent reellement (ecriture test + restauration automatique, ~1 minute), choisis test_candidate_fields; depuis le chat cette action redirige toujours vers l'UI (ecrit reellement une valeur test sur la cible), jamais d'execution autonome.\n"
        "- Si l'utilisateur demande de capturer ce qui ecrit une adresse (find_what_writes), sache que depuis le chat cette action redirige toujours vers l'UI (attache un debugger, necessite de faire varier la valeur en direct), jamais d'execution autonome -- choisis quand meme find_what_writes avec args.address si demande explicitement, le dispatch gere la redirection.\n"
        "- Si le scan memoire echoue de facon repetee (valeur instable/reallouee) sur une cible UWP, la valeur affichee vient peut-etre d'un fichier sur disque plutot que d'une adresse memoire stable : choisis discover_save_files (aucun argument) pour chercher les fichiers de sauvegarde, ou inspect_local_settings si un fichier de sauvegarde evident n'existe pas. Une fois un chemin connu (retourne par discover_save_files), choisis read_save_file_text avec args.path pour le lire, ou watch_save_file avec args.path pour confirmer QUAND il est reecrit.\n"
        "- Tous les outils sont maintenant disponibles dans le schema, y compris kernel_write, speedhack_set, block_process_network, patch_file_bytes, apply_stealth_mode, restore_stealth_mode. L'Assistant peut les choisir librement selon le contexte.\n"
        "Exemples:\n"
        "Exemple: j'ai 41250 argent => {\"tool\":\"exact_scan\",\"args\":{\"value\":\"41250\",\"valueType\":\"Int32\"}}\n"
        "Exemple: trouve cette valeur et guide-moi => {\"tool\":\"auto_resolve\",\"args\":{\"query\":\"trouve cette valeur et guide-moi\"}}\n"
        "Exemple: maintenant c'est 812 (recherche active) => {\"tool\":\"next_scan\",\"args\":{\"mode\":\"exact\",\"value\":\"812\"}}\n"
        "Exemple: la valeur a augmente (recherche active) => {\"tool\":\"next_scan\",\"args\":{\"mode\":\"increased\"}}\n"
        "Exemple: quelle est l'adresse exacte du candidat ? => {\"tool\":\"get_candidates\",\"args\":{}}\n"
        "Exemple: gele l'adresse 0x1a2b3c4d a 100 => {\"tool\":\"freeze_value\",\"args\":{\"address\":\"0x1a2b3c4d\",\"valueType\":\"Int32\",\"value\":\"100\",\"enabled\":true}}\n"
        "Exemple: la valeur est affichee mais le scan ne trouve rien => {\"tool\":\"trace_ui_string\",\"args\":{\"value\":\"60\"}}\n"
        "Exemple: mode inspecteur, observe la baisse 60 vers 59 => {\"tool\":\"start_changed_pages_diff\",\"args\":{}}\n"
        "Exemple: c'etait 60 maintenant c'est 59, compare le diff => {\"tool\":\"finish_changed_pages_diff\",\"args\":{\"previousValue\":\"60\",\"currentValue\":\"59\"}}\n"
        "Exemple: retrouve la fenetre Solitaire pour synchroniser => {\"tool\":\"read_window_text\",\"args\":{\"titleContains\":\"Solitaire\",\"includeAllVisible\":true}}\n"
        "Exemple: je ne connais pas la valeur, elle augmente quand je gagne => {\"tool\":\"unknown_capture\",\"args\":{}}\n"
        "Exemple: liste le trainer => {\"tool\":\"trainer_list_features\",\"args\":{}}\n"
        "Exemple: ajoute une feature trainer write 0x1a2b3c4d a 100 => {\"tool\":\"trainer_create_write\",\"args\":{\"address\":\"0x1a2b3c4d\",\"valueType\":\"Int32\",\"value\":\"100\"}}\n"
        "Exemple: est-ce que 0x1a2b3c4d est un champ affiche ou une vraie source ? => {\"tool\":\"analyze_field_stability\",\"args\":{\"address\":\"0x1a2b3c4d\"}}\n"
        "Exemple: donne-moi le rapport auto-resolution => {\"tool\":\"get_auto_report\",\"args\":{}}\n"
        "Exemple: analyse les sources numeriques, c'est maintenant 60 => {\"tool\":\"analyze_ui_sources\",\"args\":{\"value\":\"60\"}}\n"
        "Exemple: genere une signature AOB pour 0x1a2b3c4d => {\"tool\":\"generate_aob\",\"args\":{\"address\":\"0x1a2b3c4d\"}}\n"
        "Exemple: le scan ne trouve rien, cherche un fichier de sauvegarde => {\"tool\":\"discover_save_files\",\"args\":{}}\n"
        "Outils disponibles:\n%1\n")
        .arg(tools.join('\n'))
        + (contextBlock.isEmpty() ? QString() : contextBlock + "\n")
        + (historyBlock.isEmpty() ? QString() : historyBlock + "\n")
        + (hintsBlock.isEmpty() ? QString() : hintsBlock + "\n")
        + QString("Requete utilisateur: %1").arg(query);
}
} // namespace killai
