#include "automation_pipe_manager.h"

#include "automation_pipe_server.h"
#include "logging/logger.h"

#include <QProcessEnvironment>
#include <QSettings>

namespace killengine {

AutomationPipeManager::AutomationPipeManager(ApplicationController* controller)
    : m_controller(controller) {}

AutomationPipeManager::~AutomationPipeManager() = default;

void AutomationPipeManager::ensureStartedIfConfigured() {
    if (m_server) {
        return;
    }
    const bool envEnabled =
        QProcessEnvironment::systemEnvironment().value("KILLENGINE_AUTOMATION_PIPE") == "1";
    QSettings settings;
    const bool persistedEnabled = settings.value("automation/pipeEnabled", false).toBool();
    if (!envEnabled && !persistedEnabled) {
        return;
    }
    // parent=nullptr : l'unique_ptr est le seul proprietaire (pas de parentage
    // Qt en plus), pour que enableAutomationMode()/disableAutomationMode()
    // puissent detruire/recreer l'instance en direct sans double-liberation.
    m_server = std::make_unique<AutomationPipeServer>(m_controller, nullptr);
    if (!m_server->start()) {
        KE_LOG_WARN() << "AutomationPipeServer: demarrage echoue, KillEngine continue sans le connecteur.";
        m_server.reset();
    }
}

QVariantMap AutomationPipeManager::enableAutomationMode() {
    QSettings settings;
    settings.setValue("automation/pipeEnabled", true);
    settings.sync();
    ensureStartedIfConfigured();

    QVariantMap result = getAutomationPipeStatus();
    result["success"] = m_server != nullptr;
    if (!m_server) {
        result["error"] = "Demarrage du pipe d'automatisation echoue (voir les logs KillEngine).";
    }
    return result;
}

QVariantMap AutomationPipeManager::disableAutomationMode() {
    QSettings settings;
    settings.setValue("automation/pipeEnabled", false);
    settings.sync();
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }
    QVariantMap result = getAutomationPipeStatus();
    result["success"] = true;
    return result;
}

QVariantMap AutomationPipeManager::getAutomationPipeStatus() const {
    QSettings settings;
    QVariantMap result;
    result["enabled"] = settings.value("automation/pipeEnabled", false).toBool()
        || QProcessEnvironment::systemEnvironment().value("KILLENGINE_AUTOMATION_PIPE") == "1";
    if (m_server) {
        const QVariantMap pipeStatus = m_server->status();
        for (auto it = pipeStatus.constBegin(); it != pipeStatus.constEnd(); ++it) {
            result[it.key()] = it.value();
        }
    } else {
        result["running"] = false;
        result["pipeName"] = AutomationPipeServer::pipeName();
        result["callCount"] = 0;
        result["lastMethod"] = QString();
        result["lastCallAt"] = QString();
    }
    return result;
}

bool AutomationPipeManager::isRunning() const {
    return m_server != nullptr;
}

} // namespace killengine
