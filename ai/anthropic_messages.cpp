#include "anthropic_messages.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace killai {

QJsonObject buildAnthropicRequestBody(const QJsonArray& toolsSchema,
                                       const QJsonArray& messages,
                                       const QString& model,
                                       int maxTokens,
                                       const QString& systemPrompt) {
    QJsonObject body;
    body["model"] = model;
    body["max_tokens"] = maxTokens;
    if (!systemPrompt.isEmpty()) {
        body["system"] = systemPrompt;
    }
    if (!toolsSchema.isEmpty()) {
        body["tools"] = toolsSchema;
    }
    body["messages"] = messages;
    return body;
}

QJsonObject makeUserMessage(const QString& text) {
    QJsonObject message;
    message["role"] = "user";
    message["content"] = text;
    return message;
}

QJsonObject makeAssistantMessage(const QJsonArray& rawContentBlocks) {
    QJsonObject message;
    message["role"] = "assistant";
    message["content"] = rawContentBlocks;
    return message;
}

QJsonObject makeToolResultMessage(const QJsonArray& toolResultBlocks) {
    QJsonObject message;
    message["role"] = "user";
    message["content"] = toolResultBlocks;
    return message;
}

QJsonObject makeToolResultBlock(const QString& toolUseId, const QVariantMap& resultData, bool isError) {
    QJsonObject block;
    block["type"] = "tool_result";
    block["tool_use_id"] = toolUseId;
    const QJsonDocument doc(QJsonObject::fromVariantMap(resultData));
    block["content"] = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    if (isError) {
        block["is_error"] = true;
    }
    return block;
}

namespace {

QString describeHttpError(int httpStatus, const QByteArray& rawBody) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(rawBody, &parseError);
    if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
        const QJsonObject errorObject = doc.object().value("error").toObject();
        const QString type = errorObject.value("type").toString();
        const QString message = errorObject.value("message").toString();
        if (!message.isEmpty()) {
            return QString("HTTP %1 - %2: %3").arg(httpStatus).arg(type.isEmpty() ? QStringLiteral("error") : type, message);
        }
    }
    return QString("HTTP %1 - réponse Anthropic non reconnue.").arg(httpStatus);
}

} // namespace

AnthropicTurnResult parseAnthropicResponse(const QByteArray& rawBody, int httpStatus) {
    AnthropicTurnResult result;

    if (httpStatus < 200 || httpStatus >= 300) {
        result.ok = false;
        result.errorMessage = describeHttpError(httpStatus, rawBody);
        return result;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(rawBody, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.ok = false;
        result.errorMessage = QString("Réponse Anthropic illisible (JSON invalide): %1").arg(parseError.errorString());
        return result;
    }

    const QJsonObject root = doc.object();
    if (root.value("type").toString() == "error") {
        result.ok = false;
        result.errorMessage = describeHttpError(httpStatus, rawBody);
        return result;
    }

    result.stopReason = root.value("stop_reason").toString();
    result.rawContentBlocks = root.value("content").toArray();

    for (const auto& blockValue : result.rawContentBlocks) {
        const QJsonObject block = blockValue.toObject();
        const QString type = block.value("type").toString();
        if (type == "text") {
            result.textOutput += block.value("text").toString();
        } else if (type == "tool_use") {
            AnthropicToolUse toolUse;
            toolUse.id = block.value("id").toString();
            toolUse.name = block.value("name").toString();
            toolUse.input = block.value("input").toObject().toVariantMap();
            result.toolUses.append(toolUse);
        }
    }

    result.ok = true;
    return result;
}

} // namespace killai
