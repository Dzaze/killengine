#pragma once

#include <QString>
#include <QtDebug>

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace killcore {

/**
 * @brief Niveaux de log de KillEngine.
 *
 * Le logging est centralisé et thread-safe.
 * Les logs sont écrits à la fois dans la console (debug) et dans un fichier.
 */
enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5
};

/**
 * @brief Logger global de KillEngine.
 *
 * Utilisation:
 *   KE_LOG(LogLevel::Info) << "Message " << 42;
 *
 * Ou les macros raccourcies:
 *   KE_LOG_INFO() << "message";
 *   KE_LOG_WARN() << "attention";
 */
class Logger {
public:
    static Logger& instance();

    /// Initialise le logger avec un dossier de sortie.
    void init(const QString& logDir = QString());

    /// Définit le niveau minimum de log.
    void setLevel(LogLevel level);

    /// Retourne le niveau de log actuel.
    LogLevel level() const;

    /// Écrit un message.
    void log(LogLevel level, std::string_view message);

    /// Retourne le chemin du fichier de log courant.
    QString logFilePath() const;

private:
    Logger() = default;

    std::mutex        m_mutex;
    LogLevel          m_minLevel{LogLevel::Info};
    std::ofstream     m_file;
    QString           m_logFilePath;
    bool              m_initialized{false};

    static const char* levelToString(LogLevel level);
};

// ---------------------------------------------------------------------------
// Stream de log temporaire (RAII)
// ---------------------------------------------------------------------------
class LogStream {
public:
    LogStream(LogLevel level);
    ~LogStream();

    template <typename T>
    LogStream& operator<<(const T& value) {
        m_buffer << value;
        return *this;
    }

private:
    LogLevel    m_level;
    std::ostringstream m_buffer;
};

// ---------------------------------------------------------------------------
// Macros pratiques
// ---------------------------------------------------------------------------
#define KE_LOG(level) ::killcore::LogStream(level)
#define KE_LOG_TRACE() KE_LOG(::killcore::LogLevel::Trace)
#define KE_LOG_DEBUG() KE_LOG(::killcore::LogLevel::Debug)
#define KE_LOG_INFO()  KE_LOG(::killcore::LogLevel::Info)
#define KE_LOG_WARN()  KE_LOG(::killcore::LogLevel::Warn)
#define KE_LOG_ERROR() KE_LOG(::killcore::LogLevel::Error)
#define KE_LOG_FATAL() KE_LOG(::killcore::LogLevel::Fatal)

} // namespace killcore