#include "ai_engine.h"
#include "intent_contract.h"
#include "logging/logger.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QVariantList>

namespace killai {

namespace {

constexpr int kMaxHistoryTurns = 12;

bool looksLikeBadTargets(const QString& q) {
    return q.contains("marche pas") || q.contains("marché pas") || q.contains("pas marché")
        || q.contains("ne marche pas") || q.contains("mauvaise adresse")
        || q.contains("pas bon") || q.contains("rien change");
}

bool describesIncrease(const QString& q) {
    return q.contains("augment") || q.contains("increased") || q.contains("monte")
        || q.contains("plus grand") || q.contains("plus haut");
}

bool describesDecrease(const QString& q) {
    return q.contains("diminu") || q.contains("decreased") || q.contains("baisse")
        || q.contains("descend") || q.contains("plus petit") || q.contains("plus bas");
}

bool describesChange(const QString& q) {
    return q.contains("chang") || q.contains("change") || q.contains("varie")
        || q.contains("différent") || q.contains("different");
}

bool describesStable(const QString& q) {
    return q.contains("pareil") || q.contains("stable") || q.contains("inchang")
        || q.contains("unchanged") || q.contains("bouge pas");
}

bool wantsInspectorMode(const QString& q) {
    return q.contains("inspecteur") || q.contains("inspector") || q.contains("codex")
        || q.contains("enquete") || q.contains("enquête") || q.contains("raisonne")
        || q.contains("comprendre") || q.contains("preuve");
}

bool describesUiCopyOrBuffer(const QString& q) {
    return q.contains("copie ui") || q.contains("buffer") || q.contains("buffers")
        || q.contains("string instable") || q.contains("texte instable")
        || q.contains("affichage decouple") || q.contains("affichage découpl")
        || q.contains("pas ecrit sur place") || q.contains("pas écrit sur place");
}

QString variationMode(const QString& q, const QString& fallback = "changed") {
    if (describesIncrease(q)) return "increased";
    if (describesDecrease(q)) return "decreased";
    if (describesStable(q)) return "unchanged";
    if (describesChange(q)) return "changed";
    return fallback;
}

/// Un match d'outil "hors memoire" (fichiers de sauvegarde UWP, LocalSettings,
/// surveillance fichier) trouve par mots-cles explicites FR/EN dans la requete.
/// tool vide = aucun match.
struct OffMemoryToolMatch {
    QString tool;
    QString rationale;
};

/// PHASE 91/99 : identifie un outil d'investigation "hors memoire" a partir de
/// mots-cles explicites dans la requete (voir docs/PHASE_TRACKER.md PHASE 90,
/// investigation Solitaire "Bulles" -- la valeur affichee vient parfois d'un
/// fichier sur disque plutot que d'une adresse memoire stable). Une seule
/// liste de mots-cles, utilisee a la fois par le fast-path de
/// AIEngine::processQuery (court-circuite le modele local avant de le lancer)
/// et par deterministicPlanWithContext (repli normal si le modele echoue) --
/// jamais deux copies qui pourraient diverger.
OffMemoryToolMatch matchOffMemoryTool(const QString& q) {
    if (q.contains("localsettings") || q.contains("local settings") || q.contains("settings.dat")
        || q.contains("ruche registre") || q.contains("registre uwp") || q.contains("registry hive")) {
        return {"inspect_local_settings",
            "J'inspecte en lecture seule la ruche LocalSettings/settings.dat du package UWP attache."};
    }

    // Sans chemin de fichier deja connu, "surveiller" n'est pas actionnable
    // directement (watch_save_file exige un path) -- on route d'abord vers
    // la decouverte, etape necessaire de toute facon avant de surveiller.
    if (q.contains("watch fichier") || q.contains("surveille fichier") || q.contains("surveiller fichier")
        || q.contains("surveillance fichier") || q.contains("watch file") || q.contains("file watch")) {
        return {"discover_save_files",
            "Je cherche d'abord les fichiers de sauvegarde disponibles -- tu pourras ensuite me demander de surveiller l'un d'eux."};
    }

    if (q.contains("fichier de sauvegarde") || q.contains("fichiers de sauvegarde")
        || q.contains("sauvegarde disque") || q.contains("sur le disque") || q.contains("on disk")
        || q.contains("dans un fichier") || q.contains("save file") || q.contains("savefile")
        || q.contains("localstate")) {
        return {"discover_save_files",
            "Je cherche les fichiers de sauvegarde/etat du processus attache sur le disque."};
    }

    return {};
}

struct TrainerToolMatch {
    QString tool;
    QVariantMap args;
    QString rationale;
};

QString firstDecimalOutsideHex(QString text) {
    text.replace(QRegularExpression(R"(0x[0-9a-fA-F]+)"), " ");
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    const auto match = re.match(text);
    return match.hasMatch() ? match.captured(0).replace(',', '.') : QString();
}

QString firstHexAddressIn(const QString& text) {
    const QRegularExpression re(R"(0x[0-9a-fA-F]+)");
    const auto match = re.match(text);
    return match.hasMatch() ? match.captured(0) : QString();
}

TrainerToolMatch matchTrainerTool(const QString& query) {
    const QString q = query.toLower();
    const bool wantsTrainer = q.contains("trainer") || q.contains("cheat table")
        || q.contains("feature trainer") || q.contains("fonction trainer");
    if (!wantsTrainer) {
        return {};
    }

    const bool wantsApply = q.contains("apply") || q.contains("activer") || q.contains("active ")
        || q.contains("lance") || q.contains("enabled");
    const bool wantsRestore = q.contains("restore") || q.contains("restaur") || q.contains("désactiv")
        || q.contains("desactiv") || q.contains("coupe");
    const bool wantsDelete = q.contains("delete") || q.contains("remove") || q.contains("supprim")
        || q.contains("efface");
    const bool wantsCreate = q.contains("create") || q.contains("add ") || q.contains("ajoute")
        || q.contains("crée") || q.contains("cree") || q.contains("sauve") || q.contains("sauvegarde");
    const bool wantsList = q.contains("list") || q.contains("liste") || q.contains("affiche")
        || q.contains("show") || q.contains("voir") || q.contains("snapshot") || q.contains("status");

    if (wantsApply) {
        return {"trainer_apply_request",
            {{"id", firstDecimalOutsideHex(q)}, {"all", q.contains("all") || q.contains("tout")}},
            "Activation Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate."};
    }
    if (wantsRestore) {
        return {"trainer_restore_request",
            {{"id", firstDecimalOutsideHex(q)}, {"all", q.contains("all") || q.contains("tout")}},
            "Restauration Trainer demandee: je prepare une confirmation UI, sans appeler directement le RiskGate."};
    }
    if (wantsDelete) {
        return {"trainer_delete_feature",
            {{"id", firstDecimalOutsideHex(q)}},
            "Suppression d'une feature Trainer demandee."};
    }
    if (wantsCreate) {
        const QString address = firstHexAddressIn(query);
        const QString value = firstDecimalOutsideHex(query);
        QVariantMap args;
        args["action"] = "write";
        args["address"] = address;
        args["value"] = value;
        args["valueType"] = q.contains("float") ? QString("Float32") : QString("Int32");
        args["name"] = "Assistant Trainer write";
        return {"trainer_create_write", args,
            "Creation d'une feature Trainer write demandee depuis une adresse et une valeur explicites."};
    }
    if (wantsList) {
        return {"trainer_list_features", {},
            "Je liste les features Trainer locales via le pont UI lecture seule."};
    }

    return {"trainer_list_features", {},
        "Demande Trainer generale: je commence par lister l'etat actuel des features."};
}

} // namespace

AIEngine::AIEngine(QObject* parent) : QObject(parent) {}
AIEngine::~AIEngine() {}

bool AIEngine::init() {
    // Keep startup non-blocking and crash-proof: embedded AI runtime is
    // initialized lazily by the first AI flow, not during application boot.
    KE_LOG_INFO() << "AIEngine::init() - embedded AI init deferred";
    m_ready = true;
    return true;
}

bool AIEngine::isReady() const { return m_ready; }

void AIEngine::noteOutcome(const QString& query, const QString& tool, const QString& outcome) {
    QVariantMap turn;
    turn["query"] = query;
    turn["tool"] = tool;
    turn["outcome"] = outcome;
    m_history.append(turn);
    while (m_history.size() > kMaxHistoryTurns) {
        m_history.removeFirst();
    }
}

void AIEngine::clearHistory() {
    m_history.clear();
}

QVariantMap AIEngine::lastHistoryTurn() const {
    return m_history.isEmpty() ? QVariantMap{} : m_history.last().toMap();
}

bool AIEngine::ensureLlamaInitialized() {
    if (qEnvironmentVariable("KILLENGINE_DISABLE_LLAMA") == "1") {
        return false;
    }
    const bool runningUnitTests = QCoreApplication::applicationFilePath().contains("killengine_unit_tests", Qt::CaseInsensitive);
    if (runningUnitTests && QProcessEnvironment::systemEnvironment().value("KILLENGINE_ENABLE_LLAMA_IN_TESTS") != "1") {
        return false;
    }
    if (!QSettings().value("ai/modelEnabled", true).toBool()) {
        return false;
    }
    if (m_llama.isAvailable()) {
        return true;
    }
    const bool ok = m_llama.init();
    if (ok) {
        KE_LOG_INFO() << "AIEngine llama.cpp runtime available.";
    } else if (!m_llama.info().errorMessage.isEmpty()) {
        KE_LOG_INFO() << "AIEngine llama.cpp unavailable: " << m_llama.info().errorMessage.toStdString();
    }
    return ok;
}

QVariantMap AIEngine::modelToolCallWithRetry(const QString& query, const QVariantMap& context, QString* backend) {
    auto generated = m_llama.planToolCall(query, m_registry, context);
    QString error;
    QVariantMap call = generated.success ? LlamaRuntime::extractToolCallJson(generated.output, &error) : QVariantMap{};

    // Retry correctif borne: une seconde tentative si le modele a repondu
    // mais sans JSON exploitable (hallucination de format, bavardage...).
    if (generated.success && call.isEmpty()) {
        KE_LOG_INFO() << "AIEngine retrying model tool call after invalid JSON: " << error.toStdString();
        const QString correctiveQuery = query + "\n(Rappel: reponds UNIQUEMENT par l'objet JSON du schema, sans texte autour.)";
        generated = m_llama.planToolCall(correctiveQuery, m_registry, context);
        if (generated.success) {
            call = LlamaRuntime::extractToolCallJson(generated.output, &error);
        }
    }

    if (backend) *backend = generated.backend;
    if (!generated.success) {
        if (backend) backend->append(QString("|error:%1").arg(generated.errorMessage));
        return {};
    }
    return call;
}

QVariantMap AIEngine::modelIntentWithRetry(const QString& query, QString* backend) {
    auto generated = m_llama.planIntent(query);
    QString error;
    QVariantMap intent = generated.success ? LlamaRuntime::extractIntentJson(generated.output, &error) : QVariantMap{};

    if (generated.success && intent.isEmpty()) {
        KE_LOG_INFO() << "AIEngine retrying model intent after invalid JSON: " << error.toStdString();
        const QString correctiveQuery = query + "\n(Rappel: reponds UNIQUEMENT par l'objet JSON du schema, sans texte autour.)";
        generated = m_llama.planIntent(correctiveQuery);
        if (generated.success) {
            intent = LlamaRuntime::extractIntentJson(generated.output, &error);
        }
    }

    if (backend) *backend = generated.backend;
    if (!generated.success) {
        if (backend) backend->append(QString("|error:%1").arg(generated.errorMessage));
        return {};
    }
    return intent;
}

QVariantMap AIEngine::processIntent(const QString& query) {
    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    const QString q = query.toLower();
    QStringList detectedNumbers;
    const QRegularExpression guardedNumberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto guardedNumberIt = guardedNumberRe.globalMatch(query);
    while (guardedNumberIt.hasNext()) {
        detectedNumbers.append(guardedNumberIt.next().captured(0).replace(',', '.'));
    }
    if (looksLikeBadTargets(q)) {
        QVariantMap result;
        result["status"] = "intent";
        result["intent"] = "ReportBadTargets";
        result["value"] = "";
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "";
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    const QString firstDetectedNumber = firstNumber(query);
    const bool looksLikeWriteWithoutValue =
        (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
         || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")
         || q.contains("write") || q.contains("écri") || q.contains("ecri"))
        && firstDetectedNumber.isEmpty();
    if (looksLikeWriteWithoutValue) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["intent"] = "Unknown";
        result["value"] = "";
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    const bool looksLikeRewriteWithValue =
        !firstDetectedNumber.isEmpty()
        && (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
            || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace"))
        && (q.contains(" le ") || q.contains(" les ") || q.contains("ça") || q.contains("ca")
            || q.contains("adresse") || q.contains("derni"));
    if (looksLikeRewriteWithValue && detectedNumbers.size() == 1) {
        QVariantMap result;
        result["status"] = "intent";
        result["intent"] = "RewriteLastTargets";
        result["value"] = firstDetectedNumber;
        result["targetValue"] = "";
        result["addresses"] = QVariantList{};
        result["confidence"] = 0.9;
        result["missing"] = "";
        result["aiBackend"] = "deterministic_guard";
        return result;
    }

    if (ensureLlamaInitialized()) {
        QString backend;
        const QVariantMap intent = modelIntentWithRetry(query, &backend);
        if (!intent.isEmpty()) {
            QString error;
            const bool valid = IntentContract::validate(intent, &error);
            intent["status"] = valid ? "intent" : "needs_clarification";
            intent["aiBackend"] = backend.isEmpty() ? QString("llama.cpp") : backend;
            intent["error"] = error;
            if (!valid && intent.value("message").toString().isEmpty()) {
                intent["message"] = intent.value("missing").toString().isEmpty()
                    ? QString("Je dois préciser l'intention avant d'agir.")
                    : intent.value("missing").toString();
            }
            return intent;
        }
        KE_LOG_INFO() << "AIEngine model intent rejected: " << backend.toStdString();
    }

    QVariantMap fallback = deterministicIntent(query);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

QVariantMap AIEngine::processQuery(const QString& query) {
    return processQuery(query, {});
}

QVariantMap AIEngine::processQuery(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();

    if (!m_ready) {
        QVariantMap result;
        result["status"] = "not_ready";
        result["message"] = "AIEngine is not initialized.";
        return result;
    }

    // PHASE 99 : fast-path pour une demande explicite d'investigation "hors
    // memoire" -- ces requetes n'ont pas besoin d'une inference LLM et
    // n'avaient jusqu'ici droit a l'outil deterministe qu'APRES un aller-retour
    // au modele local (potentiellement plusieurs dizaines de secondes) qui
    // finissait de toute facon par echouer/retomber sur ce meme outil via
    // deterministicPlanWithContext. Court-circuite ce retard, avant meme
    // ensureLlamaInitialized(). Ne s'applique que process attache (sinon on
    // laisse le chemin normal produire le message de clarification usuel).
    if (context.value("processAttached", true).toBool()) {
        if (const auto trainerMatch = matchTrainerTool(query); !trainerMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(trainerMatch.tool, trainerMatch.args, trainerMatch.rationale);
            result["aiBackend"] = "deterministic_trainer_fastpath";
            return result;
        }
        if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
            QVariantMap result = makeToolCall(offMemoryMatch.tool, {}, offMemoryMatch.rationale);
            result["aiBackend"] = "deterministic_offmemory_fastpath";
            return result;
        }
    }

    if (ensureLlamaInitialized()) {
        // L'historique conversationnel enrichit le contexte transmis au modele:
        // il sait ce qui a deja echoue et peut proposer une vraie alternative.
        QVariantMap modelContext = context;
        if (!m_history.isEmpty()) {
            modelContext["history"] = m_history;
        }
        QString backend;
        const QVariantMap call = modelToolCallWithRetry(query, modelContext, &backend);
        if (!call.isEmpty()) {
            QString error;
            QVariantMap result;
            result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
            result["tool"] = call.value("tool").toString();
            result["args"] = call.value("args").toMap();
            result["rationale"] = "Plan généré par le modèle local llama.cpp/Qwen.";
            result["state"] = m_stateMachine.currentStateName();
            result["aiBackend"] = backend.isEmpty() ? QString("llama.cpp") : backend;
            result["error"] = error;
            if (result.value("status").toString() == "tool_call") {
                return result;
            }
            KE_LOG_INFO() << "AIEngine model tool call rejected: " << error.toStdString();
        } else {
            KE_LOG_INFO() << "AIEngine model produced no tool call: " << backend.toStdString();
        }
    }

    auto fallback = deterministicPlanWithContext(query, context);
    fallback["aiBackend"] = "deterministic";
    if (m_llama.info().available == false && !m_llama.info().errorMessage.isEmpty()) {
        fallback["aiBackendNote"] = m_llama.info().errorMessage;
    }
    return fallback;
}

QVariantMap AIEngine::deterministicIntent(const QString& query) {
    const QString q = query.toLower();
    QVariantMap result;
    result["status"] = "intent";
    result["intent"] = "Unknown";
    result["value"] = "";
    result["targetValue"] = "";
    result["addresses"] = QVariantList{};
    result["confidence"] = 0.5;
    result["missing"] = "";

    QVariantList addresses;
    const QRegularExpression addressRe(R"(0x[0-9a-fA-F]{5,16})");
    auto addressIt = addressRe.globalMatch(query);
    while (addressIt.hasNext()) {
        addresses.append(addressIt.next().captured(0));
    }

    QStringList numbers;
    const QRegularExpression numberRe(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto numberIt = numberRe.globalMatch(query);
    while (numberIt.hasNext()) {
        numbers.append(numberIt.next().captured(0).replace(',', '.'));
    }

    if (looksLikeBadTargets(q)) {
        result["intent"] = "ReportBadTargets";
        result["confidence"] = 0.8;
    } else if (q.contains("autre") || q.contains("nouveau") || q.contains("reset") || q.contains("recommence")) {
        result["intent"] = numbers.isEmpty() ? "ResetContext" : "ExactScan";
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.75;
    } else if (!addresses.isEmpty()) {
        result["intent"] = numbers.isEmpty() ? "ActivateMemoryTargets" : "WriteMemoryTargets";
        result["addresses"] = addresses;
        if (!numbers.isEmpty()) result["value"] = numbers.first();
        result["confidence"] = 0.9;
    } else if (numbers.size() >= 2) {
        result["intent"] = "GuidedScan";
        result["value"] = numbers.at(0);
        result["targetValue"] = numbers.at(1);
        result["confidence"] = 0.85;
    } else if (numbers.size() == 1) {
        if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
            || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")) {
            result["intent"] = "RewriteLastTargets";
        } else {
            result["intent"] = "ExactScan";
        }
        result["value"] = numbers.first();
        result["confidence"] = 0.7;
    } else if (q.contains("passe") || q.contains("passer") || q.contains("mets") || q.contains("met ")
               || q.contains("veux") || q.contains("voudrais") || q.contains("augmente") || q.contains("remplace")) {
        result["status"] = "needs_clarification";
        result["missing"] = "Tu veux le passer à quelle valeur ?";
        result["message"] = result["missing"];
    } else {
        result["status"] = "needs_clarification";
        result["missing"] = "Quelle valeur veux-tu chercher ?";
        result["message"] = result["missing"];
    }

    QString error;
    if (result.value("status").toString() == "intent" && !IntentContract::validate(result, &error)) {
        result["status"] = "needs_clarification";
        result["error"] = error;
        result["message"] = result.value("missing").toString().isEmpty()
            ? QString("Il manque une information pour continuer.")
            : result.value("missing").toString();
    }
    return result;
}

QVariantMap AIEngine::deterministicPlan(const QString& query) {
    const QString q = query.toLower();

    if (q.contains("unknown") || q.contains("inconnue")) {
        if (q.contains("capture") || q.contains("initial")) {
            m_stateMachine.setState(AIState::WaitingForUserChange);
            return makeToolCall("unknown_capture", {}, "Capture initiale pour valeur inconnue.");
        }

        QString mode = "changed";
        if (q.contains("augment") || q.contains("increased")) mode = "increased";
        if (q.contains("diminu") || q.contains("decreased")) mode = "decreased";
        if (q.contains("pareil") || q.contains("unchanged")) mode = "unchanged";
        return makeToolCall("unknown_compare", {{"mode", mode}, {"valueType", inferValueType(query)}}, "Comparaison unknown initial value.");
    }

    if (q.contains("next") || q.contains("changed") || q.contains("change") ||
        q.contains("increased") || q.contains("augment") ||
        q.contains("decreased") || q.contains("diminu") ||
        q.contains("inchang")) {
        QString mode = "changed";
        if (q.contains("exact")) mode = "exact";
        if (q.contains("increased") || q.contains("augment")) mode = "increased";
        if (q.contains("decreased") || q.contains("diminu")) mode = "decreased";
        if (q.contains("unchanged") || q.contains("inchang")) mode = "unchanged";
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", mode}, {"value", firstNumber(query)}}, "Réduction des candidats.");
    }

    if (q.contains("freeze") || q.contains("geler")) {
        return makeToolCall("freeze_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
            {"enabled", true},
        }, "Freeze demandé par l'utilisateur.");
    }

    // Demande explicite du kernel avant le check d'ecriture generique
    // ci-dessous, sinon "ecris X via le kernel" tombe dans write_value et le
    // choix explicite de l'utilisateur est perdu.
    const bool wantsKernel = q.contains("kernel") || q.contains("noyau");
    if (wantsKernel && (q.contains("write") || q.contains("écri") || q.contains("mettre"))) {
        return makeToolCall("kernel_write", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, "Écriture kernel demandée explicitement par l'utilisateur.");
    }

    // Speedhack : placé avant les checks génériques ci-dessous pour la même
    // raison que wantsKernel plus haut — "ralentis le jeu" ne doit jamais
    // tomber dans exact_scan juste parce qu'aucun nombre n'est fourni.
    {
        const bool wantsSpeedOff = q.contains("désactiv") || q.contains("desactiv") ||
            ((q.contains("stop") || q.contains("arrêt") || q.contains("normal")) &&
             (q.contains("vitesse") || q.contains("speed") || q.contains("temps") || q.contains("speedhack")));
        const bool wantsPause = q.contains("pause") &&
            (q.contains("temps") || q.contains("vitesse") || q.contains("speedhack") || q.contains("jeu") || q.contains("game"));
        const bool wantsSlow = q.contains("ralent") || q.contains("slow");
        const bool wantsFast = q.contains("accélér") || q.contains("acceler") || q.contains("speed up") || q.contains("speedup");
        const bool wantsSpeedGeneric = q.contains("vitesse") || q.contains("speed");
        if (wantsSpeedOff) {
            return makeToolCall("speedhack_set", {{"mode", "off"}}, "Désactivation du speedhack demandée.");
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, "Pause du temps demandée (speedhack).");
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = firstNumber(query).toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? "Ralentissement demandé (speedhack)." : "Accélération demandée (speedhack).");
        }
    }

