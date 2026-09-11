#include <gtest/gtest.h>

#include "ai_engine.h"
#include "intent_contract.h"
#include "llama_runtime.h"
#include "model_locator.h"
#include "query_text_utils.h"
#include "tool_validator.h"
#include "tool_registry.h"

#include <QSet>
#include <QSettings>

namespace {

class ScopedModelDisabled {
public:
    ScopedModelDisabled() {
        QSettings settings;
        m_previous = settings.value("ai/modelEnabled", true);
        m_previousEnv = qEnvironmentVariable("KILLENGINE_DISABLE_LLAMA");
        qputenv("KILLENGINE_DISABLE_LLAMA", "1");
        settings.setValue("ai/modelEnabled", false);
        settings.sync();
    }

    ~ScopedModelDisabled() {
        if (m_previousEnv.isNull()) {
            qunsetenv("KILLENGINE_DISABLE_LLAMA");
        } else {
            qputenv("KILLENGINE_DISABLE_LLAMA", m_previousEnv.toUtf8());
        }
        QSettings settings;
        settings.setValue("ai/modelEnabled", m_previous);
        settings.sync();
    }

private:
    QVariant m_previous;
    QString m_previousEnv;
};

// Sauvegarde/restaure "ui/language" autour du test, meme pattern que
// ScopedUiLanguage (tests/unit/test_localization.cpp) : les assertions ici
// verifient du texte KE_TXT en dur en francais, donc elles sont fragiles a la
// langue reellement persistee sur la machine qui execute les tests (trouve le
// 11/09/2026 en corrigeant la persistance du switch FR/EN de la sidebar --
// avant ce fix, "ui/language" n'etait jamais ecrit par le switch rapide donc
// ce probleme etait invisible).
class ScopedUiLanguage {
public:
    explicit ScopedUiLanguage(const QString& language) {
        QSettings settings;
        m_previous = settings.value("ui/language");
        settings.setValue("ui/language", language);
        settings.sync();
    }

    ~ScopedUiLanguage() {
        QSettings settings;
        if (m_previous.isValid()) {
            settings.setValue("ui/language", m_previous);
        } else {
            settings.remove("ui/language");
        }
        settings.sync();
    }

private:
    QVariant m_previous;
};

} // namespace

TEST(AIToolValidatorTest, AcceptsExactScan) {
    killai::ToolValidator validator;
    QVariantMap args;
    args["value"] = "42";
    args["valueType"] = "Int32";

    QVariantMap call;
    call["tool"] = "exact_scan";
    call["args"] = args;

    QString error;
    EXPECT_TRUE(validator.validate(call, &error));
    EXPECT_TRUE(error.isEmpty());
}

