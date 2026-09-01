#include "cdp_client.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QCoreApplication>
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

    timer.start();

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

QJsonObject CdpClient::getObjectProperties(const QString& objectId, bool ownProperties)
{
    QJsonObject params;
    params[QStringLiteral("objectId")] = objectId;
    params[QStringLiteral("ownProperties")] = ownProperties;

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

namespace {

struct HttpJsonResult {
    QJsonDocument document;
    QString error;
    int httpStatus = 0;
};

bool isWdpMsedgeRoot(const QJsonObject& object)
{
    return object.contains(QStringLiteral("targets"))
        && object[QStringLiteral("targets")].isArray()
        && object.contains(QStringLiteral("version"));
}

bool shouldKeepTarget(const QJsonObject& target, bool pageTargetsOnly)
{
    if (target[QStringLiteral("webSocketDebuggerUrl")].toString().isEmpty()) {
        return false;
    }

    if (!pageTargetsOnly) {
        return true;
    }

    return target[QStringLiteral("type")].toString().compare(QStringLiteral("page"), Qt::CaseInsensitive) == 0;
}

QJsonObject withWdpBrowserMetadata(QJsonObject target, const QJsonObject& browser)
{
    const QJsonObject version = browser[QStringLiteral("version")].toObject();
    const QJsonObject info = browser[QStringLiteral("info")].toObject();

    target[QStringLiteral("wdpBrowserProcessId")] = info[QStringLiteral("browserProcessId")];
    target[QStringLiteral("wdpBrowserWebSocketDebuggerUrl")] =
        version[QStringLiteral("webSocketDebuggerUrl")].toString();
    target[QStringLiteral("wdpBrowser")] = version[QStringLiteral("Browser")].toString();

    return target;
}

QJsonArray flattenCdpTargets(const QJsonDocument& document, bool pageTargetsOnly)
{
    QJsonArray result;
    if (!document.isArray()) {
        return result;
    }

    const QJsonArray values = document.array();
    for (const QJsonValue& value : values) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject object = value.toObject();
        if (isWdpMsedgeRoot(object)) {
            const QJsonArray targets = object[QStringLiteral("targets")].toArray();
            for (const QJsonValue& targetValue : targets) {
                if (!targetValue.isObject()) {
                    continue;
                }

                QJsonObject target = withWdpBrowserMetadata(targetValue.toObject(), object);
                if (shouldKeepTarget(target, pageTargetsOnly)) {
                    result.append(target);
                }
            }
            continue;
        }

        if (shouldKeepTarget(object, pageTargetsOnly)) {
            result.append(object);
        }
    }

    return result;
}

HttpJsonResult fetchJson(const QString& httpUrl, int timeoutMs = 5000)
{
    HttpJsonResult result;
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(httpUrl)};
    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    timer.setInterval(5000);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start();

    loop.exec();

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonParseError parseError;
        result.document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            result.error = QStringLiteral("Réponse CDP non JSON depuis %1 : %2")
                .arg(httpUrl, parseError.errorString());
        }
    } else if (timer.isActive()) {
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        result.httpStatus = status.isValid() ? status.toInt() : 0;
        result.error = QStringLiteral("Endpoint CDP inaccessible (%1) : %2")
            .arg(httpUrl, reply->errorString());
    } else {
        result.error = QStringLiteral("Timeout en interrogeant l'endpoint CDP %1").arg(httpUrl);
    }

    reply->deleteLater();
    return result;
}

QString wdpMsedgeUrl()
{
    return QStringLiteral("http://127.0.0.1:50080/msedge");
}

bool isWdpUrl(const QString& httpUrl)
{
    const QUrl url(httpUrl);
    return url.port() == 50080 && url.path().contains(QStringLiteral("msedge"), Qt::CaseInsensitive);
}

} // namespace

QJsonArray discoverCdpPages(const QString& httpUrl, bool pageTargetsOnly)
{
    const HttpJsonResult response = fetchJson(httpUrl);
    if (!response.error.isEmpty()) {
        qWarning() << response.error;
        return {};
    }

    return flattenCdpTargets(response.document, pageTargetsOnly);
}

QJsonArray discoverCdpPagesWithFallback(const QString& directHttpUrl,
                                        QString* statusMessage,
                                        bool pageTargetsOnly)
{
    QJsonArray directPages = discoverCdpPages(directHttpUrl, pageTargetsOnly);
    if (!directPages.isEmpty()) {
        if (statusMessage) {
            *statusMessage = QStringLiteral("Endpoint CDP direct disponible : %1").arg(directHttpUrl);
        }
        return directPages;
    }

    if (isWdpUrl(directHttpUrl)) {
        if (statusMessage) {
            *statusMessage = QStringLiteral(
                "Aucune target CDP sur %1. Vérifier que la cible WebView2 est lancée avec "
                "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--enable-features=msEdgeDevToolsWdpRemoteDebugging.")
                .arg(directHttpUrl);
        }
        return {};
    }

    const QString wdpUrl = wdpMsedgeUrl();
    QJsonArray wdpPages = discoverCdpPages(wdpUrl, pageTargetsOnly);
    if (!wdpPages.isEmpty()) {
        if (statusMessage) {
            *statusMessage = QStringLiteral(
                "Endpoint CDP direct indisponible (%1) ; fallback Windows Device Portal actif via %2.")
                .arg(directHttpUrl, wdpUrl);
        }
        return wdpPages;
    }

    if (statusMessage) {
        *statusMessage = QStringLiteral(
            "Aucune target CDP trouvée depuis %1. Pour une app desktop/Electron/CEF, vérifier le port direct "
            "(ex: --remote-debugging-port et /json). Pour une app UWP/Store WebView2, installer "
            "Tools.DeveloperMode.Core, activer Portail d'appareil, installer Remote Tools for Microsoft Edge, "
            "puis relancer la cible avec --enable-features=msEdgeDevToolsWdpRemoteDebugging.")
            .arg(directHttpUrl);
    }

    return {};
}

QString findCdpWebSocketUrl(const QString& httpUrl,
                            const QString& pageTitle,
                            const QString& pageUrl,
                            bool pageTargetsOnly)
{
    QJsonArray pages = discoverCdpPagesWithFallback(httpUrl, nullptr, pageTargetsOnly);

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
