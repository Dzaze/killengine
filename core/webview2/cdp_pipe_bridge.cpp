#include "cdp_pipe_bridge.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

#include <windows.h>

namespace killcore {

// ============================================================================
// CdpPipeBridge
// ============================================================================

CdpPipeBridge::CdpPipeBridge(QObject* parent)
    : QObject(parent)
    , m_socket(std::make_unique<QLocalSocket>(this))
{
    connect(m_socket.get(), &QLocalSocket::connected, this, &CdpPipeBridge::onConnected);
    connect(m_socket.get(), &QLocalSocket::disconnected, this, &CdpPipeBridge::onDisconnected);
    connect(m_socket.get(), QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
            this, &CdpPipeBridge::onError);
    connect(m_socket.get(), &QLocalSocket::readyRead, this, &CdpPipeBridge::onReadyRead);
}

CdpPipeBridge::~CdpPipeBridge() {
    disconnect();
}

bool CdpPipeBridge::createServer(const QString& pipeName) {
    // En mode serveur, on crée un QLocalServer et on attend une connexion
    auto* server = new QLocalServer(this);
    
    // Supprimer le pipe existant s'il y en a un
    QLocalServer::removeServer(pipeName);
    
    if (!server->listen(pipeName)) {
        emit error(QString("Failed to create pipe server: %1").arg(server->errorString()));
        delete server;
        return false;
    }
    
    m_pipeName = pipeName;
    m_isServer = true;
    
    connect(server, &QLocalServer::newConnection, this, [this, server]() {
        if (m_connected) {
            // Déjà connecté, rejeter la nouvelle connexion
            auto* newSocket = server->nextPendingConnection();
            newSocket->close();
            newSocket->deleteLater();
            return;
        }
        
        m_socket.reset(server->nextPendingConnection());
        
        connect(m_socket.get(), &QLocalSocket::disconnected, this, &CdpPipeBridge::onDisconnected);
        connect(m_socket.get(), QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
                this, &CdpPipeBridge::onError);
        connect(m_socket.get(), &QLocalSocket::readyRead, this, &CdpPipeBridge::onReadyRead);
        
        m_connected = true;
        emit connected();
        emit clientConnected();
    });
    
    return true;
}

bool CdpPipeBridge::connectToServer(const QString& pipeName) {
    if (m_connected) {
        disconnect();
    }
    
    m_pipeName = pipeName;
    m_isServer = false;
    
    m_socket->connectToServer(pipeName);
    
    // Attendre la connexion avec timeout
    if (!m_socket->waitForConnected(5000)) {
        emit error(QString("Failed to connect to pipe: %1").arg(m_socket->errorString()));
        return false;
    }
    
    return true;
}

bool CdpPipeBridge::isConnected() const {
    return m_connected && m_socket->state() == QLocalSocket::ConnectedState;
}

void CdpPipeBridge::disconnect() {
    if (m_socket) {
        m_socket->disconnectFromServer();
    }
    m_connected = false;
    m_pendingCommands.clear();
    m_readBuffer.clear();
}

int CdpPipeBridge::sendCommand(const QString& method, const QJsonObject& params, CommandCallback callback) {
    if (!isConnected()) {
        if (callback) {
            callback(QJsonObject(), "Not connected to pipe");
        }
        return 0;
    }
    
    int id = m_nextCommandId++;
    
    QJsonObject message;
    message["id"] = id;
    message["method"] = method;
    message["params"] = params;
    
    if (callback) {
        m_pendingCommands[id] = callback;
    }
    
    sendMessage(message);
    return id;
}

QJsonObject CdpPipeBridge::sendCommandSync(const QString& method, const QJsonObject& params, int timeoutMs) {
    if (!isConnected()) {
        QJsonObject error;
        error["error"] = "Not connected to pipe";
        return error;
    }
    
    QJsonObject result;
    bool finished = false;
    
    int id = sendCommand(method, params, [&result, &finished](const QJsonObject& res, const QString& err) {
        if (!err.isEmpty()) {
            result["error"] = err;
        } else {
            result = res;
        }
        finished = true;
    });
    
    if (id == 0) {
        QJsonObject error;
        error["error"] = "Failed to send command";
        return error;
    }
    
    // Attendre la réponse avec timeout
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);
    
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &CdpPipeBridge::disconnected, &loop, &QEventLoop::quit);
    
    // Connecter à un signal personnalisé pour la réponse
    auto checkFinished = [&finished, &loop]() {
        if (finished) {
            loop.quit();
        }
    };
    
    QTimer checkTimer;
    connect(&checkTimer, &QTimer::timeout, checkFinished);
    checkTimer.start(10);
    
    timer.start();
    loop.exec();
    
    if (!finished) {
        m_pendingCommands.remove(id);
        result["error"] = "Timeout waiting for response";
    }
    
    return result;
}