TEST(AIToolValidatorTest, RejectsMissingArg) {
    killai::ToolValidator validator;
    QVariantMap call;
    call["tool"] = "exact_scan";
    call["args"] = QVariantMap{};

    QString error;
    EXPECT_FALSE(validator.validate(call, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(AIToolRegistryTest, ExposesModernSafeAutoTools) {
    killai::ToolRegistry registry;

    EXPECT_TRUE(registry.hasTool("auto_resolve"));
    EXPECT_TRUE(registry.hasTool("encrypted_scan"));
    EXPECT_TRUE(registry.hasTool("exact_scan_module"));
    EXPECT_TRUE(registry.hasTool("trace_ui_string"));
    EXPECT_TRUE(registry.hasTool("read_window_text"));
    EXPECT_TRUE(registry.hasTool("list_process_modules"));
    EXPECT_TRUE(registry.hasTool("start_changed_pages_diff"));
    EXPECT_TRUE(registry.hasTool("finish_changed_pages_diff"));
    EXPECT_TRUE(registry.hasTool("trainer_list_features"));
    EXPECT_TRUE(registry.hasTool("trainer_create_write"));
    EXPECT_TRUE(registry.hasTool("trainer_delete_feature"));
    EXPECT_TRUE(registry.hasTool("analyze_field_stability"));

    const auto autoResolve = registry.toolMetadata("auto_resolve");
    EXPECT_EQ(autoResolve.value("risk").toString(), "safe");
    EXPECT_FALSE(autoResolve.value("requiresConfirmation").toBool());
    EXPECT_TRUE(autoResolve.value("requiredArgs").toStringList().contains("query"));

    // PHASE 130 : n'ecrit jamais rien (juste une capture findWhatWrites
    // passive) -- categorise "debug" pour la doc/taxonomie, mais s'execute
    // directement comme discover_save_files, pas de RiskGate.
    const auto fieldStability = registry.toolMetadata("analyze_field_stability");
    EXPECT_EQ(fieldStability.value("risk").toString(), "debug");
    EXPECT_FALSE(fieldStability.value("requiresConfirmation").toBool());
    EXPECT_TRUE(fieldStability.value("safe").toBool());
    EXPECT_TRUE(fieldStability.value("requiredArgs").toStringList().contains("address"));
}

TEST(AIToolRegistryTest, GetCandidatesIsSafeAndArgless) {
    // PHASE (T6, 08/09/2026) : ajoute apres une evaluation live ou l'absence
    // de cet outil forcait l'assistant a relancer un scan complet au lieu de
    // repondre "quelle est l'adresse ?" (next_scan/exact_scan ne renvoient
    // qu'un compteur, jamais les adresses reelles).
    killai::ToolRegistry registry;

    ASSERT_TRUE(registry.hasTool("get_candidates"));
    const auto tool = registry.toolMetadata("get_candidates");
    EXPECT_EQ(tool.value("risk").toString(), "safe");
    EXPECT_FALSE(tool.value("requiresConfirmation").toBool());
    EXPECT_TRUE(tool.value("requiredArgs").toStringList().isEmpty());
}

TEST(AIToolRegistryTest, MarksRiskyToolsAsConfirmationRequired) {
    killai::ToolRegistry registry;

    // PHASE 140 : generate_aob/suggest_patch retires de cette liste -- verifie
    // n'ecrire jamais rien (voir ReclassifiesReadOnlyPatchWorkflowToolsAsNoConfirmation
    // ci-dessous), reclasses requiresConfirmation=false. test_candidate_fields
    // ajoute : ecrit reellement (probe + restauration), doit rester true.
    for (const QString& toolName : {"write_value", "freeze_value", "find_what_writes", "test_candidate_fields", "trainer_apply_request", "trainer_restore_request"}) {
        const auto tool = registry.toolMetadata(toolName);
        ASSERT_FALSE(tool.isEmpty()) << toolName.toStdString();
        EXPECT_TRUE(tool.value("requiresConfirmation").toBool()) << toolName.toStdString();
        EXPECT_FALSE(tool.value("safe").toBool()) << toolName.toStdString();
    }
}

TEST(AIToolRegistryTest, ReclassifiesReadOnlyPatchWorkflowToolsAsNoConfirmation) {
    // PHASE 140 : generate_aob/suggest_patch/disassemble_backward verifies
    // (core inspection avant edition) comme n'ecrivant jamais rien --
    // lecture memoire seule + calcul, jamais de WriteProcessMemory. Reclasses
    // requiresConfirmation=false, meme precedent que analyze_field_stability
    // (PHASE 130). Restent categorie "patch" pour la taxonomie (etapes d'un
    // workflow de patch), seul le flag de confirmation change.
    killai::ToolRegistry registry;
    for (const QString& toolName : {"generate_aob", "suggest_patch", "disassemble_backward"}) {
        const auto tool = registry.toolMetadata(toolName);
        ASSERT_FALSE(tool.isEmpty()) << toolName.toStdString();
        EXPECT_EQ(tool.value("risk").toString().toStdString(), "patch") << toolName.toStdString();
        EXPECT_FALSE(tool.value("requiresConfirmation").toBool()) << toolName.toStdString();
        EXPECT_TRUE(tool.value("safe").toBool()) << toolName.toStdString();
    }
}

TEST(AIEngineTest, PlansExactScanFromNumber) {
    ScopedModelDisabled disableModel;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto result = engine.processQuery("j'ai 41250 argent");
    EXPECT_EQ(result.value("status").toString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString(), "exact_scan");
    EXPECT_EQ(result.value("args").toMap().value("value").toString(), "41250");
    EXPECT_EQ(result.value("aiBackend").toString(), "deterministic");
}

TEST(IntentContractTest, AcceptsGuidedScanIntent) {
    QVariantMap intent;
    intent["intent"] = "GuidedScan";
    intent["value"] = "900";
    intent["targetValue"] = "1000";

    QString error;
    EXPECT_TRUE(killai::IntentContract::validate(intent, &error));
    EXPECT_TRUE(error.isEmpty());
}

TEST(IntentContractTest, RejectsMemoryTargetsWithoutAddress) {
    QVariantMap intent;
    intent["intent"] = "ActivateMemoryTargets";

    QString error;
    EXPECT_FALSE(killai::IntentContract::validate(intent, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(IntentContractTest, ValidatesMemoryTargetAddresses) {
    QVariantMap intent;
    intent["intent"] = "WriteMemoryTargets";
    intent["value"] = "500";
    intent["addresses"] = QVariantList{"0x2d3a80afb0c", "0x2d3f7e25594"};

    QString error;
    EXPECT_TRUE(killai::IntentContract::validate(intent, &error));
    EXPECT_TRUE(error.isEmpty());
}

TEST(AIEngineTest, ProducesGuidedScanIntent) {
    ScopedModelDisabled disableModel;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto result = engine.processIntent("j'ai 900 en score je veux le passer a 1000");
    EXPECT_EQ(result.value("status").toString(), "intent");
    EXPECT_EQ(result.value("intent").toString(), "GuidedScan");
    EXPECT_EQ(result.value("value").toString(), "900");
    EXPECT_EQ(result.value("targetValue").toString(), "1000");
}

TEST(AIEngineTest, AsksClarificationForMissingWriteValue) {
    ScopedModelDisabled disableModel;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto result = engine.processIntent("passe le score");
    EXPECT_EQ(result.value("status").toString(), "needs_clarification");
    EXPECT_FALSE(result.value("message").toString().isEmpty());
}

TEST(AIEngineTest, RecognizesNaturalRewritePhrasesInConversationSequence) {
    ScopedModelDisabled disableModel;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto guided = engine.processIntent("j'ai un score de 20 je le veux a 1000");
    EXPECT_EQ(guided.value("status").toString(), "intent");
    EXPECT_EQ(guided.value("intent").toString(), "GuidedScan");
    EXPECT_EQ(guided.value("value").toString(), "20");
    EXPECT_EQ(guided.value("targetValue").toString(), "1000");

    const auto rewritePass = engine.processIntent("ok passe le a 2000");
    EXPECT_EQ(rewritePass.value("status").toString(), "intent");
    EXPECT_EQ(rewritePass.value("intent").toString(), "RewriteLastTargets");
    EXPECT_EQ(rewritePass.value("value").toString(), "2000");

    const auto rewriteWant = engine.processIntent("je les veux a 3000");
    EXPECT_EQ(rewriteWant.value("status").toString(), "intent");
    EXPECT_EQ(rewriteWant.value("intent").toString(), "RewriteLastTargets");
    EXPECT_EQ(rewriteWant.value("value").toString(), "3000");
}

TEST(AIEngineTest, RecognizesBadTargetRecoveryRequest) {
    ScopedModelDisabled disableModel;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto result = engine.processIntent("ça n'a pas marché mauvaise adresse");
    EXPECT_EQ(result.value("status").toString(), "intent");
    EXPECT_EQ(result.value("intent").toString(), "ReportBadTargets");
}

TEST(LlamaRuntimeTest, ExtractsToolCallJsonFromModelText) {
    QString error;
    const auto call = killai::LlamaRuntime::extractToolCallJson(
        "prefix {\"tool\":\"exact_scan\",\"args\":{\"value\":\"42\",\"valueType\":\"Int32\"}} suffix",
        &error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(call.value("tool").toString(), "exact_scan");
    EXPECT_EQ(call.value("args").toMap().value("value").toString(), "42");
}

TEST(LlamaRuntimeTest, ExtractsIntentJsonFromModelText) {
    QString error;
    const auto intent = killai::LlamaRuntime::extractIntentJson(
        R"(prefix {"intent":"GuidedScan","value":"900","targetValue":"1000","addresses":[],"confidence":0.95,"missing":""} suffix)",
        &error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(intent.value("intent").toString(), "GuidedScan");
    EXPECT_EQ(intent.value("value").toString(), "900");
    EXPECT_EQ(intent.value("targetValue").toString(), "1000");
}

TEST(LlamaRuntimeTest, ExtractsLastToolCallWhenPromptContainsJson) {
    QString error;
    const auto call = killai::LlamaRuntime::extractToolCallJson(
        R"(> Schema obligatoire: {"tool":"...","args":{...}}
{"tool":"exact_scan","args":{"value":"41250","valueType":"Int32"}}
[ Prompt: 49.3 t/s | Generation: 11.0 t/s ])",
        &error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(call.value("tool").toString(), "exact_scan");
    EXPECT_EQ(call.value("args").toMap().value("value").toString(), "41250");
    EXPECT_EQ(call.value("args").toMap().value("valueType").toString(), "Int32");
}

// PHASE (08/09/2026) : bug reel confirme en conditions live via le pipe
// d'automatisation -- une reponse modele tronquee commencant par '{' et ne se
// refermant jamais (donc findJsonObjectEnd() ne trouve jamais de fin valide)
// bouclait indefiniment : QString::lastIndexOf('{', -1) ne signifie PAS
// "rien avant l'index 0" mais "recompte depuis la fin de la chaine" (doc Qt),
// donc quand start valait 0, `start - 1 == -1` relancait la recherche depuis
// la fin et retrouvait indefiniment le MEME '{', gelant a 100% CPU le thread
// appelant (le thread principal Qt en conditions reelles -- gel total de
// l'application, meme un simple ping ne repondait plus). Ce test doit
// terminer quasi instantanement ; s'il bloque, la regression est revenue.
TEST(LlamaRuntimeTest, ExtractToolCallJsonDoesNotHangOnUnterminatedJsonAtStart) {
    QString error;
    const auto call = killai::LlamaRuntime::extractToolCallJson("{\"tool\":\"exact_scan\"", &error);
    EXPECT_TRUE(call.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(LlamaRuntimeTest, ExtractIntentJsonDoesNotHangOnUnterminatedJsonAtStart) {
    QString error;
    const auto intent = killai::LlamaRuntime::extractIntentJson("{\"intent\":\"Unknown\"", &error);
    EXPECT_TRUE(intent.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

// PHASE 140 : bug reel trouve en corrigeant l'ecart "outils annonces mais non
// dispatches" (PHASE 139) -- la ligne "Schema obligatoire" dans
// LlamaRuntime::buildPrompt (ai/llama_runtime.cpp) est codee en dur, PAS
// generee depuis ToolRegistry::availableTools() (contrairement au bloc
// "Outils disponibles" du meme prompt) : get_auto_report/disassemble_backward/
// test_candidate_fields manquaient a cette ligne, donc invisibles pour le
// modele local meme si leur dispatch existe. Ce test verifie que TOUS les
// outils du registre apparaissent dans le segment "Schema obligatoire" du
// prompt, pour empecher ce meme type de derive de revenir silencieusement.
TEST(LlamaRuntimeTest, SchemaLineListsEveryRegisteredTool) {
    killai::ToolRegistry registry;
    const QString prompt = killai::LlamaRuntime::buildPrompt("test query", registry, {});

    QString schemaLine;
    for (const QString& line : prompt.split('\n')) {
        if (line.contains("Schema obligatoire")) {
            schemaLine = line;
            break;
        }
    }
    ASSERT_FALSE(schemaLine.isEmpty()) << "Ligne \"Schema obligatoire\" introuvable dans le prompt.";

    // PHASE 148 : audit complet de ce qui manquait encore (voir
    // docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md pour le detail). Exclusion
    // reduite a ce qui reste DELIBEREMENT hors du choix libre du modele :
    //   - kernel_write/speedhack_set/block_process_network (risk=injection) :
    //     leur propre description dit deja "a utiliser seulement si
    //     l'utilisateur le demande explicitement" -- exclusion volontaire,
    //     restent joignables via fast-path deterministe sur mot-cle explicite
    //     uniquement (ai/ai_engine.cpp, deterministicPlan/
    //     deterministicPlanWithContext), jamais un choix libre du LLM.
    //   - patch_file_bytes : ecrit reellement sur disque, redirige toujours
    //     vers l'UI/pipe cote dispatch (PHASE 148 -- corrige un bypass
    //     RiskGate existant), pas encore de fast-path explicite non plus,
    //     laisse hors schema par prudence tant que ce chemin n'est pas
    //     davantage exerce.
    // discover_save_files/inspect_local_settings/read_save_file_text/
    // watch_save_file sont maintenant DANS le schema (PHASE 148) -- plus
    // besoin de les exclure ici.
    // PHASE 271 : tous les outils exposes au LLM sans restriction.
    // La liste d'exclusion est videe pour permettre a l'Assistant d'acceder
    // a toutes les capacites, y compris kernel_write, speedhack_set,
    // block_process_network, patch_file_bytes, apply_stealth_mode, restore_stealth_mode.
    static const QSet<QString> kDeliberatelyExcludedFromModelSchema = {
        // Aucun outil exclu - tous les outils sont accessibles au LLM
    };

    for (const auto& item : registry.availableTools()) {
        const QString toolName = item.toMap().value("name").toString();
        ASSERT_FALSE(toolName.isEmpty());
        if (kDeliberatelyExcludedFromModelSchema.contains(toolName)) {
            continue;
        }
        EXPECT_TRUE(schemaLine.contains(toolName))
            << "Outil '" << toolName.toStdString() << "' enregistre dans ToolRegistry mais absent de la ligne "
            << "\"Schema obligatoire\" -- le modele local ne peut jamais le choisir. " << schemaLine.toStdString();
    }
}

// PHASE 271 : tous les outils sont maintenant exposes au LLM sans restriction.
// Ce test verifie que les outils precedemment exclus (kernel_write,
// speedhack_set, block_process_network, patch_file_bytes, apply_stealth_mode,
// restore_stealth_mode) sont bien presents dans le schema pour que l'Assistant
// puisse y acceder librement.
TEST(LlamaRuntimeTest, SchemaLineIncludesAllTools) {
    killai::ToolRegistry registry;
    const QString prompt = killai::LlamaRuntime::buildPrompt("test query", registry, {});

    QString schemaLine;
    for (const QString& line : prompt.split('\n')) {
        if (line.contains("Schema obligatoire")) {
            schemaLine = line;
            break;
        }
    }
    ASSERT_FALSE(schemaLine.isEmpty());

    // Tous les outils, y compris ceux a risque, sont maintenant accessibles
    for (const QString& toolName : {"kernel_write", "speedhack_set", "block_process_network", "patch_file_bytes",
                                     "apply_stealth_mode", "restore_stealth_mode"}) {
        EXPECT_TRUE(schemaLine.contains(toolName))
            << toolName.toStdString() << " doit etre present dans le schema pour etre accessible a l'Assistant.";
    }
}

// PHASE 148 : block_process_network gagne les memes fast-paths deterministes
// que kernel_write/speedhack_set (jamais un choix libre du LLM, uniquement
// sur mot-cle explicite -- voir ai_engine.cpp deterministicPlan/
// deterministicPlanWithContext).
TEST(AIEngineContextualFallbackTest, BlockNetworkFastPathMatchesExplicitCutRequestFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("coupe le réseau du jeu", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "block_process_network");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "on");
}

TEST(AIEngineContextualFallbackTest, BlockNetworkFastPathMatchesExplicitRestoreRequestEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("restore network for the target", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "block_process_network");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "off");
}

TEST(ModelLocatorTest, ProvidesCandidateQwenPaths) {
    const auto paths = killai::ModelLocator::candidateModelPaths();
    EXPECT_FALSE(paths.isEmpty());
    EXPECT_TRUE(paths.join('|').contains("model"));
    EXPECT_TRUE(paths.join('|').contains("qwen"));
}

// --- Contexte de session : le fallback deterministe choisit le bon outil ---

TEST(AIEngineContextualFallbackTest, AsksToAttachProcessWhenDetached) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("cherche 41250", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("message").toString().contains("processus"));
}

TEST(AIEngineContextualFallbackTest, PureGreetingDoesNotAskForSearchValue) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("salut", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_social_guard");
    EXPECT_TRUE(result.value("message").toString().contains("Salut"));
    EXPECT_FALSE(result.value("message").toString().contains("valeur affichée (ex:"));
    EXPECT_FALSE(result.value("message").toString().contains("processus"));
}

TEST(AIEngineContextualFallbackTest, PureThanksDoesNotAskForSearchValue) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("merci mon pote !", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_social_guard");
    EXPECT_TRUE(result.value("message").toString().contains("chercher"));
}

TEST(AIEngineContextualFallbackTest, GreetingWithConcreteValueStillPlansScan) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    const auto result = engine.processQuery("salut cherche 41250", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookBroadDisplayedValueIsReadOnlyPlan) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto result = engine.processQuery("je ne trouve pas la valeur affichée", context);

    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(result.value("actionStatus").toString().toStdString(), "not_executed");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_investigation_playbook");
    EXPECT_EQ(result.value("investigationTopic").toString().toStdString(), "displayed_value_not_found");
    EXPECT_FALSE(result.contains("tool"));
    EXPECT_EQ(result.value("source").toString().toStdString(), "docs/INVESTIGATION_PLAYBOOK.md");
    EXPECT_TRUE(result.value("message").toString().contains("n'exécute rien automatiquement"));
    // PHASE 120-B (29/08/2026) : le texte affiche a l'utilisateur ne doit plus
    // reference de terminologie interne au depot (numeros de PHASE, chemins de
    // fichiers docs/) -- constate en direct par le proprietaire dans le chat
    // reel, corrige la meme session.
    EXPECT_FALSE(result.value("message").toString().contains("PHASE 120"));
    EXPECT_FALSE(result.value("message").toString().contains("docs/"));
    EXPECT_FALSE(result.value("message").toString().contains("RiskGate"));
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookFreezeFlickerNeverCreatesExecutableAction) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto result = engine.processQuery("le freeze clignote sur 0x1a2b3c4d et ne tient pas", context);

    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(result.value("actionStatus").toString().toStdString(), "not_executed");
    EXPECT_EQ(result.value("investigationTopic").toString().toStdString(), "freeze_flickers");
    EXPECT_FALSE(result.contains("tool"));
    EXPECT_FALSE(result.value("message").toString().contains("j'exécute"));
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookAutoChainsFieldStabilityWhenBothMatch) {
    // PHASE 120-C (29/08/2026, accord propriétaire explicite) : quand le
    // message combine le symptôme playbook ET les mots-clés du fast-path
    // analyze_field_stability déjà approuvé sans confirmation, enchaîner
    // directement plutôt que renvoyer la simple citation.
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto result = engine.processQuery(
        "le freeze clignote sur 0x1a2b3c4d, teste la stabilité du champ", context);

    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_field_stability");
    EXPECT_EQ(result.value("investigationTopic").toString().toStdString(), "freeze_flickers");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_investigation_playbook_autochain");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookAutoChainsSaveFileDiscoveryWhenBothMatch) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto result = engine.processQuery("cette valeur vit peut-être dans un fichier de sauvegarde", context);

    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "discover_save_files");
    EXPECT_EQ(result.value("investigationTopic").toString().toStdString(), "save_file_or_uwp");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_investigation_playbook_autochain");
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookNeverAutoChainsWhatWritesOrCodePatch) {
    // "what_writes_value" attache un debugger (jamais autonome, PHASE 140) ;
    // "code_patch_request" ne peut structurellement jamais co-matcher son
    // propre fast-path generate_aob (son matcher exige !directReadOnlyTool,
    // qui inclut wantsGenerateAobQuery) -- les deux doivent toujours rester
    // de simples citations, jamais un tool_call, quel que soit le contenu du
    // message.
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto whatWrites = engine.processQuery("qui écrit cette valeur sur 0x1a2b3c4d ?", context);
    EXPECT_EQ(whatWrites.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(whatWrites.value("investigationTopic").toString().toStdString(), "what_writes_value");

    const auto codePatch = engine.processQuery(
        "je veux patcher le code proprement, génère une signature AOB pour 0x1a2b3c4d", context);
    EXPECT_EQ(codePatch.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(codePatch.value("tool").toString().toStdString(), "generate_aob");
    EXPECT_FALSE(codePatch.contains("investigationTopic"));
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookRecoveryActionsAreSafeNavigationOnly) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto freeze = engine.processQuery("le freeze clignote sur 0x1a2b3c4d et ne tient pas", context);
    ASSERT_TRUE(freeze.contains("recoveryActions"));
    const auto freezeActions = freeze.value("recoveryActions").toList();
    ASSERT_EQ(freezeActions.size(), 1);
    EXPECT_EQ(freezeActions.at(0).toMap().value("id").toString().toStdString(), "open_expert");

    const auto managed = engine.processQuery(
        "coreclr est charge et scanPointerChains ne renvoie aucune chaine de pointeurs", context);
    EXPECT_EQ(managed.value("investigationTopic").toString().toStdString(), "managed_runtime_pointer_chain");
    ASSERT_TRUE(managed.contains("recoveryActions"));
    const auto managedActions = managed.value("recoveryActions").toList();
    ASSERT_EQ(managedActions.size(), 1);
    EXPECT_EQ(managedActions.at(0).toMap().value("id").toString().toStdString(), "open_clr_inspector");
}

TEST(AIEngineContextualFallbackTest, InvestigationPlaybookDoesNotHijackConcreteExistingTools) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;

    const auto trace = engine.processQuery("la valeur 60 est affichee mais introuvable", context);
    EXPECT_EQ(trace.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(trace.value("tool").toString().toStdString(), "trace_ui_string");

    const auto findWrites = engine.processQuery("capture ce qui écrit 0x1a2b3c4d", context);
    EXPECT_EQ(findWrites.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(findWrites.value("tool").toString().toStdString(), "find_what_writes");
}

TEST(AIEngineContextualFallbackTest, RefinesWithNextScanWhenScanActive) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(120);
    const auto result = engine.processQuery("maintenant c'est 812", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "next_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "exact");
}

TEST(AIEngineContextualFallbackTest, ExactScanWhenNoActiveSearch) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    const auto result = engine.processQuery("j'ai 41250 argent", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}

TEST(AIEngineContextualFallbackTest, TraceUiStringForDisplayedValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("la valeur 60 est affichee mais introuvable", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trace_ui_string");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "60");
}

TEST(AIEngineContextualFallbackTest, InspectorModeStartsChangedPagesDiff) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("mode inspecteur, on observe les copies UI avant d'ecrire", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "start_changed_pages_diff");
}

TEST(AIEngineContextualFallbackTest, LiteralChangedPagesStartsChangedPagesDiff) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("Changed Pages : l'XP est maintenant a 490, prepare la comparaison", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "start_changed_pages_diff");
}

TEST(AIEngineContextualFallbackTest, ModuleSourcePivotDoesNotConsumeIncreasedAsNextScan) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(1596);
    const auto result = engine.processQuery(
        "Sur Bubble Solitaire XP, exact et increased ne convergent pas. Je pense qu il faut chercher dans les DLL/modules et retrouver la vraie source XP plutot qu une copie affichee.",
        context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "list_process_modules");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_module_listing_fastpath");
}

TEST(AIEngineContextualFallbackTest, ModuleXpDiscoveryWithoutValueListsModulesInsteadOfAutoResolve) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;

    const auto result = engine.processQuery("il vas falloir trouver les xp dans les dll", context);

    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "list_process_modules");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_module_listing_fastpath");
    EXPECT_TRUE(result.value("rationale").toString().contains("valeur affichée"));
}

TEST(AIEngineContextualFallbackTest, NamedModuleValueRoutesToModuleBoundedScan) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(21533);
    const auto result = engine.processQuery(
        "Ok maintenant utilise Microsoft.MicrosoftSolitaireCollection.dll pour continuer la recherche de la vraie source XP 215 vers 9000.",
        context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan_module");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_module_scan_fastpath");
    const auto args = result.value("args").toMap();
    EXPECT_EQ(args.value("module").toString().toStdString(), "Microsoft.MicrosoftSolitaireCollection.dll");
    EXPECT_EQ(args.value("value").toString().toStdString(), "215");
    EXPECT_EQ(args.value("targetValue").toString().toStdString(), "9000");
}

// PHASE 99 : demande explicite d'investigation "hors memoire" (LocalSettings,
// fichier de sauvegarde, watch fichier) doit resoudre l'outil deterministe
// SANS passer par le modele local -- aiBackend le prouve directement, plutot
// que de mesurer un delai (non fiable en test).
TEST(AIEngineContextualFallbackTest, OffMemoryFastPathInspectsLocalSettingsFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("inspecte LocalSettings settings.dat", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "inspect_local_settings");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_offmemory_fastpath");
}

TEST(AIEngineContextualFallbackTest, OffMemoryFastPathInspectsLocalSettingsEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("check the local settings registry hive", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "inspect_local_settings");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_offmemory_fastpath");
}

TEST(AIEngineContextualFallbackTest, OffMemoryFastPathDiscoversSaveFilesFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("cherche un fichier de sauvegarde sur le disque", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "discover_save_files");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_offmemory_fastpath");
}

TEST(AIEngineContextualFallbackTest, OffMemoryFastPathDiscoversSaveFilesEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("is there a save file on disk", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "discover_save_files");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_offmemory_fastpath");
}

TEST(AIEngineContextualFallbackTest, OffMemoryFastPathRoutesWatchRequestToDiscoveryFirst) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto resultFr = engine.processQuery("surveille le fichier de sauvegarde", context);
    EXPECT_EQ(resultFr.value("tool").toString().toStdString(), "discover_save_files");
    const auto resultEn = engine.processQuery("watch file for changes", context);
    EXPECT_EQ(resultEn.value("tool").toString().toStdString(), "discover_save_files");
}

TEST(AIEngineContextualFallbackTest, OffMemoryFastPathStillRequiresAttachedProcess) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("inspecte LocalSettings settings.dat", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("message").toString().contains("processus"));
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathListsFeaturesFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("liste le trainer", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_list_features");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_trainer_fastpath");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathListsFeaturesEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("show trainer features", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_list_features");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_trainer_fastpath");
}

TEST(QueryTextUtilsTest, SharedTrainerPredicateCoversSmartSearchBypassPhrases) {
    EXPECT_TRUE(killai::wantsTrainerQuery("liste le trainer"));
    EXPECT_TRUE(killai::wantsTrainerQuery("ouvre la cheat table"));
    EXPECT_TRUE(killai::wantsTrainerQuery("ajoute une fonction trainer"));
    EXPECT_FALSE(killai::wantsTrainerQuery("cherche 41250"));
}

TEST(QueryTextUtilsTest, SharedSocialPredicateCanReusePreExtractedTokens) {
    EXPECT_TRUE(killai::looksLikePureSocialQuery("salut !!", {}, {}));
    EXPECT_FALSE(killai::looksLikePureSocialQuery("salut cherche 41250", {"41250"}, {}));
    EXPECT_FALSE(killai::looksLikePureSocialQuery("merci 0x12345", {}, {"0x12345"}));
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathCreatesWriteFeatureFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("ajoute une feature trainer write 0x12345 a 900", context);
    const auto args = result.value("args").toMap();
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_create_write");
    EXPECT_EQ(args.value("address").toString().toStdString(), "0x12345");
    EXPECT_EQ(args.value("value").toString().toStdString(), "900");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathCreatesWriteFeatureEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("create trainer write feature 0x1a2b3c4d to 100", context);
    const auto args = result.value("args").toMap();
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_create_write");
    EXPECT_EQ(args.value("address").toString().toStdString(), "0x1a2b3c4d");
    EXPECT_EQ(args.value("value").toString().toStdString(), "100");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathDeletesFeatureFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("supprime la feature trainer 3", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_delete_feature");
    EXPECT_EQ(result.value("args").toMap().value("id").toString().toStdString(), "3");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathDeletesFeatureEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("delete trainer feature 12", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_delete_feature");
    EXPECT_EQ(result.value("args").toMap().value("id").toString().toStdString(), "12");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathApplyRequiresUiConfirmationFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("active tout le trainer", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_apply_request");
    EXPECT_TRUE(result.value("args").toMap().value("all").toBool());
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathApplyRequiresUiConfirmationEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("apply trainer feature 7", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_apply_request");
    EXPECT_EQ(result.value("args").toMap().value("id").toString().toStdString(), "7");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathRestoreRequiresUiConfirmationFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("restaure la feature trainer 4", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_restore_request");
    EXPECT_EQ(result.value("args").toMap().value("id").toString().toStdString(), "4");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathRestoreRequiresUiConfirmationEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("restore all trainer features", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_restore_request");
    EXPECT_TRUE(result.value("args").toMap().value("all").toBool());
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathStillRequiresAttachedProcess) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("liste le trainer", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("message").toString().contains("processus"));
}

// PHASE 137bis : durcissement demande par le propriétaire (liste de 7
// chantiers, point 3) sur ce que Codex venait de livrer en PHASE 129 --
// phrases ambiguës et champs requis manquants. `ToolValidator::validate`
// (ai/tool_validator.cpp) ne vérifie que la PRESENCE de la clé dans args, pas
// qu'elle soit non vide (`args.contains(required)`) -- donc matchTrainerTool
// qui insère toujours "address"/"value"/"id" (même vides) fait passer la
// validation ; c'est le dispatch C++ (application_controller.cpp, pas
// unit-testable ici, voir AGENTS.md) qui renvoie ensuite needs_clarification
// sur un champ vide. Ces tests documentent ce partage de responsabilité
// explicitement, pour qu'un futur agent ne suppose pas que le fast-path lui-
// même filtre les champs manquants.
TEST(AIEngineContextualFallbackTest, TrainerFastPathCreateWithoutAddressOrValueStillProducesToolCall) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("ajoute une feature trainer", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_create_write");
    const auto args = result.value("args").toMap();
    EXPECT_TRUE(args.value("address").toString().isEmpty());
    EXPECT_TRUE(args.value("value").toString().isEmpty());
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathDeleteWithoutIdStillProducesToolCall) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("supprime la feature trainer", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_delete_feature");
    EXPECT_TRUE(result.value("args").toMap().value("id").toString().isEmpty());
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathAmbiguousApplyAndDeletePhraseAppliesWins) {
    // Documente la precedence reelle du if-chain de matchTrainerTool
    // (ai/ai_engine.cpp) : wantsApply est teste AVANT wantsDelete. Une phrase
    // qui matche les deux mots-cles route donc vers apply, pas delete --
    // comportement a connaitre avant de reordonner ce chain un jour.
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("active puis supprime la feature trainer 3", context);
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_apply_request");
}

TEST(AIEngineContextualFallbackTest, TrainerFastPathIsCaseInsensitive) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("SUPPRIME LA FEATURE TRAINER 5", context);
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_delete_feature");
    EXPECT_EQ(result.value("args").toMap().value("id").toString().toStdString(), "5");
}

TEST(AIEngineContextualFallbackTest, FieldStabilityFastPathMatchesDisplayedFieldFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("est-ce que 0x1a2b3c4d est un champ affiché ou une vraie source ?", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_field_stability");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_field_stability_fastpath");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, FieldStabilityFastPathMatchesDisplayedFieldEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("is 0x1a2b3c4d a displayed field or a real source?", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_field_stability");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_field_stability_fastpath");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, FieldStabilityFastPathWithoutAddressDoesNotCallTool) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    // Sans adresse 0x..., "valeur affichee/champ affiche" doit rester une
    // guidance, pas un appel analyze_field_stability invalide.
    const auto result = engine.processQuery("est-ce un champ affiché ou la vraie source ?", context);
    EXPECT_NE(result.value("tool").toString().toStdString(), "analyze_field_stability");
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("error").toString().isEmpty());
}

TEST(AIEngineContextualFallbackTest, NegatedFreezeDoesNotRouteToFreezeTool) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery(
        "Je teste Solitaire : conseille-moi comment trouver une valeur affichee sans ecrire ni freeze, en privilegient Trace UI string ou Changed Pages.",
        context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_EQ(result.value("actionStatus").toString().toStdString(), "not_executed");
    EXPECT_NE(result.value("tool").toString().toStdString(), "freeze_value");
    EXPECT_NE(result.value("tool").toString().toStdString(), "analyze_field_stability");
    EXPECT_NE(result.value("status").toString().toStdString(), "invalid_tool_call");
    EXPECT_TRUE(result.value("message").toString().contains("Trace UI string"));
    EXPECT_TRUE(result.value("message").toString().contains("Changed Pages"));
}

TEST(AIEngineContextualFallbackTest, FieldStabilityFastPathStillRequiresAttachedProcess) {
    ScopedModelDisabled guard;
    ScopedUiLanguage lang("fr");
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("is 0x1a2b3c4d a displayed field?", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
    EXPECT_TRUE(result.value("message").toString().contains("processus"));
}

TEST(AIEngineContextualFallbackTest, FieldStabilityFastPathDoesNotCollideWithTrainer) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("liste le trainer", context);
    EXPECT_EQ(result.value("tool").toString().toStdString(), "trainer_list_features");
}

// PHASE 140 : fast-paths pour 2 des 7 outils "annonces mais non dispatches"
// trouves en PHASE 139 -- get_auto_report et analyze_ui_sources (les 5 autres
// n'ont volontairement pas de fast-path deterministe, voir
// docs/KILLENGINE_ASSISTANT_TOOLS_MAP.md pour le detail par outil).
TEST(AIEngineContextualFallbackTest, AutoReportFastPathMatchesFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("donne-moi le rapport auto-résolution", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "get_auto_report");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_auto_report_fastpath");
}

TEST(AIEngineContextualFallbackTest, AutoReportFastPathMatchesEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("show me the auto report", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "get_auto_report");
}

TEST(AIEngineContextualFallbackTest, AutoReportFastPathStillRequiresAttachedProcess) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = false;
    const auto result = engine.processQuery("donne-moi le rapport auto-résolution", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "needs_clarification");
}

TEST(AIEngineContextualFallbackTest, UiSourcesFastPathMatchesWithValueFr) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("analyse les sources numériques, c'est maintenant 60", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_ui_sources");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_ui_sources_fastpath");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "60");
}

