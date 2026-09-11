#include "code_patch.h"

#include "localization/localization.h"
#include "logging/logger.h"
#include "memory/memory_writer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryFile>

namespace killcore {

namespace {

int hexNibble(QChar ch) {
    const ushort c = ch.toUpper().unicode();
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

// PHASE 122 : sur cette machine, VirtualProtectEx(PAGE_EXECUTE_READWRITE)
// cross-process est refusé (ERROR_ACCESS_DENIED) spécifiquement quand
// l'appelant est KillEngine.exe (binaire maison non signé) -- le même appel
// depuis powershell.exe (signé, système) réussit instantanément sur la même
// cible/adresse. Voir docs/POWER_UP_ROADMAP.md section O. `MemoryWriter::write`
// tente déjà l'écriture directe puis un VirtualProtectEx de secours ; quand les
// deux échouent avec ERROR_ACCESS_DENIED, on délègue l'opération à
// scripts/killengine-patch-relay.ps1 via un sous-processus powershell.exe --
// scopé aux patchs de code (petits, connus, un par un), pas à MemoryWriter
// générique.
constexpr uint32_t kErrorAccessDenied = 5;

#ifdef Q_OS_WIN

bool shouldTryRelayFallback(const MemoryWriteResult& write) {
    return !write.success && !write.protectionChanged && write.errorCode == kErrorAccessDenied;
}

QString findPatchRelayScript() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("scripts/killengine-patch-relay.ps1"),
        appDir.filePath("../scripts/killengine-patch-relay.ps1"),
        appDir.filePath("../../scripts/killengine-patch-relay.ps1"),
        QDir::current().filePath("scripts/killengine-patch-relay.ps1"),
    };
    for (const auto& candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.exists() && file.isFile()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

struct RelayPatchResult {
    bool success{false};
    QByteArray originalBytes;
    size_t bytesWritten{0};
    QString error;
};

/// Lance scripts/killengine-patch-relay.ps1 dans un sous-processus powershell.exe
/// (fichier de paramètres JSON temporaire, comme runLuaScriptProcess dans
/// application_controller.cpp) et interprète son JSON de résultat.
RelayPatchResult runPatchRelay(uint32_t pid, uint64_t address, const QByteArray& bytes) {
    RelayPatchResult result;

    const QString scriptPath = findPatchRelayScript();
    if (scriptPath.isEmpty()) {
        result.error = KE_TXT("Relais PowerShell introuvable (scripts/killengine-patch-relay.ps1 absent à côté de KillEngine.exe).",
            "PowerShell relay not found (scripts/killengine-patch-relay.ps1 missing next to KillEngine.exe).");
        return result;
    }

    QTemporaryFile paramsFile(QDir::temp().filePath("killengine-patch-relay-XXXXXX.json"));
    if (!paramsFile.open()) {
        result.error = KE_TXT("Impossible de créer le fichier de paramètres du relais : %1", "Unable to create the relay's parameters file: %1").arg(paramsFile.errorString());
        return result;
    }
    QJsonObject params;
    params["pid"] = static_cast<qint64>(pid);
    params["addressHex"] = QString::number(address, 16);
    params["patchBytesHex"] = QString::fromLatin1(bytes.toHex(' '));
    paramsFile.write(QJsonDocument(params).toJson(QJsonDocument::Compact));
    paramsFile.flush();
    const QString paramsPath = paramsFile.fileName();
    paramsFile.close();

    QProcess process;
    process.setProgram(QStringLiteral("powershell.exe"));
    process.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-File"), scriptPath,
        QStringLiteral("-ParamsFile"), paramsPath,
    });
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(3000)) {
        result.error = KE_TXT("Impossible de démarrer le relais PowerShell : %1", "Unable to start the PowerShell relay: %1").arg(process.errorString());
        return result;
    }
    if (!process.waitForFinished(10000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = KE_TXT("Le relais PowerShell n'a pas répondu (timeout de 10s).", "The PowerShell relay did not respond (10s timeout).");
        return result;
    }

    const QByteArray stdoutData = process.readAllStandardOutput();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(stdoutData, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        const QString stderrText = QString::fromUtf8(process.readAllStandardError());
        result.error = KE_TXT("Réponse du relais PowerShell illisible.%1", "Unreadable PowerShell relay response.%1")
                           .arg(stderrText.isEmpty() ? QString() : QStringLiteral(" stderr: %1").arg(stderrText));
        return result;
    }

    const QJsonObject obj = doc.object();
    result.success = obj.value("success").toBool();
    result.bytesWritten = static_cast<size_t>(obj.value("bytesWritten").toInt());
    const QString originalHex = QString(obj.value("originalBytesHex").toString()).remove(' ');
    if (!originalHex.isEmpty()) {
        result.originalBytes = QByteArray::fromHex(originalHex.toLatin1());
    }
    if (!result.success) {
        result.error = obj.value("error").toString();
        if (result.error.isEmpty()) {
            result.error = KE_TXT("Le relais PowerShell a échoué sans message d'erreur.", "The PowerShell relay failed with no error message.");
        }
    }
    return result;
}

#endif // Q_OS_WIN

CodePatchResult writePatchBytes(const ProcessHandle& process, uint64_t address, const QByteArray& bytes, bool verify) {
    CodePatchResult result;
    result.address = address;

    if (bytes.isEmpty()) {
        result.error = KE_TXT("Aucun byte de patch.", "No patch bytes.");
        return result;
    }

    MemoryWriter writer(process);
    const auto write = writer.write(address, bytes, verify);
    result.success = write.success;
    result.verified = write.verified;
    result.protectionChanged = write.protectionChanged;
    result.bytesWritten = write.bytesWritten;
    result.previousBytes = write.previousValue;
    result.error = write.errorMessage;

#ifdef Q_OS_WIN
    if (!result.success && shouldTryRelayFallback(write)) {
        KE_LOG_WARN() << "CodePatch: WriteProcessMemory et VirtualProtectEx directs refuses "
                          "(ERROR_ACCESS_DENIED) a 0x" << std::hex << address
                       << ", tentative via relais PowerShell externe...";
        const auto relay = runPatchRelay(process.pid(), address, bytes);
        if (relay.success) {
            result.success = true;
            result.protectionChanged = true;
            result.bytesWritten = relay.bytesWritten;
            if (result.previousBytes.isEmpty() && !relay.originalBytes.isEmpty()) {
                result.previousBytes = relay.originalBytes;
            }
            result.error.clear();

            if (verify) {
                MemoryReader reader(process);
                const auto after = reader.read(address, static_cast<size_t>(bytes.size()));
                result.verified = after.success && after.data == bytes;
                if (!result.verified) {
                    result.error = KE_TXT("Échec de la vérification d'écriture après le patch via relais (valeur écrasée ou illisible).",
                        "Write verification failed after relay patch (value was overwritten or unreadable).");
                }
            } else {
                result.verified = true;
            }
            KE_LOG_WARN() << "CodePatch: relais PowerShell a reussi a 0x" << std::hex << address;
        } else {
            result.error = KE_TXT("%1 Relais PowerShell (fallback) : %2", "%1 PowerShell relay (fallback): %2")
                                .arg(result.error, relay.error);
            KE_LOG_WARN() << "CodePatch: relais PowerShell a aussi echoue : " << relay.error.toStdString();
        }
    }
#endif

    return result;
}

} // namespace

