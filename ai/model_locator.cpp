#include "model_locator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace killai {

QStringList ModelLocator::candidateModelPaths() {
    QStringList candidates;
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString envModel = env.value("KILLENGINE_QWEN_GGUF").trimmed();
    if (!envModel.isEmpty()) {
        candidates << envModel;
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    candidates << appDir.filePath("models/qwen.gguf");
    candidates << appDir.filePath("models/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("models/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("../../models/qwen.gguf");
    candidates << appDir.filePath("../../models/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << appDir.filePath("../../models/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    candidates << QDir::current().filePath("models/qwen.gguf");
    candidates << QDir::current().filePath("models/Qwen3.5-2B-Q4_K_M.gguf");
    candidates << QDir::current().filePath("models/Qwen_Qwen3.5-2B-Q4_K_M.gguf");
    return candidates;
}

ModelInfo ModelLocator::findQwenGguf() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString envModel = env.value("KILLENGINE_QWEN_GGUF").trimmed();

    for (const auto& path : candidateModelPaths()) {
        const QFileInfo file(path);
        if (file.exists() && file.isFile() && file.suffix().compare("gguf", Qt::CaseInsensitive) == 0) {
            return {true, file.absoluteFilePath(), path == envModel ? "KILLENGINE_QWEN_GGUF" : "models directory", {}};
        }
    }

    ModelInfo info;
    info.errorMessage = "No Qwen GGUF model found. Set KILLENGINE_QWEN_GGUF or place qwen.gguf in models/.";
    return info;
}

} // namespace killai
