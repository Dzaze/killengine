#include <gtest/gtest.h>

#include "ai_engine.h"
#include "llama_runtime.h"
#include "model_locator.h"
#include "tool_validator.h"

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

TEST(AIEngineTest, PlansExactScanFromNumber) {
    killai::AIEngine engine;
    ASSERT_TRUE(engine.init());

    const auto result = engine.processQuery("j'ai 41250 argent");
    EXPECT_EQ(result.value("status").toString(), "tool_call");
    EXPECT_EQ(result.value("tool").toString(), "exact_scan");
    EXPECT_EQ(result.value("args").toMap().value("value").toString(), "41250");

    if (qEnvironmentVariableIsSet("KILLENGINE_QWEN_GGUF") && qEnvironmentVariableIsSet("KILLENGINE_LLAMA_CLI")) {
        EXPECT_EQ(result.value("aiBackend").toString(), "llama.cpp");
    }
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

TEST(ModelLocatorTest, ProvidesCandidateQwenPaths) {
    const auto paths = killai::ModelLocator::candidateModelPaths();
    EXPECT_FALSE(paths.isEmpty());
    EXPECT_TRUE(paths.join('|').contains("models"));
}
