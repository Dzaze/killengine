#include "application_controller.h"
#include "automation_pipe_server.h"
#include "crash_handler.h"
#include "logging/logger.h"

#include <QApplication>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QMainWindow>
#include <QUrl>
#include <QIcon>
#include <QDir>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Enable High-DPI and console output in debug
#ifdef _DEBUG
#include <cstdio>
#include <io.h>
#include <fcntl.h>

static void attachConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        freopen_s(reinterpret_cast<FILE**>(stdout), "CONOUT$", "w", stdout);
        freopen_s(reinterpret_cast<FILE**>(stderr), "CONOUT$", "w", stderr);
        _setmode(_fileno(stdout), _O_U8TEXT);
        _setmode(_fileno(stderr), _O_U8TEXT);
    }
}
#endif

namespace {

int runApplication(int argc, char* argv[]) {
    // High-DPI support (Qt 6 handles this automatically, but be explicit)
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("KillEngine");
    app.setOrganizationName("KillEngine");
    app.setApplicationVersion(KILLENGINE_VERSION);

#ifdef _DEBUG
    attachConsole();
#endif

    // Initialize logging
    killcore::Logger::instance().init();
    killcore::Logger::instance().setLevel(killcore::LogLevel::Debug);
    killengine::CrashHandler::install();

    KE_LOG_INFO() << "========================================";
    KE_LOG_INFO() << "  KillEngine " << KILLENGINE_VERSION << " starting...";
    KE_LOG_INFO() << "========================================";
    KE_LOG_INFO() << "Qt version: " << qVersion();
    KE_LOG_INFO() << "Log file: " << killcore::Logger::instance().logFilePath().toStdString();

    // Application icon
    KE_LOG_INFO() << "Setting application icon...";
    app.setWindowIcon(QIcon(":/resources/icons/killengine_app.png"));
    KE_LOG_INFO() << "Application icon set.";

    // WebEngine settings — enable what we need (set per-profile in Qt 6.8)
    // Settings are applied per-page after the view is created below

    // Create the main window
    KE_LOG_INFO() << "Creating main window...";
    QMainWindow mainWindow;
    mainWindow.setWindowTitle("KillEngine");
    mainWindow.resize(1280, 800);
    mainWindow.setMinimumSize(960, 600);
    KE_LOG_INFO() << "Main window created.";

    // Create WebEngineView
    KE_LOG_INFO() << "Creating QWebEngineView...";
    QWebEngineView* view = new QWebEngineView(&mainWindow);
    KE_LOG_INFO() << "QWebEngineView created.";

    // Profil WebEngine persistant explicite -- QWebEngineProfile::defaultProfile()
    // s'est avere ne PAS survivre a un redemarrage de l'app dans ce build (Trainer,
    // journal d'action, workspace, tout ce qui passe par window.localStorage cote
    // Vue perdait son contenu a chaque relance, meme apres une fermeture propre --
    // verifie en cherchant un dossier "Local Storage"/leveldb sous le repertoire
    // de donnees de l'app : aucun n'existait). Un profil nomme avec un chemin de
    // stockage explicite force Chromium a ecrire une vraie base LevelDB sur disque,
    // au lieu de dependre du profil par defaut dont le chemin peut ne pas etre
    // fige a temps si WebEngine s'initialise avant que setApplicationName/
    // setOrganizationName aient un effet visible pour lui.
    const QString webEngineStoragePath =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("webengine");
    KE_LOG_INFO() << "WebEngine persistent storage path: " << webEngineStoragePath.toStdString();
    auto* webEngineProfile = new QWebEngineProfile(QStringLiteral("KillEngineProfile"), &mainWindow);
    webEngineProfile->setPersistentStoragePath(webEngineStoragePath);
    webEngineProfile->setCachePath(QDir(webEngineStoragePath).filePath("cache"));
    webEngineProfile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    webEngineProfile->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
    KE_LOG_INFO() << "QWebEngineProfile offTheRecord=" << (webEngineProfile->isOffTheRecord() ? "true" : "false");

    auto* webEnginePage = new QWebEnginePage(webEngineProfile, view);
    view->setPage(webEnginePage);

    // Setup QWebChannel
    KE_LOG_INFO() << "Creating ApplicationController...";
    killengine::ApplicationController* controller = new killengine::ApplicationController(&mainWindow);
    KE_LOG_INFO() << "ApplicationController created.";

    // Connecteur d'automatisation local (demandé le 19/08/2026 : pilotage
    // temps réel par un agent IA pendant une session de test manuelle) —
    // désactivé par défaut, n'écoute que si explicitement demandé au lancement.
    // Voir automation_pipe_server.h pour le protocole et les garde-fous.
    if (QProcessEnvironment::systemEnvironment().value("KILLENGINE_AUTOMATION_PIPE") == "1") {
        auto* automationPipe = new killengine::AutomationPipeServer(controller, &mainWindow);
        if (!automationPipe->start()) {
            KE_LOG_WARN() << "AutomationPipeServer: démarrage échoué, KillEngine continue sans le connecteur.";
        }
    }

    KE_LOG_INFO() << "Creating QWebChannel...";
    QWebChannel* channel = new QWebChannel(&mainWindow);
    KE_LOG_INFO() << "QWebChannel created.";
    KE_LOG_INFO() << "Registering QWebChannel object...";
    channel->registerObject(QStringLiteral("killengine"), controller);
    KE_LOG_INFO() << "QWebChannel object registered.";
    view->page()->setWebChannel(channel);
    KE_LOG_INFO() << "QWebChannel attached to page.";

    // Load the UI
    // In development: load from ui/dist/index.html
    // In production: load from qrc:/index.html
    QUrl url;

    QString devPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../ui/dist/index.html");
    if (QDir::isAbsolutePath(devPath) && QFile::exists(devPath)) {
        KE_LOG_INFO() << "Loading UI from dev path: " << devPath.toStdString();
        url = QUrl::fromLocalFile(devPath);
    } else {
        // Try resource path
        QString resPath = ":/index.html";
        if (QFile::exists(resPath)) {
            KE_LOG_INFO() << "Loading UI from resources";
            url = QUrl("qrc:/index.html");
        } else {
            KE_LOG_WARN() << "UI not found! Neither dev nor resource path exists.";
            KE_LOG_WARN() << "Run 'npm run build' in ui/ directory first, or build the QRC.";
        }
    }

    if (!url.isEmpty()) {
        view->load(url);
    }

    mainWindow.setCentralWidget(view);
    mainWindow.show();

    KE_LOG_INFO() << "KillEngine UI loaded. Entering event loop...";

    int result = app.exec();

    KE_LOG_INFO() << "KillEngine shutting down (exit code " << result << ")";
    return result;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return runApplication(argc, argv);
    } catch (const std::exception& e) {
        const QString detail = QString::fromUtf8(e.what());
        killengine::CrashHandler::writeReport("main_exception", detail);
        KE_LOG_FATAL() << "Unhandled exception in main: " << detail.toStdString();
        return 1;
    } catch (...) {
        killengine::CrashHandler::writeReport("main_exception", "unknown exception");
        KE_LOG_FATAL() << "Unknown unhandled exception in main.";
        return 1;
    }
}
