#include "process/package_storage.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <appmodel.h>
#endif

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <vector>

namespace killcore {

bool resolvePackageFamilyName(const ProcessHandle& process, QString* familyName, QString* error) {
    if (familyName) familyName->clear();

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        if (error) *error = "Process handle invalide.";
        return false;
    }

    UINT32 length = 0;
    LONG rc = GetPackageFamilyName(process.rawHandle(), &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        // APPMODEL_ERROR_NO_PACKAGE (15700) : process Win32 classique, pas un
        // package UWP -- pas d'erreur système, juste "rien à trouver ici".
        if (error) *error = "Ce processus n'est pas un package UWP/AppContainer (pas de dossier LocalState associé).";
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(length));
    rc = GetPackageFamilyName(process.rawHandle(), &length, buffer.data());
    if (rc != ERROR_SUCCESS) {
        if (error) *error = "GetPackageFamilyName a échoué malgré la première résolution de taille.";
        return false;
    }

    if (familyName) *familyName = QString::fromWCharArray(buffer.data());
    return true;
#else
    (void)process;
    if (error) *error = "Non supporté sur cette plateforme.";
    return false;
#endif
}

bool listPackageSaveFiles(
    const QString& familyName,
    int maxResults,
    bool excludeNoise,
    QVector<PackageSaveFileEntry>* files,
    QString* error) {

    if (files) files->clear();
    if (!files) {
        return false;
    }

    const QString trimmedFamily = familyName.trimmed();
    if (trimmedFamily.isEmpty()) {
        if (error) *error = "Package family name vide.";
        return false;
    }

    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty()) {
        if (error) *error = "Variable d'environnement LOCALAPPDATA introuvable.";
        return false;
    }

    const QString root = QDir(localAppData).filePath(QString("Packages/%1").arg(trimmedFamily));
    QDir rootDir(root);
    if (!rootDir.exists()) {
        if (error) *error = QString("Dossier package introuvable: %1").arg(root);
        return false;
    }

    const int cap = maxResults > 0 ? maxResults : 50;
    QVector<PackageSaveFileEntry> collected;

    QDirIterator it(root, QDir::Files | QDir::NoSymLinks | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (excludeNoise) {
            // QDirIterator retourne des chemins avec '/' (convention Qt),
            // meme sur Windows -- ne pas filtrer avec des separateurs '\'.
            const QString lowerPath = path.toLower();
            if (lowerPath.contains("/ebwebview/") || lowerPath.contains("/cache/")
                || lowerPath.contains("/cache2/") || lowerPath.contains("/dlfiles/")
                || lowerPath.contains("/gpucache/") || lowerPath.contains("/grshadercache/")
                || lowerPath.contains("/vunglesdk/") || lowerPath.contains("/leveldb/")) {
                continue;
            }
        }
        const QFileInfo info = it.fileInfo();
        PackageSaveFileEntry entry;
        entry.path = path;
        entry.sizeBytes = info.size();
        entry.lastWriteTimeIso = info.lastModified().toString(Qt::ISODate);
        collected.append(entry);
    }

    std::sort(collected.begin(), collected.end(), [](const PackageSaveFileEntry& a, const PackageSaveFileEntry& b) {
        return a.lastWriteTimeIso > b.lastWriteTimeIso;
    });

    if (collected.size() > cap) {
        collected.resize(cap);
    }

    *files = collected;
    return true;
}

bool readPackageSaveFileText(
    const QString& path,
    int maxBytes,
    QString* text,
    bool* truncated,
    QString* error) {

    if (text) text->clear();
    if (truncated) *truncated = false;

    QFile file(path);
    if (!file.exists()) {
        if (error) *error = "Fichier introuvable.";
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("Ouverture impossible: %1").arg(file.errorString());
        return false;
    }

    const int cap = maxBytes > 0 ? maxBytes : 65536;
    const QByteArray raw = file.read(cap);
    if (truncated) *truncated = file.bytesAvailable() > 0;
    file.close();

    // Best-effort : garde les octets imprimables ASCII et les sauts de
    // ligne/tabulations, remplace le reste par '.' -- voir PHASE 90, les
    // fichiers .sgi sont un conteneur binaire enveloppant un blob JSON
    // directement lisible une fois décodé ainsi, pas la peine de deviner
    // l'encodage exact du conteneur.
    QString decoded;
    decoded.reserve(raw.size());
    for (unsigned char ch : raw) {
        if ((ch >= 0x20 && ch <= 0x7E) || ch == '\n' || ch == '\r' || ch == '\t') {
            decoded.append(QChar::fromLatin1(static_cast<char>(ch)));
        } else {
            decoded.append(QChar::fromLatin1('.'));
        }
    }

    if (text) *text = decoded;
    return true;
}

} // namespace killcore
