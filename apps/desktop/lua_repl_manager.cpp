#include "lua_repl_manager.h"

#include "lua_runtime_locator.h"
#include "scripting/lua_repl_protocol.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>

#include <algorithm>

namespace killengine {

namespace {

// Pas de \n final ici, volontairement : le driver (killengine_repl_driver.lua)
// écrit SENTINEL suivi d'un "\n", mais sous Windows le CRT du process lua.exe
// traduit ce "\n" en "\r\n" avant qu'il n'atteigne le pipe (mode texte
// standard de stdout) -- un sentinel exigeant un "\n" brut ne matchait donc
// jamais, et chaque ligne timeoutait après 15s malgré une exécution Lua
// réussie (bug live trouvé le 03/09/2026 en testant le REPL via le pipe
// d'automation contre une vraie KillEngine.exe). Le reliquat de saut de
// ligne après le marqueur est avalé explicitement dans workerLoop().
const QByteArray kSentinel = "\x01KE_REPL_END\x01";
const QByteArray kStopCommand = "\x01KE_REPL_STOP\x01";

QString readFileText(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream stream(&file);
    return stream.readAll();
}

} // namespace

LuaReplManager::LuaReplManager(QObject* parent) : QObject(parent) {
}

LuaReplManager::~LuaReplManager() {
    stop();
}

QVariantMap LuaReplManager::start(const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    if (m_running.load()) {
        result["error"] = "Un REPL Lua est déjà en cours — appelle stopLuaRepl() d'abord.";
        return result;
    }

    const QString luaPath = findLuaExecutable(options.value("luaPath").toString());
    if (luaPath.isEmpty()) {
        result["error"] = "Aucun interpréteur Lua trouvé. Place lua.exe dans runtime\\lua à côté de KillEngine.exe, ajoute Lua au PATH, ou renseigne options.luaPath.";
        return result;
    }
    const QString helperPath = findKillEngineLuaHelper();
    if (helperPath.isEmpty()) {
        result["error"] = "scripts/killengine.lua introuvable — le REPL a besoin du module ke.*.";
        return result;
    }
    const QString helperDir = QFileInfo(helperPath).absolutePath();
    const QString driverPath = QDir(helperDir).filePath("killengine_repl_driver.lua");
    if (!QFileInfo::exists(driverPath)) {
        result["error"] = QString("Driver REPL introuvable : %1").arg(driverPath);
        return result;
    }

    m_luaPath = luaPath;
    m_helperPath = helperPath;
    m_driverPath = driverPath;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingLines.clear();
        m_results.clear();
        m_historyOrder.clear();
        m_nextRequestId = 1;
    }
    m_stopRequested = false;

    const QString pipeName = options.value("pipeName", QStringLiteral("KillEngineAutomationPipe")).toString();
    const int lineTimeoutMs = std::clamp(options.value("timeoutMs", 15000).toInt(), 1000, 120000);

    m_worker = std::thread(&LuaReplManager::workerLoop, this, luaPath, driverPath, helperDir, pipeName, lineTimeoutMs);

    // Laisse au process le temps de démarrer avant de répondre "success" —
    // workerLoop met m_running à true dès que QProcess::start() a réussi.
    // Poll bornée (pas de condition_variable dédiée : le cas d'échec précoce
    // — lua.exe introuvable au dernier moment, etc. — reste rare et ce n'est
    // qu'un appel start(), pas une boucle chaude).
    QElapsedTimer startTimer;
    startTimer.start();
    while (!m_running.load() && !startTimer.hasExpired(3000)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!m_running.load()) {
        if (m_worker.joinable()) {
            m_stopRequested = true;
            m_cv.notify_all();
            m_worker.join();
        }
        result["error"] = "Le process Lua REPL n'a pas démarré (voir stderr/logs).";
        return result;
    }

    result["success"] = true;
    result["luaPath"] = luaPath;
    result["helperPath"] = helperPath;
    result["driverPath"] = driverPath;
    result["pipeName"] = pipeName;
    return result;
}

