#include "clr_inspector_bridge.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "inject/dll_injector.h"
#include "localization/localization.h"
#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "patch/instruction_patch_suggester.h"
#include "process/process_suspend.h"
#include "scanner/scan_types.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <utility>

namespace killengine {

namespace {

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok) {
        return false;
    }
    *address = parsed;
    return true;
}

} // namespace

ClrInspectorBridge::ClrInspectorBridge(
    const killcore::ProcessHandle& handle,
    TelemetryCallback telemetry,
    IsAttachedCallback isAttached,
    PidCallback pid,
    ProcessNameCallback processName)
    : m_handle(handle),
      m_appendScanTelemetry(std::move(telemetry)),
      m_isAttached(std::move(isAttached)),
      m_pid(std::move(pid)),
      m_processName(std::move(processName)) {
}

ClrInspectorBridge::~ClrInspectorBridge() {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        callClrInspectorRpc(QStringLiteral("shutdown"), {}, 1000);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            if (!m_process->waitForFinished(1000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
    }
}

QString ClrInspectorBridge::clrInspectorPipeName() const {
    return QStringLiteral("KillEngineClrInspectorPipe_%1").arg(QCoreApplication::applicationPid());
}

QString ClrInspectorBridge::findClrInspectorExecutable() const {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("KillEngineClrInspector.exe"),
        appDir.filePath("tools/clr_inspector/KillEngineClrInspector.exe"),
        appDir.filePath("../../tools/clr_inspector/KillEngineClrInspector/bin/Release/net8.0/KillEngineClrInspector.exe"),
        appDir.filePath("../../tools/clr_inspector/KillEngineClrInspector/bin/Debug/net8.0/KillEngineClrInspector.exe"),
        QDir::current().filePath("tools/clr_inspector/KillEngineClrInspector/bin/Release/net8.0/KillEngineClrInspector.exe"),
        QDir::current().filePath("tools/clr_inspector/KillEngineClrInspector/bin/Debug/net8.0/KillEngineClrInspector.exe"),
    };
    for (const auto& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

bool ClrInspectorBridge::ensureClrInspectorStarted(QString* error) {
    if (m_process && m_process->state() != QProcess::NotRunning) {
        return true;
    }

    const QString executable = findClrInspectorExecutable();
    if (executable.isEmpty()) {
        if (error) {
            *error = KE_TXT(
                "KillEngineClrInspector.exe introuvable. Construis le helper avec "
                "scripts/build-clr-inspector.ps1 -Configuration Release avant d'utiliser l'inspecteur CLR.",
                "KillEngineClrInspector.exe not found. Build the helper with "
                "scripts/build-clr-inspector.ps1 -Configuration Release before using the CLR Inspector.");
        }
        return false;
    }

    m_process = std::make_unique<QProcess>();
    m_process->setProgram(executable);
    m_process->setWorkingDirectory(QFileInfo(executable).absolutePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("KILLENGINE_CLR_INSPECTOR_PIPE_NAME"), clrInspectorPipeName());
    m_process->setProcessEnvironment(env);
    m_process->start();
    if (!m_process->waitForStarted(3000)) {
        if (error) {
            *error = KE_TXT("Impossible de demarrer KillEngineClrInspector.exe : %1",
                            "Unable to start KillEngineClrInspector.exe: %1")
                .arg(m_process->errorString());
        }
        m_process.reset();
        return false;
    }

    KE_LOG_INFO() << "ClrInspector: started helper " << executable.toStdString()
                  << " pipe=" << clrInspectorPipeName().toStdString();
    return true;
}

QVariantMap ClrInspectorBridge::callClrInspectorRpc(const QString& method, const QVariantList& params, int timeoutMs) {
    QVariantMap result;
    result["success"] = false;
    result["method"] = method;

    QString startError;
    if (!ensureClrInspectorStarted(&startError)) {
        result["error"] = startError;
        return result;
    }

    QJsonObject request;
    request["id"] = m_requestId++;
    request["method"] = method;
    request["params"] = QJsonArray::fromVariantList(params);
    QByteArray requestBytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    requestBytes.append('\n');

    QByteArray responseLine;

#ifdef Q_OS_WIN
    const QString pipePath = QStringLiteral("\\\\.\\pipe\\%1").arg(clrInspectorPipeName());
    const std::wstring pipePathW = pipePath.toStdWString();
    QElapsedTimer connectTimer;
    connectTimer.start();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    while (connectTimer.elapsed() < timeoutMs) {
        pipe = CreateFileW(
            pipePathW.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            break;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            result["error"] = KE_TXT("Ouverture du pipe ClrMD echouee (error=%1).",
                                     "Couldn't open the ClrMD pipe (error=%1).").arg(err);
            return result;
        }
        const int remainingMs = timeoutMs - static_cast<int>(connectTimer.elapsed());
        if (remainingMs <= 0) {
            break;
        }
        WaitNamedPipeW(pipePathW.c_str(), static_cast<DWORD>(std::min(250, remainingMs)));
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        result["error"] = KE_TXT("Pipe ClrMD indisponible : timeout sur %1",
                                 "ClrMD pipe unavailable: timeout on %1").arg(pipePath);
        return result;
    }

    DWORD written = 0;
    if (!WriteFile(pipe, requestBytes.constData(), static_cast<DWORD>(requestBytes.size()), &written, nullptr)
        || written != static_cast<DWORD>(requestBytes.size())) {
        const DWORD err = GetLastError();
        CloseHandle(pipe);
        result["error"] = KE_TXT("Ecriture vers le pipe ClrMD echouee (error=%1).",
                                 "Couldn't write to the ClrMD pipe (error=%1).").arg(err);
        return result;
    }

    char buffer[512];
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        DWORD bytesRead = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            responseLine.append(buffer, static_cast<qsizetype>(bytesRead));
            const qsizetype newline = responseLine.indexOf('\n');
            if (newline >= 0) {
                responseLine = responseLine.left(newline).trimmed();
                break;
            }
            if (bytesRead == 0) {
                break;
            }
            continue;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
            responseLine.append(buffer, static_cast<qsizetype>(bytesRead));
            continue;
        }
        if (err == ERROR_BROKEN_PIPE) {
            break;
        }
        CloseHandle(pipe);
        result["error"] = KE_TXT("Lecture du pipe ClrMD echouee (error=%1).",
                                 "Couldn't read from the ClrMD pipe (error=%1).").arg(err);
        return result;
    }
    CloseHandle(pipe);
#else
    result["error"] = KE_TXT("Inspecteur CLR disponible uniquement sur Windows pour l'instant.",
                             "CLR Inspector is only available on Windows for now.");
    return result;
#endif

    if (responseLine.contains('\n')) {
        responseLine = responseLine.left(responseLine.indexOf('\n')).trimmed();
    } else {
        responseLine = responseLine.trimmed();
    }

    if (responseLine.isEmpty()) {
        result["error"] = KE_TXT("Pas de reponse du helper ClrMD.", "No response from the ClrMD helper.");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument responseDoc = QJsonDocument::fromJson(responseLine, &parseError);
    if (parseError.error != QJsonParseError::NoError || !responseDoc.isObject()) {
        result["error"] = KE_TXT("Reponse ClrMD JSON invalide : %1",
                                 "Invalid ClrMD JSON response: %1").arg(parseError.errorString());
        result["raw"] = QString::fromUtf8(responseLine);
        return result;
    }

    const QJsonObject response = responseDoc.object();
    if (response.contains("error")) {
        result["error"] = response.value("error").toString();
        return result;
    }

    result["success"] = true;
    result["result"] = response.value("result").toVariant();
    return result;
}

QVariantMap ClrInspectorBridge::getClrInspectorStatus() const {
    QVariantMap result;
    result["success"] = true;
    result["available"] = !findClrInspectorExecutable().isEmpty();
    result["helperPath"] = findClrInspectorExecutable();
    result["pipeName"] = clrInspectorPipeName();
    result["running"] = m_process && m_process->state() != QProcess::NotRunning;
    result["attachedProcess"] = m_isAttached();
    result["pid"] = m_pid();
    result["processName"] = m_processName();
    return result;
}

QVariantMap ClrInspectorBridge::attachClrInspector() {
    if (!m_isAttached() || m_pid() <= 0) {
        return {{"success", false}, {"error", QStringLiteral("Aucun processus attache.")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("attach"), {m_pid()}, 10000);
    m_appendScanTelemetry(QStringLiteral("clr_inspector_attach"), {
        {"success", response.value("success").toBool()},
        {"pid", m_pid()},
        {"processName", m_processName()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::detachClrInspector() {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        return {{"success", true}, {"result", QStringLiteral("not running")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("detach"), {}, 3000);
    m_appendScanTelemetry(QStringLiteral("clr_inspector_detach"), {
        {"success", response.value("success").toBool()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::shutdownClrInspector() {
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        m_process.reset();
        return {{"success", true}, {"result", QStringLiteral("not running")}};
    }
    QVariantMap response = callClrInspectorRpc(QStringLiteral("shutdown"), {}, 2000);
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->waitForFinished(1000);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            m_process->waitForFinished(1000);
        }
    }
    m_process.reset();
    return response;
}

QVariantMap ClrInspectorBridge::flushClrInspectorCache() {
    return callClrInspectorRpc(QStringLiteral("flushCachedData"), {}, 5000);
}

QVariantMap ClrInspectorBridge::findClrObjectsByType(const QString& typeSubstring) {
    const QString filter = typeSubstring.trimmed().isEmpty()
        ? QStringLiteral("KillEngine.ClrTestTarget")
        : typeSubstring.trimmed();
    return callClrInspectorRpc(QStringLiteral("findObjectsByType"), {filter}, 15000);
}

QVariantMap ClrInspectorBridge::findClrObjectsByFieldValue(const QString& typeSubstring, const QString& fieldName, const QString& expectedValue, int maxResults) {
    const QString typeFilter = typeSubstring.trimmed();
    const QString field = fieldName.trimmed();
    const QString value = expectedValue.trimmed();
    if (typeFilter.isEmpty() || field.isEmpty() || value.isEmpty()) {
        return {{"success", false}, {"error", KE_TXT("Type, champ et valeur requis pour le locator CLR.",
                                                     "Type, field and value are required for the CLR locator.")}};
    }
    const int boundedMax = std::clamp(maxResults, 1, 200);
    QVariantMap response = callClrInspectorRpc(QStringLiteral("findObjectsByFieldValue"), {typeFilter, field, value, boundedMax}, 20000);
    m_appendScanTelemetry(QStringLiteral("clr_inspector_field_locator"), {
        {"success", response.value("success").toBool()},
        {"typeSubstring", typeFilter},
        {"fieldName", field},
        {"matchesReturned", response.value("result").toMap().value("matchesReturned").toInt()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::readClrObject(const QString& addressHex) {
    const QString address = addressHex.trimmed();
    if (address.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Adresse objet CLR manquante.")}};
    }
    return callClrInspectorRpc(QStringLiteral("readObject"), {address}, 10000);
}

QVariantMap ClrInspectorBridge::writeClrPrimitiveField(const QString& objectAddressHex, const QString& fieldName, const QString& value) {
    const QString address = objectAddressHex.trimmed();
    const QString field = fieldName.trimmed();
    const QString text = value.trimmed();
    if (address.isEmpty() || field.isEmpty() || text.isEmpty()) {
        return {{"success", false}, {"error", KE_TXT("Adresse objet, champ et valeur requis.",
                                                     "Object address, field and value are required.")}};
    }

    QVariantMap response = callClrInspectorRpc(QStringLiteral("writePrimitiveField"), {address, field, text}, 10000);
    m_appendScanTelemetry(QStringLiteral("clr_inspector_write_field"), {
        {"success", response.value("success").toBool()},
        {"objectAddress", address},
        {"fieldName", field},
        {"verified", response.value("result").toMap().value("verified").toBool()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::writeClrPrimitivePath(const QString& objectAddressHex, const QString& path, const QString& value) {
    const QString address = objectAddressHex.trimmed();
    const QString pathText = path.trimmed();
    const QString text = value.trimmed();
    if (address.isEmpty() || pathText.isEmpty() || text.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Adresse objet, chemin et valeur requis.")}};
    }

    QVariantMap response = callClrInspectorRpc(QStringLiteral("writePrimitivePath"), {address, pathText, text}, 10000);
    m_appendScanTelemetry(QStringLiteral("clr_inspector_write_path"), {
        {"success", response.value("success").toBool()},
        {"objectAddress", address},
        {"path", pathText},
        {"verified", response.value("result").toMap().value("verified").toBool()},
        {"error", response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::writeClrPrimitivePathBatch(const QString& objectAddressHex, const QVariantList& operations) {
    const QString address = objectAddressHex.trimmed();
    if (address.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Adresse objet requise pour la transaction CLR.")}};
    }
    if (operations.isEmpty() || operations.size() > 32) {
        return {{"success", false}, {"error", QStringLiteral("La transaction CLR attend entre 1 et 32 operations.")}};
    }

    QVariantList sanitized;
    sanitized.reserve(operations.size());
    for (const QVariant& item : operations) {
        const QVariantMap op = item.toMap();
        const QString path = op.value(QStringLiteral("path")).toString().trimmed();
        const QString value = op.value(QStringLiteral("value")).toString().trimmed();
        if (path.isEmpty() || value.isEmpty()) {
            return {{"success", false}, {"error", QStringLiteral("Chaque operation CLR doit fournir path et value.")}};
        }
        sanitized.append(QVariantMap{{QStringLiteral("path"), path}, {QStringLiteral("value"), value}});
    }

    QVariantMap response = callClrInspectorRpc(QStringLiteral("writePrimitivePathBatch"), {address, sanitized}, 20000);
    const QVariantMap result = response.value(QStringLiteral("result")).toMap();
    m_appendScanTelemetry(QStringLiteral("clr_inspector_write_path_batch"), {
        {"success", response.value("success").toBool() && result.value("success").toBool()},
        {"objectAddress", address},
        {"operationCount", sanitized.size()},
        {"rolledBack", result.value("rolledBack").toBool()},
        {"error", response.value("error").toString().isEmpty() ? result.value("error").toString() : response.value("error").toString()},
    });
    return response;
}

QVariantMap ClrInspectorBridge::writeClrPrimitivePathByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QString& path, const QString& value) {
    // PHASE 59 : meme patron que resolveProfileTarget/activateProfileTarget
    // pour LocatorKind::ClrField -- relocalise l'objet root juste avant
    // d'agir (findClrObjectsByFieldValue), jamais une adresse memorisee par
    // l'appelant qui pourrait avoir bouge apres un GC compactant.
    QVariantMap locatorProbe = findClrObjectsByFieldValue(typeSubstring, identityField, identityValue, 2);
    if (!locatorProbe.value("success").toBool()) {
        const QString error = locatorProbe.value("error").toString();
        return {{"success", false}, {"error", error.isEmpty() ? QStringLiteral("Locator CLR introuvable. Attache d'abord le helper CLR.") : error}};
    }
    const QVariantList matches = locatorProbe.value("result").toMap().value("matches").toList();
    if (matches.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Aucun objet CLR ne correspond a ce locator.")}};
    }
    if (matches.size() > 1) {
        return {{"success", false}, {"error", QStringLiteral(
            "Locator ambigu : %1 objets correspondent a ce type+champ+valeur. Precise un champ d'identite plus selectif.")
            .arg(matches.size())}};
    }

    const QString resolvedAddress = matches.first().toMap().value("address").toString();
    QVariantMap result = writeClrPrimitivePath(resolvedAddress, path, value);
    result["relocated"] = true;
    result["resolvedAddress"] = resolvedAddress;
    result["typeSubstring"] = typeSubstring;
    result["identityField"] = identityField;
    result["identityValue"] = identityValue;
    return result;
}

QVariantMap ClrInspectorBridge::writeClrPrimitivePathBatchByLocator(const QString& typeSubstring, const QString& identityField, const QString& identityValue, const QVariantList& operations) {
    QVariantMap locatorProbe = findClrObjectsByFieldValue(typeSubstring, identityField, identityValue, 2);
    if (!locatorProbe.value("success").toBool()) {
        const QString error = locatorProbe.value("error").toString();
        return {{"success", false}, {"error", error.isEmpty() ? QStringLiteral("Locator CLR introuvable. Attache d'abord le helper CLR.") : error}};
    }
    const QVariantList matches = locatorProbe.value("result").toMap().value("matches").toList();
    if (matches.isEmpty()) {
        return {{"success", false}, {"error", QStringLiteral("Aucun objet CLR ne correspond a ce locator.")}};
    }
    if (matches.size() > 1) {
        return {{"success", false}, {"error", QStringLiteral(
            "Locator ambigu : %1 objets correspondent a ce type+champ+valeur. Precise un champ d'identite plus selectif.")
            .arg(matches.size())}};
    }

    const QString resolvedAddress = matches.first().toMap().value("address").toString();
    QVariantMap result = writeClrPrimitivePathBatch(resolvedAddress, operations);
    result["relocated"] = true;
    result["resolvedAddress"] = resolvedAddress;
    result["typeSubstring"] = typeSubstring;
    result["identityField"] = identityField;
    result["identityValue"] = identityValue;
    return result;
}

QVariantMap ClrInspectorBridge::writeClrPrimitivePathBatchAtomic(const QString& objectAddressHex, const QVariantList& operations) {
    // PHASE 59 : meme patron que writeMemoryValuesAtomic -- suspend toutes
    // les threads du process attache (sauf le thread appelant, voir
    // ProcessThreadsSuspendGuard) pendant TOUT l'appel RPC vers le helper
    // ClrMD, pas seulement autour d'un WriteProcessMemory isole. Le helper
    // .NET (ClrSession.WritePrimitivePathBatch) n'a besoin de rien savoir de
    // cette suspension -- c'est une garantie apportee entierement par
    // l'appelant natif. Best-effort honnete, pas une atomicite parfaite :
    // voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour le risque documente
    // (interaction possible avec un GC/JIT qui attendait un signal d'une
    // thread desormais suspendue).
    if (!m_isAttached() || m_pid() <= 0) {
        return {{"success", false}, {"error", QStringLiteral("Aucun processus attache.")}};
    }

    int suspendedThreadCount = 0;
    QVariantMap response;
    {
        killcore::ProcessThreadsSuspendGuard suspendGuard(static_cast<uint32_t>(m_pid()));
        suspendedThreadCount = suspendGuard.suspendedCount();
        response = writeClrPrimitivePathBatch(objectAddressHex, operations);
        // suspendGuard sort de portee ici -> reprend toutes les threads
        // suspendues AVANT de retourner le resultat a l'appelant (pas de
        // travail superflu pendant que la cible est figee).
    }

    response["suspendedThreadCount"] = suspendedThreadCount;
    response["suspendedDuringTransaction"] = true;
    return response;
}

QVariantMap ClrInspectorBridge::enumerateClrRoots(const QString& typeSubstring) {
    const QString filter = typeSubstring.trimmed();
    QVariantList params;
    if (!filter.isEmpty()) {
        params.append(filter);
    }
    return callClrInspectorRpc(QStringLiteral("enumerateRoots"), params, 15000);
}

namespace {

void appendImm64(QByteArray* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out->append(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void appendImm32(QByteArray* out, int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
        out->append(static_cast<char>((bits >> (8 * i)) & 0xFF));
    }
}

// Shellcode x64 fixe pour appeler un setter d'instance CLR reellement JITte :
// pose "this" en RCX (+ la valeur en RDX si le setter attend un parametre)
// puis "call" l'adresse native deja resolue par le helper ClrMD. Design fige,
// ne pas devier sans mettre a jour docs/KILLENGINE_CLR_INSPECTOR_SPEC.md.
// 0x28 = 0x20 shadow space (convention d'appel x64 Windows) + 8 octets pour
// garder RSP aligne sur 16 octets a l'entree du "call".
//
// PHASE 59 -- parametre Single/Double : la convention d'appel x64 Windows
// passe un 2e argument FLOTTANT en XMM1, pas RDX. On charge quand meme le
// bit pattern IEEE754 dans RDX (immediate 64 bits, deja zero-etendu pour un
// float 32 bits par encodeInstanceMethodParameterImmediate) puis on copie
// RDX -> XMM1 via "movq xmm1, rdx" (66 48 0F 6E CA). RDX porte alors une
// valeur non pertinente pour l'appelee (registre caller-saved, sans risque)
// -- seul XMM1 est lu par le JIT pour un parametre float/double.
//
// PHASE 227 -- parametre STRUCT DE 9+ OCTETS ("structByRefBytes" non vide) :
// la convention d'appel x64 Windows passe alors le struct PAR POINTEUR CACHE
// vers une copie fournie par l'appelant (aucune variante "paire de
// registres" sur cette ABI, contrairement a System V/Linux) -- RDX doit donc
// porter une ADRESSE, pas les octets eux-memes. On ecrit les octets du
// struct A LA SUITE de ce shellcode, DANS LE MEME buffer injecte par
// killcore::injectShellcode (qui alloue une seule region pour tout le
// contenu passe), et on charge RDX via un LEA RIP-RELATIF plutot qu'un
// immediate -- evite d'avoir a connaitre l'adresse choisie par
// VirtualAllocEx a l'avance (elle n'est connue qu'apres l'allocation, alors
// que ce shellcode est construit avant). Le displacement est un simple ecart
// d'octets DANS NOTRE PROPRE buffer (position des octets struct moins
// position juste apres l'instruction LEA), donc entierement connu a la
// construction, comme le reste de ce shellcode "fixe". Verifie bout-en-bout
// par injection reelle (pas suppose depuis la doc ABI seule) -- voir
// ResolveInstanceMethodAddress_ThenRealShellcodeCall_WithLargeStructParameter.
QByteArray buildCallInstanceMethodShellcode(
    uint64_t objectAddress, bool hasParam, uint64_t paramImmediate, uint64_t nativeCodeAddress,
    bool paramIsFloat = false, const QByteArray& structByRefBytes = QByteArray()) {
    QByteArray code;
    code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0x83));
    code.append(static_cast<char>(0xEC)); code.append(static_cast<char>(0x28));   // sub rsp, 0x28

    code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0xB9));   // mov rcx, <objectAddress>
    appendImm64(&code, objectAddress);

    int leaDisplacementPatchOffset = -1;
    if (hasParam) {
        if (!structByRefBytes.isEmpty()) {
            code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0x8D));
            code.append(static_cast<char>(0x15));                                 // lea rdx, [rip+disp32]
            leaDisplacementPatchOffset = code.size();
            appendImm32(&code, 0); // patch une fois la position des octets struct connue, plus bas
        } else {
            code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0xBA)); // mov rdx, <valeur / bit pattern IEEE754>
            appendImm64(&code, paramImmediate);

            if (paramIsFloat) {
                code.append(static_cast<char>(0x66)); code.append(static_cast<char>(0x48));
                code.append(static_cast<char>(0x0F)); code.append(static_cast<char>(0x6E));
                code.append(static_cast<char>(0xCA));                                  // movq xmm1, rdx
            }
        }
    }

    code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0xB8));   // mov rax, <nativeCodeAddress>
    appendImm64(&code, nativeCodeAddress);

    code.append(static_cast<char>(0xFF)); code.append(static_cast<char>(0xD0));   // call rax

    code.append(static_cast<char>(0x48)); code.append(static_cast<char>(0x83));
    code.append(static_cast<char>(0xC4)); code.append(static_cast<char>(0x28));   // add rsp, 0x28

    code.append(static_cast<char>(0x33)); code.append(static_cast<char>(0xC0));   // xor eax, eax
    code.append(static_cast<char>(0xC3));                                        // ret

    if (leaDisplacementPatchOffset >= 0) {
        const int structDataOffset = code.size(); // les octets struct suivent immediatement le "ret"
        const int32_t disp32 = static_cast<int32_t>(structDataOffset - (leaDisplacementPatchOffset + 4));
        std::memcpy(code.data() + leaDisplacementPatchOffset, &disp32, sizeof(disp32));
        code.append(structByRefBytes);
    }

    return code;
}

// Convertit le nom de type CLR renvoye par resolveInstanceMethodAddress (ex:
// "Int32") vers le token attendu par killcore::parseValueType. Meme
// perimetre que le helper .NET (bool + entiers 8/16/32/64 signes/non-signes
// + Single/Double depuis PHASE 59) : killcore::ValueType n'a pas de variante
// booleenne, "Boolean" est donc mappe sur uint8 avec normalisation texte
// true/false -> 1/0 avant de deleguer au parseur numerique existant (pas de
// parseur booleen reimplemente).
bool clrParameterTypeToKillcoreToken(const QString& clrTypeName, QString* token, bool* isBoolean) {
    static const QHash<QString, QString> table = {
        {QStringLiteral("SByte"), QStringLiteral("int8")}, {QStringLiteral("Byte"), QStringLiteral("uint8")},
        {QStringLiteral("Int16"), QStringLiteral("int16")}, {QStringLiteral("UInt16"), QStringLiteral("uint16")},
        {QStringLiteral("Int32"), QStringLiteral("int32")}, {QStringLiteral("UInt32"), QStringLiteral("uint32")},
        {QStringLiteral("Int64"), QStringLiteral("int64")}, {QStringLiteral("UInt64"), QStringLiteral("uint64")},
        {QStringLiteral("Single"), QStringLiteral("float32")}, {QStringLiteral("Double"), QStringLiteral("float64")},
    };
    if (clrTypeName == QStringLiteral("Boolean")) {
        *isBoolean = true;
        *token = QStringLiteral("uint8");
        return true;
    }
    *isBoolean = false;
    const auto it = table.constFind(clrTypeName);
    if (it == table.constEnd()) {
        return false;
    }
    *token = it.value();
    return true;
}

// Parse valueText selon le token killcore (deja normalise depuis le type CLR
// resolu) en reutilisant EXACTEMENT le meme parseur numerique que
// writeMemoryValueConfirmed/writeMemoryValuesAtomic (killcore::parseValueType
// + killcore::parseScanValue + killcore::scanValueToBytes), puis etend les
// octets obtenus (little-endian natif x64) sur 64 bits -- sign-extend pour
// les types signes, zero-extend pour les non-signes (et pour le bool mappe
// sur uint8), pour construire l'immediate RDX du shellcode.
bool encodeInstanceMethodParameterImmediate(const QString& valueText, const QString& killcoreToken, bool isBoolean, uint64_t* outImmediate, QString* error) {
    QString normalizedText = valueText.trimmed();
    if (isBoolean) {
        const QString lower = normalizedText.toLower();
        if (lower == QStringLiteral("true") || lower == QStringLiteral("1")) {
            normalizedText = QStringLiteral("1");
        } else if (lower == QStringLiteral("false") || lower == QStringLiteral("0")) {
            normalizedText = QStringLiteral("0");
        } else {
            if (error) *error = QStringLiteral("Valeur booleenne invalide : '%1' (attendu true/false/1/0).").arg(valueText);
            return false;
        }
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(killcoreToken, &type)) {
        if (error) *error = QStringLiteral("Type de parametre non reconnu : %1").arg(killcoreToken);
        return false;
    }

    killcore::ScanValue scanValue;
    QString parseError;
    if (!killcore::parseScanValue(normalizedText, type, &scanValue, &parseError)) {
        if (error) *error = parseError;
        return false;
    }

    const QByteArray bytes = killcore::scanValueToBytes(scanValue);
    uint64_t immediate = 0;
    const size_t copyLen = std::min<size_t>(static_cast<size_t>(bytes.size()), sizeof(immediate));
    std::memcpy(&immediate, bytes.constData(), copyLen);

    const bool isSigned = (type == killcore::ValueType::Int8 || type == killcore::ValueType::Int16
        || type == killcore::ValueType::Int32 || type == killcore::ValueType::Int64);
    if (isSigned && bytes.size() < 8 && bytes.size() > 0) {
        const bool negative = (static_cast<uint8_t>(bytes[bytes.size() - 1]) & 0x80) != 0;
        if (negative) {
            for (int i = static_cast<int>(bytes.size()); i < 8; ++i) {
                immediate |= (static_cast<uint64_t>(0xFF) << (8 * i));
            }
        }
    }

    *outImmediate = immediate;
    return true;
}

// PHASE 76 : meme table que clrParameterTypeToKillcoreToken ci-dessus, mais
// indexee par nom de ClrElementType (renvoye pour chaque CHAMP d'un
// parametre struct par ClrSession.ResolveInstanceMethodAddress -- ex.
// "Int32","Float","Boolean") plutot que par nom de TYPE CLR complet (utilise
// pour un parametre primitif DIRECT -- ex. "Int32","Single","Boolean"). Les
// deux nomenclatures different pour SByte/Byte ("Int8"/"UInt8" cote
// ElementType) et Single ("Float" cote ElementType) -- reutiliser
// directement clrParameterTypeToKillcoreToken pour un champ de struct
// donnerait un mauvais token pour ces trois cas precis.
bool clrElementTypeNameToKillcoreToken(const QString& elementTypeName, QString* token, bool* isBoolean) {
    static const QHash<QString, QString> table = {
        {QStringLiteral("Int8"), QStringLiteral("int8")}, {QStringLiteral("UInt8"), QStringLiteral("uint8")},
        {QStringLiteral("Int16"), QStringLiteral("int16")}, {QStringLiteral("UInt16"), QStringLiteral("uint16")},
        {QStringLiteral("Int32"), QStringLiteral("int32")}, {QStringLiteral("UInt32"), QStringLiteral("uint32")},
        {QStringLiteral("Int64"), QStringLiteral("int64")}, {QStringLiteral("UInt64"), QStringLiteral("uint64")},
        {QStringLiteral("Float"), QStringLiteral("float32")}, {QStringLiteral("Double"), QStringLiteral("float64")},
    };
    if (elementTypeName == QStringLiteral("Boolean")) {
        *isBoolean = true;
        *token = QStringLiteral("uint8");
        return true;
    }
    *isBoolean = false;
    const auto it = table.constFind(elementTypeName);
    if (it == table.constEnd()) {
        return false;
    }
    *token = it.value();
    return true;
}

// PHASE 76 (registre, <=8 octets) / PHASE 227 (pointeur cache, >8 octets) :
// construit les octets bruts d'un parametre STRUCT dont ClrSession.
// ResolveInstanceMethodAddress a deja valide le perimetre cote helper (tous
// les champs primitifs) -- "structFields" porte le layout (nom/elementType/
// offset/size) de chaque champ, "bufferSize" la taille totale deja calculee
// cote helper (parameterStructSize). Reutilise le format de saisie deja
// retenu pour l'ecriture d'un element struct de tableau ENTIER (ClrSession.
// WriteIndexedStructValue) : "Champ1=Valeur1,Champ2=Valeur2", tous les champs
// requis -- composition des octets aux bons offsets. L'appelant choisit quoi
// faire du resultat selon parameterStructPassedByRef : copier dans
// l'immediate RDX (<=8 octets, mecanisme d'origine PHASE 76) ou l'ecrire tel
// quel a la suite du shellcode et charger RDX via LEA RIP-relatif (PHASE 227,
// voir buildCallInstanceMethodShellcode).
bool encodeStructParameterBytes(
    const QVariantList& structFields, const QString& valueText, int bufferSize, QByteArray* outBytes, QString* error) {
    QHash<QString, QString> assignments;
    const QStringList parts = valueText.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const int separator = part.indexOf(QLatin1Char('='));
        const QString trimmedPart = part.trimmed();
        if (separator <= 0) {
            if (error) *error = QStringLiteral("Format d'ecriture struct invalide (attendu \"Champ=Valeur\") : '%1'.").arg(trimmedPart);
            return false;
        }
        const QString name = part.left(separator).trimmed();
        const QString value = part.mid(separator + 1).trimmed();
        if (name.isEmpty() || value.isEmpty()) {
            if (error) *error = QStringLiteral("Format d'ecriture struct invalide (attendu \"Champ=Valeur\") : '%1'.").arg(trimmedPart);
            return false;
        }
        assignments.insert(name, value);
    }
    if (assignments.isEmpty()) {
        if (error) *error = QStringLiteral("Parametre struct : aucune assignation fournie (format attendu \"Champ1=Valeur1,Champ2=Valeur2\").");
        return false;
    }

    QSet<QString> knownFieldNames;
    for (const QVariant& fieldVariant : structFields) {
        knownFieldNames.insert(fieldVariant.toMap().value("name").toString());
    }
    QStringList unknown;
    for (auto it = assignments.constBegin(); it != assignments.constEnd(); ++it) {
        if (!knownFieldNames.contains(it.key())) {
            unknown.append(it.key());
        }
    }
    if (!unknown.isEmpty()) {
        if (error) *error = QStringLiteral("Champ(s) inconnu(s) pour le parametre struct : %1.").arg(unknown.join(QStringLiteral(", ")));
        return false;
    }

    if (bufferSize <= 0 || bufferSize > 512) {
        if (error) *error = QStringLiteral("Parametre struct : taille de buffer invalide (%1).").arg(bufferSize);
        return false;
    }
    QByteArray buffer(bufferSize, '\0');
    QStringList missing;
    for (const QVariant& fieldVariant : structFields) {
        const QVariantMap field = fieldVariant.toMap();
        const QString fieldName = field.value("name").toString();
        if (!assignments.contains(fieldName)) {
            missing.append(fieldName);
            continue;
        }

        QString killcoreToken;
        bool isBoolean = false;
        const QString clrElementType = field.value("elementType").toString();
        if (!clrElementTypeNameToKillcoreToken(clrElementType, &killcoreToken, &isBoolean)) {
            if (error) *error = QStringLiteral("Champ struct de type non supporte : %1 (%2).").arg(fieldName, clrElementType);
            return false;
        }

        uint64_t fieldImmediate = 0;
        QString parseError;
        if (!encodeInstanceMethodParameterImmediate(assignments.value(fieldName), killcoreToken, isBoolean, &fieldImmediate, &parseError)) {
            if (error) *error = QStringLiteral("Champ %1 : %2").arg(fieldName, parseError);
            return false;
        }

        const int offset = field.value("offset").toInt();
        const int fieldSize = field.value("size").toInt();
        if (offset < 0 || fieldSize <= 0 || fieldSize > 8 || offset + fieldSize > buffer.size()) {
            if (error) *error = QStringLiteral("Champ struct %1 : offset/taille invalide (%2/%3).").arg(fieldName).arg(offset).arg(fieldSize);
            return false;
        }
        for (int i = 0; i < fieldSize; ++i) {
            buffer[offset + i] = static_cast<char>((fieldImmediate >> (8 * i)) & 0xFF);
        }
    }

    if (!missing.isEmpty()) {
        if (error) *error = QStringLiteral(
            "Champ(s) manquant(s) pour le parametre struct : %1 -- l'appel d'un setter a parametre struct exige "
            "tous ses champs (format \"Champ1=Valeur1,Champ2=Valeur2\").").arg(missing.join(QStringLiteral(", ")));
        return false;
    }

    *outBytes = buffer;
    return true;
}

} // namespace

QVariantMap ClrInspectorBridge::callClrInstanceMethod(const QString& objectAddressHex, const QString& methodName, const QString& valueText, const QString& valueType) {
    QVariantMap result;
    result["success"] = false;
    result["verified"] = false;
    result["objectAddress"] = objectAddressHex;
    result["methodName"] = methodName;

    const QString address = objectAddressHex.trimmed();
    const QString method = methodName.trimmed();
    if (address.isEmpty() || method.isEmpty()) {
        result["error"] = QStringLiteral("Adresse objet et nom de methode requis.");
        return result;
    }
    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = QStringLiteral("Aucun processus attache.");
        return result;
    }

    // 1) Resolution de l'adresse native deja JITtee via le helper ClrMD --
    // ClrMD ne peut jamais executer de code lui-meme (DAC passif), cet appel
    // se limite a une lecture. La fenetre entre cette resolution et
    // l'injection ci-dessous reste best-effort vis-a-vis du GC (comme le
    // reste de ce module pour les locators), pas une garantie absolue.
    QVariantMap resolveResponse = callClrInspectorRpc(QStringLiteral("resolveInstanceMethodAddress"), {address, method}, 10000);
    if (!resolveResponse.value("success").toBool()) {
        result["error"] = resolveResponse.value("error").toString();
        m_appendScanTelemetry(QStringLiteral("clr_inspector_call_instance_method"), {
            {"success", false}, {"objectAddress", address}, {"methodName", method}, {"error", result.value("error")},
        });
        return result;
    }

    const QVariantMap resolved = resolveResponse.value("result").toMap();
    const QString resolvedMethodName = resolved.value("methodName").toString();
    const QString nativeCodeAddressHex = resolved.value("nativeCodeAddress").toString();
    const QString parameterTypeName = resolved.value("parameterType").toString(); // vide si setter 0-arg
    const bool hasParam = !parameterTypeName.isEmpty();
    // Chantier "setters a parametre objet/string" : le helper ClrMD a deja
    // resolu si le parametre non primitif est un type REFERENCE (accepte)
    // ou un type VALEUR/struct (rejete cote helper, jamais retourne ici --
    // voir ClrSession.ResolveInstanceMethodAddress). Ce booleen pilote
    // uniquement comment ENCODER valueText (adresse hex brute en RDX, pas de
    // conversion IEEE754/entiere) -- il n'introduit aucune nouvelle logique
    // de validation de type ici, le helper reste l'autorite.
    const bool parameterIsReferenceType = resolved.value("parameterIsReferenceType").toBool();
    // PHASE 76 : chantier "setters a parametre struct" -- le helper ClrMD
    // accepte desormais un parametre struct dans le cas taille 1/2/4/8
    // octets + tous champs primitifs (voir ClrSession.
    // ResolveInstanceMethodAddress), et fournit le layout des champs
    // (nom/elementType/offset/size) necessaire pour composer l'immediate RDX
    // ci-dessous -- voir encodeStructParameterImmediate.
    const bool parameterIsStruct = resolved.value("parameterIsStruct").toBool();
    const QVariantList parameterStructFields = resolved.value("parameterStructFields").toList();
    const int parameterStructSize = resolved.value("parameterStructSize").toInt();
    // PHASE 227 : struct de plus de 8 octets -> passage par pointeur cache
    // (voir buildCallInstanceMethodShellcode) plutot que par registre.
    const bool parameterStructPassedByRef = resolved.value("parameterStructPassedByRef").toBool();
    result["methodName"] = resolvedMethodName;
    result["nativeCodeAddress"] = nativeCodeAddressHex;
    result["parameterType"] = parameterTypeName;
    result["parameterIsReferenceType"] = parameterIsReferenceType;
    result["parameterIsStruct"] = parameterIsStruct;
    result["parameterStructPassedByRef"] = parameterStructPassedByRef;

    uint64_t objectAddress = 0;
    uint64_t nativeCodeAddress = 0;
    if (!parseHexAddress(address, &objectAddress) || !parseHexAddress(nativeCodeAddressHex, &nativeCodeAddress)) {
        result["error"] = QStringLiteral("Adresse objet ou adresse native invalide apres resolution.");
        return result;
    }

    // 2) Si un parametre est attendu : parse valueText en octets bruts puis
    // en immediate 64 bits sign/zero-etendu. Le type CLR resolu (autoritaire,
    // via ClrMD) pilote l'encodage ; valueType (si fourni explicitement par
    // l'appelant) doit correspondre a la meme categorie, sinon rejet clair
    // plutot que d'injecter un shellcode avec une valeur mal typee.
    uint64_t paramImmediate = 0;
    // PHASE 59 : determine si le parametre resolu est Single/Double -- pilote
    // le choix RDX (entiers/bool) vs XMM1 (flottants) dans le shellcode ;
    // voir buildCallInstanceMethodShellcode.
    bool paramIsFloat = false;
    // PHASE 227 : non vide seulement pour un struct >8 octets -- les octets
    // sont alors ecrits a la suite du shellcode et RDX charge un pointeur
    // RIP-relatif vers eux, voir buildCallInstanceMethodShellcode.
    QByteArray structByRefBytes;
    if (hasParam) {
        if (valueText.trimmed().isEmpty()) {
            result["error"] = QStringLiteral("Ce setter attend un parametre (%1) mais aucune valeur n'a ete fournie.").arg(parameterTypeName);
            return result;
        }

        if (parameterIsReferenceType) {
            // Chantier "setters a parametre objet/string" : valueText est
            // une adresse hex (0x...) vers un objet/string DEJA EXISTANT sur
            // le tas managed -- PAS d'allocation d'un nouvel objet/string
            // (demanderait JIT_NewObj/FastAllocateString ou equivalents,
            // hors de portee, decision arbitree en amont). RDX porte
            // directement cette adresse : plus SIMPLE a encoder que le cas
            // float/double (pas de conversion IEEE754/entiere), voir
            // buildCallInstanceMethodShellcode.
            if (!valueType.trimmed().isEmpty()) {
                static const QSet<QString> acceptedReferenceValueTypeTokens = {
                    QStringLiteral("object"), QStringLiteral("reference"), QStringLiteral("ref"),
                    QStringLiteral("address"), QStringLiteral("pointer"),
                };
                if (!acceptedReferenceValueTypeTokens.contains(valueType.trimmed().toLower())) {
                    result["error"] = QStringLiteral(
                        "Type fourni ('%1') incoherent avec le parametre reel du setter (reference/objet, '%2').")
                        .arg(valueType, parameterTypeName);
                    return result;
                }
            }

            // "null"/"0" reste une valeur legitime (efface la reference,
            // meme convention que ClrSession.ParseReferenceValue deja
            // utilisee pour writePrimitivePath sur un changement de
            // reference) -- pas d'adresse a valider dans ce cas, 0 ne pointe
            // vers aucun objet par definition.
            const QString trimmedValue = valueText.trimmed();
            const bool isNullReference = trimmedValue == QStringLiteral("0")
                || trimmedValue.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0;

            uint64_t paramObjectAddress = 0;
            if (!isNullReference) {
                if (!parseHexAddress(trimmedValue, &paramObjectAddress)) {
                    result["error"] = QStringLiteral(
                        "Valeur invalide pour un parametre objet/reference ('%1') : attendu une adresse hex (0x...) "
                        "d'un objet DEJA EXISTANT sur le tas, ou 'null'/'0' pour effacer la reference.").arg(valueText);
                    return result;
                }

                // Validation minimale AVANT de construire/injecter le
                // shellcode : ne fait pas confiance a une adresse arbitraire
                // fournie par l'appelant -- la resout via le helper ClrMD
                // (readObject, deja utilise plus bas pour la revalidation
                // post-appel) pour confirmer qu'elle pointe reellement vers
                // un objet CLR lisible.
                QVariantMap paramReadResponse = callClrInspectorRpc(QStringLiteral("readObject"), {trimmedValue}, 5000);
                if (!paramReadResponse.value("success").toBool()) {
                    result["error"] = QStringLiteral(
                        "Parametre objet invalide : l'adresse '%1' ne pointe pas vers un objet CLR lisible (%2).")
                        .arg(valueText, paramReadResponse.value("error").toString());
                    return result;
                }
            }

            paramImmediate = paramObjectAddress;
        } else if (parameterIsStruct) {
            // PHASE 76 (registre) / PHASE 227 (pointeur cache) : valueText
            // porte "Champ1=Valeur1,Champ2=Valeur2" (meme format que
            // ClrSession.WriteIndexedStructValue), valide et encode par
            // encodeStructParameterBytes a partir du layout resolu par le
            // helper. Si le struct tient dans un registre (<=8 octets),
            // les octets sont copies dans l'immediate RDX comme avant PHASE
            // 227 ; sinon ils restent dans structByRefBytes, ecrits a la
            // suite du shellcode (voir buildCallInstanceMethodShellcode).
            if (!valueType.trimmed().isEmpty()) {
                static const QSet<QString> acceptedStructValueTypeTokens = {
                    QStringLiteral("struct"), QStringLiteral("valuetype"),
                };
                if (!acceptedStructValueTypeTokens.contains(valueType.trimmed().toLower())) {
                    result["error"] = QStringLiteral(
                        "Type fourni ('%1') incoherent avec le parametre reel du setter (struct, '%2').")
                        .arg(valueType, parameterTypeName);
                    return result;
                }
            }

            QByteArray structBytes;
            QString parseError;
            if (!encodeStructParameterBytes(parameterStructFields, valueText, parameterStructSize, &structBytes, &parseError)) {
                result["error"] = parseError;
                return result;
            }
            if (parameterStructPassedByRef) {
                structByRefBytes = structBytes;
            } else {
                paramImmediate = 0;
                std::memcpy(&paramImmediate, structBytes.constData(),
                            std::min<size_t>(static_cast<size_t>(structBytes.size()), sizeof(paramImmediate)));
            }
        } else {
            QString killcoreToken;
            bool isBoolean = false;
            if (!clrParameterTypeToKillcoreToken(parameterTypeName, &killcoreToken, &isBoolean)) {
                result["error"] = QStringLiteral("Type de parametre CLR non supporte : %1.").arg(parameterTypeName);
                return result;
            }
            paramIsFloat = (killcoreToken == QStringLiteral("float32") || killcoreToken == QStringLiteral("float64"));
            if (!valueType.trimmed().isEmpty()) {
                const QString requested = valueType.trimmed().toLower();
                const bool matches = isBoolean
                    ? (requested == QStringLiteral("bool") || requested == QStringLiteral("boolean"))
                    : (requested == killcoreToken);
                if (!matches) {
                    result["error"] = QStringLiteral(
                        "Type fourni ('%1') incoherent avec le parametre reel du setter ('%2').").arg(valueType, parameterTypeName);
                    return result;
                }
            }

            QString parseError;
            if (!encodeInstanceMethodParameterImmediate(valueText, killcoreToken, isBoolean, &paramImmediate, &parseError)) {
                result["error"] = parseError;
                return result;
            }
        }
    }

    // 3) Construit le shellcode fixe et l'injecte via la primitive deja
    // existante et deja testee killcore::injectShellcode (ne reinvente pas
    // CreateRemoteThread/VirtualAllocEx).
    // m_handle est ouvert ReadOnly par attachProcess : l'injection exige
    // PROCESS_VM_OPERATION/VM_WRITE (VirtualAllocEx), d'ou un handle dedie
    // AllAccess -- meme patron que startInProcessExecuteWatch. Prouve en
    // conditions reelles le 25/08/2026 : VirtualAllocEx renvoyait
    // systematiquement error=5 sur m_handle ReadOnly, jamais visible des
    // tests .NET (NativeSetterInvoker ouvre son propre handle hors KillEngine).
    const QByteArray shellcode = buildCallInstanceMethodShellcode(objectAddress, hasParam, paramImmediate, nativeCodeAddress, paramIsFloat, structByRefBytes);
    killcore::ProcessHandle injectionHandle(static_cast<uint32_t>(m_pid()), killcore::ProcessAccess::AllAccess);
    if (!injectionHandle.isValid()) {
        result["error"] = QStringLiteral("Impossible d'ouvrir le processus avec les droits necessaires a l'injection (PROCESS_VM_OPERATION/PROCESS_VM_WRITE).");
        m_appendScanTelemetry(QStringLiteral("clr_inspector_call_instance_method"), {
            {"success", false}, {"objectAddress", address}, {"methodName", resolvedMethodName}, {"error", result.value("error")},
        });
        return result;
    }
    const auto injected = killcore::injectShellcode(injectionHandle, shellcode);
    if (!injected.success) {
        result["error"] = injected.error;
        m_appendScanTelemetry(QStringLiteral("clr_inspector_call_instance_method"), {
            {"success", false}, {"objectAddress", address}, {"methodName", resolvedMethodName}, {"error", injected.error},
        });
        return result;
    }

    // 4) Attend la fin du thread distant (timeout borne -- meme patron que
    // injectDll, voir core/inject/dll_injector.cpp), sans attente infinie.
    bool threadCompleted = false;
#ifdef Q_OS_WIN
    if (injected.remoteThreadHandle) {
        const HANDLE remoteThread = reinterpret_cast<HANDLE>(injected.remoteThreadHandle);
        const DWORD waitResult = WaitForSingleObject(remoteThread, 3000);
        threadCompleted = (waitResult == WAIT_OBJECT_0);
        CloseHandle(remoteThread);
    }
#endif

    // 5) Reverifie apres coup en relisant l'objet via le helper ClrMD
    // (comme writePrimitiveField). Contrairement a writePrimitiveField, cette
    // methode generique ignore quel champ backing le setter modifie -- donc
    // "verified" signifie ici "le thread distant a termine dans le delai ET
    // l'objet reste lisible ensuite" (preuve que l'appel n'a pas fait
    // planter/corrompre la cible), pas une comparaison de valeur exacte.
    QVariantMap rereadResponse = callClrInspectorRpc(QStringLiteral("readObject"), {address}, 5000);
    const bool rereadOk = rereadResponse.value("success").toBool();

    result["threadCompleted"] = threadCompleted;
    result["verified"] = threadCompleted && rereadOk;
    result["success"] = threadCompleted && rereadOk;
    if (!threadCompleted) {
        result["error"] = QStringLiteral("Le thread distant n'a pas termine dans le delai imparti (timeout 3000 ms).");
    } else if (!rereadOk) {
        result["error"] = QStringLiteral("Appel effectue mais la relecture de l'objet a echoue apres coup : %1")
            .arg(rereadResponse.value("error").toString());
    }

    m_appendScanTelemetry(QStringLiteral("clr_inspector_call_instance_method"), {
        {"success", result.value("success").toBool()},
        {"objectAddress", address},
        {"methodName", resolvedMethodName},
        {"hasParam", hasParam},
        {"verified", result.value("verified").toBool()},
        {"error", result.value("error").toString()},
    });
    return result;
}

QVariantMap ClrInspectorBridge::findClrGcRootPath(const QString& targetObjectAddressHex, int maxDepth, int maxRootsScanned) {
    const QString address = targetObjectAddressHex.trimmed();
    if (address.isEmpty()) {
        QVariantMap result;
        result["success"] = false;
        result["error"] = QStringLiteral("Adresse objet cible requise.");
        return result;
    }

    const int boundedDepth = std::clamp(maxDepth, 1, 12);
    const int boundedRoots = std::clamp(maxRootsScanned, 1, 20000);

    // BFS multi-source garantissant le plus court chemin (voir
    // ClrSession.FindGcRootPath cote helper, borne en interne par un budget
    // de temps ET un nombre total de noeuds visites -- un gros tas peut donc
    // toujours prendre plusieurs secondes). Timeout cote appelant natif
    // volontairement plus large que findObjectsByFieldValue (20000 ms)
    // puisque ce parcours reste structurellement plus couteux -- aligne sur
    // le budget de temps interne du helper (15s) plus une marge pour
    // l'aller-retour pipe.
    return callClrInspectorRpc(QStringLiteral("findGcRootPath"), {address, boundedDepth, boundedRoots}, 25000);
}

QVariantMap ClrInspectorBridge::generateClrObjectReport(const QString& objectAddressHex, int maxDepth, int maxNodes, bool includeGcRootChain) {
    const QString address = objectAddressHex.trimmed();
    if (address.isEmpty()) {
        QVariantMap result;
        result["success"] = false;
        result["error"] = QStringLiteral("Adresse objet requise.");
        return result;
    }

    // Bornes deliberement plus faibles que findClrGcRootPath : DescribeObject
    // (cote helper) fait un travail bien plus lourd par noeud (deballage
    // complet des champs/collections) que la simple enumeration de
    // references du chemin GCRoot -- voir ClrSession.GenerateObjectReport.
    // 0 = laisse le helper appliquer ses valeurs par defaut (3/50).
    const int boundedDepth = maxDepth <= 0 ? 0 : std::clamp(maxDepth, 1, 6);
    const int boundedNodes = maxNodes <= 0 ? 0 : std::clamp(maxNodes, 1, 300);

    return callClrInspectorRpc(
        QStringLiteral("generateObjectReport"),
        {address, boundedDepth, boundedNodes, includeGcRootChain},
        25000);
}

QVariantMap ClrInspectorBridge::disassembleClrMethod(const QString& objectAddressHex, const QString& methodName, int instructionCount) {
    QVariantMap result;
    result["success"] = false;
    result["objectAddress"] = objectAddressHex;
    result["methodName"] = methodName;

    const QString address = objectAddressHex.trimmed();
    const QString method = methodName.trimmed();
    if (address.isEmpty() || method.isEmpty()) {
        result["error"] = QStringLiteral("Adresse objet et nom de methode requis.");
        return result;
    }
    if (!m_isAttached() || !m_handle.isValid()) {
        result["error"] = QStringLiteral("Aucun processus attache.");
        return result;
    }

    // 1) Resolution de l'adresse native deja JITtee -- meme voie RPC que
    // callClrInstanceMethod (resolveInstanceMethodAddress). Duplique
    // volontairement l'appel minimal plutot que de factoriser : les deux
    // methodes divergent ensuite completement (l'une injecte/execute, l'autre
    // lit passivement), factoriser au-dela de cet appel commun ajouterait un
    // couplage pour peu de gain.
    QVariantMap resolveResponse = callClrInspectorRpc(QStringLiteral("resolveInstanceMethodAddress"), {address, method}, 10000);
    if (!resolveResponse.value("success").toBool()) {
        result["error"] = resolveResponse.value("error").toString();
        return result;
    }

    const QVariantMap resolved = resolveResponse.value("result").toMap();
    const QString resolvedMethodName = resolved.value("methodName").toString();
    const QString nativeCodeAddressHex = resolved.value("nativeCodeAddress").toString();
    result["methodName"] = resolvedMethodName;
    result["nativeCodeAddress"] = nativeCodeAddressHex;

    uint64_t nativeCodeAddress = 0;
    if (!parseHexAddress(nativeCodeAddressHex, &nativeCodeAddress)) {
        result["error"] = QStringLiteral("Adresse native invalide apres resolution.");
        return result;
    }

    // 2) Lit un buffer de bytes depuis l'adresse native resolue -- 15 octets
    // est la longueur MAX d'une instruction x64, donc instructionCount * 15
    // couvre le pire cas ; borne a une taille raisonnable (960 octets = 64
    // instructions max) pour ne jamais lire un buffer demesure.
    const int boundedCount = std::clamp(instructionCount, 1, 64);
    const int bufferSize = std::clamp(boundedCount * 15, 32, 960);

    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(nativeCodeAddress, static_cast<size_t>(bufferSize), 4096);
    if (!read.success && !read.partial) {
        result["error"] = QStringLiteral("Lecture du code natif JITte echouee : %1").arg(read.errorMessage);
        return result;
    }

    // 3) Desassemble en avant via le decodeur x64 existant (ne le
    // reimplemente pas) -- liste partielle possible si le buffer est trop
    // court ou si un decodage echoue avant instructionCount atteint.
    const QList<killcore::InstructionInfo> decoded = killcore::disassembleForwardWindow(read.data, boundedCount);

    QVariantList instructions;
    uint64_t cumulativeOffset = 0;
    for (const auto& instruction : decoded) {
        QVariantMap entry;
        entry["address"] = QStringLiteral("0x%1").arg(nativeCodeAddress + cumulativeOffset, 0, 16);
        entry["length"] = instruction.length;
        entry["disassembly"] = instruction.disassembly;
        entry["mnemonicHint"] = instruction.mnemonicHint;
        entry["rawBytesText"] = instruction.rawBytesText;
        entry["decoder"] = instruction.decoder;
        instructions.append(entry);
        cumulativeOffset += static_cast<uint64_t>(instruction.length);
    }

    result["success"] = !instructions.isEmpty();
    result["instructions"] = instructions;
    result["requestedInstructionCount"] = boundedCount;
    result["returnedInstructionCount"] = instructions.size();
    result["truncated"] = instructions.size() < boundedCount;
    result["bufferBytesRead"] = static_cast<int>(read.bytesRead);
    if (instructions.isEmpty()) {
        result["error"] = QStringLiteral("Aucune instruction decodee depuis l'adresse native resolue.");
    }
    return result;
}

} // namespace killengine
