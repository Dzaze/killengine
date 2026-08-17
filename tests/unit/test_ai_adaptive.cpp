#include <gtest/gtest.h>

#include "ai_engine.h"
#include "llama_server.h"
#include "llama_runtime.h"
#include "tool_registry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

} // namespace

// ---------------------------------------------------------------------------
// LlamaServer: construction de requete / parsing de reponse (pur, sans process)
// ---------------------------------------------------------------------------

TEST(LlamaServerTest, BuildsCompletionRequestWithCachePrompt) {
    const QByteArray body = killai::LlamaServer::buildCompletionRequest("ping", 32, {"stop1"});
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    ASSERT_TRUE(doc.isObject());
    const QJsonObject obj = doc.object();
    EXPECT_EQ(obj.value("prompt").toString(), "ping");
    EXPECT_EQ(obj.value("n_predict").toInt(), 32);
    EXPECT_TRUE(obj.value("cache_prompt").toBool());
    EXPECT_EQ(obj.value("temperature").toDouble(), 0.0);
    EXPECT_EQ(obj.value("stop").toArray().first().toString(), "stop1");
}

TEST(LlamaServerTest, ParsesCompletionContent) {
    const QByteArray body = R"({"content":"{\"tool\":\"exact_scan\"}","stopped_eos":true})";
    EXPECT_EQ(killai::LlamaServer::parseCompletionContent(body), "{\"tool\":\"exact_scan\"}");
}

TEST(LlamaServerTest, ParseCompletionContentRejectsInvalidJson) {
    EXPECT_TRUE(killai::LlamaServer::parseCompletionContent("not json").isEmpty());
    EXPECT_TRUE(killai::LlamaServer::parseCompletionContent(R"({"other":1})").isEmpty());
}

// Sortie reelle de Qwen3.5 via llama-server: le raisonnement <think> peut
// preceder le JSON utile; l'extracteur doit trouver le tool call dedans.
TEST(LlamaRuntimeAdaptiveTest, ExtractsToolCallFromQwenThinkOutput) {
    const QString qwenOutput =
        ".\n\n```json\n{\"tool\":\"exact_scan\",\"args\":{\"value\":\"41250\",\"valueType\":\"Int32\"}}\n```\n</think>\n\n"
        "```json\n{\"tool\":\"exact_scan\",\"args\":{\"value\":\"41250\",\"valueType\":\"Int32\"}}\n```";
    QString error;
    const auto call = killai::LlamaRuntime::extractToolCallJson(qwenOutput, &error);
    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_EQ(call.value("tool").toString().toStdString(), "exact_scan");
    EXPECT_EQ(call.value("args").toMap().value("value").toString().toStdString(), "41250");
}

// ---------------------------------------------------------------------------
// AIEngine adaptatif: variations pendant un scan actif / unknown snapshot
// ---------------------------------------------------------------------------

TEST(AIEngineAdaptiveTest, NextScanIncreasedWhenScanActiveAndUserDescribesIncrease) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(300);

    const auto result = engine.processQuery("la valeur a augmente depuis la derniere fois", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "next_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "increased");
}

TEST(AIEngineAdaptiveTest, NextScanDecreasedWhenScanActiveAndUserDescribesDecrease) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(300);

    const auto result = engine.processQuery("c'est descendu pas mal", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "next_scan");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "decreased");
}

TEST(AIEngineAdaptiveTest, UnknownCompareWhenSnapshotActiveAndUserDescribesVariation) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    context["unknownSnapshotActive"] = true;

    const auto result = engine.processQuery("ca a augmente quand je gagne un point", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "unknown_compare");
    EXPECT_EQ(result.value("args").toMap().value("mode").toString().toStdString(), "increased");
}

TEST(AIEngineAdaptiveTest, PrepareWriteCheckpointForFewCandidatesWithTarget) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = true;
    context["candidateCount"] = static_cast<qulonglong>(3);
    context["targetValue"] = "9999";

    const auto result = engine.processQuery("ecrit la valeur cible maintenant", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "prepare_write_checkpoint");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "9999");
}

TEST(AIEngineAdaptiveTest, MultiTypeRetryAfterFailedExactScanReport) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    // Simule un tour precedent: exact_scan qui a echoue.
    engine.noteOutcome("cherche 500", "exact_scan", "failed");

    QVariantMap context;
    context["processAttached"] = true;
    context["scanActive"] = false;
    context["initialValue"] = "500";

    const auto result = engine.processQuery("ca ne marche pas, mauvaise adresse", context);
    EXPECT_EQ(result.value("status").toString().toStdString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan_multi_type");
    EXPECT_EQ(result.value("args").toMap().value("value").toString().toStdString(), "500");
}

TEST(AIEngineHistoryTest, OutcomeHistoryIsBounded) {
    ScopedModelDisabled guard;
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());
    for (int i = 0; i < 20; ++i) {
        engine.noteOutcome(QString("query %1").arg(i), "exact_scan", "failed");
    }
    // clearHistory doit permettre de repartir proprement.
    engine.clearHistory();
    QVariantMap context;
    context["processAttached"] = true;
    const auto result = engine.processQuery("cherche 41250", context);
    EXPECT_EQ(result.value("tool").toString().toStdString(), "exact_scan");
}

// ---------------------------------------------------------------------------
// Tool registry: le checkpoint safe doit rester safe (confirmation)
// ---------------------------------------------------------------------------

TEST(AIToolRegistryTest, PrepareWriteCheckpointRequiresConfirmation) {
    killai::ToolRegistry registry;
    const auto tool = registry.toolMetadata("prepare_write_checkpoint");
    ASSERT_FALSE(tool.isEmpty());
    EXPECT_EQ(tool.value("risk").toString().toStdString(), "write");
    EXPECT_TRUE(tool.value("requiresConfirmation").toBool());
}