#include "llama_runtime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>

namespace killai {

namespace {

int findJsonObjectEnd(const QString& text, int start) {
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (int i = start; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return -1;
}

} // namespace

bool LlamaRuntime::init() {
    const auto model = ModelLocator::findQwenGguf();
    const QString executable = findExecutable();

    m_info.modelPath = model.path;
    m_info.executablePath = executable;

    if (executable.isEmpty()) {
        m_info.available = false;
        m_info.errorMessage = "llama-cli executable not found. Set KILLENGINE_LLAMA_CLI.";
        return false;
    }

    if (!model.found) {
        m_info.available = false;
        m_info.errorMessage = model.errorMessage;
        return false;
    }

    m_info.available = true;
    m_info.errorMessage.clear();
    return true;
}

bool LlamaRuntime::isAvailable() const {
    return m_info.available;
}

LlamaRuntimeInfo LlamaRuntime::info() const {
    return m_info;
}

LlamaGenerationResult LlamaRuntime::planToolCall(const QString& query, const ToolRegistry& registry) const {
    LlamaGenerationResult result;
    if (!m_info.available) {
        result.errorMessage = m_info.errorMessage;
        return result;
    }

    QProcess process;
    process.setProgram(m_info.executablePath);
    process.setArguments({
        "-m", m_info.modelPath,
        "-p", buildPrompt(query, registry),
        "-n", "128",
        "--temp", "0",
        "--no-display-prompt",
        "--single-turn",
        "--reasoning", "off",
        "--no-warmup",
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(5000)) {
        result.errorMessage = "llama-cli failed to start.";
        return result;
    }

    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(3000);
        result.errorMessage = "llama-cli timed out.";
        return result;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.errorMessage = QString("llama-cli failed: %1").arg(output);
        return result;
    }

    result.success = true;
    result.output = output;
    return result;
}

LlamaGenerationResult LlamaRuntime::planIntent(const QString& query) const {
    LlamaGenerationResult result;
    if (!m_info.available) {
        result.errorMessage = m_info.errorMessage;
        return result;
    }

    QProcess process;
    process.setProgram(m_info.executablePath);
    process.setArguments({
        "-m", m_info.modelPath,
        "-p", buildIntentPrompt(query),
        "-n", "128",
        "--temp", "0",
        "--no-display-prompt",
        "--single-turn",
        "--reasoning", "off",
        "--no-warmup",
    });
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(5000)) {
        result.errorMessage = "llama-cli failed to start.";
        return result;
    }

    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(3000);
        result.errorMessage = "llama-cli timed out.";
        return result;
    }

    const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.errorMessage = QString("llama-cli failed: %1").arg(output);
        return result;
    }

    result.success = true;
    result.output = output;
    return result;
}

QVariantMap LlamaRuntime::extractToolCallJson(const QString& text, QString* error) {
    int start = text.lastIndexOf('{');
    if (start < 0) {
        if (error) *error = "Model output did not contain a JSON object.";
        return {};
    }

    QString lastError;
    while (start >= 0) {
        const int end = findJsonObjectEnd(text, start);
        if (end > start) {
            QJsonParseError parseError;
            const QByteArray jsonBytes = text.mid(start, end - start + 1).toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QVariantMap parsed = document.object().toVariantMap();
                if (parsed.contains("tool") && parsed.contains("args")) {
                    QVariantMap call;
                    call["tool"] = parsed.value("tool").toString();
                    call["args"] = parsed.value("args").toMap();
                    if (error) error->clear();
                    return call;
                }
                lastError = "JSON object did not contain a tool call.";
            } else if (parseError.error != QJsonParseError::NoError) {
                lastError = QString("Model output JSON parse failed: %1.").arg(parseError.errorString());
            }
        }

        start = text.lastIndexOf('{', start - 1);
    }

    if (error) *error = lastError.isEmpty() ? "Model output did not contain a tool call JSON object." : lastError;
    return {};
}

QVariantMap LlamaRuntime::extractIntentJson(const QString& text, QString* error) {
    int start = text.lastIndexOf('{');
    if (start < 0) {
        if (error) *error = "Model output did not contain a JSON object.";
        return {};
    }

    QString lastError;
    while (start >= 0) {
        const int end = findJsonObjectEnd(text, start);
        if (end > start) {
            QJsonParseError parseError;
            const QByteArray jsonBytes = text.mid(start, end - start + 1).toUtf8();
            const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                const QVariantMap parsed = document.object().toVariantMap();
                if (parsed.contains("intent")) {
                    if (error) error->clear();
                    return parsed;
                }
                lastError = "JSON object did not contain an intent.";
            } else if (parseError.error != QJsonParseError::NoError) {
                lastError = QString("Model output JSON parse failed: %1.").arg(parseError.errorString());
            }
        }

        start = text.lastIndexOf('{', start - 1);
    }

    if (error) *error = lastError.isEmpty() ? "Model output did not contain an intent JSON object." : lastError;
    return {};
}

