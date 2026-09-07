#pragma once

#include <QHash>
#include <QString>
#include <QVariantMap>

#include "claude_backend_client.h"

namespace killengine {

class ApplicationController;

/// Backend IA externe (T4, docs/EXTERNAL_AI_BACKEND_ROADMAP.md) : câble
/// killai::ClaudeBackendClient (T3) à la vraie surface ApplicationController.
///
/// Possède aussi le pont "action en attente" générique qui permet à la
/// boucle agentique (bloquante, même thread — voir sendMessage) de mettre en
/// pause son exécution pour :
///  - une confirmation RiskGate réelle côté frontend (kind
///    "confirm_and_execute_in_cpp" — la vraie écriture/action a lieu en C++
///    APRÈS approbation, pas côté frontend) ;
///  - une action Trainer qui n'existe aujourd'hui que côté Pinia
///    (kind "trainer_*", aucun Q_INVOKABLE équivalent).
///
/// Décision d'écart notée dans docs/EXTERNAL_AI_BACKEND_ROADMAP.md (T3) :
/// aucune réutilisation du dispatch existant de SmartSearchManager::startSmartSearch
/// (chaîne non factorisée, ~860 lignes) — executeTool() ci-dessous est un
/// dispatcher indépendant, écrit contre le même registre d'outils
/// (ai/tool_registry.cpp) que celui exposé à Claude.
class ClaudeChatManager {
public:
    explicit ClaudeChatManager(ApplicationController* controller);

    /// Envoie `userMessage` au backend Claude actif (nécessite une clé API
    /// enregistrée). Bloquant (voir killai::ClaudeBackendClient::sendMessage).
    QVariantMap sendMessage(const QString& userMessage);

    QVariantMap setApiKey(const QString& apiKey);
    QVariantMap clearApiKey();
    bool hasApiKey() const;

    QVariantMap setActiveBackend(const QString& backend);
    QString activeBackend() const;

    int requestCount() const;

    /// Appelé par ApplicationController::resolveClaudePendingAction
    /// (Q_INVOKABLE), lui-même appelé par le frontend après une confirmation
    /// RiskGate ou l'exécution d'une action Trainer côté Pinia.
    void resolvePendingAction(const QString& pendingId, const QVariantMap& result);

private:
    QVariantMap executeTool(const QString& toolName, const QVariantMap& args);

    /// Émet ApplicationController::claudePendingActionRequested et pompe
    /// QCoreApplication::processEvents() (même patron que l'attente du modèle
    /// local, ai/llama_server.cpp) jusqu'à ce que resolvePendingAction() soit
    /// appelée pour ce pendingId, ou jusqu'au timeout.
    QVariantMap waitForFrontendAction(const QString& kind, QVariantMap payload, int timeoutMs = 120000);

    /// Attend un signal de complétion asynchrone existant sur
    /// ApplicationController (ex: processNetworkBlockFinished) via une
    /// QEventLoop bornée par timeout — même patron que
    /// core/webview2/cdp_client.cpp::sendCommandSync.
    QVariantMap waitForControllerSignal(void (ApplicationController::*signal)(const QVariantMap&), int timeoutMs = 20000);

    /// Demande une confirmation RiskGate réelle au frontend pour `toolName`
    /// (bloquant, via waitForFrontendAction). Retourne {"approved": bool}.
    QVariantMap requestConfirmation(const QString& toolName, const QVariantMap& args, const QString& description);

    QString decryptedApiKey(bool* ok, QString* errorMessage) const;

    struct PendingEntry {
        bool resolved = false;
        QVariantMap result;
    };

    ApplicationController* m_controller;
    killai::ClaudeBackendClient m_client;
    QHash<QString, PendingEntry> m_pending;
    int m_nextPendingId = 1;
};

} // namespace killengine
