#include <gtest/gtest.h>

#include "claude_backend_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace killai;

namespace {

QByteArray toolUseResponseBody(const QString& toolUseId, const QString& toolName, const QJsonObject& input) {
    QJsonObject response;
    response["stop_reason"] = "tool_use";
    QJsonArray content;
    content.append(QJsonObject{
        {"type", "tool_use"},
        {"id", toolUseId},
        {"name", toolName},
        {"input", input},
    });
    response["content"] = content;
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QByteArray finalTextResponseBody(const QString& text) {
    QJsonObject response;
    response["stop_reason"] = "end_turn";
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"}, {"text", text}});
    response["content"] = content;
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

} // namespace

TEST(ClaudeBackendClientTest, MissingApiKeyFailsWithoutCallingTransport) {
    int transportCalls = 0;
    ClaudeBackendClient client([&](const QString&, const QJsonObject&, int& httpStatus, bool& ok, QString&) -> QByteArray {
        ++transportCalls;
        httpStatus = 200;
        ok = true;
        return finalTextResponseBody("unused");
    });

    const QVariantMap result = client.sendMessage("", "test", QJsonArray(), nullptr);

    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_FALSE(result.value("error").toString().isEmpty());
    EXPECT_EQ(transportCalls, 0);
    EXPECT_EQ(client.requestCount(), 0);
}

TEST(ClaudeBackendClientTest, SingleTurnTextOnlyResponseNeedsNoToolExecution) {
    ClaudeBackendClient client([&](const QString&, const QJsonObject&, int& httpStatus, bool& ok, QString&) -> QByteArray {
        httpStatus = 200;
        ok = true;
        return finalTextResponseBody("Le gain crédité était de 15.");
    });

    const QVariantMap result = client.sendMessage("sk-ant-test", "que s'est-il passé ?", QJsonArray(), nullptr);

    EXPECT_TRUE(result.value("success").toBool());
    EXPECT_EQ(result.value("message").toString(), "Le gain crédité était de 15.");
    EXPECT_EQ(result.value("toolCallsExecuted").toInt(), 0);
    EXPECT_EQ(result.value("requestCount").toInt(), 1);
    EXPECT_EQ(client.requestCount(), 1);
}

TEST(ClaudeBackendClientTest, ExecutesToolUseThenFeedsResultBackForFinalAnswer) {
    int callIndex = 0;
    QVariantMap capturedArgs;
    QString capturedToolName;

    ClaudeBackendClient client([&](const QString& apiKey, const QJsonObject& requestBody, int& httpStatus, bool& ok, QString&) -> QByteArray {
        EXPECT_EQ(apiKey, "sk-ant-test");
        httpStatus = 200;
        ok = true;
        ++callIndex;
        if (callIndex == 1) {
            return toolUseResponseBody("toolu_1", "exact_scan", QJsonObject{{"value", "215"}, {"valueType", "Int32"}});
        }
        // Second call must carry the assistant tool_use turn + the tool_result turn.
        const QJsonArray messages = requestBody.value("messages").toArray();
        EXPECT_EQ(messages.size(), 3); // user, assistant(tool_use), user(tool_result)
        EXPECT_EQ(messages.at(1).toObject().value("role").toString(), "assistant");
        const QJsonArray toolResultContent = messages.at(2).toObject().value("content").toArray();
        EXPECT_EQ(toolResultContent.at(0).toObject().value("tool_use_id").toString(), "toolu_1");
        return finalTextResponseBody("3 candidats trouvés.");
    });

    ClaudeBackendClient::ToolExecutor executor = [&](const QString& toolName, const QVariantMap& args) -> QVariantMap {
        capturedToolName = toolName;
        capturedArgs = args;
        QVariantMap result;
        result["success"] = true;
        result["matches"] = 3;
        return result;
    };

    const QVariantMap result = client.sendMessage("sk-ant-test", "cherche 215", QJsonArray(), executor);

    EXPECT_TRUE(result.value("success").toBool());
    EXPECT_EQ(result.value("message").toString(), "3 candidats trouvés.");
    EXPECT_EQ(result.value("toolCallsExecuted").toInt(), 1);
    EXPECT_EQ(result.value("requestCount").toInt(), 2);
    EXPECT_EQ(capturedToolName, "exact_scan");
    EXPECT_EQ(capturedArgs.value("value").toString(), "215");
}

TEST(ClaudeBackendClientTest, NetworkFailurePropagatesExplicitErrorWithoutSilentFallback) {
    ClaudeBackendClient client([&](const QString&, const QJsonObject&, int&, bool& ok, QString& errorMessage) -> QByteArray {
        ok = false;
        errorMessage = "Connection refused";
        return {};
    });

    const QVariantMap result = client.sendMessage("sk-ant-test", "test", QJsonArray(), nullptr);

    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_TRUE(result.value("error").toString().contains("Connection refused"));
    EXPECT_EQ(client.requestCount(), 1);
}

TEST(ClaudeBackendClientTest, InvalidApiKeyHttpErrorPropagatesExplicitly) {
    ClaudeBackendClient client([&](const QString&, const QJsonObject&, int& httpStatus, bool& ok, QString&) -> QByteArray {
        httpStatus = 401;
        ok = true;
        QJsonObject errorBody;
        errorBody["type"] = "error";
        errorBody["error"] = QJsonObject{{"type", "authentication_error"}, {"message", "invalid x-api-key"}};
        return QJsonDocument(errorBody).toJson(QJsonDocument::Compact);
    });

    const QVariantMap result = client.sendMessage("sk-ant-bad", "test", QJsonArray(), nullptr);

    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_TRUE(result.value("error").toString().contains("401"));
}

TEST(ClaudeBackendClientTest, StopsAfterMaxToolTurnsInsteadOfLoopingForever) {
    int callCount = 0;
    ClaudeBackendClient client([&](const QString&, const QJsonObject&, int& httpStatus, bool& ok, QString&) -> QByteArray {
        ++callCount;
        httpStatus = 200;
        ok = true;
        return toolUseResponseBody(QString("toolu_%1").arg(callCount), "exact_scan", QJsonObject{{"value", "1"}});
    });

    ClaudeBackendClient::ToolExecutor executor = [](const QString&, const QVariantMap&) -> QVariantMap {
        QVariantMap result;
        result["success"] = true;
        return result;
    };

    const QVariantMap result = client.sendMessage("sk-ant-test", "boucle", QJsonArray(), executor, 3);

    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_EQ(callCount, 3);
    EXPECT_EQ(result.value("toolCallsExecuted").toInt(), 3);
}