TEST(AIEngineContextualFallbackTest, UiSourcesFastPathMatchesWithValueEn) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("analyze sources, it's 60 now", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_ui_sources");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "60");
}

TEST(AIEngineContextualFallbackTest, UiSourcesFastPathWithoutValueIsInvalidToolCall) {
    // "value" est requiredArgs cote tool_registry.cpp -- meme comportement
    // documente pour analyze_field_stability (PHASE 130) : le mot-cle matche,
    // le validateur rejette a raison un tool_call sans la donnee necessaire.
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("analyse les sources numériques", context);
    EXPECT_EQ(result.value("tool").toString().toStdString(), "analyze_ui_sources");
    EXPECT_EQ(result.value("status").toString().toStdString(), "invalid_tool_call");
}

// PHASE 140 : fast-paths pour les 5 outils restants d'analyze_field_stability
// qui necessitent une adresse (generate_aob/suggest_patch/disassemble_backward
// executent reellement ; find_what_writes/test_candidate_fields redirigent
// toujours vers l'UI cote dispatch, requiredArgs volontairement vide).
TEST(AIEngineContextualFallbackTest, GenerateAobFastPathMatchesWithAddress) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("génère une signature aob pour 0x1a2b3c4d", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "generate_aob");
    EXPECT_EQ(result.value("aiBackend").toString().toStdString(), "deterministic_generate_aob_fastpath");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, SuggestPatchFastPathMatchesWithAddress) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("suggère un patch pour 0x1a2b3c4d", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "suggest_patch");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, DisassembleBackwardFastPathMatchesWithAddress) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("désassemble en arrière 0x1a2b3c4d", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "disassemble_backward");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, FindWhatWritesFastPathMatchesAndValidatesWithoutRequiredArgs) {
    // requiredArgs vide (PHASE 140) : le tool_call doit rester valide meme
    // sans "size", puisque le dispatch redirige toujours vers l'UI plutot
    // que d'executer avec ces args.
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("capture ce qui écrit 0x1a2b3c4d", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "find_what_writes");
    EXPECT_EQ(result.value("args").toMap().value("address").toString().toStdString(), "0x1a2b3c4d");
}

