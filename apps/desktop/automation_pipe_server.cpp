#include "automation_pipe_server.h"
#include "application_controller.h"
#include "logging/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QMetaMethod>
#include <QMetaType>
#include <QVector>

namespace killengine {

namespace {

// Nombre max d'arguments positionnels gérés par le dispatcher générique —
// QMetaMethod::invoke() plafonne lui-même à 10 QGenericArgument (val0..val9).
// Aucune méthode Q_INVOKABLE d'ApplicationController n'en approche (6 au
// maximum, forceWriteInstructionValue) : large marge, pas une limite serrée.
constexpr int kMaxDispatchArgs = 10;

// Réflexion QMetaMethod à l'exécution : trouve la méthode Q_INVOKABLE/slot
// nommée `methodName` avec le bon nombre d'arguments sur `controller`, convertit
// chaque argument JSON vers le QMetaType attendu (même conversion souple que
// QVariant::convert utilise déjà partout dans l'app), invoque, puis sérialise
// le retour. Aucune méthode n'est câblée à la main : une nouvelle méthode
// Q_INVOKABLE sur ApplicationController devient appelable ici sans y toucher.
bool invokeControllerMethod(
    QObject* controller,
    const QString& methodName,
    const QJsonArray& jsonParams,
    QVariant* outResult,
    QString* outError) {
    const QMetaObject* metaObject = controller->metaObject();
    const QByteArray methodNameUtf8 = methodName.toUtf8();

    QMetaMethod matched;
    bool found = false;
    for (int i = 0; i < metaObject->methodCount(); ++i) {
        const QMetaMethod method = metaObject->method(i);
        if (method.methodType() == QMetaMethod::Signal) {
            continue;
        }
        if (method.name() != methodNameUtf8) {
            continue;
        }
        if (method.parameterCount() != jsonParams.size()) {
            continue;
        }
        matched = method;
        found = true;
        break;
    }

    if (!found) {
        *outError = QString("Méthode inconnue ou nombre d'arguments incorrect : %1(%2 argument(s)).")
            .arg(methodName)
            .arg(jsonParams.size());
        return false;
    }

    if (matched.parameterCount() > kMaxDispatchArgs) {
        *outError = QString("Trop d'arguments pour %1 (limite dispatcher : %2).")
            .arg(methodName)
            .arg(kMaxDispatchArgs);
        return false;
    }

    // Stockage stable : QGenericArgument pointe vers QVariant::constData(),
    // qui doit rester en vie jusqu'à l'appel invoke() plus bas.
    QVector<QVariant> storage;
    storage.reserve(matched.parameterCount());
    for (int i = 0; i < matched.parameterCount(); ++i) {
        QVariant argVariant = jsonParams.at(i).toVariant();
        const QMetaType targetType = matched.parameterMetaType(i);
        if (argVariant.metaType() != targetType && !argVariant.convert(targetType)) {
            *outError = QString("Argument %1 de %2 non convertible vers %3.")
                .arg(i)
                .arg(methodName)
                .arg(QString::fromUtf8(targetType.name()));
            return false;
        }
        storage.append(argVariant);
    }

    QGenericArgument genericArgs[kMaxDispatchArgs];
    for (int i = 0; i < storage.size(); ++i) {
        genericArgs[i] = QGenericArgument(storage[i].typeName(), storage[i].constData());
    }

    const QMetaType returnType = matched.returnMetaType();
    bool ok = false;
    QVariant returnValue;
    if (returnType == QMetaType::fromType<void>()) {
        ok = matched.invoke(
            controller, Qt::DirectConnection,
            genericArgs[0], genericArgs[1], genericArgs[2], genericArgs[3], genericArgs[4],
            genericArgs[5], genericArgs[6], genericArgs[7], genericArgs[8], genericArgs[9]);
    } else {
        returnValue = QVariant(returnType);
        QGenericReturnArgument returnArg(returnValue.typeName(), returnValue.data());
        ok = matched.invoke(
            controller, Qt::DirectConnection, returnArg,
            genericArgs[0], genericArgs[1], genericArgs[2], genericArgs[3], genericArgs[4],
            genericArgs[5], genericArgs[6], genericArgs[7], genericArgs[8], genericArgs[9]);
    }

    if (!ok) {
        *outError = QString("Échec d'invocation pour %1.").arg(methodName);
        return false;
    }

    *outResult = returnValue;
    return true;
}

} // namespace

AutomationPipeServer::AutomationPipeServer(ApplicationController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    connect(&m_server, &QLocalServer::newConnection, this, &AutomationPipeServer::onNewConnection);
}

QString AutomationPipeServer::pipeName() {
    return QStringLiteral("KillEngineAutomationPipe");
}

bool AutomationPipeServer::start() {
    // Nettoie un pipe fantôme laissé par un précédent arrêt anormal (crash) —
    // inoffensif si rien n'existe, évite un "address in use" au redémarrage.
    QLocalServer::removeServer(pipeName());
    if (!m_server.listen(pipeName())) {
        KE_LOG_ERROR() << "AutomationPipeServer: échec d'écoute sur " << pipeName().toStdString()
                        << " (" << m_server.errorString().toStdString() << ")";
        return false;
    }
    KE_LOG_INFO() << "AutomationPipeServer: écoute sur \\\\.\\pipe\\" << pipeName().toStdString()
                  << " — connecteur d'automatisation actif (KILLENGINE_AUTOMATION_PIPE=1).";
    return true;
}

void AutomationPipeServer::onNewConnection() {
    while (QLocalSocket* socket = m_server.nextPendingConnection()) {
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, &AutomationPipeServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &AutomationPipeServer::onDisconnected);
        KE_LOG_INFO() << "AutomationPipeServer: nouvelle connexion.";
    }
}

