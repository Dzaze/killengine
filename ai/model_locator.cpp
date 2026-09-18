#include "model_locator.h"

#include "localization/localization.h"
#include "paths/portable_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSettings>

namespace killai {

QString ModelLocator::toPortableModelReference(const QString& rawPath) {
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    if (!QFileInfo(trimmed).isAbsolute()) {
        return trimmed; // Deja relatif (saisie manuelle) -- resolu contre la racine a la lecture.
    }

    const QString root = QDir::cleanPath(killcore::PortablePaths::root());
    const QString absolute = QDir::cleanPath(QFileInfo(trimmed).absoluteFilePath());
    const QString relative = QDir(root).relativeFilePath(absolute);

    // QDir::relativeFilePath() prefixe le resultat de ".." des qu'on sort du
    // dossier racine (comparaison par segments de chemin, pas un prefixe
    // textuel naif qui matcherait a tort "C:/App" contre "C:/AppFoo").
    const bool isUnderRoot = !relative.startsWith("..") && !QFileInfo(relative).isAbsolute();
    return isUnderRoot ? relative : trimmed;
}

QString ModelLocator::resolveModelReference(const QString& storedValue) {
    const QString trimmed = storedValue.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    if (QFileInfo(trimmed).isAbsolute()) {
        return trimmed; // Ancien style absolu OU reference externe -- utilise tel quel.
    }
    // Chemin relatif : toujours resolu contre la racine portable, jamais
    // contre QDir::currentPath() (qui depend du repertoire de lancement).
    return QDir::cleanPath(QDir(killcore::PortablePaths::root()).absoluteFilePath(trimmed));
}

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
    // PORT-4 (docs/PORTABILITY_ROADMAP.md, 18/09/2026) : une valeur stockee
    // relative (reference portable) doit se resoudre contre la racine du
    // paquet, jamais contre QDir::currentPath() -- sinon un chemin relatif
    // enregistre depuis Settings redevenait invalide des que l'app etait
    // lancee depuis un autre repertoire de travail.
    const QString configuredModel = ModelLocator::resolveModelReference(
        QSettings().value("ai/modelPath", "").toString());
    const QString envModel = ModelLocator::resolveModelReference(
        env.value("KILLENGINE_QWEN_GGUF"));
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
    const QString configuredModel = ModelLocator::resolveModelReference(
        QSettings().value("ai/modelPath", "").toString());
    const QString envModel = ModelLocator::resolveModelReference(
        env.value("KILLENGINE_QWEN_GGUF"));

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
    info.errorMessage = KE_TXT(
        "Aucun modèle GGUF trouvé. Place-en un dans model/<nom-ia>/, renseigne modelPath, ou définis KILLENGINE_QWEN_GGUF.",
        "No GGUF model found. Ship one in model/<ai-name>/, set modelPath, or set KILLENGINE_QWEN_GGUF.");
    return info;
}

} // namespace killai
