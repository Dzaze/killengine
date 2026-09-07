#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

#include <functional>

namespace killai {

// PHASE (Backend IA externe, T3) : client HTTP pour l'API Messages Anthropic
// + boucle agentique (tool-calling). Alternative optionnelle au modèle local
// embarqué, jamais activée par défaut (voir docs/EXTERNAL_AI_BACKEND_ROADMAP.md).
// Requête non-streamée pour la v1.
//
// Cette classe ne connaît AUCUN outil KillEngine spécifique : le mapping
// "nom d'outil -> exécution réelle (ApplicationController, RiskGate, etc.)"
// est injecté par l'appelant via ToolExecutor (câblé par T4), pour ne pas
// dupliquer/re-dériver la logique de dispatch déjà présente dans
// SmartSearchManager::startSmartSearch.
class ClaudeBackendClient {
public:
    using ToolExecutor = std::function<QVariantMap(const QString& toolName, const QVariantMap& args)>;

    // Effectue le POST HTTP réel (ou un remplacement injecté pour les
    // tests) : renvoie le corps brut de la réponse, remplit `httpStatus`,
    // et positionne `ok`/`errorMessage` en cas d'échec réseau (jamais
    // d'exception, jamais de corps vide silencieux).
    using HttpPostFn = std::function<QByteArray(const QString& apiKey,
                                                 const QJsonObject& requestBody,
                                                 int& httpStatus,
                                                 bool& ok,
                                                 QString& errorMessage)>;

    // `httpPost` vide (défaut) utilise un vrai QNetworkAccessManager. Un
    // remplacement peut être injecté pour les tests unitaires (aucun accès
    // réseau réel dans la suite de tests).
    explicit ClaudeBackendClient(HttpPostFn httpPost = {});

    // Envoie `userMessage` à Claude avec `toolsSchema`, exécute les
    // tool_use retournés via `executor` jusqu'à une réponse texte finale ou
    // `maxToolTurns` itérations (garde-fou anti-boucle infinie). Bloquant
    // (QEventLoop interne côté transport réel). Retourne
    // {success, message, error, stopReason, toolCallsExecuted, requestCount}.
    // Erreur explicite (401, réseau, boucle non terminée) — jamais de
    // fallback silencieux vers le modèle local (décision roadmap #6).
    QVariantMap sendMessage(const QString& apiKey,
                             const QString& userMessage,
                             const QJsonArray& toolsSchema,
                             const ToolExecutor& executor,
                             int maxToolTurns = 8);

    // Compteur de transparence — nombre de requêtes HTTP envoyées à
    // l'API Anthropic depuis la construction de ce client (décision
    // roadmap #5 : pas de limite imposée, juste un compteur affiché).
    int requestCount() const { return m_requestCount; }

private:
    QByteArray defaultHttpPost(const QString& apiKey, const QJsonObject& requestBody, int& httpStatus, bool& ok, QString& errorMessage);

    HttpPostFn m_httpPost;
    int m_requestCount = 0;
};

} // namespace killai
