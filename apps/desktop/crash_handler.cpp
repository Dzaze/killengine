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

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killengine {

namespace {

QString timestampForFile() {
    return QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz");
}

void writeAndLog(const QString& reason, const QString& detail) {
    const QString path = CrashHandler::writeReport(reason, detail);
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
    writeAndLog("windows_unhandled_exception", detail);
    return EXCEPTION_EXECUTE_HANDLER;
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

QString CrashHandler::writeReport(const QString& reason, const QString& detail) {
    const QString path = QDir(crashDirectory()).filePath("killengine_" + timestampForFile() + ".crash.txt");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return path;
    }

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
    return path;
}

} // namespace killengine