PatchBytes parsePatchBytes(const QString& bytesText) {
    PatchBytes parsed;
    const QStringList tokens = bytesText.simplified().split(' ', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        parsed.error = KE_TXT("Bytes de patch vides.", "Empty patch bytes.");
        return parsed;
    }

    for (const QString& rawToken : tokens) {
        QString token = rawToken.trimmed();
        if (token.startsWith("0x", Qt::CaseInsensitive)) {
            token = token.mid(2);
        }
        if (token == "?" || token == "??") {
            parsed.error = KE_TXT("Un patch doit contenir des bytes exacts, pas de wildcard.", "A patch must contain exact bytes, no wildcards.");
            parsed.bytes.clear();
            return parsed;
        }
        if (token.size() != 2) {
            parsed.error = KE_TXT("Byte de patch invalide : '%1'. Utilise par exemple '90 90'.", "Invalid patch byte: '%1'. Use for example '90 90'.").arg(rawToken);
            parsed.bytes.clear();
            return parsed;
        }

        const int hi = hexNibble(token.at(0));
        const int lo = hexNibble(token.at(1));
        if (hi < 0 || lo < 0) {
            parsed.error = KE_TXT("Octet hex invalide : '%1'.", "Invalid hex byte: '%1'.").arg(rawToken);
            parsed.bytes.clear();
            return parsed;
        }

        parsed.bytes.append(static_cast<char>((hi << 4) | lo));
    }

    return parsed;
}

CodePatchResult applyCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& patchBytes, bool verify) {
    return writePatchBytes(process, address, patchBytes, verify);
}

CodePatchResult restoreCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& originalBytes, bool verify) {
    return writePatchBytes(process, address, originalBytes, verify);
}

} // namespace killcore
