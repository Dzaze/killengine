#include "model_locator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>

namespace killai {

QStringList ModelLocator::discoverModelFiles() {
    QStringList discovered;
    const QList<QDir> dirs = {
        QDir(QCoreApplication::applicationDirPath()).filePath("model"),
        QDir(QCoreApplication::applicationDirPath()).filePath("../../model"),
        QDir::current().filePath("model"),
    };

    for (const auto& dir : dirs) {
        if (!dir.exists()) {
            continue;
        }
        const auto files = dir.entryInfoList({"*.gguf"}, QDir::Files | QDir::Readable, QDir::Name);
        for (const auto& file : files) {
            const QString path = file.absoluteFilePath();
            if (!discovered.contains(path, Qt::CaseInsensitive)) {
                discovered << path;
            }
        }
        const auto modelDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
        for (const auto& modelDir : modelDirs) {
            const auto nestedFiles = QDir(modelDir.absoluteFilePath()).entryInfoList({"*.gguf"}, QDir::Files | QDir::Readable, QDir::Name);
            for (const auto& file : nestedFiles) {
                const QString path = file.absoluteFilePath();
                if (!discovered.contains(path, Qt::CaseInsensitive)) {
                    discovered << path;
                }
            }
        }
    }
    return discovered;
}

QStringList ModelLocator::candidateModelPaths() {
    QStringList candidates;
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString configuredModel = QSettings().value("ai/modelPath", "").toString().trimmed();
    const QString envModel = env.value("KILLENGINE_QWEN_GGUF").trimmed();
    if (!configuredModel.isEmpty()) {
        candidates << configuredModel;
    }
    if (!envModel.isEmpty()) {
        candidates << envModel;
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    candidates << appDir.filePath("model/qwen/qwen.gguf");
    candidates << appDir.filePath("model/qwen/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("../../model/qwen/qwen.gguf");
    candidates << appDir.filePath("../../model/qwen/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("../../model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    candidates << QDir::current().filePath("model/qwen/qwen.gguf");
    candidates << QDir::current().filePath("model/qwen/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << QDir::current().filePath("model/qwen/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    candidates << discoverModelFiles();
    QStringList unique;
    for (const auto& candidate : candidates) {
        if (!candidate.trimmed().isEmpty() && !unique.contains(candidate, Qt::CaseInsensitive)) {
            unique << candidate;
        }
    }
    return unique;
}

ModelInfo ModelLocator::findQwenGguf() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString configuredModel = QSettings().value("ai/modelPath", "").toString().trimmed();
    const QString envModel = env.value("KILLENGINE_QWEN_GGUF").trimmed();

    for (const auto& path : candidateModelPaths()) {
        const QFileInfo file(path);
        if (file.exists() && file.isFile() && file.suffix().compare("gguf", Qt::CaseInsensitive) == 0) {
            const QString source = path == configuredModel
                ? QString("settings")
                : (path == envModel ? QString("KILLENGINE_QWEN_GGUF") : QString("model directory"));
            return {true, file.absoluteFilePath(), source, {}};
        }
    }

    ModelInfo info;
    info.errorMessage = "No GGUF model found. Ship one in model/<ai-name>/, set modelPath, or set KILLENGINE_QWEN_GGUF.";
    return info;
}

} // namespace killai