QVariantMap LuaReplManager::sendLine(const QString& line) {
    QVariantMap result;
    if (!m_running.load()) {
        result["success"] = false;
        result["error"] = "REPL non démarré — appelle startLuaRepl d'abord.";
        return result;
    }

    int requestId = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        requestId = m_nextRequestId++;
        m_pendingLines.emplace_back(requestId, line);
    }
    m_cv.notify_one();

    result["success"] = true;
    result["started"] = true;
    result["requestId"] = requestId;
    return result;
}

QVariantMap LuaReplManager::lineResult(int requestId) const {
    QVariantMap result;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_results.constFind(requestId);
    if (it == m_results.constEnd()) {
        result["found"] = false;
        result["finished"] = false;
        return result;
    }
    result = historyEntryToVariant(it.value());
    result["found"] = true;
    return result;
}

QVariantMap LuaReplManager::historyEntryToVariant(const HistoryEntry& entry) const {
    QVariantMap variant;
    variant["requestId"] = entry.requestId;
    variant["line"] = entry.line;
    variant["output"] = entry.output;
    variant["error"] = entry.error;
    variant["finished"] = entry.finished;
    variant["elapsedMs"] = entry.finished ? static_cast<qulonglong>(entry.finishedAtMs - entry.startedAtMs) : 0;
    return variant;
}

QVariantMap LuaReplManager::history(int maxEntries) const {
    QVariantMap result;
    QVariantList entries;
    std::lock_guard<std::mutex> lock(m_mutex);
    const int total = m_historyOrder.size();
    const int clampedMax = maxEntries <= 0 ? total : std::min(maxEntries, total);
    const int startIndex = total - clampedMax;
    for (int i = std::max(0, startIndex); i < total; ++i) {
        const auto it = m_results.constFind(m_historyOrder.at(i));
        if (it != m_results.constEnd()) {
            entries.append(historyEntryToVariant(it.value()));
        }
    }
    result["success"] = true;
    result["entries"] = entries;
    result["totalCount"] = total;
    return result;
}

QVariantMap LuaReplManager::completions(const QString& prefix) const {
    QVariantMap result;
    const QString helperPath = m_helperPath.isEmpty() ? findKillEngineLuaHelper() : m_helperPath;
    if (helperPath.isEmpty()) {
        result["success"] = false;
        result["error"] = "scripts/killengine.lua introuvable.";
        return result;
    }
    const QString source = readFileText(helperPath);
    const auto names = killcore::extractKeCompletions(source, prefix);
    QVariantList list;
    for (const auto& name : names) {
        list.append(name);
    }
    result["success"] = true;
    result["completions"] = list;
    return result;
}

QVariantMap LuaReplManager::stop() {
    QVariantMap result;
    if (!m_worker.joinable()) {
        result["success"] = true;
        result["wasRunning"] = false;
        return result;
    }
    m_stopRequested = true;
    m_cv.notify_all();
    m_worker.join();
    m_running = false;
    result["success"] = true;
    result["wasRunning"] = true;
    return result;
}

QVariantMap LuaReplManager::status() const {
    QVariantMap result;
    result["running"] = m_running.load();
    std::lock_guard<std::mutex> lock(m_mutex);
    result["pendingLines"] = static_cast<int>(m_pendingLines.size());
    result["historyCount"] = m_historyOrder.size();
    result["luaPath"] = m_luaPath;
    result["helperPath"] = m_helperPath;
    return result;
}

