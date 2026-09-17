#include "application_controller.h"
#include "crash_handler.h"
#include "logging/logger.h"
#include "paths/portable_paths.h"

#include <QApplication>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QMainWindow>
#include <QShortcut>
#include <QUrl>
#include <QIcon>
#include <QDir>
#include <QSettings>

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

    // Portabilite reelle (13/09/2026, docs/PORTABILITY_ROADMAP.md candidat P1) :
    // QSettings() par defaut ecrit dans le registre Windows
    // (HKCU\Software\KillEngine\KillEngine), ce qui casse le mode portable
    // annonce par scripts/package-windows.ps1 (KillEngine-portable.zip) -- un
    // dossier deplace/copie perd silencieusement tous les reglages. Les
    // ~30+ sites QSettings() du projet utilisent tous le constructeur par
    // defaut : ce seul changement les redirige tous vers un fichier INI a
    // cote de l'executable, sans toucher un autre fichier. Doit s'executer
    // AVANT toute construction de QSettings (Logger/CrashHandler/
    // ApplicationController plus bas en dependent tous indirectement).
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                        killcore::PortablePaths::root());

#ifdef _DEBUG
    attachConsole();
#endif

    // Initialize logging
    // Portabilite reelle (13/09/2026, docs/PORTABILITY_ROADMAP.md candidat P3) :
    // Logger::init() sans argument retombe sur AppLocalDataLocation
    // (%LOCALAPPDATA%), meme probleme que P1/P2 -- Logger::init() accepte deja
    // un logDir explicite, il suffisait de le renseigner.
    killcore::Logger::instance().init(killcore::PortablePaths::ensureSubdir("logs"));
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
    //
    // Portabilite reelle (13/09/2026, docs/PORTABILITY_ROADMAP.md candidat P2) :
    // c'est ici, pas dans QSettings (candidat P1, deja traite), que vivent les
    // features Trainer, le workspace (bookmarks/templates/projets) et le
    // journal d'action -- tout ce que le frontend Vue persiste via
    // window.localStorage. AppLocalDataLocation (%LOCALAPPDATA%) cassait le
    // mode portable au meme titre que le registre : deplacer le dossier de
    // l'app perdait silencieusement cette donnee. Redirige vers un dossier
    // relatif a l'executable, comme le reste des donnees portables du projet
    // (modele IA, helper CLR Inspector, KillEngine.ini).
    const QString webEngineStoragePath = killcore::PortablePaths::ensureSubdir("webengine");
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

    // PHASE 119 -- pont pipe d'automatisation -> Vue/Pinia (callVueStoreAction) :
    // le controller a besoin de la page pour y injecter du JS via runJavaScript().
    controller->setWebEnginePage(webEnginePage);

    // Connecteur d'automatisation local (demandé le 19/08/2026 : pilotage
    // temps réel par un agent IA pendant une session de test manuelle,
    // étendu au mode Automation opt-in du 29/08/2026 pour les utilisateurs
    // avancés) — désactivé par défaut, ne démarre que si explicitement
    // demandé (variable d'environnement dev OU toggle Settings persistant).
    // Voir ApplicationController::ensureAutomationPipeStartedIfConfigured()
    // et automation_pipe_server.h pour le protocole et les garde-fous.
    controller->ensureAutomationPipeStartedIfConfigured();

    KE_LOG_INFO() << "Creating QWebChannel...";
    QWebChannel* channel = new QWebChannel(&mainWindow);
    KE_LOG_INFO() << "QWebChannel created.";
    KE_LOG_INFO() << "Registering QWebChannel object...";
    channel->registerObject(QStringLiteral("killengine"), controller);
    KE_LOG_INFO() << "QWebChannel object registered.";
    view->page()->setWebChannel(channel);
    KE_LOG_INFO() << "QWebChannel attached to page.";

    // Load the UI. The Vue bundle (ui/dist, built by scripts/build.ps1) is never
    // compiled into the .qrc -- it ships as loose files, same portable-by-design
    // convention as model/, tools/clr_inspector/ and runtime/lua/ (see
    // docs/PORTABILITY_ROADMAP.md). Two on-disk locations are tried before the
    // resource fallback:
    //   1. Dev checkout: ../../ui/dist/index.html relative to build/bin.
    //   2. Portable package: ui/dist/index.html copied next to the exe by
    //      scripts/package-windows.ps1 (AM-1, docs/PHASE_TRACKER.md, 16/09/2026).
    // Without candidate 2, a ZIP extracted outside the repo silently fell back
    // to the ":/index.html" resource placeholder below -- a real "Interface non
    // construite" screen shipped to every end user, since candidate 1 only ever
    // resolves inside a dev checkout (or, misleadingly, when testing the
    // package from inside dist/ still nested under the repo).
    QUrl url;

    QString devPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../ui/dist/index.html");
    QString packagedPath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("ui/dist/index.html");
    if (QFile::exists(devPath)) {
        KE_LOG_INFO() << "Loading UI from dev path: " << devPath.toStdString();
        url = QUrl::fromLocalFile(devPath);
    } else if (QFile::exists(packagedPath)) {
        KE_LOG_INFO() << "Loading UI from packaged path: " << packagedPath.toStdString();
        url = QUrl::fromLocalFile(packagedPath);
    } else {
        // Try resource path
        QString resPath = ":/index.html";
        if (QFile::exists(resPath)) {
            KE_LOG_WARN() << "UI bundle not found on disk, loading built-in placeholder from resources";
            url = QUrl("qrc:/index.html");
        } else {
            KE_LOG_WARN() << "UI not found! Neither dev, packaged nor resource path exists.";
            KE_LOG_WARN() << "Run 'npm run build' in ui/ directory first, or build the QRC.";
        }
    }

    if (!url.isEmpty()) {
        view->load(url);
    }

    mainWindow.setCentralWidget(view);
    mainWindow.show();

    // F12 — DevTools Chromium (onglet Console : erreurs/warnings JS de l'UI
    // Vue en temps reel, meme mecanisme qu'un navigateur). Fenetre separee,
    // creee une seule fois puis reutilisee/relevee aux appuis suivants.
    auto* devToolsWindow = new QMainWindow(&mainWindow);
    devToolsWindow->setWindowTitle("KillEngine — DevTools");
    devToolsWindow->resize(1000, 700);
    auto* devToolsView = new QWebEngineView(devToolsWindow);
    auto* devToolsPage = new QWebEnginePage(webEngineProfile, devToolsView);
    devToolsView->setPage(devToolsPage);
    devToolsWindow->setCentralWidget(devToolsView);
    webEnginePage->setDevToolsPage(devToolsPage);

    auto* devToolsShortcut = new QShortcut(QKeySequence(Qt::Key_F12), &mainWindow);
    devToolsShortcut->setContext(Qt::ApplicationShortcut);
    QObject::connect(devToolsShortcut, &QShortcut::activated, [devToolsWindow]() {
        devToolsWindow->show();
        devToolsWindow->raise();
        devToolsWindow->activateWindow();
    });

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