TEST(AIEngineContextualFallbackTest, TestCandidateFieldsFastPathMatchesWithoutRequiredArgs) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("teste les champs candidats", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "test_candidate_fields");
}

TEST(AIEngineContextualFallbackTest, InspectorModeFinishesChangedPagesDiffWithTwoValues) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("compare le diff pages, avant 60 maintenant 59", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "finish_changed_pages_diff");
    EXPECT_EQ(result.value("args").toMap().value("previousValue").toString().toStdString(), "60");
    EXPECT_EQ(result.value("args").toMap().value("currentValue").toString().toStdString(), "59");
}

TEST(AIEngineContextualFallbackTest, EncryptedScanForObfuscatedValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("la valeur 500 semble chiffree", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "encrypted_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "xor");
}

TEST(AIEngineContextualFallbackTest, UnknownCaptureWhenValueUnknown) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    const auto result = engine.processQuery("je ne sais pas la valeur, elle augmente quand je gagne", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "unknown_capture");
}

TEST(AIEngineContextualFallbackTest, AutoResolveForGuidedObjectiveWithoutValue) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("trouve cette valeur et guide-moi", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "auto_resolve");
}

TEST(AIEngineContextualFallbackTest, LegacyOverloadStillPlansExactScan) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    const auto result = engine.processQuery("j'ai 41250 argent");
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}
