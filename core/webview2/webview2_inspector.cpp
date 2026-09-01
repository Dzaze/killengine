#include "webview2_inspector.h"

#include "cdp_client.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QThread>
#include <QTimer>

namespace killcore {
namespace {

bool waitForCdpConnection(CdpClient* client, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    bool connected = client->isConnected();
    bool failed = false;

    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QMetaObject::Connection connectedConnection = QObject::connect(client, &CdpClient::connected, &loop, [&]() {
        connected = true;
        loop.quit();
    });
    QMetaObject::Connection errorConnection = QObject::connect(client, &CdpClient::error, &loop, [&]() {
        failed = true;
        loop.quit();
    });

    timer.start(timeoutMs);
    if (!connected) {
        loop.exec();
    }

    QObject::disconnect(connectedConnection);
    QObject::disconnect(errorConnection);
    return connected && !failed;
}

QVariant remoteObjectToVariant(const QJsonObject& remoteObject)
{
    if (remoteObject.contains(QStringLiteral("value"))) {
        return remoteObject[QStringLiteral("value")].toVariant();
    }
    if (remoteObject.contains(QStringLiteral("description"))) {
        return remoteObject[QStringLiteral("description")].toString();
    }
    return {};
}

QJsonObject evaluationResultObject(const QJsonObject& response)
{
    return response[QStringLiteral("result")].toObject()[QStringLiteral("result")].toObject();
}

QString jsStringLiteral(const QString& value)
{
    QJsonArray array;
    array.append(value);
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

} // namespace

WebView2Inspector::WebView2Inspector(QObject* parent)
    : QObject(parent)
    , m_client(std::make_unique<CdpClient>())
{
    connect(m_client.get(), &CdpClient::connected, this, &WebView2Inspector::connected);
    connect(m_client.get(), &CdpClient::disconnected, this, &WebView2Inspector::disconnected);
    connect(m_client.get(), &CdpClient::error, this, &WebView2Inspector::error);
    connect(m_client.get(), &CdpClient::eventReceived, this, &WebView2Inspector::onCdpEvent);
}

WebView2Inspector::~WebView2Inspector() = default;

bool WebView2Inspector::connectToWebView(const QString& httpUrl, const QString& pageTitle, const QString& pageUrl)
{
    QString status;
    QJsonArray pages = discoverCdpPagesWithFallback(httpUrl, &status, true);
    QString wsUrl;

    for (const QJsonValue& pageValue : pages) {
        if (!pageValue.isObject()) {
            continue;
        }

        const QJsonObject page = pageValue.toObject();
        const QString title = page[QStringLiteral("title")].toString();
        const QString url = page[QStringLiteral("url")].toString();

        if (!pageTitle.isEmpty() && !title.contains(pageTitle, Qt::CaseInsensitive)) {
            continue;
        }
        if (!pageUrl.isEmpty() && !url.contains(pageUrl, Qt::CaseInsensitive)) {
            continue;
        }

        wsUrl = page[QStringLiteral("webSocketDebuggerUrl")].toString();
        break;
    }

    if (wsUrl.isEmpty() && pageTitle.isEmpty() && pageUrl.isEmpty() && !pages.isEmpty()) {
        wsUrl = pages.first().toObject()[QStringLiteral("webSocketDebuggerUrl")].toString();
    }

    if (wsUrl.isEmpty()) {
        emit error(status.isEmpty() ? QStringLiteral("Aucune page WebView2/CDP disponible") : status);
        return false;
    }

    return connectToWebSocket(wsUrl);
}

bool WebView2Inspector::connectToWebSocket(const QString& wsUrl)
{
    if (!m_client->connectTo(wsUrl)) {
        emit error(QStringLiteral("Impossible d'initier la connexion CDP WebSocket"));
        return false;
    }

    return waitForCdpConnection(m_client.get(), 5000) && m_client->isConnected();
}

bool WebView2Inspector::isConnected() const
{
    return m_client && m_client->isConnected();
}

void WebView2Inspector::disconnect()
{
    if (m_client) {
        m_client->disconnect();
    }
    clearCache();
}

QJsonObject WebView2Inspector::getDocument()
{
    QJsonObject result = m_client->sendCommandSync(QStringLiteral("DOM.getDocument"));
    return result[QStringLiteral("result")].toObject()[QStringLiteral("root")].toObject();
}

int WebView2Inspector::querySelector(const QString& selector)
{
    if (m_selectorCache.contains(selector)) {
        return m_selectorCache.value(selector);
    }

    QJsonObject root = getDocument();
    const int documentNodeId = root[QStringLiteral("nodeId")].toInt();
    if (documentNodeId == 0) {
        return 0;
    }

    QJsonObject params;
    params[QStringLiteral("nodeId")] = documentNodeId;
    params[QStringLiteral("selector")] = selector;

    QJsonObject result = m_client->sendCommandSync(QStringLiteral("DOM.querySelector"), params);
    const int nodeId = result[QStringLiteral("result")].toObject()[QStringLiteral("nodeId")].toInt();
    if (nodeId != 0) {
        m_selectorCache.insert(selector, nodeId);
    }
    return nodeId;
}

QList<int> WebView2Inspector::querySelectorAll(const QString& selector)
{
    QList<int> nodeIds;
    QJsonObject root = getDocument();
    const int documentNodeId = root[QStringLiteral("nodeId")].toInt();
    if (documentNodeId == 0) {
        return nodeIds;
    }

    QJsonObject params;
    params[QStringLiteral("nodeId")] = documentNodeId;
    params[QStringLiteral("selector")] = selector;

    QJsonObject result = m_client->sendCommandSync(QStringLiteral("DOM.querySelectorAll"), params);
    const QJsonArray ids = result[QStringLiteral("result")].toObject()[QStringLiteral("nodeIds")].toArray();
    for (const QJsonValue& id : ids) {
        nodeIds.append(id.toInt());
    }
    return nodeIds;
}

QString WebView2Inspector::getElementText(int nodeId)
{
    QJsonObject params;
    params[QStringLiteral("nodeId")] = nodeId;
    params[QStringLiteral("objectGroup")] = QStringLiteral("killengine");

    QJsonObject resolved = m_client->sendCommandSync(QStringLiteral("DOM.resolveNode"), params);
    const QString objectId = resolved[QStringLiteral("result")]
        .toObject()[QStringLiteral("object")]
        .toObject()[QStringLiteral("objectId")]
        .toString();
    if (objectId.isEmpty()) {
        return {};
    }

    QJsonObject callParams;
    callParams[QStringLiteral("objectId")] = objectId;
    callParams[QStringLiteral("functionDeclaration")] = QStringLiteral("function() { return this.innerText || this.textContent || ''; }");
    callParams[QStringLiteral("returnByValue")] = true;

    QJsonObject result = m_client->sendCommandSync(QStringLiteral("Runtime.callFunctionOn"), callParams);
    return remoteObjectToVariant(evaluationResultObject(result)).toString();
}

QString WebView2Inspector::getElementAttribute(int nodeId, const QString& name)
{
    QJsonObject params;
    params[QStringLiteral("nodeId")] = nodeId;

    QJsonObject result = m_client->sendCommandSync(QStringLiteral("DOM.getAttributes"), params);
    const QJsonArray attributes = result[QStringLiteral("result")].toObject()[QStringLiteral("attributes")].toArray();
    for (int i = 0; i + 1 < attributes.size(); i += 2) {
        if (attributes.at(i).toString() == name) {
            return attributes.at(i + 1).toString();
        }
    }
    return {};
}

QVariantMap WebView2Inspector::getComputedStyles(int nodeId)
{
    QJsonObject params;
    params[QStringLiteral("nodeId")] = nodeId;
    QJsonObject result = m_client->sendCommandSync(QStringLiteral("CSS.getComputedStyleForNode"), params);

    QVariantMap styles;
    const QJsonArray computed = result[QStringLiteral("result")].toObject()[QStringLiteral("computedStyle")].toArray();
    for (const QJsonValue& itemValue : computed) {
        const QJsonObject item = itemValue.toObject();
        styles.insert(item[QStringLiteral("name")].toString(), item[QStringLiteral("value")].toString());
    }
    return styles;
}

QJsonObject WebView2Inspector::evaluateJavaScript(const QString& expression, bool returnByValue)
{
    return m_client->evaluateJavaScript(expression, returnByValue);
}

QVariant WebView2Inspector::callFunction(const QString& functionCode)
{
    QJsonObject result = evaluateJavaScript(QStringLiteral("(%1)()").arg(functionCode), true);
    return remoteObjectToVariant(evaluationResultObject(result));
}

QJsonObject WebView2Inspector::probeGlobalScope()
{
    QJsonObject result;
    if (!isConnected()) {
        result[QStringLiteral("error")] = QStringLiteral("Non connecte.");
        return result;
    }

    // returnByValue=false : on veut le RemoteObject de window (avec son
    // objectId), pas une tentative de serialisation JSON de l'objet global
    // entier (echouerait / serait enorme).
    const QJsonObject windowEval = m_client->evaluateJavaScript(QStringLiteral("window"), false);
    const QJsonObject windowRemote = evaluationResultObject(windowEval);
    const QString objectId = windowRemote.value(QStringLiteral("objectId")).toString();
    if (objectId.isEmpty()) {
        result[QStringLiteral("error")] = QStringLiteral("Impossible d'obtenir l'objectId de window.");
        return result;
    }

    const QJsonObject propsResponse = m_client->getObjectProperties(objectId, /*ownProperties=*/true);
    const QJsonArray props = propsResponse.value(QStringLiteral("result")).toObject()
        .value(QStringLiteral("result")).toArray();

    QJsonArray globals;
    for (const QJsonValue& propVal : props) {
        const QJsonObject prop = propVal.toObject();
        const QString name = prop.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            continue;
        }
        const QJsonObject value = prop.value(QStringLiteral("value")).toObject();
        QJsonObject entry;
        entry[QStringLiteral("name")] = name;
        entry[QStringLiteral("type")] = value.value(QStringLiteral("type")).toString();
        entry[QStringLiteral("subtype")] = value.value(QStringLiteral("subtype")).toString();
        entry[QStringLiteral("className")] = value.value(QStringLiteral("className")).toString();
        globals.append(entry);
    }
    result[QStringLiteral("globals")] = globals;