void AutomationPipeServer::onReadyRead() {
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket || !m_buffers.contains(socket)) {
        return;
    }
    QByteArray& buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    int newlineIndex;
    while ((newlineIndex = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            handleLine(socket, line);
        }
    }
}

void AutomationPipeServer::onDisconnected() {
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) {
        return;
    }
    m_buffers.remove(socket);
    socket->deleteLater();
    KE_LOG_INFO() << "AutomationPipeServer: connexion fermée.";
}

void AutomationPipeServer::writeResponse(QLocalSocket* socket, const QByteArray& json) {
    if (!socket || socket->state() != QLocalSocket::ConnectedState) {
        return;
    }
    socket->write(json);
    socket->write("\n");
    socket->flush();
}

void AutomationPipeServer::handleLine(QLocalSocket* socket, const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    QJsonObject response;

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        response["error"] = QString("JSON invalide : %1").arg(parseError.errorString());
        writeResponse(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
        return;
    }

    const QJsonObject request = doc.object();
    if (request.contains("id")) {
        response["id"] = request.value("id");
    }

    const QString method = request.value("method").toString();
    if (method.isEmpty()) {
        response["error"] = QStringLiteral("Champ \"method\" manquant ou vide.");
        writeResponse(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
        return;
    }

    const QJsonArray params = request.value("params").toArray();

    QVariant result;
    QString error;
    const bool ok = invokeControllerMethod(m_controller, method, params, &result, &error);

    if (ok) {
        response["result"] = QJsonValue::fromVariant(result);
    } else {
        response["error"] = error;
    }

    // Trace d'audit : les appels pilotés par ce connecteur apparaissent dans le
    // même flux scan_telemetry.jsonl que le reste de l'app (demande explicite
    // de l'utilisateur : "en plus des log ca permet que tu voye..."), avec le
    // nom de méthode et le résultat — pas les params complets (peuvent contenir
    // des valeurs mémoire arbitraires, pas la peine de les dupliquer ici).
    QVariantMap auditPayload;
    auditPayload["method"] = method;
    auditPayload["argCount"] = params.size();
    auditPayload["success"] = ok;
    if (!ok) {
        auditPayload["error"] = error;
    }
    m_controller->logAiAudit(QStringLiteral("automation_pipe_call"), auditPayload);

    writeResponse(socket, QJsonDocument(response).toJson(QJsonDocument::Compact));
}

} // namespace killengine
