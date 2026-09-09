#include "claude_chat_manager.h"

#include "application_controller.h"

#include "anthropic_tool_schema.h"
#include "localization/localization.h"
#include "security/dpapi_key_store.h"
#include "tool_registry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>

namespace killengine {

namespace {

QVariantMap makeErrorResult(const QString& error) {
    QVariantMap result;
    result["success"] = false;
    result["error"] = error;
    return result;
}

QString trimmedHex(const QVariantMap& args, const QString& key) {
    return args.value(key).toString().trimmed();
}

// PHASE (Backend IA externe, T6 — test terrain 07/09/2026) : Claude n'avait
// aucun system prompt (contrairement au modèle local, voir la méthodologie
// détaillée dans ai/llama_runtime.cpp::buildPrompt, "Posture Inspecteur").
// Constaté en direct : sans ce cadrage, Claude improvise -- pivote vers
// watch_save_file sans avoir épuisé la réduction de candidats en cours, et
// invente un nom de fichier jamais retourné par discover_save_files (pure
// hallucination). Ce prompt adapte la méthodologie du modèle local au
// tool-calling natif de Claude (pas de schéma JSON bricolé nécessaire, T2
// fournit déjà un vrai input_schema par outil) et ajoute des règles anti-
// hallucination explicites que le modèle local n'avait pas besoin d'énoncer
// (sa boucle est bien plus contrainte côté C++, voir la décision d'écart de
// T3 dans docs/EXTERNAL_AI_BACKEND_ROADMAP.md).
const QString kSystemPrompt = QStringLiteral(
    "Tu es l'assistant IA intégré à KillEngine, un outil d'investigation et de modification mémoire "
    "pour jeux/applications Windows (memory scanning, reverse engineering léger). Tu as accès à de "
    "vrais outils (scan mémoire, écriture, debug, fichiers de sauvegarde, réseau, WebView2, etc.) -- "
    "utilise-les activement plutôt que de deviner ou de répondre en langage libre quand un outil "
    "existe pour la tâche.\n\n"
    "Règles fondamentales, sans exception :\n"
    "1. Ne jamais affirmer un fait que tu n'as pas obtenu via un appel d'outil. N'invente jamais un "
    "nom de fichier, une adresse, une valeur ou un chemin. Si tu ne sais pas, appelle l'outil de "
    "découverte approprié (ex: discover_save_files avant de lire ou modifier un fichier de "
    "sauvegarde) plutôt que de supposer -- utilise ensuite exactement le chemin/la valeur retournés, "
    "jamais un nom plausible que tu inventes.\n"
    "2. Ne change jamais de stratégie sans preuve. Si un scan mémoire (exact_scan/next_scan) est en "
    "cours et progresse (le nombre de candidats diminue), continue à le réduire avec next_scan "
    "jusqu'à un petit nombre de candidats (idéalement moins de 10) avant d'envisager autre chose. Ne "
    "pivote vers une autre approche (fichier de sauvegarde, chaîne de pointeurs, etc.) que si le scan "
    "mémoire échoue de façon répétée ou stagne franchement -- jamais juste parce qu'une nouvelle "
    "valeur est arrivée.\n"
    "3. Observe avant d'écrire. Une adresse n'est fiable que si elle a survécu à plusieurs réductions "
    "et si l'hypothèse explique les échecs précédents. Une valeur affichée à l'écran n'est pas "
    "forcément la vraie source -- utilise analyze_field_stability ou disassemble_backward avant de "
    "figer/patcher si un doute existe.\n"
    "4. Les outils à risque (risk write/debug/patch/injection/script dans leur description) "
    "déclenchent une vraie confirmation de l'utilisateur (clic RiskGate) avant exécution -- appelle "
    "l'outil normalement le moment venu, inutile de redemander la permission en texte avant. Si "
    "l'utilisateur refuse la confirmation, n'insiste pas et propose une alternative.\n"
    "5. Reste concis et concret : pas de liste d'emojis ni de message d'accueil marketing, "
    "l'utilisateur est en pleine investigation technique. Annonce brièvement ce que tu fais et "
    "pourquoi, puis appelle l'outil.\n"
    "6. Ne répète jamais un scan identique qui vient d'échouer. Change d'hypothèse et choisis "
    "l'outil le moins invasif qui produit une preuve nouvelle (encrypted_scan, trace_ui_string, "
    "unknown_capture selon le contexte).\n\n"
    "Workflow typique d'une recherche de valeur :\n"
    "- Valeur affichée connue, pas d'adresse -> exact_scan (ou exact_scan_multi_type si le type est "
    "incertain).\n"
    "- Recherche déjà active, nouvelle valeur donnée -> next_scan(mode=\"exact\", value=...).\n"
    "- Recherche déjà active, variation décrite sans valeur précise -> "
    "next_scan(mode=\"increased\"/\"decreased\"/\"changed\").\n"
    "- Scan mémoire répétitivement infructueux (valeur instable/réallouée, cible UWP/Store) -> "
    "discover_save_files, puis read_save_file_text avec le chemin RÉELLEMENT retourné, puis "
    "watch_save_file pour confirmer QUAND le fichier est réécrit avant toute tentative d'édition.\n"
    "- Avant de figer/patcher une adresse trouvée par scan -> analyze_field_stability pour juger si "
    "c'est un champ affiché recalculé ou une vraie source.\n"
    "- Pour obtenir l'adresse exacte des candidats restants -> get_candidates (aucun argument, lecture "
    "seule). Ne devine JAMAIS une adresse toi-même : next_scan/exact_scan ne renvoient qu'un nombre de "
    "candidats, jamais leur adresse -- get_candidates est le seul moyen honnête de la connaître. Si le "
    "résultat revient tronqué (displaySuppressed=true), il reste trop de candidats : continue à réduire "
    "avec next_scan avant de rappeler get_candidates, plutôt que de relancer un scan. Plusieurs adresses "
    "réelles peuvent légitimement correspondre à la même valeur logique (copies redondantes, checksums) "
    "-- ne suppose pas qu'une seule adresse est forcément la bonne, examine celles que get_candidates "
    "retourne.\n\n"
    "Tu es en conversation continue : l'historique complet des tours précédents t'est fourni à chaque "
    "message, utilise-le au lieu de redemander une information déjà donnée.");

} // namespace

ClaudeChatManager::ClaudeChatManager(ApplicationController* controller)
    : m_controller(controller) {
}

QVariantMap ClaudeChatManager::setApiKey(const QString& apiKey) {
    if (apiKey.trimmed().isEmpty()) {
        return makeErrorResult(KE_TXT("Clé API vide.", "The API key is empty."));
    }
    bool ok = false;
    QString errorMessage;
    const QByteArray encrypted = killcore::DpapiKeyStore::encrypt(apiKey.trimmed().toUtf8(), &ok, &errorMessage);
    if (!ok) {
        return makeErrorResult(KE_TXT("Échec du chiffrement de la clé: %1", "Couldn't encrypt the API key: %1").arg(errorMessage));
    }
    QSettings settings;
    settings.setValue("ai/externalApiKeyBlob", encrypted);
    settings.sync();

    QVariantMap result;
    result["success"] = true;
    return result;
}

QVariantMap ClaudeChatManager::clearApiKey() {
    QSettings settings;
    settings.remove("ai/externalApiKeyBlob");
    settings.sync();

    QVariantMap result;
    result["success"] = true;
    return result;
}

bool ClaudeChatManager::hasApiKey() const {
    QSettings settings;
    return !settings.value("ai/externalApiKeyBlob").toByteArray().isEmpty();
}

QString ClaudeChatManager::decryptedApiKey(bool* ok, QString* errorMessage) const {
    QSettings settings;
    const QByteArray encrypted = settings.value("ai/externalApiKeyBlob").toByteArray();
    if (encrypted.isEmpty()) {
        if (ok) *ok = false;
        if (errorMessage) *errorMessage = KE_TXT("Aucune clé API Claude enregistrée.", "No Claude API key saved.");
        return {};
    }

    bool decryptOk = false;
    QString decryptError;
    const QByteArray plain = killcore::DpapiKeyStore::decrypt(encrypted, &decryptOk, &decryptError);
    if (!decryptOk) {
        if (ok) *ok = false;
        if (errorMessage) *errorMessage = KE_TXT("Échec du déchiffrement de la clé: %1", "Couldn't decrypt the API key: %1").arg(decryptError);
        return {};
    }

    if (ok) *ok = true;
    return QString::fromUtf8(plain);
}

QVariantMap ClaudeChatManager::setActiveBackend(const QString& backend) {
    if (backend != "local" && backend != "claude") {
        return makeErrorResult(KE_TXT("Backend inconnu: '%1' (attendu 'local' ou 'claude').", "Unknown backend: '%1' (expected 'local' or 'claude').").arg(backend));
    }
    QSettings settings;
    settings.setValue("ai/activeBackend", backend);
    settings.sync();

    QVariantMap result;
    result["success"] = true;
    result["activeBackend"] = backend;
    return result;
}

QString ClaudeChatManager::activeBackend() const {
    QSettings settings;
    return settings.value("ai/activeBackend", "local").toString();
}

int ClaudeChatManager::requestCount() const {
    return m_client.requestCount();
}

void ClaudeChatManager::resetConversation() {
    m_client.resetConversation();
    // Le contexte de scan (candidats en memoire) redemarre aussi a
    // l'attach/detach -- voir ApplicationController::attachProcess/detachProcess,
    // meme raison que le reset de l'historique de conversation.
    m_scanActive = false;
}

void ClaudeChatManager::resolvePendingAction(const QString& pendingId, const QVariantMap& result) {
    auto it = m_pending.find(pendingId);
    if (it == m_pending.end()) {
        // Id inconnu, deja resolu, ou expire cote wait -- pas d'erreur, la
        // reponse arrive juste trop tard pour etre consommee.
        return;
    }
    it->resolved = true;
    it->result = result;
}

QVariantMap ClaudeChatManager::waitForFrontendAction(const QString& kind, QVariantMap payload, int timeoutMs) {
    const QString pendingId = QString("claude_pending_%1").arg(m_nextPendingId++);
    m_pending.insert(pendingId, PendingEntry{});

    payload["pendingId"] = pendingId;
    payload["kind"] = kind;
    emit m_controller->claudePendingActionRequested(payload);

    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        const auto it = m_pending.find(pendingId);
        if (it != m_pending.end() && it->resolved) {
            const QVariantMap result = it->result;
            m_pending.erase(it);
            return result;
        }
    }

