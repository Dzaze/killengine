#include "cdp_client.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QDebug>

namespace killcore {

CdpClient::CdpClient(QObject* parent)
    : QObject(parent)
    , m_socket(std::make_unique<QWebSocket>())
{
    connect(m_socket.get(), &QWebSocket::connected, this, &CdpClient::onConnected);
    connect(m_socket.get(), &QWebSocket::disconnected, this, &CdpClient::onDisconnected);
    connect(m_socket.get(), QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &CdpClient::onError);
    connect(m_socket.get(), &QWebSocket::textMessageReceived, this, &CdpClient::onTextMessageReceived);
}

CdpClient::~CdpClient() = default;

bool CdpClient::connectTo(const QString& wsUrl)
{
    if (m_connected) {
        disconnect();
    }

    m_wsUrl = wsUrl;
    m_socket->open(QUrl(wsUrl));
    return true;
}

bool CdpClient::isConnected() const
{
    return m_connected && m_socket->state() == QAbstractSocket::ConnectedState;
}

int CdpClient::sendCommand(const QString& method, const QJsonObject& params, CommandCallback callback)
{
    if (!isConnected()) {
        if (callback) {
            callback(QJsonObject(), QStringLiteral("Not connected"));
        }
        return 0;
    }

    int id = m_nextCommandId++;

    QJsonObject message;
    message[QStringLiteral("id")] = id;
    message[QStringLiteral("method")] = method;
    message[QStringLiteral("params")] = params;

    QJsonDocument doc(message);
    QString jsonString = doc.toJson(QJsonDocument::Compact);

    m_pendingCommands[id] = callback;
    m_socket->sendTextMessage(jsonString);

    return id;
}

QJsonObject CdpClient::sendCommandSync(const QString& method, const QJsonObject& params, int timeoutMs)
{
    QJsonObject result;
    QString error;
    bool finished = false;

    int id = sendCommand(method, params, [&result, &error, &finished](const QJsonObject& res, const QString& err) {
        result = res;
        error = err;
        finished = true;
    });

    if (id == 0) {
        QJsonObject errResult;
        errResult[QStringLiteral("error")] = QStringLiteral("Failed to send command");
        return errResult;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);

    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    while (!finished && timer.isActive()) {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    }

    if (!finished) {
        m_pendingCommands.remove(id);
        QJsonObject errResult;
        errResult[QStringLiteral("error")] = QStringLiteral("Timeout");
        return errResult;
    }

    if (!error.isEmpty()) {
        QJsonObject errResult;
        errResult[QStringLiteral("error")] = error;
        return errResult;
    }

    return result;
}

bool CdpClient::enableDomain(const QString& domain)
{
    QString method = domain + QStringLiteral(".enable");
    QJsonObject result = sendCommandSync(method);
    return !result.contains(QStringLiteral("error"));
}

bool CdpClient::disableDomain(const QString& domain)
{
    QString method = domain + QStringLiteral(".disable");
    QJsonObject result = sendCommandSync(method);
    return !result.contains(QStringLiteral("error"));
}

QJsonObject CdpClient::evaluateJavaScript(const QString& expression, bool returnByValue)
{
    QJsonObject params;
    params[QStringLiteral("expression")] = expression;
    params[QStringLiteral("returnByValue")] = returnByValue;

    return sendCommandSync(QStringLiteral("Runtime.evaluate"), params);
}

QJsonObject CdpClient::getObjectProperties(const QString& objectId)
{
    QJsonObject params;
    params[QStringLiteral("objectId")] = objectId;

    return sendCommandSync(QStringLiteral("Runtime.getProperties"), params);
}

void CdpClient::disconnect()
{
    m_socket->close();
    m_connected = false;
    m_pendingCommands.clear();
}

void CdpClient::onConnected()
{
    m_connected = true;
    emit connected();
}

void CdpClient::onDisconnected()
{
    m_connected = false;
    m_pendingCommands.clear();
    emit disconnected();
}

void CdpClient::onError(QAbstractSocket::SocketError error)
{
    QString errorString = m_socket->errorString();
    emit this->error(errorString);
}

void CdpClient::onTextMessageReceived(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        return;
    }

    QJsonObject obj = doc.object();

    // Check if it's a response to a command
    if (obj.contains(QStringLiteral("id"))) {
        int id = obj[QStringLiteral("id")].toInt();
        auto it = m_pendingCommands.find(id);
        if (it != m_pendingCommands.end()) {
            CommandCallback callback = it.value();
            m_pendingCommands.erase(it);

            QString error;
            if (obj.contains(QStringLiteral("error"))) {
                QJsonObject errObj = obj[QStringLiteral("error")].toObject();
                error = errObj[QStringLiteral("message")].toString();
            }

            if (callback) {
                callback(obj, error);
            }
        }
    }
    // Check if it's an event
    else if (obj.contains(QStringLiteral("method"))) {
        QString method = obj[QStringLiteral("method")].toString();
        QJsonObject params = obj[QStringLiteral("params")].toObject();
        emit eventReceived(method, params);
    }
}

QJsonArray discoverCdpPages(const QString& httpUrl)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(httpUrl));
    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(5000);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start();

    loop.exec();

    QJsonArray result;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isArray()) {
            result = doc.array();
        }
    }

    reply->deleteLater();
    return result;
}

QString findCdpWebSocketUrl(const QString& httpUrl, const QString& pageTitle, const QString& pageUrl)
{
    QJsonArray pages = discoverCdpPages(httpUrl);

    for (const QJsonValue& pageValue : pages) {
        if (!pageValue.isObject()) continue;

        QJsonObject page = pageValue.toObject();
        QString wsUrl = page[QStringLiteral("webSocketDebuggerUrl")].toString();
        QString title = page[QStringLiteral("title")].toString();
        QString url = page[QStringLiteral("url")].toString();

        if (wsUrl.isEmpty()) continue;

        // Match by title if provided
        if (!pageTitle.isEmpty() && title.contains(pageTitle, Qt::CaseInsensitive)) {
            return wsUrl;
        }

        // Match by URL if provided
        if (!pageUrl.isEmpty() && url.contains(pageUrl, Qt::CaseInsensitive)) {
            return wsUrl;
        }

        // If no filters provided, return first available
        if (pageTitle.isEmpty() && pageUrl.isEmpty()) {
            return wsUrl;
        }
    }

    return QString();
}

} // namespace killcore