void LuaReplManager::workerLoop(QString luaPath, QString driverPath, QString helperDir, QString pipeName, int lineTimeoutMs) {
    QProcess process;
    process.setProgram(luaPath);
    process.setArguments({driverPath});
    process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    process.setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("KILLENGINE_ROOT"), QDir(helperDir).absoluteFilePath(".."));
    env.insert(QStringLiteral("KILLENGINE_AUTOMATION_PIPE_NAME"), pipeName);
    const QString existingLuaPath = env.value(QStringLiteral("LUA_PATH"));
    const QString helperPattern = QDir(helperDir).filePath("?.lua").replace('\\', '/');
    env.insert(QStringLiteral("LUA_PATH"), helperPattern + QStringLiteral(";") + existingLuaPath);
    process.setProcessEnvironment(env);

    process.start();
    if (!process.waitForStarted(3000)) {
        return; // m_running reste false -- start() detecte l'echec via le timeout de poll.
    }
    m_running = true;

    QByteArray stdoutBuffer;

    while (!m_stopRequested.load()) {
        std::pair<int, QString> job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return m_stopRequested.load() || !m_pendingLines.empty(); });
            if (m_stopRequested.load()) {
                break;
            }
            job = m_pendingLines.front();
            m_pendingLines.pop_front();
        }

        const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();

        if (process.state() != QProcess::Running) {
            HistoryEntry entry;
            entry.requestId = job.first;
            entry.line = job.second;
            entry.error = "Process Lua REPL non actif (terminé de façon inattendue).";
            entry.finished = true;
            entry.startedAtMs = startedAt;
            entry.finishedAtMs = startedAt;
            QVariantMap resultVariant;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_results.insert(job.first, entry);
                m_historyOrder.append(job.first);
                resultVariant = historyEntryToVariant(entry);
            }
            QMetaObject::invokeMethod(this, [this, resultVariant]() {
                emit lineFinished(resultVariant);
            }, Qt::QueuedConnection);
            continue;
        }

        process.write(job.second.toUtf8());
        process.write("\n");
        process.waitForBytesWritten(2000);

        QElapsedTimer timer;
        timer.start();
        killcore::ReplExtraction extraction;
        while (!extraction.found && !timer.hasExpired(lineTimeoutMs)) {
            if (process.state() != QProcess::Running) {
                break;
            }
            if (process.waitForReadyRead(200)) {
                stdoutBuffer += process.readAllStandardOutput();
                extraction = killcore::extractReplOutput(stdoutBuffer, kSentinel);
            }
        }

        QString output;
        QString error;
        if (extraction.found) {
            output = extraction.output;
            stdoutBuffer = extraction.remaining;
            // Avale le saut de ligne qui suivait le marqueur (voir kSentinel
            // ci-dessus) pour qu'il ne fuite pas en tête de la sortie de la
            // prochaine commande.
            if (stdoutBuffer.startsWith("\r\n")) {
                stdoutBuffer.remove(0, 2);
            } else if (stdoutBuffer.startsWith('\n')) {
                stdoutBuffer.remove(0, 1);
            }
        } else {
            output = QString::fromUtf8(stdoutBuffer);
            stdoutBuffer.clear();
            error = process.state() != QProcess::Running
                ? "Process Lua REPL terminé de façon inattendue pendant l'exécution de cette ligne."
                : QString("Timeout (%1 ms) en attendant la sortie de cette ligne.").arg(lineTimeoutMs);
        }

        const QByteArray stderrBytes = process.readAllStandardError();
        if (!stderrBytes.isEmpty()) {
            const QString stderrText = QString::fromUtf8(stderrBytes);
            error = error.isEmpty() ? stderrText : error + "\n" + stderrText;
        }

        HistoryEntry entry;
        entry.requestId = job.first;
        entry.line = job.second;
        entry.output = output;
        entry.error = error;
        entry.finished = true;
        entry.startedAtMs = startedAt;
        entry.finishedAtMs = QDateTime::currentMSecsSinceEpoch();

        QVariantMap resultVariant;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_results.insert(job.first, entry);
            m_historyOrder.append(job.first);
            resultVariant = historyEntryToVariant(entry);
        }
        QMetaObject::invokeMethod(this, [this, resultVariant]() {
            emit lineFinished(resultVariant);
        }, Qt::QueuedConnection);
    }

    if (process.state() == QProcess::Running) {
        process.write(kStopCommand);
        process.write("\n");
        process.waitForBytesWritten(500);
        process.closeWriteChannel();
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    m_running = false;
}

} // namespace killengine
