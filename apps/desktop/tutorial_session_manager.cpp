#include "tutorial_session_manager.h"

#include "localization/localization.h"
#include "logging/logger.h"
#include "paths/portable_paths.h"
#include "process/job_object.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace killengine {

namespace {
constexpr int kGracefulTerminateWaitMs = 3000;
constexpr int kForcedKillWaitMs = 3000;
} // namespace

TutorialSessionManager::TutorialSessionManager(QObject* parent) : QObject(parent) {
}

TutorialSessionManager::~TutorialSessionManager() {
    if (isActive()) {
        close();
    }
}

bool TutorialSessionManager::isActive() const {
    return m_childProcess && m_childProcess->state() != QProcess::NotRunning;
}

QVariantMap TutorialSessionManager::start() {
    if (isActive()) {
        // Session déjà active : pas de relance, l'appelant (futur 15C) est
        // responsable de focaliser la fenêtre existante à partir de ce statut.
        QVariantMap result = status();
        result[QStringLiteral("success")] = true;
        result[QStringLiteral("alreadyActive")] = true;
        return result;
    }

    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_sessionRoot = killcore::PortablePaths::ensureSubdir(
        QStringLiteral("data/tutorial-sessions/%1").arg(m_sessionId));

    if (m_sessionRoot.isEmpty()) {
        QVariantMap result;
        result[QStringLiteral("success")] = false;
        result[QStringLiteral("error")] = KE_TXT(
            "Impossible de créer le dossier de session tutoriel.",
            "Could not create the tutorial session folder.");
        return result;
    }

    const QString selfExecutable = QCoreApplication::applicationFilePath();
    auto childProcess = std::make_unique<QProcess>();
    childProcess->setProgram(selfExecutable);
    childProcess->setArguments({QStringLiteral("--tutorial-session-root=%1").arg(m_sessionRoot)});
    childProcess->start();

    if (!childProcess->waitForStarted(5000)) {
        KE_LOG_ERROR() << "TutorialSessionManager::start: waitForStarted failed:"
                        << childProcess->errorString().toStdString();
        QVariantMap result;
        result[QStringLiteral("success")] = false;
        result[QStringLiteral("error")] = KE_TXT(
            "Impossible de démarrer l'instance tutoriel.",
            "Could not start the tutorial instance.");
        m_sessionId.clear();
        m_sessionRoot.clear();
        return result;
    }

    auto jobObject = std::make_unique<killcore::JobObject>();
    bool assigned = false;
    if (jobObject->isValid()) {
        assigned = jobObject->assignProcess(childProcess->processId());
    }
    if (!assigned) {
        KE_LOG_ERROR() << "TutorialSessionManager::start: Job Object assignment failed for pid"
                        << childProcess->processId()
                        << "-- la session enfant ne sera pas garantie de mourir si cette instance"
                        << "crashe, mais reste fonctionnelle.";
    }

    m_childProcess = std::move(childProcess);
    m_jobObject = std::move(jobObject);

    QVariantMap result = status();
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("alreadyActive")] = false;
    result[QStringLiteral("jobObjectAssigned")] = assigned;
    return result;
}

QVariantMap TutorialSessionManager::status() const {
    QVariantMap result;
    const bool active = isActive();
    result[QStringLiteral("active")] = active;
    if (active) {
        result[QStringLiteral("sessionId")] = m_sessionId;
        result[QStringLiteral("sessionRoot")] = m_sessionRoot;
        result[QStringLiteral("pid")] = static_cast<qint64>(m_childProcess->processId());
    }
    return result;
}

QVariantMap TutorialSessionManager::close() {
    if (!isActive()) {
        QVariantMap result;
        result[QStringLiteral("success")] = true;
        result[QStringLiteral("wasActive")] = false;
        return result;
    }

    m_childProcess->terminate();
    if (!m_childProcess->waitForFinished(kGracefulTerminateWaitMs)) {
        m_childProcess->kill();
        m_childProcess->waitForFinished(kForcedKillWaitMs);
    }

    // Libère le Job Object explicitement avant de continuer : plus aucun
    // process ne doit en dépendre à ce stade (l'enfant vient d'être arrêté).
    m_jobObject.reset();
    m_childProcess.reset();

    QVariantMap result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("wasActive")] = true;

    // Suppression du dossier de session -- uniquement si son chemin absolu
    // est bien sous data/tutorial-sessions/, jamais une suppression aveugle
    // d'un chemin qui pourrait avoir été altéré entre-temps.
    const QString sessionsRoot = killcore::PortablePaths::ensureSubdir(
        QStringLiteral("data/tutorial-sessions"));
    const QString absoluteSession = QFileInfo(m_sessionRoot).absoluteFilePath();
    const QString absoluteSessionsRoot = QFileInfo(sessionsRoot).absoluteFilePath();
    if (!m_sessionRoot.isEmpty() &&
        absoluteSession.startsWith(absoluteSessionsRoot + QStringLiteral("/"))) {
        QDir sessionDir(absoluteSession);
        if (sessionDir.exists() && !sessionDir.removeRecursively()) {
            KE_LOG_ERROR() << "TutorialSessionManager::close: échec de suppression de"
                            << absoluteSession.toStdString();
        }
    }

    m_sessionId.clear();
    m_sessionRoot.clear();
    return result;
}

} // namespace killengine