bool CdpPipeBridge::enableDomain(const QString& domain) {
    QJsonObject params;
    params["id"] = domain;
    auto result = sendCommandSync(domain + ".enable", params);
    return !result.contains("error");
}

bool CdpPipeBridge::disableDomain(const QString& domain) {
    QJsonObject params;
    params["id"] = domain;
    auto result = sendCommandSync(domain + ".disable", params);
    return !result.contains("error");
}

QJsonObject CdpPipeBridge::evaluateJavaScript(const QString& expression, bool returnByValue) {
    QJsonObject params;
    params["expression"] = expression;
    params["returnByValue"] = returnByValue;
    return sendCommandSync("Runtime.evaluate", params);
}

QString CdpPipeBridge::makePipeNameForProcess(quint32 processId) {
    return QString("KillEngineCdpBridge_%1").arg(processId);
}

bool CdpPipeBridge::pipeExists(const QString& pipeName) {
    // Essayer de se connecter en mode client pour vérifier l'existence
    QLocalSocket socket;
    socket.connectToServer(pipeName);
    bool exists = socket.waitForConnected(100);
    if (exists) {
        socket.disconnectFromServer();
    }
    return exists;
}

void CdpPipeBridge::onConnected() {
    m_connected = true;
    emit connected();
}

void CdpPipeBridge::onDisconnected() {
    m_connected = false;
    m_pendingCommands.clear();
    m_readBuffer.clear();
    emit disconnected();
}

void CdpPipeBridge::onError(QLocalSocket::LocalSocketError error) {
    QString errorMsg;
    switch (error) {
        case QLocalSocket::ConnectionRefusedError:
            errorMsg = "Connection refused - is the helper running?";
            break;
        case QLocalSocket::PeerClosedError:
            errorMsg = "Peer closed connection";
            break;
        case QLocalSocket::ServerNotFoundError:
            errorMsg = "Pipe not found";
            break;
        case QLocalSocket::SocketTimeoutError:
            errorMsg = "Connection timeout";
            break;
        default:
            errorMsg = QString("Socket error: %1").arg(static_cast<int>(error));
            break;
    }
    emit this->error(errorMsg);
}

void CdpPipeBridge::onReadyRead() {
    m_readBuffer.append(m_socket->readAll());
    
    // Protocol: chaque message est précédé de sa taille (4 bytes, little-endian)
    while (m_readBuffer.size() >= 4) {
        quint32 msgSize = *reinterpret_cast<const quint32*>(m_readBuffer.constData());
        
        if (m_readBuffer.size() < 4 + static_cast<int>(msgSize)) {
            // Message incomplet, attendre plus de données
            break;
        }
        
        QByteArray messageData = m_readBuffer.mid(4, msgSize);
        m_readBuffer.remove(0, 4 + msgSize);
        
        processMessage(messageData);
    }
}

void CdpPipeBridge::processMessage(const QByteArray& data) {
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "Invalid JSON message received";
        return;
    }
    
    QJsonObject message = doc.object();
    
    // Vérifier si c'est une réponse à une commande
    if (message.contains("id")) {
        int id = message["id"].toInt();
        auto it = m_pendingCommands.find(id);
        if (it != m_pendingCommands.end()) {
            auto callback = it.value();
            m_pendingCommands.erase(it);
            
            if (message.contains("error")) {
                QString errorMsg = message["error"].toObject()["message"].toString();
                callback(QJsonObject(), errorMsg);
            } else {
                callback(message["result"].toObject(), QString());
            }
        }
    }
    // Ou un événement
    else if (message.contains("method")) {
        QString method = message["method"].toString();
        QJsonObject params = message["params"].toObject();
        emit eventReceived(method, params);
    }
}

void CdpPipeBridge::sendMessage(const QJsonObject& message) {
    QJsonDocument doc(message);
    QByteArray data = doc.toJson(QJsonDocument::Compact);
    
    // Protocol: taille (4 bytes) + données
    quint32 size = static_cast<quint32>(data.size());
    QByteArray packet;
    packet.append(reinterpret_cast<const char*>(&size), 4);
    packet.append(data);
    
    m_socket->write(packet);
    m_socket->flush();
}

// ============================================================================
// Helper functions
// ============================================================================

bool launchCdpPipeHelper(quint32 processId, const QString& pipeName) {
    // Cette fonction sera implémentée avec l'injection DLL
    // Pour l'instant, retourne false (nécessite l'implémentation du helper)
    qWarning() << "launchCdpPipeHelper not yet implemented - requires DLL injection";
    return false;
}

bool isCdpPipeHelperActive(quint32 processId) {
    QString pipeName = CdpPipeBridge::makePipeNameForProcess(processId);
    return CdpPipeBridge::pipeExists(pipeName);
}

} // namespace killcore
