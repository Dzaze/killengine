#pragma once

#include <QVariantMap>

#include <memory>

namespace killengine {

class ApplicationController;
class AutomationPipeServer;

/// Cycle de vie du pipe d'automatisation (candidat C11 de
/// docs/REFACTOR_ROADMAP.md, extrait le 29/08/2026) : démarrage conditionnel
/// (env var dev OU toggle Settings persistant), activation/désactivation
/// depuis l'UI, statut. Le protocole JSON-RPC lui-même (AutomationPipeServer)
/// n'est pas touché — seul son propriétaire/cycle de vie change de classe.
class AutomationPipeManager {
public:
    explicit AutomationPipeManager(ApplicationController* controller);
    ~AutomationPipeManager();

    /// Démarre AutomationPipeServer si KILLENGINE_AUTOMATION_PIPE=1 (chemin dev
    /// existant) OU si QSettings "automation/pipeEnabled" est vrai (mode
    /// Automation persistant). Appelée une fois au démarrage depuis main.cpp
    /// ET depuis enableAutomationMode() pour le cas où le toggle est activé en
    /// cours de session.
    void ensureStartedIfConfigured();

    QVariantMap enableAutomationMode();
    QVariantMap disableAutomationMode();
    QVariantMap getAutomationPipeStatus() const;
    bool isRunning() const;

private:
    ApplicationController* m_controller;
    std::unique_ptr<AutomationPipeServer> m_server;
};

} // namespace killengine
