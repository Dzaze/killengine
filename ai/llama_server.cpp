// Serviteur llama.cpp persistant: charge le modele une fois, sert les
// completions en HTTP local via QProcess + QTcpSocket.

#include "llama_server.h"

#include "logging/logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpSocket>
#include <QThread>

namespace killai {

namespace {

constexpr int kDefaultPort = 8827;
constexpr int kDefaultCompletionTimeoutMs = 15000;
constexpr int kDefaultStartupTimeoutMs = 90000;

bool serverDisabledByEnv() {
    return qEnvironmentVariable("KILLENGINE_DISABLE_LLAMA_SERVER") == "1";
}

bool serverDisabledBySettings() {
    return !QSettings().value("ai/useServer", true).toBool();
}

int completionTimeoutMs() {
    bool ok = false;
    const int value = qEnvironmentVariable("KILLENGINE_LLAMA_SERVER_TIMEOUT_MS").toInt(&ok);
    return ok && value > 0 ? value : kDefaultCompletionTimeoutMs;
}

int startupTimeoutMs() {
    bool ok = false;
    const int value = qEnvironmentVariable("KILLENGINE_LLAMA_SERVER_STARTUP_MS").toInt(&ok);
    return ok && value > 0 ? value : kDefaultStartupTimeoutMs;
}

/// POST /completion manuel sur 127.0.0.1:port, borne dans le temps.
/// Le serveur ferme la connexion (Connection: close) une fois la reponse envoyee.
bool httpPostJson(int port, const QByteArray& body, int timeoutMs, QByteArray* response, QString* error) {
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    if (!socket.waitForConnected(timeoutMs)) {
        if (error) *error = QString("llama-server connect timeout (port %1): %2").arg(port).arg(socket.errorString());
        return false;
    }

    QByteArray request;
    request += "POST /completion HTTP/1.1\r\n";
    request += QString("Host: 127.0.0.1:%1\r\n").arg(port).toUtf8();
    request += "Content-Type: application/json\r\n";
    request += QString("Content-Length: %1\r\n").arg(body.size()).toUtf8();
    request += "Connection: close\r\n\r\n";
    request += body;

    socket.write(request);
    if (!socket.waitForBytesWritten(timeoutMs)) {
        if (error) *error = "llama-server write timeout.";
        return false;
    }

    // Lecture jusqu'a fermeture de la connexion ou deadline globale.
    QByteArray all;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (true) {
        if (socket.state() == QAbstractSocket::UnconnectedState) break;
        if (socket.bytesAvailable() > 0) {
            all += socket.readAll();
            continue;
        }
        const int remaining = static_cast<int>(deadline - QDateTime::currentMSecsSinceEpoch());
        if (remaining <= 0) {
            if (error) *error = QString("llama-server read timeout (%1 ms).").arg(timeoutMs);
            return false;
        }
        if (!socket.waitForReadyRead(qMin(250, remaining))) {
            if (socket.state() == QAbstractSocket::UnconnectedState) break;
        }
    }
    all += socket.readAll();

    if (all.isEmpty()) {
        if (error) *error = "llama-server returned empty response.";
        return false;
    }

    // Separation headers / corps.
    const int headerEnd = all.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (error) *error = "llama-server malformed HTTP response (no header terminator).";
        return false;
    }
    const QByteArray headers = all.left(headerEnd);
    const QByteArray statusLine = headers.left(headers.indexOf("\r\n"));
    if (!statusLine.contains("200")) {
        if (error) *error = QString("llama-server HTTP error: %1").arg(QString::fromUtf8(statusLine));
        return false;
    }

    // Content-Length prioritaire, sinon corps jusqu'a la fin.
    QByteArray responseBody;
    static const QRegularExpression reContentLength(
        "Content-Length:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    const auto match = reContentLength.match(QString::fromLatin1(headers));
    const int bodyStart = headerEnd + 4;
    if (match.hasMatch()) {
        const int contentLength = match.captured(1).toInt();
        const qint64 bodyDeadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        while (all.size() < bodyStart + contentLength) {
            if (socket.state() == QAbstractSocket::UnconnectedState && socket.bytesAvailable() == 0) break;
            if (QDateTime::currentMSecsSinceEpoch() > bodyDeadline) break;
            socket.waitForReadyRead(250);
            all += socket.readAll();
        }
        responseBody = all.mid(bodyStart, contentLength);
    } else {
        responseBody = all.mid(bodyStart);
    }

    if (response) *response = responseBody;
    return true;
}

} // namespace

LlamaServer& LlamaServer::instance() {
    static LlamaServer server;
    return server;
}

LlamaServer::LlamaServer() = default;

LlamaServer::~LlamaServer() {
    shutdown();
}

void LlamaServer::connectQuitHook() {
    if (m_quitHookConnected) return;
    m_quitHookConnected = true;
    if (auto app = QCoreApplication::instance()) {
        // Sans contexte QObject: le lambda ferme l'instance partagee a la sortie.
        QObject::connect(app, &QCoreApplication::aboutToQuit, []() {
            LlamaServer::instance().shutdown();
        });
    }
}

QString LlamaServer::locateExecutable() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString envExe = env.value("KILLENGINE_LLAMA_SERVER").trimmed();
    if (!envExe.isEmpty() && QFileInfo::exists(envExe)) {
        return QFileInfo(envExe).absoluteFilePath();
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("llama-server.exe"),
        appDir.filePath("llama.cpp/llama-server.exe"),
        appDir.filePath("../../third_party/llama.cpp/llama-server.exe"),
        QDir::current().filePath("third_party/llama.cpp/llama-server.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/Release/llama-server.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/llama-server.exe"),
    };
    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

void LlamaServer::setModel(const QString& modelPath, const QString& executablePath) {
    const bool changed = m_modelPath != modelPath || m_executablePath != executablePath;
    m_modelPath = modelPath;
    m_executablePath = executablePath;
    if (changed && isRunning()) {
        // Modele change pendant que le serveur tourne: on le redemarre pour
        // qu'il recharge le bon GGUF.
        shutdown();
    }
}

bool LlamaServer::isRunning() const {
    return m_process.state() == QProcess::Running && m_port > 0;
}

bool LlamaServer::healthCheck(int timeoutMs) {
    if (m_port <= 0) return false;
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", m_port);
    if (!socket.waitForConnected(timeoutMs)) return false;
    socket.write(QString("GET /health HTTP/1.1\r\nHost: 127.0.0.1:%1\r\nConnection: close\r\n\r\n")
                     .arg(m_port)
                     .toUtf8());
    socket.waitForBytesWritten(timeoutMs);
    socket.waitForReadyRead(timeoutMs);
    const QByteArray all = socket.readAll();
    socket.disconnectFromHost();
    return all.startsWith("HTTP/1.1 200") || all.startsWith("HTTP/1.0 200") || all.contains("200 OK");
}

bool LlamaServer::startAndWait(QString* error) {
    if (m_executablePath.isEmpty() || m_modelPath.isEmpty()) {
        if (error) *error = "llama-server executable or model not configured.";
        return false;
    }
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }

    // Port fixe par defaut pour permettre le cache prompt entre requetes; un
    // port libre peut etre choisi via KILLENGINE_LLAMA_SERVER_PORT.
    bool ok = false;
    int port = qEnvironmentVariable("KILLENGINE_LLAMA_SERVER_PORT").toInt(&ok);
    if (!ok || port <= 0) port = kDefaultPort;
    m_port = port;

    QStringList args{
        "-m", m_modelPath,
        "--port", QString::number(m_port),
        "--host", "127.0.0.1",
        "--no-webui",
        "-c", "4096",
        "--parallel", "1",
    };
    bool threadsOk = false;
    const int threads = qEnvironmentVariable("KILLENGINE_LLAMA_SERVER_THREADS").toInt(&threadsOk);
    if (threadsOk && threads > 0) args << "-t" << QString::number(threads);

    m_process.setProgram(m_executablePath);
    m_process.setArguments(args);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.start();

    if (!m_process.waitForStarted(5000)) {
        m_lastServerError = m_process.readAllStandardError();
        if (error) {
            *error = QString("llama-server failed to start: %1").arg(QString::fromUtf8(m_lastServerError).trimmed());
        }
        return false;
    }

    // Attente bornee du chargement du modele (health checks repetes).
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < startupTimeoutMs()) {
        if (m_process.state() != QProcess::Running) {
            m_lastServerError = m_process.readAllStandardError();
            if (error) {
                *error = QString("llama-server exited during startup: %1")
                             .arg(QString::fromUtf8(m_lastServerError).trimmed());
            }
            return false;
        }
        if (healthCheck(1000)) {
            KE_LOG_INFO() << "llama-server up on port " << m_port << " (startup " << elapsed.elapsed() << " ms)";
            return true;
        }
        QThread::msleep(300);
    }

    if (error) *error = "llama-server startup timeout (model load).";
    return false;
}

bool LlamaServer::ensureRunning(QString* error) {
    if (serverDisabledByEnv() || serverDisabledBySettings()) {
        if (error) *error = "llama-server disabled by settings/env.";
        return false;
    }
    if (isRunning() && healthCheck(1500)) return true;
    if (isRunning()) shutdown();
    connectQuitHook();
    return startAndWait(error);
}

LlamaServerCompletion LlamaServer::complete(const QString& prompt, int nPredict, const QStringList& stop) {
    LlamaServerCompletion result;
    QString error;
    if (!ensureRunning(&error)) {
        result.errorMessage = error;
        return result;
    }

    const QByteArray body = buildCompletionRequest(prompt, nPredict, stop);
    QByteArray response;
    if (!httpPostJson(m_port, body, completionTimeoutMs(), &response, &error)) {
        // Le serveur peut etre mort entre-temps: une tentative de redemarrage.
        if (!ensureRunning(&error)) {
            result.errorMessage = error;
            return result;
        }
        if (!httpPostJson(m_port, body, completionTimeoutMs(), &response, &error)) {
            result.errorMessage = error;
            return result;
        }
    }

    const QString content = parseCompletionContent(response);
    if (content.isEmpty()) {
        result.errorMessage = "llama-server completion returned empty content.";
        return result;
    }
    result.success = true;
    result.content = content;
    result.fromServer = true;
    return result;
}

void LlamaServer::shutdown() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(3000)) {
            m_process.kill();
            m_process.waitForFinished(2000);
        }
    }
    m_port = 0;
}

QByteArray LlamaServer::buildCompletionRequest(const QString& prompt, int nPredict, const QStringList& stop) {
    QJsonObject root;
    root.insert("prompt", prompt);
    root.insert("n_predict", nPredict);
    root.insert("temperature", 0.0);
    root.insert("cache_prompt", true);
    root.insert("stream", false);
    QJsonArray stopArray;
    for (const auto& s : stop) stopArray.append(s);
    root.insert("stop", stopArray);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QString LlamaServer::parseCompletionContent(const QByteArray& body) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return {};
    return document.object().value("content").toString();
}

} // namespace killai