    if (q.contains("write") || q.contains("écri") || q.contains("mettre")) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", firstNumber(query)},
        }, "Écriture mémoire demandée.");
    }

    const QString value = firstNumber(query);
    if (!value.isEmpty()) {
        m_stateMachine.setState(AIState::FirstScanRunning);
        return makeToolCall("exact_scan", {{"value", value}, {"valueType", inferValueType(query)}}, "Premier scan exact depuis une valeur détectée.");
    }

    QVariantMap result;
    result["status"] = "needs_clarification";
    result["message"] = "Je n'ai pas trouvé de valeur ou d'action claire.";
    result["state"] = m_stateMachine.currentStateName();
    result["availableTools"] = m_registry.availableTools();
    return result;
}

QVariantMap AIEngine::deterministicPlanWithContext(const QString& query, const QVariantMap& context) {
    const QString q = query.toLower();
    const bool processAttached = context.value("processAttached", true).toBool();
    const bool scanActive = context.value("scanActive").toBool();
    const bool unknownSnapshotActive = context.value("unknownSnapshotActive", false).toBool();
    const auto candidateCount = context.value("candidateCount").toULongLong();
    const QString contextTargetValue = context.value("targetValue").toString();
    const QString contextInitialValue = context.value("initialValue").toString();
    const QString value = firstNumber(query);
    const QStringList numbers = allNumbers(query);
    const bool describesVariation = describesIncrease(q) || describesDecrease(q) || describesChange(q) || describesStable(q);
    const bool inspectorOrUiCopy = wantsInspectorMode(q) || describesUiCopyOrBuffer(q);

    // Garde-fou : sans processus attache, aucun scan n'a de sens.
    if (!processAttached) {
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = "Attache d'abord un processus dans l'onglet Processus, puis relance ta recherche.";
        result["state"] = m_stateMachine.currentStateName();
        return result;
    }

    // PHASE 91/99 : demande explicite d'investigation "hors memoire" (voir
    // matchOffMemoryTool ci-dessus pour la liste de mots-cles).
    if (const auto trainerMatch = matchTrainerTool(query); !trainerMatch.tool.isEmpty()) {
        return makeToolCall(trainerMatch.tool, trainerMatch.args, trainerMatch.rationale);
    }
    if (const auto offMemoryMatch = matchOffMemoryTool(q); !offMemoryMatch.tool.isEmpty()) {
        return makeToolCall(offMemoryMatch.tool, {}, offMemoryMatch.rationale);
    }

    // Mode Inspecteur: obtenir une preuve nouvelle avant toute ecriture.
    if (inspectorOrUiCopy || q.contains("diff pages") || q.contains("pages modifiees") || q.contains("pages modifiées")) {
        if (numbers.size() >= 2 && (q.contains("compare") || q.contains("compar") || q.contains("maintenant")
            || q.contains("avant") || q.contains("apres") || q.contains("après"))) {
            return makeToolCall("finish_changed_pages_diff", {
                {"previousValue", numbers.at(0)},
                {"currentValue", numbers.at(1)},
            }, "Mode Inspecteur: je compare les pages modifiees entre l'ancienne et la nouvelle valeur affichee.");
        }

        if (q.contains("fenetre") || q.contains("fenêtre") || q.contains("window") || q.contains("uwp") || q.contains("store")) {
            QVariantMap args;
            if (q.contains("solitaire")) {
                args["titleContains"] = "Solitaire";
                args["includeAllVisible"] = true;
            }
            return makeToolCall("read_window_text", args,
                "Mode Inspecteur: je verifie la fenetre visible pour synchroniser l'observation.");
        }

        return makeToolCall("start_changed_pages_diff", {},
            "Mode Inspecteur: je capture un snapshot lecture seule avant la prochaine variation.");
    }

    // Recherche active + nouvelle valeur observee => reduction plutot que nouveau scan.
    if (scanActive && !value.isEmpty()) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", "exact"}, {"value", value}}, "Une recherche est deja active: je reduis les candidats avec la nouvelle valeur observee.");
    }

    // Recherche active + variation decrite sans valeur => next_scan increased/decreased/changed.
    if (scanActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("next_scan", {{"mode", variationMode(q)}},
            "Variation decrite pendant une recherche active: je reduis les candidats par comparaison.");
    }

    // Snapshot unknown capture + variation decrite => unknown_compare.
    if (unknownSnapshotActive && value.isEmpty() && describesVariation) {
        m_stateMachine.setState(AIState::Refining);
        return makeToolCall("unknown_compare", {{"mode", variationMode(q)}, {"valueType", "Auto"}},
            "Snapshot unknown actif: je compare avec la variation decrite.");
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
    const bool wantsKernel = q.contains("kernel") || q.contains("noyau");
    if (wantsKernel && (q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("kernel_write", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, "Ecriture kernel demandee explicitement par l'utilisateur.");
    }
    // Speedhack : place avant les checks generiques ci-dessous, meme raison
    // que wantsKernel plus haut.
    {
        const bool wantsSpeedOff = q.contains("désactiv") || q.contains("desactiv") ||
            ((q.contains("stop") || q.contains("arrêt") || q.contains("normal")) &&
             (q.contains("vitesse") || q.contains("speed") || q.contains("temps") || q.contains("speedhack")));
        const bool wantsPause = q.contains("pause") &&
            (q.contains("temps") || q.contains("vitesse") || q.contains("speedhack") || q.contains("jeu") || q.contains("game"));
        const bool wantsSlow = q.contains("ralent") || q.contains("slow");
        const bool wantsFast = q.contains("accélér") || q.contains("acceler") || q.contains("speed up") || q.contains("speedup");
        const bool wantsSpeedGeneric = q.contains("vitesse") || q.contains("speed");
        if (wantsSpeedOff) {
            return makeToolCall("speedhack_set", {{"mode", "off"}}, "Desactivation du speedhack demandee.");
        }
        if (wantsPause) {
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", 0.0}}, "Pause du temps demandee (speedhack).");
        }
        if (wantsSlow || wantsFast || wantsSpeedGeneric) {
            double factor = wantsSlow ? 0.5 : 2.0;
            bool parsedOk = false;
            const double parsed = value.toDouble(&parsedOk);
            if (parsedOk && parsed > 0.0) factor = parsed;
            return makeToolCall("speedhack_set", {{"mode", "set"}, {"factor", factor}},
                wantsSlow ? "Ralentissement demande (speedhack)." : "Acceleration demandee (speedhack).");
        }
    }
    if ((q.contains("write") || q.contains("mettre")) && !value.isEmpty()) {
        return makeToolCall("write_value", {
            {"address", firstHexAddress(query)},
            {"valueType", inferValueType(query)},
            {"value", value},
        }, "Ecriture memoire demandee.");
    }

    // Ecriture de la cible sans nouvelle valeur + peu de candidats =>
    // checkpoint safe (prepare) plutot que write direct.
    if ((q.contains("écri") || q.contains("ecri") || q.contains("write") || q.contains("checkpoint")
         || q.contains("finalis") || q.contains("valide"))
        && value.isEmpty() && scanActive && candidateCount > 0 && candidateCount <= 10
        && !contextTargetValue.isEmpty()) {
        return makeToolCall("prepare_write_checkpoint", {{"value", contextTargetValue}},
            "Peu de candidats et valeur cible connue: je prepare le checkpoint d'ecriture (sans ecrire).");
    }

    // Signalement d'echec: proposer une alternative adaptee plutot que refaire pareil.
    if (looksLikeBadTargets(q)) {
        const QVariantMap lastTurn = lastHistoryTurn();
        const QString lastOutcome = lastTurn.value("outcome").toString();
        if (!contextInitialValue.isEmpty()
            && (lastOutcome == "failed" || lastTurn.value("tool").toString() == "exact_scan")) {
            return makeToolCall("exact_scan_multi_type", {{"value", contextInitialValue}},
                "Les dernieres adresses ne marchent pas: je relance en multi-type pour couvrir d'autres representations.");
        }
        QVariantMap result;
        result["status"] = "needs_clarification";
        result["message"] = "Compris, ces adresses ne sont pas les bonnes. Donne-moi une valeur observee pour relancer "
                            "en multi-type, ou decris la valeur (affichee a l'ecran, chiffree, inconnue...).";
        result["state"] = m_stateMachine.currentStateName();
        return result;
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
QVariantMap AIEngine::makeToolCall(const QString& tool, const QVariantMap& args, const QString& rationale) {
    QVariantMap call;
    call["tool"] = tool;
    call["args"] = args;

    QString error;
    QVariantMap result;
    result["status"] = m_validator.validate(call, &error) ? "tool_call" : "invalid_tool_call";
    result["tool"] = tool;
    result["args"] = args;
    result["rationale"] = rationale;
    result["state"] = m_stateMachine.currentStateName();
    result["error"] = error;
    return result;
}

QString AIEngine::inferValueType(const QString& query) {
    const QString q = query.toLower();
    if (q.contains("double") || q.contains("float64")) return "Float64";
    if (q.contains("float") || q.contains("float32")) return "Float32";
    if (q.contains("int64") || q.contains("long")) return "Int64";
    return "Int32";
}

QString AIEngine::firstNumber(const QString& query) {
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    const auto match = re.match(query);
    if (!match.hasMatch()) return {};
    return match.captured(0).replace(',', '.');
}

QStringList AIEngine::allNumbers(const QString& query) {
    QStringList numbers;
    const QRegularExpression re(R"([-+]?\d+(?:[\.,]\d+)?)");
    auto it = re.globalMatch(query);
    while (it.hasNext()) {
        numbers.append(it.next().captured(0).replace(',', '.'));
    }
    return numbers;
}

QString AIEngine::firstHexAddress(const QString& query) {
    const QRegularExpression re(R"(0x[0-9a-fA-F]+)");
    const auto match = re.match(query);
    return match.hasMatch() ? match.captured(0) : QString();
}

} // namespace killai