    m_pending.remove(pendingId);
    return makeErrorResult(KE_TXT("Délai dépassé en attendant une action utilisateur dans l'interface KillEngine.", "Timed out waiting for a user action in the KillEngine interface."));
}

QVariantMap ClaudeChatManager::waitForControllerSignal(void (ApplicationController::*signal)(const QVariantMap&), int timeoutMs) {
    QVariantMap captured;
    bool done = false;

    QEventLoop loop;
    const QMetaObject::Connection connection = QObject::connect(m_controller, signal, [&](const QVariantMap& result) {
        captured = result;
        done = true;
        loop.quit();
    });

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    QObject::disconnect(connection);

    if (!done) {
        return makeErrorResult(KE_TXT("Délai dépassé en attendant la fin de l'opération asynchrone.", "Timed out waiting for the background operation to finish."));
    }
    return captured;
}

QVariantMap ClaudeChatManager::requestConfirmation(const QString& toolName, const QVariantMap& args, const QString& description) {
    killai::ToolRegistry registry;
    const QVariantMap metadata = registry.toolMetadata(toolName);

    QVariantMap payload;
    payload["toolName"] = toolName;
    payload["args"] = args;
    payload["description"] = description;
    payload["risk"] = metadata.value("risk", "injection");
    return waitForFrontendAction("confirm_and_execute_in_cpp", payload);
}

