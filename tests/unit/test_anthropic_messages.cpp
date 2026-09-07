#include <gtest/gtest.h>

#include "anthropic_messages.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace killai;

TEST(AnthropicMessagesTest, RequestBodyOmitsToolsWhenSchemaIsEmpty) {
    QJsonArray messages;
    messages.append(makeUserMessage("hello"));

    const QJsonObject body = buildAnthropicRequestBody(QJsonArray(), messages);

    EXPECT_FALSE(body.contains("tools"));
    EXPECT_EQ(body.value("messages").toArray().size(), 1);
    EXPECT_FALSE(body.value("model").toString().isEmpty());
    EXPECT_GT(body.value("max_tokens").toInt(), 0);
}

TEST(AnthropicMessagesTest, RequestBodyOmitsSystemFieldWhenEmpty) {
    const QJsonObject body = buildAnthropicRequestBody(QJsonArray(), QJsonArray());
    EXPECT_FALSE(body.contains("system"));
}

TEST(AnthropicMessagesTest, RequestBodyIncludesSystemPromptWhenProvided) {
    const QJsonObject body = buildAnthropicRequestBody(QJsonArray(), QJsonArray(), "claude-sonnet-4-5", 4096, "Tu es l'assistant KillEngine.");
    ASSERT_TRUE(body.contains("system"));
    EXPECT_EQ(body.value("system").toString(), "Tu es l'assistant KillEngine.");
}

TEST(AnthropicMessagesTest, RequestBodyIncludesToolsWhenSchemaProvided) {
    QJsonArray tools;
    tools.append(QJsonObject{{"name", "exact_scan"}});

    const QJsonObject body = buildAnthropicRequestBody(tools, QJsonArray());

    ASSERT_TRUE(body.contains("tools"));
    EXPECT_EQ(body.value("tools").toArray().size(), 1);
}

TEST(AnthropicMessagesTest, MakeUserMessageHasUserRoleAndPlainTextContent) {
    const QJsonObject message = makeUserMessage("où est le compteur XP ?");
    EXPECT_EQ(message.value("role").toString(), "user");
    EXPECT_EQ(message.value("content").toString(), "où est le compteur XP ?");
}

TEST(AnthropicMessagesTest, MakeAssistantMessageReplaysRawContentBlocksVerbatim) {
    QJsonArray blocks;
    blocks.append(QJsonObject{{"type", "text"}, {"text", "je vais scanner"}});
    blocks.append(QJsonObject{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "exact_scan"}});

    const QJsonObject message = makeAssistantMessage(blocks);

    EXPECT_EQ(message.value("role").toString(), "assistant");
    EXPECT_EQ(message.value("content").toArray().size(), 2);
    EXPECT_EQ(message.value("content").toArray().at(1).toObject().value("id").toString(), "toolu_1");
}

TEST(AnthropicMessagesTest, MakeToolResultBlockSerializesResultDataAsJsonAndFlagsErrors) {
    QVariantMap resultData;
    resultData["success"] = true;
    resultData["matches"] = 3;

    const QJsonObject okBlock = makeToolResultBlock("toolu_42", resultData, false);
    EXPECT_EQ(okBlock.value("type").toString(), "tool_result");
    EXPECT_EQ(okBlock.value("tool_use_id").toString(), "toolu_42");
    EXPECT_FALSE(okBlock.contains("is_error"));
    const QByteArray content = okBlock.value("content").toString().toUtf8();
    const QJsonObject reparsed = QJsonDocument::fromJson(content).object();
    EXPECT_TRUE(reparsed.value("success").toBool());
    EXPECT_EQ(reparsed.value("matches").toInt(), 3);

    const QJsonObject errorBlock = makeToolResultBlock("toolu_43", resultData, true);
    EXPECT_TRUE(errorBlock.value("is_error").toBool());
}

TEST(AnthropicMessagesTest, ParsesSuccessfulResponseWithTextAndToolUseBlocks) {
    QJsonObject response;
    response["stop_reason"] = "tool_use";
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"}, {"text", "Je vais lancer un scan exact."}});
    content.append(QJsonObject{
        {"type", "tool_use"},
        {"id", "toolu_1"},
        {"name", "exact_scan"},
        {"input", QJsonObject{{"value", "215"}, {"valueType", "Int32"}}},
    });
    response["content"] = content;

    const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
    const AnthropicTurnResult result = parseAnthropicResponse(body, 200);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stopReason, "tool_use");
    EXPECT_EQ(result.textOutput, "Je vais lancer un scan exact.");
    ASSERT_EQ(result.toolUses.size(), 1);
    EXPECT_EQ(result.toolUses.at(0).id, "toolu_1");
    EXPECT_EQ(result.toolUses.at(0).name, "exact_scan");
    EXPECT_EQ(result.toolUses.at(0).input.value("value").toString(), "215");
    EXPECT_EQ(result.rawContentBlocks.size(), 2);
}

TEST(AnthropicMessagesTest, ParsesFinalTextOnlyResponseWithNoToolUses) {
    QJsonObject response;
    response["stop_reason"] = "end_turn";
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"}, {"text", "Le gain crédité était de 15."}});
    response["content"] = content;

    const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
    const AnthropicTurnResult result = parseAnthropicResponse(body, 200);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stopReason, "end_turn");
    EXPECT_EQ(result.textOutput, "Le gain crédité était de 15.");
    EXPECT_TRUE(result.toolUses.isEmpty());
}

TEST(AnthropicMessagesTest, HttpErrorStatusReportsExplicitAuthenticationError) {
    QJsonObject errorBody;
    errorBody["type"] = "error";
    QJsonObject errorDetails;
    errorDetails["type"] = "authentication_error";
    errorDetails["message"] = "invalid x-api-key";
    errorBody["error"] = errorDetails;

    const QByteArray body = QJsonDocument(errorBody).toJson(QJsonDocument::Compact);
    const AnthropicTurnResult result = parseAnthropicResponse(body, 401);

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.errorMessage.contains("401"));
    EXPECT_TRUE(result.errorMessage.contains("authentication_error"));
    EXPECT_TRUE(result.errorMessage.contains("invalid x-api-key"));
}

TEST(AnthropicMessagesTest, MalformedJsonBodyFailsExplicitlyInsteadOfCrashing) {
    const QByteArray body = "this is not json";
    const AnthropicTurnResult result = parseAnthropicResponse(body, 200);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errorMessage.isEmpty());
}
