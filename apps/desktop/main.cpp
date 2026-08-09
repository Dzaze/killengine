#include "application_controller.h"
#include "logging/logger.h"

#include <QApplication>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QMainWindow>
#include <QUrl>
#include <QIcon>
#include <QDir>
#include <QStandardPaths>

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

int main(int argc, char* argv[]) {
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

    // Setup QWebChannel
    KE_LOG_INFO() << "Creating ApplicationController...";
    killengine::ApplicationController* controller = new killengine::ApplicationController(&mainWindow);
    KE_LOG_INFO() << "ApplicationController created.";

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
