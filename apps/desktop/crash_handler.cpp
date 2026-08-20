#include "crash_handler.h"

#include "logging/logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include <csignal>
#include <exception>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <DbgHelp.h>
#endif

namespace killengine {

namespace {

QString timestampForFile() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
}

void writeAndLog(const QString& reason, const QString& detail, void* exceptionPointers = nullptr) {
    const QString path = CrashHandler::writeReport(reason, detail, exceptionPointers);
    KE_LOG_FATAL() << "Crash report written: " << path.toStdString()
                   << " reason=" << reason.toStdString()
                   << " detail=" << detail.toStdString();
}

void terminateHandler() {
    QString detail = "std::terminate called";
    if (auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& e) {
            detail = QString::fromUtf8(e.what());
        } catch (...) {
            detail = "unknown non-std exception";
        }
    }
    writeAndLog("terminate", detail);
    std::abort();
}

void signalHandler(int signalNumber) {
    writeAndLog("signal", QString::number(signalNumber));
    std::_Exit(128 + signalNumber);
}

#ifdef Q_OS_WIN
LONG WINAPI windowsUnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    QString detail = "unknown SEH exception";
    if (exceptionInfo && exceptionInfo->ExceptionRecord) {
        detail = QString("code=0x%1 address=0x%2")
            .arg(QString::number(exceptionInfo->ExceptionRecord->ExceptionCode, 16))
            .arg(QString::number(reinterpret_cast<quintptr>(exceptionInfo->ExceptionRecord->ExceptionAddress), 16));
    }
    // Seul chemin qui a le vrai EXCEPTION_POINTERS du crash (contexte CPU +
    // enregistrement d'exception) : le transmettre donne un minidump exact,
    // contrairement aux autres handlers ci-dessus qui n'ont que l'etat au
    // moment de l'appel du handler (deja utile, mais pas le point de faute).
    writeAndLog("windows_unhandled_exception", detail, exceptionInfo);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Ecrit un minidump Windows a `path`. `exceptionPointers` (nullable) vient
// directement d'un handler SEH pour un dump exact du point de crash ; sans
// lui, MiniDumpWriteDump capture quand meme l'etat courant (piles de tous
// les threads, modules charges) au moment de l'appel — toujours plus
// exploitable dans WinDbg/Visual Studio qu'un simple rapport texte.
bool writeMinidumpFile(const QString& path, EXCEPTION_POINTERS* exceptionPointers) {
    const std::wstring widePath = path.toStdWString();
    const HANDLE hFile = CreateFileW(
        widePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionParam;
    MINIDUMP_EXCEPTION_INFORMATION* exceptionParamPtr = nullptr;
    if (exceptionPointers) {
        exceptionParam.ThreadId = GetCurrentThreadId();
        exceptionParam.ExceptionPointers = exceptionPointers;
        exceptionParam.ClientPointers = FALSE;
        exceptionParamPtr = &exceptionParam;
    }

    // Segments de donnees des modules (globales), infos threads, modules
    // dechages : dump raisonnable pour une analyse post-mortem sans
    // embarquer toute la memoire du processus (MiniDumpWithFullMemory serait
    // inutilement lourd pour un simple diagnostic de crash applicatif).
    const auto dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithDataSegs | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo);

    const BOOL ok = MiniDumpWriteDump(
        GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, exceptionParamPtr, nullptr, nullptr);

    CloseHandle(hFile);
    return ok == TRUE;
}
#endif

} // namespace

void CrashHandler::install() {
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGFPE, signalHandler);
    std::signal(SIGILL, signalHandler);
    std::signal(SIGINT, signalHandler);
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGTERM, signalHandler);

#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(windowsUnhandledExceptionHandler);
#endif
}

QString CrashHandler::crashDirectory() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty()) {
        dir = QDir::currentPath();
    }
    dir += "/crashes";
    QDir().mkpath(dir);
    return dir;
}

QString CrashHandler::writeReport(const QString& reason, const QString& detail, void* exceptionPointers) {
    // Meme horodatage pour le .txt et le .dmp : les deux fichiers se
    // reconnaissent comme une paire d'un seul coup d'oeil dans le dossier crashes/.
    const QString stamp = timestampForFile();
    const QString path = QDir(crashDirectory()).filePath("killengine_" + stamp + ".crash.txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return path;
    }

#ifdef Q_OS_WIN
    const QString dumpPath = QDir(crashDirectory()).filePath("killengine_" + stamp + ".dmp");
    const bool dumpWritten = writeMinidumpFile(dumpPath, reinterpret_cast<EXCEPTION_POINTERS*>(exceptionPointers));
#else
    (void)exceptionPointers;
#endif

    QTextStream out(&file);
    out << "KillEngine Crash Report\n";
    out << "createdAt=" << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "\n";
    out << "version=" << QCoreApplication::applicationVersion() << "\n";
    out << "reason=" << reason << "\n";
    out << "detail=" << detail << "\n";
    out << "os=" << QSysInfo::prettyProductName() << "\n";
    out << "cpu=" << QSysInfo::currentCpuArchitecture() << "\n";
    out << "kernel=" << QSysInfo::kernelType() << " " << QSysInfo::kernelVersion() << "\n";
    out << "logFile=" << killcore::Logger::instance().logFilePath() << "\n";
#ifdef Q_OS_WIN
    out << "minidump=" << (dumpWritten ? dumpPath : QStringLiteral("<échec>")) << "\n";
#endif
    return path;
}

} // namespace killengine