    const QJsonObject mediaEval = m_client->evaluateJavaScript(
        QStringLiteral(
            "JSON.stringify({video: document.querySelectorAll('video').length, "
            "audio: document.querySelectorAll('audio').length, "
            "iframes: Array.from(document.querySelectorAll('iframe')).map(f => f.src)})"),
        true);
    const QVariant mediaJson = remoteObjectToVariant(evaluationResultObject(mediaEval));
    const QJsonDocument mediaDoc = QJsonDocument::fromJson(mediaJson.toString().toUtf8());
    result[QStringLiteral("media")] = mediaDoc.isObject() ? mediaDoc.object() : QJsonObject();

    result[QStringLiteral("success")] = true;
    return result;
}

QJsonArray WebView2Inspector::findDisplayedValues(int value)
{
    return findDisplayedText(QString::number(value));
}

QJsonArray WebView2Inspector::findDisplayedText(const QString& text)
{
    const QString expression = QStringLiteral(R"JS(
(() => {
  const needle = %1;
  return [...document.querySelectorAll('body *')]
    .map((el) => ({ tag: el.tagName, text: (el.innerText || el.textContent || '').trim(), id: el.id || '', className: el.className || '' }))
    .filter((entry) => entry.text.includes(needle))
    .slice(0, 100);
})()
)JS").arg(jsStringLiteral(text));

    QJsonObject result = evaluateJavaScript(expression, true);
    return evaluationResultObject(result)[QStringLiteral("value")].toArray();
}

QJsonArray WebView2Inspector::getElementsByClassName(const QString& className)
{
    const QString expression = QStringLiteral(R"JS(
(() => [...document.getElementsByClassName(%1)]
  .map((el) => ({ tag: el.tagName, text: (el.innerText || el.textContent || '').trim().slice(0, 500), id: el.id || '', className: el.className || '' }))
  .slice(0, 100))()
)JS").arg(jsStringLiteral(className));

    QJsonObject result = evaluateJavaScript(expression, true);
    return evaluationResultObject(result)[QStringLiteral("value")].toArray();
}

QJsonArray WebView2Inspector::getElementsByTagName(const QString& tagName)
{
    const QString expression = QStringLiteral(R"JS(
(() => [...document.getElementsByTagName(%1)]
  .map((el) => ({ tag: el.tagName, text: (el.innerText || el.textContent || '').trim().slice(0, 500), id: el.id || '', className: el.className || '' }))
  .slice(0, 100))()
)JS").arg(jsStringLiteral(tagName));

    QJsonObject result = evaluateJavaScript(expression, true);
    return evaluationResultObject(result)[QStringLiteral("value")].toArray();
}

bool WebView2Inspector::enableDomMonitoring()
{
    m_domMonitoringEnabled = m_client->enableDomain(QStringLiteral("DOM"));
    return m_domMonitoringEnabled;
}

void WebView2Inspector::disableDomMonitoring()
{
    m_client->disableDomain(QStringLiteral("DOM"));
    m_domMonitoringEnabled = false;
}

bool WebView2Inspector::enableConsoleMonitoring()
{
    m_consoleMonitoringEnabled = m_client->enableDomain(QStringLiteral("Runtime"));
    return m_consoleMonitoringEnabled;
}

void WebView2Inspector::disableConsoleMonitoring()
{
    m_client->disableDomain(QStringLiteral("Runtime"));
    m_consoleMonitoringEnabled = false;
}

QString WebView2Inspector::getPageTitle()
{
    return callFunction(QStringLiteral("() => document.title")).toString();
}

QString WebView2Inspector::getPageUrl()
{
    return callFunction(QStringLiteral("() => location.href")).toString();
}

QJsonObject WebView2Inspector::getPerformanceMetrics()
{
    return m_client->sendCommandSync(QStringLiteral("Performance.getMetrics"));
}

bool WebView2Inspector::waitForDomReady(int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (callFunction(QStringLiteral("() => document.readyState")).toString() != QStringLiteral("loading")) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
    return false;
}

int WebView2Inspector::waitForElement(const QString& selector, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        const int nodeId = querySelector(selector);
        if (nodeId != 0) {
            return nodeId;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
    return 0;
}

QJsonArray WebView2Inspector::listAvailablePages(const QString& httpUrl)
{
    QString status;
    QJsonArray pages = discoverCdpPagesWithFallback(httpUrl, &status, true);
    if (pages.isEmpty() && !status.isEmpty()) {
        qWarning() << status;
    }
    return pages;
}

void WebView2Inspector::onCdpEvent(const QString& method, const QJsonObject& params)
{
    if (m_domMonitoringEnabled && method.startsWith(QStringLiteral("DOM."))) {
        emit domMutated(QJsonArray{params});
    }

    if (m_consoleMonitoringEnabled && method == QStringLiteral("Runtime.consoleAPICalled")) {
        const QString level = params[QStringLiteral("type")].toString();
        const QJsonArray args = params[QStringLiteral("args")].toArray();
        QStringList parts;
        for (const QJsonValue& argValue : args) {
            const QJsonObject arg = argValue.toObject();
            parts.append(arg[QStringLiteral("value")].toVariant().toString());
        }
        emit consoleMessage(level, parts.join(QLatin1Char(' ')), QString());
    }
}

void WebView2Inspector::clearCache()
{
    m_selectorCache.clear();
}

int WebView2Inspector::getNodeIdFromRemoteObject(const QJsonObject& remoteObject)
{
    const QString objectId = remoteObject[QStringLiteral("objectId")].toString();
    if (objectId.isEmpty()) {
        return 0;
    }

    QJsonObject params;
    params[QStringLiteral("objectId")] = objectId;
    QJsonObject result = m_client->sendCommandSync(QStringLiteral("DOM.requestNode"), params);
    return result[QStringLiteral("result")].toObject()[QStringLiteral("nodeId")].toInt();
}

} // namespace killcore
