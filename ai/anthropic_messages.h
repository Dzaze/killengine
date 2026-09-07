#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantMap>

namespace killai {

// PHASE (Backend IA externe, T3) : briques pures (aucun accès réseau) de
// construction/parsing du protocole Messages API Anthropic — séparées de
// ClaudeBackendClient (qui fait le vrai POST HTTP) pour rester testables
// unitairement sans réseau. Voir docs/EXTERNAL_AI_BACKEND_ROADMAP.md.

// Corps JSON d'une requête POST /v1/messages : {model, max_tokens, system
// (omis si vide), tools (omis si vide), messages}. `messages` est
// l'historique complet de la conversation (rôles user/assistant), construit
// et maintenu par l'appelant au fil de la boucle agentique. `systemPrompt`
// est un champ top-level du protocole Anthropic (pas un message) — son
// contenu (méthodologie KillEngine) est entièrement à la charge de
// l'appelant, cette fonction reste agnostique de son contenu.
QJsonObject buildAnthropicRequestBody(const QJsonArray& toolsSchema,
                                       const QJsonArray& messages,
                                       const QString& model = QStringLiteral("claude-sonnet-4-5"),
                                       int maxTokens = 4096,
                                       const QString& systemPrompt = QString());

// Message utilisateur simple {role: "user", content: text}.
QJsonObject makeUserMessage(const QString& text);

// Message assistant re-sérialisé tel que reçu de l'API (les blocs text +
// tool_use bruts de la réponse) — à rejouer tel quel dans l'historique avant
// d'ajouter le tool_result correspondant (exigé par le protocole Anthropic).
QJsonObject makeAssistantMessage(const QJsonArray& rawContentBlocks);

// Message "user" contenant un ou plusieurs blocs tool_result, à ajouter à
// l'historique juste après le makeAssistantMessage qui contenait les
// tool_use correspondants.
QJsonObject makeToolResultMessage(const QJsonArray& toolResultBlocks);

// Bloc tool_result individuel (à assembler dans makeToolResultMessage).
// `resultData` est sérialisé en JSON compact comme contenu texte du bloc.
QJsonObject makeToolResultBlock(const QString& toolUseId, const QVariantMap& resultData, bool isError);

struct AnthropicToolUse {
    QString id;
    QString name;
    QVariantMap input;
};

struct AnthropicTurnResult {
    bool ok = false;               // requête + parsing réussis
    QString errorMessage;          // rempli si ok == false — jamais de fallback silencieux
    QString stopReason;            // "end_turn", "tool_use", "max_tokens", ...
    QString textOutput;            // concaténation des blocs text de la réponse
    QJsonArray rawContentBlocks;   // blocs bruts de la réponse (à rejouer via makeAssistantMessage)
    QList<AnthropicToolUse> toolUses; // blocs tool_use extraits pour exécution
};

// Parse le corps JSON d'une réponse HTTP de /v1/messages, succès ou erreur.
// `httpStatus` permet de distinguer un 401 (clé invalide) ou tout autre code
// d'erreur HTTP d'un corps d'erreur applicatif Anthropic
// ({"type":"error","error":{...}}). `ok` reste false et `errorMessage` est
// toujours rempli en cas d'échec, quelle qu'en soit la cause.
AnthropicTurnResult parseAnthropicResponse(const QByteArray& rawBody, int httpStatus);

} // namespace killai