QString LlamaRuntime::findExecutable() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString envExe = env.value("KILLENGINE_LLAMA_CLI").trimmed();
    if (!envExe.isEmpty() && QFileInfo::exists(envExe)) {
        return QFileInfo(envExe).absoluteFilePath();
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDir.filePath("llama-cli.exe"),
        appDir.filePath("llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        appDir.filePath("../../third_party/llama.cpp/build/bin/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/Release/llama-cli.exe"),
        QDir::current().filePath("third_party/llama.cpp/build/bin/llama-cli.exe"),
    };

    for (const auto& candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return {};
}

QString LlamaRuntime::buildIntentPrompt(const QString& query) {
    return QString(
        "Tu es l'interpreteur d'intention de KillEngine.\n"
        "Reponds uniquement avec un objet JSON compact et rien d'autre.\n"
        "Schema: {\"intent\":\"Unknown|ResetContext|ExactScan|GuidedScan|RefineScan|ActivateMemoryTargets|WriteMemoryTargets|RewriteLastTargets|WriteProfileTargets\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[],\"confidence\":0.0,\"missing\":\"\"}\n"
        "Regles:\n"
        "- ExactScan: l'utilisateur donne une valeur actuelle a chercher.\n"
        "- GuidedScan: l'utilisateur donne une valeur actuelle et une valeur cible.\n"
        "- RefineScan: l'utilisateur donne une nouvelle valeur observee pour reduire une recherche deja active.\n"
        "- ActivateMemoryTargets: l'utilisateur donne une ou plusieurs adresses 0x sans valeur a ecrire.\n"
        "- WriteMemoryTargets: l'utilisateur donne adresse(s) 0x et valeur a ecrire, ou demande d'ecrire sur adresses actives.\n"
        "- RewriteLastTargets: l'utilisateur demande de repasser/modifier les dernieres adresses trouvees.\n"
        "- WriteProfileTargets: l'utilisateur demande explicitement d'ecrire sur une cible nommee/profil.\n"
        "- ResetContext: l'utilisateur veut une autre recherche ou repartir de zero.\n"
        "- Si une valeur requise manque, mets intent Unknown et missing avec la question courte a poser.\n"
        "Exemples:\n"
        "j'ai 900 en score je veux le passer a 1000 => {\"intent\":\"GuidedScan\",\"value\":\"900\",\"targetValue\":\"1000\",\"addresses\":[],\"confidence\":0.95,\"missing\":\"\"}\n"
        "0x2d3a80afb0c et 0x2d3f7e25594 => {\"intent\":\"ActivateMemoryTargets\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[\"0x2d3a80afb0c\",\"0x2d3f7e25594\"],\"confidence\":0.95,\"missing\":\"\"}\n"
        "passe le score => {\"intent\":\"Unknown\",\"value\":\"\",\"targetValue\":\"\",\"addresses\":[],\"confidence\":0.4,\"missing\":\"Tu veux le passer a quelle valeur ?\"}\n"
        "Requete utilisateur: %1")
        .arg(query);
}

QString LlamaRuntime::buildPrompt(const QString& query, const ToolRegistry& registry) {
    QStringList tools;
    for (const auto& item : registry.availableTools()) {
        const auto tool = item.toMap();
        tools << QString("- %1 required=%2")
                     .arg(tool.value("name").toString(), tool.value("requiredArgs").toStringList().join(","));
    }

    return QString(
        "Tu es le planner local de KillEngine.\n"
        "Reponds uniquement avec un objet JSON compact et rien d'autre.\n"
        "Schema obligatoire: {\"tool\":\"exact_scan|next_scan|unknown_capture|unknown_compare|write_value|freeze_value\",\"args\":{...}}\n"
        "Regles:\n"
        "- Si la requete contient une valeur numerique actuelle sans adresse, utilise exact_scan.\n"
        "- Pour exact_scan, args doit contenir value en string et valueType.\n"
        "- Si aucun type explicite n'est donne, valueType vaut Int32.\n"
        "Exemple: j'ai 41250 argent => {\"tool\":\"exact_scan\",\"args\":{\"value\":\"41250\",\"valueType\":\"Int32\"}}\n"
        "Outils disponibles:\n%1\n"
        "Requete utilisateur: %2")
        .arg(tools.join('\n'), query);
}

} // namespace killai
