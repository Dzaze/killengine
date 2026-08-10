#include "logger.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

namespace killcore {

// ---------------------------------------------------------------------------
// Logger (singleton)
// ---------------------------------------------------------------------------
Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?    ";
}

void Logger::init(const QString& logDir) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_file.is_open()) {
        m_file.close();
    }
    m_initialized = false;

    QString dir = logDir;
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dir.isEmpty()) {
            dir = QDir::currentPath();
        }
        dir += "/logs";
    }

    QDir().mkpath(dir);

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    m_logFilePath = dir + "/killengine_" + timestamp + ".log";

    m_file.open(m_logFilePath.toStdString(), std::ios::out | std::ios::app);
    m_initialized = m_file.is_open();

    if (m_initialized) {
        m_file << "=== KillEngine Log Started ===" << std::endl;
    }
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minLevel = level;
}

LogLevel Logger::level() const {
    return m_minLevel;
}

void Logger::log(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < static_cast<int>(m_minLevel)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    std::string line = QString("[%1] [%2] %3")
                           .arg(timestamp)
                           .arg(QString::fromLatin1(levelToString(level)))
                           .arg(QString::fromUtf8(message.data(), static_cast<int>(message.size())))
                           .toStdString();

    // Console output (debug only)
#ifdef _DEBUG
    qDebug() << QString::fromStdString(line);
#endif

    // File output
    if (m_file.is_open()) {
        m_file << line << std::endl;
        m_file.flush();
    }
}

QString Logger::logFilePath() const {
    return m_logFilePath;
}

// ---------------------------------------------------------------------------
// LogStream
// ---------------------------------------------------------------------------
LogStream::LogStream(LogLevel level)
    : m_level(level) {
}

LogStream::~LogStream() {
    Logger::instance().log(m_level, m_buffer.str());
}

} // namespace killcore
