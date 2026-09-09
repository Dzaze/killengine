#include "claude_backend_client.h"

#include "anthropic_messages.h"
#include "localization/localization.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace killai {

namespace {
constexpr const char* kAnthropicMessagesUrl = "https://api.anthropic.com/v1/messages";
constexpr const char* kAnthropicVersion = "2023-06-01";
} // namespace

ClaudeBackendClient::ClaudeBackendClient(HttpPostFn httpPost)
    : m_httpPost(std::move(httpPost)) {
    if (!m_httpPost) {
        m_httpPost = [this](const QString& apiKey, const QJsonObject& requestBody, int& httpStatus, bool& ok, QString& errorMessage) {
            return defaultHttpPost(apiKey, requestBody, httpStatus, ok, errorMessage);
        };
    }
}

QByteArray ClaudeBackendClient::defaultHttpPost(const QString& apiKey, const QJsonObject& requestBody, int& httpStatus, bool& ok, QString& errorMessage) {
    QNetworkAccessManager manager;

    QNetworkRequest request{QUrl(QString::fromLatin1(kAnthropicMessagesUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("x-api-key", apiKey.toUtf8());
    request.setRawHeader("anthropic-version", kAnthropicVersion);

    const QByteArray body = QJsonDocument(requestBody).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = manager.post(request, body);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(60000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        reply->deleteLater();
        ok = false;
        httpStatus = 0;
        errorMessage = KE_TXT("Délai dépassé en attendant l'API Anthropic (60s).",
                              "Timed out waiting for the Anthropic API (60s).");
        return {};
    }
    timer.stop();

    httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const bool networkFailed = reply->error() != QNetworkReply::NoError && httpStatus == 0;
    if (networkFailed) {
        ok = false;
        errorMessage = reply->errorString();
    } else {
        ok = true;
    }
    reply->deleteLater();
    return responseBody;
}

namespace {
// Garde-fou anti-croissance illimitée : reset explicite (message dédié
// renvoyé à l'appelant) plutôt qu'une troncature silencieuse au milieu d'un
// echange tool_use/tool_result, qui casserait le format attendu par l'API.
constexpr int kMaxHistoryMessages = 200;
} // namespace

QVariantMap ClaudeBackendClient::sendMessage(const QString& apiKey,
                                              const QString& userMessage,
                                              const QJsonArray& toolsSchema,
                                              const ToolExecutor& executor,
                                              int maxToolTurns,
                                              const QString& systemPrompt) {
    QVariantMap result;

    if (apiKey.trimmed().isEmpty()) {
        result["success"] = false;
        result["error"] = KE_TXT("Clé API Claude manquante.", "Claude API key is missing.");
        return result;
    }

    bool conversationWasReset = false;
    if (m_conversationHistory.size() >= kMaxHistoryMessages) {
        resetConversation();
        conversationWasReset = true;
    }

    // L'historique complet (tous les tours precedents) est conserve comme
    // etat membre entre deux appels a sendMessage() -- sans ca, chaque
    // nouveau message utilisateur reparaitrait de zero pour Claude (bug
    // constate en test terrain le 07/09/2026, voir
    // docs/EXTERNAL_AI_BACKEND_ROADMAP.md T6).
    m_conversationHistory.append(makeUserMessage(userMessage));

    int toolCallsExecuted = 0;
    bool receivedAnyResponse = false;

    for (int turn = 0; turn < maxToolTurns; ++turn) {
        const QJsonObject requestBody = buildAnthropicRequestBody(toolsSchema, m_conversationHistory, QStringLiteral("claude-sonnet-4-5"), 4096, systemPrompt);

        int httpStatus = 0;
        bool networkOk = false;
        QString networkError;
        const QByteArray rawBody = m_httpPost(apiKey, requestBody, httpStatus, networkOk, networkError);
        ++m_requestCount;

        if (!networkOk) {
            if (!receivedAnyResponse) {
                // Le message utilisateur n'a jamais eu de reponse -- le
                // retirer pour eviter deux messages "user" consecutifs au
                // prochain appel (l'API Anthropic attend une alternance).
                m_conversationHistory.removeLast();
            }
            result["success"] = false;
            result["error"] = KE_TXT("Erreur réseau: %1", "Network error: %1").arg(networkError);
            result["toolCallsExecuted"] = toolCallsExecuted;
            result["requestCount"] = m_requestCount;
            result["conversationReset"] = conversationWasReset;
            return result;
        }

        const AnthropicTurnResult parsed = parseAnthropicResponse(rawBody, httpStatus);
        if (!parsed.ok) {
            if (!receivedAnyResponse) {
                m_conversationHistory.removeLast();
            }
            result["success"] = false;
            result["error"] = parsed.errorMessage;
            result["toolCallsExecuted"] = toolCallsExecuted;
            result["requestCount"] = m_requestCount;
            result["conversationReset"] = conversationWasReset;
            return result;
        }
        receivedAnyResponse = true;

        if (parsed.toolUses.isEmpty()) {
            m_conversationHistory.append(makeAssistantMessage(parsed.rawContentBlocks));
            result["success"] = true;
            result["message"] = parsed.textOutput;
            result["stopReason"] = parsed.stopReason;
            result["toolCallsExecuted"] = toolCallsExecuted;
            result["requestCount"] = m_requestCount;
            result["conversationReset"] = conversationWasReset;
            return result;
        }

        m_conversationHistory.append(makeAssistantMessage(parsed.rawContentBlocks));

        QJsonArray toolResultBlocks;
        for (const auto& toolUse : parsed.toolUses) {
            QVariantMap toolResult;
            if (executor) {
                toolResult = executor(toolUse.name, toolUse.input);
            } else {
                toolResult["success"] = false;
                toolResult["error"] = KE_TXT("Aucun exécuteur d'outil configuré côté KillEngine.",
                                             "No KillEngine tool executor is configured.");
            }
            ++toolCallsExecuted;
            const bool isError = !toolResult.value("success", true).toBool();
            toolResultBlocks.append(makeToolResultBlock(toolUse.id, toolResult, isError));
        }
        m_conversationHistory.append(makeToolResultMessage(toolResultBlocks));
    }

    result["success"] = false;
    result["error"] = KE_TXT("Boucle d'appels d'outils non terminée après %1 tours.",
                             "Tool-call loop did not finish after %1 turns.").arg(maxToolTurns);
    result["toolCallsExecuted"] = toolCallsExecuted;
    result["requestCount"] = m_requestCount;
    result["conversationReset"] = conversationWasReset;
    return result;
}

void ClaudeBackendClient::resetConversation() {
    m_conversationHistory = QJsonArray();
}

} // namespace killai