QVariantMap ClaudeChatManager::sendMessage(const QString& userMessage) {
    bool ok = false;
    QString errorMessage;
    const QString apiKey = decryptedApiKey(&ok, &errorMessage);
    if (!ok) {
        return makeErrorResult(errorMessage);
    }

    killai::ToolRegistry registry;
    const QJsonArray toolsSchema = killai::toolsToAnthropicSchema(registry.availableTools());

    const killai::ClaudeBackendClient::ToolExecutor executor = [this](const QString& toolName, const QVariantMap& args) {
        return executeTool(toolName, args);
    };

    return m_client.sendMessage(apiKey, userMessage, toolsSchema, executor, 8, kSystemPrompt);
}

QVariantMap ClaudeChatManager::executeTool(const QString& tool, const QVariantMap& args) {
    // ------------------------------------------------------------------
    // Outils jamais exécutables de façon autonome depuis un backend chat,
    // confirmation ou non -- même politique que le chat local (PHASE 140,
    // apps/desktop/smart_search_manager.cpp) : find_what_writes/
    // test_candidate_fields tournent en tâche de fond avec suivi visuel
    // (jusqu'à ~1 minute), patch_file_bytes édite un fichier réel sans
    // équivalent UI cliquable. Toujours rediriger, jamais exécuter.
    // ------------------------------------------------------------------
    if (tool == "find_what_writes" || tool == "test_candidate_fields" || tool == "patch_file_bytes") {
        return makeErrorResult(
            KE_TXT("Cette action nécessite l'interface KillEngine (onglet Expert) et ne peut pas s'exécuter de façon "
            "autonome depuis un backend IA externe : elle attache un debugger en tâche de fond ou édite un "
            "fichier réel sans confirmation cliquable équivalente. Explique à l'utilisateur comment le faire "
            "manuellement dans l'UI plutôt que de retenter cet outil.", "This action requires the KillEngine interface (Expert tab) and can't run autonomously from an external AI backend: it attaches a debugger in the background or edits an actual file without an equivalent confirmation button. Explain how to do this manually in the UI instead of retrying this tool."));
    }
    if (tool == "prepare_write_checkpoint") {
        // Prepare des suggestions depuis la liste de candidats de scan
        // accumulee par SmartSearchManager::startSmartSearch (variable
        // locale "candidates", jamais peuplee par ce chemin Claude qui
        // court-circuite entierement cette fonction) -- pas d'equivalent
        // Q_INVOKABLE independant. write_value connait deja l'adresse et la
        // valeur explicitement, utilisable directement a la place.
        return makeErrorResult(
            KE_TXT("Cet outil dépend de l'historique de scan interne au modèle local, non disponible depuis ce "
            "backend. Utilise directement write_value avec l'adresse et la valeur déjà identifiées.", "This tool depends on the local model's internal scan history, which isn't available from this backend. Use write_value directly with the address and value already identified."));
    }

    // ------------------------------------------------------------------
    // Lecture seule / sans confirmation, appel direct synchrone.
    // ------------------------------------------------------------------
    if (tool == "exact_scan") {
        if (m_scanActive) {
            // Coup de semonce a usage unique (voir commentaire sur m_scanActive,
            // claude_chat_manager.h) : force Claude a reconsiderer sa decision
            // sans le bloquer indefiniment si une toute nouvelle recherche est
            // reellement voulue -- le desarmement immediat laisse un vrai
            // rappel d'exact_scan reussir la fois suivante.
            m_scanActive = false;
            return makeErrorResult(
                KE_TXT("Un scan est déjà actif avec des candidats en mémoire depuis un précédent exact_scan. "
                "Si l'objectif est de réduire cette liste avec la nouvelle valeur observée, utilise next_scan "
                "(mode=\"exact\", value=...) à la place -- rappeler exact_scan redémarre un scan complet et "
                "perd toute la réduction déjà faite. Si tu veux vraiment abandonner cette recherche et en "
                "démarrer une toute nouvelle, tu peux rappeler exact_scan.", "A scan is already active with candidates in memory from an earlier exact_scan. To narrow this list down with the newly observed value, use next_scan (mode=\"exact\", value=...) instead — calling exact_scan again restarts the entire scan and discards all refinement so far. If you really want to abandon this search and start a new one, you can call exact_scan again."));
        }
        const QVariantMap result = m_controller->startExactScan(args.value("value").toString(), args.value("valueType").toString());
        if (result.value("success").toBool()) {
            m_scanActive = true;
        }
        return result;
    }
    if (tool == "exact_scan_multi_type") {
        if (m_scanActive) {
            m_scanActive = false;
            return makeErrorResult(
                KE_TXT("Un scan est déjà actif avec des candidats en mémoire. Si l'objectif est de réduire cette "
                "liste, utilise next_scan (mode=\"exact\", value=...) à la place. Si tu veux vraiment "
                "démarrer une toute nouvelle recherche, tu peux rappeler exact_scan_multi_type.", "A scan is already active with candidates in memory. To narrow this list down, use next_scan (mode=\"exact\", value=...) instead. If you really want to start a new search, you can call exact_scan_multi_type again."));
        }
        const QVariantMap result = m_controller->startExactScanMultiType(args.value("value").toString(), args.value("valueType").toString());
        if (result.value("success").toBool()) {
            m_scanActive = true;
        }
        return result;
    }
    if (tool == "exact_scan_module") {
        if (m_scanActive) {
            m_scanActive = false;
            return makeErrorResult(
                KE_TXT("Un scan est déjà actif avec des candidats en mémoire. Si l'objectif est de réduire cette "
                "liste, utilise next_scan (mode=\"exact\", value=...) à la place. Si tu veux vraiment "
                "démarrer une toute nouvelle recherche, tu peux rappeler exact_scan_module.", "A scan is already active with candidates in memory. To narrow this list down, use next_scan (mode=\"exact\", value=...) instead. If you really want to start a new search, you can call exact_scan_module again."));
        }
        // Reproduit la resolution de module de SmartSearchManager (meme
        // recherche insensible a la casse, exacte puis partielle) via
        // startExactScanExpert avec start/stopAddress bornes au module.
        const QString requestedModule = trimmedHex(args, "module");
        const QVariantList moduleList = m_controller->getProcessModules(m_controller->m_pid);
        QVariantMap matched;
        for (const auto& item : moduleList) {
            const QVariantMap module = item.toMap();
            const QString name = module.value("name").toString();
            if (name.compare(requestedModule, Qt::CaseInsensitive) == 0) {
                matched = module;
                break;
            }
            if (matched.isEmpty() && name.contains(requestedModule, Qt::CaseInsensitive)) {
                matched = module;
            }
        }
        if (matched.isEmpty()) {
            return makeErrorResult(KE_TXT("Module '%1' introuvable dans le processus attaché.", "Module '%1' not found in the attached process.").arg(requestedModule));
        }
        const qulonglong base = matched.value("baseAddress").toULongLong();
        const qulonglong size = matched.value("size").toULongLong();
        QVariantMap expertOptions;
        expertOptions["startAddress"] = QString::number(base, 16);
        expertOptions["stopAddress"] = QString::number(base + size, 16);
        expertOptions["writableOnly"] = false;
        expertOptions["executableOnly"] = false;
        expertOptions["copyOnWriteOnly"] = false;
        QVariantMap result = m_controller->startExactScanExpert(args.value("value").toString(), args.value("valueType").toString(), expertOptions);
        result["module"] = matched.value("name");
        if (result.value("success").toBool()) {
            m_scanActive = true;
        }
        return result;
    }
    if (tool == "next_scan") {
        return m_controller->nextScan(args.value("mode").toString(), args.value("value").toString());
    }
    if (tool == "get_candidates") {
        // PHASE (T6, evaluation live 08/09/2026) : sans cet outil, aucun moyen
        // honnete de repondre "quelle est l'adresse ?" une fois la liste
        // reduite -- next_scan/exact_scan ne renvoient qu'un compteur, jamais
        // les adresses. pageSize=50 suffit largement une fois reduit ; si
        // candidateStore reste trop gros, ApplicationController::getCandidates
        // renvoie displaySuppressed=true (aucune adresse), signal clair pour
        // continuer a reduire avant de rappeler cet outil.
        QVariantMap result = m_controller->getCandidates(0, 50, QString());
        result["success"] = true;
        return result;
    }
    if (tool == "unknown_capture") {
        return m_controller->captureUnknownSnapshot();
    }
    if (tool == "unknown_compare") {
        return m_controller->unknownNextScan(args.value("mode").toString(), args.value("valueType").toString());
    }
    if (tool == "auto_resolve") {
        QVariantMap options;
        options["executeSafe"] = true;
        return m_controller->startAutoResolve(args.value("query").toString(), options);
    }
    if (tool == "encrypted_scan") {
        QVariantMap options;
        options["mode"] = "xor";
        options["key"] = "0";
        options["keySearchBits"] = 16;
        return m_controller->scanEncryptedValue(args.value("value").toString(), args.value("valueType").toString(), options);
    }
    if (tool == "trace_ui_string") {
        QVariantMap options;
        options["ascii"] = true;
        options["utf16"] = true;
        options["writableOnly"] = true;
        return m_controller->scanUiStrings(args.value("value").toString(), options);
    }
    if (tool == "analyze_ui_sources") {
        QVariantMap uiCandidate;
        uiCandidate["address"] = trimmedHex(args, "address");
        uiCandidate["byteLength"] = args.value("byteLength", 0);
        return m_controller->analyzeUiStringSources(uiCandidate, args.value("value").toString(), {});
    }
    if (tool == "read_window_text") {
        QVariantMap options = args;
        if (!options.contains("includeAllVisible")) options["includeAllVisible"] = true;
        return m_controller->readAttachedWindowText(options);
    }
    if (tool == "list_process_modules") {
        QVariantMap result;
        const QVariantList modules = m_controller->getProcessModules(m_controller->m_pid);
        result["success"] = true;
        result["modules"] = modules;
        result["moduleCount"] = modules.size();
        return result;
    }
    if (tool == "start_changed_pages_diff") {
        QVariantMap options;
        options["maxBytesMb"] = 64;
        options["blockSize"] = 64 * 1024;
        options["privateOnly"] = true;
        options["writableOnly"] = true;
        return m_controller->startChangedPagesDiff(options);
    }
    if (tool == "finish_changed_pages_diff") {
        return m_controller->finishChangedPagesDiff(args.value("previousValue").toString(), args.value("currentValue").toString(), {});
    }
    if (tool == "analyze_field_stability") {
        return m_controller->analyzeFieldStability(trimmedHex(args, "address"), {});
    }
    if (tool == "get_auto_report") {
        return m_controller->getAutoResolveReport(50);
    }
    if (tool == "generate_aob") {
        return m_controller->generateAobSignature(trimmedHex(args, "address"), {});
    }
    if (tool == "suggest_patch") {
        return m_controller->suggestCodePatches(trimmedHex(args, "address"), {});
    }
    if (tool == "disassemble_backward") {
        return m_controller->disassembleBackward(trimmedHex(args, "address"), {});
    }
    if (tool == "discover_save_files") {
        return m_controller->discoverProcessSaveFiles(50);
    }
    if (tool == "inspect_local_settings") {
        return m_controller->inspectProcessLocalSettings(200);
    }
    if (tool == "read_save_file_text") {
        return m_controller->readProcessSaveFileText(args.value("path").toString(), 65536);
    }
    if (tool == "watch_save_file") {
        QVariantMap options;
        options["timeoutMs"] = 5000;
        return m_controller->watchSaveFileForChanges(args.value("path").toString(), options);
    }
    if (tool == "get_stealth_status") {
        return m_controller->getStealthStatus();
    }
    if (tool == "getWebView2InspectorStatus") {
        return m_controller->getWebView2InspectorStatus();
    }
    if (tool == "listWebView2CdpTargets") {
        return m_controller->listWebView2CdpTargets(args.value("browserProcessId").toInt(), {});
    }
    if (tool == "disconnectWebView2Inspector") {
        return m_controller->disconnectWebView2Inspector();
    }
    if (tool == "findWebView2DisplayedValues") {
        return m_controller->findWebView2DisplayedValues(args.value("value").toString(), {});
    }
    if (tool == "findWebView2DisplayedText") {
        return m_controller->findWebView2DisplayedText(args.value("text").toString(), {});
    }
    if (tool == "probeWebView2GlobalScope") {
        return m_controller->probeWebView2GlobalScope();
    }
    if (tool == "get_process_network_modules") {
        return m_controller->getProcessNetworkModules();
    }
    if (tool == "get_http_proxy_requests") {
        return m_controller->getHttpProxyRequests();
    }

    // ------------------------------------------------------------------
    // Lecture seule mais asynchrone côté backend (méthode *Async, résultat
    // réel livré par un signal *Finished) -- on lance l'appel puis on
    // attend le signal correspondant via une QEventLoop bornée, aucune
    // confirmation nécessaire (risk=safe côté ai/tool_registry.cpp).
    // ------------------------------------------------------------------
    if (tool == "get_process_network_connections") {
        m_controller->getProcessNetworkConnectionsAsync();
        return waitForControllerSignal(&ApplicationController::processNetworkConnectionsFinished);
    }

    // ------------------------------------------------------------------
    // Outils Trainer : aucun Q_INVOKABLE équivalent n'existe (CRUD Trainer
    // purement côté Pinia, voir ui/src/stores/trainer.ts) -- pont vers le
    // frontend, qui exécute l'action réelle et renvoie le résultat.
    // ------------------------------------------------------------------
    if (tool == "trainer_list_features" || tool == "trainer_create_write" || tool == "trainer_delete_feature") {
        QVariantMap payload;
        payload["args"] = args;
        if (tool == "trainer_create_write") {
            // Reproduit la resolution de locator resilient de
            // SmartSearchManager::startSmartSearch (PHASE 163) : AOB d'abord
            // (image statique), pointer chain ensuite (objet alloue), sinon
            // absolute -- appels C++ purs, aucun risque de reentrance ici
            // (ce n'est pas un callback JS imbrique dans le meme appel).
            const QString address = trimmedHex(args, "address");
            QVariantMap locator;
            locator["locatorKind"] = "absolute";
            if (!address.isEmpty()) {
                QVariantMap signatureOptions;
                signatureOptions["beforeBytes"] = 0;
                signatureOptions["length"] = 20;
                const QVariantMap signature = m_controller->generateAobSignature(address, signatureOptions);
                if (signature.value("success").toBool() && !signature.value("codeReadProtected").toBool()) {
                    const QString pattern = signature.value("pattern").toString();
                    const QVariantMap quality = signature.value("signatureQuality").toMap();
                    if (!pattern.isEmpty() && quality.value("fixedBytes").toInt() >= 3 && quality.value("score").toInt() >= 35) {
                        QVariantMap scanOptions;
                        scanOptions["executableOnly"] = false;
                        scanOptions["imageOnly"] = true;
                        scanOptions["maxResults"] = 2;
                        const QVariantMap scan = m_controller->scanAobPattern(pattern, scanOptions);
                        if (scan.value("success").toBool() && scan.value("matchesFound").toInt() == 1) {
                            locator["locatorKind"] = "aob";
                            locator["aobPattern"] = pattern;
                        }
                    }
                }
                if (locator.value("locatorKind").toString() == "absolute") {
                    QVariantMap pointerOptions;
                    pointerOptions["maxDepth"] = 3;
                    pointerOptions["maxResults"] = 5;
                    pointerOptions["onlyModuleBase"] = true;
                    const QVariantMap pointerScan = m_controller->scanPointerChains(address, pointerOptions);
                    const QVariantList chains = pointerScan.value("chains").toList();
                    if (pointerScan.value("success").toBool() && !chains.isEmpty()) {
                        locator["locatorKind"] = "pointer_chain";
                        locator["pointerChain"] = chains.first();
                    }
                }
            }
            payload["locator"] = locator;
        }
        return waitForFrontendAction(tool, payload);
    }
    if (tool == "trainer_apply_request" || tool == "trainer_restore_request") {
        // applyTrainerFeature/restoreTrainerFeature ouvrent déjà leur propre
        // confirmation RiskGate côté Pinia (PHASE 120-D) -- un seul aller-
        // retour frontend suffit, pas de requestConfirmation() séparé ici.
        QVariantMap payload;
        payload["args"] = args;
        return waitForFrontendAction(tool, payload);
    }

    // ------------------------------------------------------------------
    // Écritures/actions sensibles : confirmation RiskGate réelle côté
    // frontend d'abord (bloquant), exécution réelle en C++ seulement après
    // approbation explicite -- jamais d'exécution automatique, même
    // politique que PHASE 271-272 (sécurité via RiskGate à l'exécution, pas
    // via censure du schéma d'outils).
    // ------------------------------------------------------------------
    if (tool == "write_value") {
        const QString address = trimmedHex(args, "address");
        const QString value = trimmedHex(args, "value");
        const QString valueType = args.value("valueType", "Int32").toString();
        if (address.isEmpty() || value.isEmpty()) {
            return makeErrorResult(KE_TXT("Adresse et valeur requises pour écrire en mémoire.", "I need an address and a value to write to memory."));
        }
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("Écriture mémoire demandée par le backend Claude : %1 (%2) à 0x%3.", "Claude requests a memory write: %1 (%2) at 0x%3.").arg(value, valueType, address));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Écriture refusée par l'utilisateur (confirmation non accordée).", "Write declined by the user (confirmation not granted)."));
        }
        return m_controller->writeMemoryValue(address, valueType, value);
    }
    if (tool == "freeze_value") {
        const QString address = trimmedHex(args, "address");
        const QString value = trimmedHex(args, "value");
        const QString valueType = args.value("valueType", "Int32").toString();
        const bool enabled = args.value("enabled").toBool();
        if (address.isEmpty() || value.isEmpty()) {
            return makeErrorResult(KE_TXT("Adresse et valeur requises pour figer une valeur.", "I need an address and a value to freeze."));
        }
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("%1 demandé par le backend Claude : %2 (%3) à 0x%4.", "Claude requests %1: %2 (%3) at 0x%4.")
                .arg(enabled ? "Freeze" : KE_TXT("Arrêt du freeze", "Stop freeze"), value, valueType, address));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Freeze refusé par l'utilisateur (confirmation non accordée).", "Freeze declined by the user (confirmation not granted)."));
        }
        return m_controller->setFreezeValue(address, valueType, value, enabled);
    }
    if (tool == "kernel_write") {
        const QString address = trimmedHex(args, "address");
        const QString value = trimmedHex(args, "value");
        const QString valueType = args.value("valueType", "Int32").toString();
        if (address.isEmpty() || value.isEmpty()) {
            return makeErrorResult(KE_TXT("Adresse et valeur requises pour écrire via le driver kernel.", "I need an address and a value to write through the kernel driver."));
        }
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("Écriture KERNEL demandée par le backend Claude : %1 (%2) à 0x%3 — contourne les protections mémoire usermode.", "Claude requests a KERNEL write: %1 (%2) at 0x%3 — bypasses user-mode memory protections.")
                .arg(value, valueType, address));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Écriture kernel refusée par l'utilisateur (confirmation non accordée).", "Kernel write declined by the user (confirmation not granted)."));
        }
        return m_controller->writeMemoryValueKernel(address, valueType, value);
    }
    if (tool == "speedhack_set") {
        const QString mode = args.value("mode", "set").toString().trimmed().toLower();
        const bool isOff = (mode == "off" || mode == "stop");
        const double factor = args.value("factor", 1.0).toDouble();
        const QVariantMap confirmation = requestConfirmation(tool, args,
            isOff ? KE_TXT("Désactivation du speedhack demandée par le backend Claude.", "Claude requests disabling the speedhack.")
                  : KE_TXT("Speedhack demandé par le backend Claude : facteur %1x.", "Claude requests a speedhack: %1x multiplier.").arg(factor));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Speedhack refusé par l'utilisateur (confirmation non accordée).", "Speedhack declined by the user (confirmation not granted)."));
        }
        if (isOff) {
            return m_controller->stopSpeedhack();
        }
        const QVariantMap status = m_controller->getSpeedhackStatus();
        if (status.value("active").toBool()) {
            return m_controller->setSpeedhackFactor(factor);
        }
        m_controller->startSpeedhackAsync(factor);
        return waitForControllerSignal(&ApplicationController::speedhackStartFinished);
    }
    if (tool == "block_process_network") {
        const QString mode = args.value("mode", "on").toString().trimmed().toLower();
        const bool isOff = (mode == "off" || mode == "stop" || mode == "unblock");
        const QVariantMap confirmation = requestConfirmation(tool, args,
            isOff ? KE_TXT("Rétablissement du réseau demandé par le backend Claude.", "Claude requests restoring network access.")
                  : KE_TXT("Coupure réseau demandée par le backend Claude (règle pare-feu, invite UAC).", "Claude requests disconnecting the network (firewall rule, UAC prompt)."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Action réseau refusée par l'utilisateur (confirmation non accordée).", "Network action declined by the user (confirmation not granted)."));
        }
        if (isOff) {
            m_controller->unblockProcessNetworkAsync();
            return waitForControllerSignal(&ApplicationController::processNetworkUnblockFinished);
        }
        m_controller->blockProcessNetworkAsync();
        return waitForControllerSignal(&ApplicationController::processNetworkBlockFinished);
    }
    if (tool == "start_http_proxy") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Démarrage d'un proxy HTTP local (injection DLL) demandé par le backend Claude.", "Claude requests starting a local HTTP proxy (DLL injection)."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Démarrage du proxy HTTP refusé par l'utilisateur (confirmation non accordée).", "HTTP proxy start declined by the user (confirmation not granted)."));
        }
        m_controller->startHttpProxyAsync(args.value("port").toInt(), args.value("interceptHttps").toBool());
        return waitForControllerSignal(&ApplicationController::httpProxyStartFinished);
    }
    if (tool == "stop_http_proxy") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Arrêt du proxy HTTP demandé par le backend Claude.", "Claude requests stopping the HTTP proxy."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Arrêt du proxy HTTP refusé par l'utilisateur (confirmation non accordée).", "HTTP proxy stop declined by the user (confirmation not granted)."));
        }
        m_controller->stopHttpProxyAsync();
        return waitForControllerSignal(&ApplicationController::httpProxyStopFinished);
    }
    if (tool == "modify_http_request") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Modification d'une requête HTTP interceptée demandée par le backend Claude.", "Claude requests modifying an intercepted HTTP request."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Modification refusée par l'utilisateur (confirmation non accordée).", "Modification declined by the user (confirmation not granted)."));
        }
        return m_controller->modifyHttpRequest(args.value("requestId").toString(), args.value("newBody").toString());
    }
    if (tool == "spoof_dns") {
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("Redirection DNS demandée par le backend Claude : %1 -> %2.", "Claude requests a DNS redirect: %1 -> %2.").arg(args.value("domain").toString(), args.value("targetIp").toString()));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Redirection DNS refusée par l'utilisateur (confirmation non accordée).", "DNS redirect declined by the user (confirmation not granted)."));
        }
        m_controller->spoofDnsAsync(args.value("domain").toString(), args.value("targetIp").toString());
        return waitForControllerSignal(&ApplicationController::dnsSpoofFinished);
    }
    if (tool == "restore_dns") {
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("Suppression de la redirection DNS demandée par le backend Claude : %1.", "Claude requests removing the DNS redirect: %1.").arg(args.value("domain").toString()));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Restauration DNS refusée par l'utilisateur (confirmation non accordée).", "DNS restore declined by the user (confirmation not granted)."));
        }
        m_controller->restoreDnsAsync(args.value("domain").toString());
        return waitForControllerSignal(&ApplicationController::dnsRestoreFinished);
    }
    if (tool == "set_lag_switch") {
        const bool enabled = args.value("enabled").toBool();
        const int delayMs = args.value("delayMs").toInt();
        const QVariantMap confirmation = requestConfirmation(tool, args,
            enabled ? KE_TXT("Lag switch demandé par le backend Claude : %1 ms de retard réseau.", "Claude requests a lag switch: %1 ms network delay.").arg(delayMs)
                    : KE_TXT("Désactivation du lag switch demandée par le backend Claude.", "Claude requests disabling the lag switch."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Lag switch refusé par l'utilisateur (confirmation non accordée).", "Lag switch declined by the user (confirmation not granted)."));
        }
        m_controller->setLagSwitchAsync(enabled, delayMs);
        return waitForControllerSignal(&ApplicationController::lagSwitchFinished);
    }
    if (tool == "apply_stealth_mode") {
        const QString profile = args.value("profile", "default").toString();
        const QVariantMap confirmation = requestConfirmation(tool, args,
            KE_TXT("Activation du mode discret demandée par le backend Claude (profil '%1').", "Claude requests enabling stealth mode (profile '%1').").arg(profile));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Mode discret refusé par l'utilisateur (confirmation non accordée).", "Stealth mode declined by the user (confirmation not granted)."));
        }
        return m_controller->applyStealthMode(profile);
    }
    if (tool == "restore_stealth_mode") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Désactivation du mode discret demandée par le backend Claude.", "Claude requests disabling stealth mode."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Désactivation refusée par l'utilisateur (confirmation non accordée).", "Deactivation declined by the user (confirmation not granted)."));
        }
        return m_controller->restoreStealthMode();
    }
    if (tool == "connectWebView2Inspector") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Connexion à une target WebView2/CDP demandée par le backend Claude (s'attache à un process externe).", "Claude requests connecting to a WebView2/CDP target (attaches to an external process)."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Connexion WebView2 refusée par l'utilisateur (confirmation non accordée).", "WebView2 connection declined by the user (confirmation not granted)."));
        }
        return m_controller->connectWebView2Inspector(args.value("browserProcessId").toInt(), {});
    }
    if (tool == "evaluateWebView2JavaScript") {
        const QVariantMap confirmation = requestConfirmation(tool, args, KE_TXT("Évaluation de JavaScript arbitraire dans la target WebView2 connectée, demandée par le backend Claude.", "Claude requests evaluating arbitrary JavaScript in the connected WebView2 target."));
        if (!confirmation.value("approved").toBool()) {
            return makeErrorResult(KE_TXT("Évaluation JavaScript refusée par l'utilisateur (confirmation non accordée).", "JavaScript evaluation declined by the user (confirmation not granted)."));
        }
        return m_controller->evaluateWebView2JavaScript(args.value("expression").toString(), {});
    }

    return makeErrorResult(KE_TXT("Outil inconnu côté backend Claude: %1", "Unknown tool in the Claude backend: %1").arg(tool));
}

} // namespace killengine
