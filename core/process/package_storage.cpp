#include "process/package_storage.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <appmodel.h>
#endif

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <cstring>
#include <vector>

namespace killcore {

namespace {

bool decodeHexBytes(const QString& input, QByteArray* bytes, QString* error) {
    if (bytes) bytes->clear();
    QString compact;
    compact.reserve(input.size());
    for (const QChar ch : input) {
        if (ch.isSpace() || ch == QLatin1Char('-')) {
            continue;
        }
        if (!ch.isDigit() && (ch.toLower() < QLatin1Char('a') || ch.toLower() > QLatin1Char('f'))) {
            if (error) *error = QString("Hex invalide: caractère '%1'.").arg(ch);
            return false;
        }
        compact.append(ch);
    }
    if (compact.isEmpty()) {
        if (error) *error = "Séquence hex vide.";
        return false;
    }
    if ((compact.size() % 2) != 0) {
        if (error) *error = "Séquence hex invalide: nombre impair de caractères.";
        return false;
    }

    QByteArray decoded;
    decoded.reserve(compact.size() / 2);
    for (int i = 0; i < compact.size(); i += 2) {
        bool ok = false;
        const int value = compact.mid(i, 2).toInt(&ok, 16);
        if (!ok || value < 0 || value > 0xFF) {
            if (error) *error = "Séquence hex invalide.";
            return false;
        }
        decoded.append(static_cast<char>(value));
    }

    if (bytes) *bytes = decoded;
    return true;
}

bool isPathUnderLocalPackages(const QString& path) {
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty()) {
        return false;
    }
    const QString normalized = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toLower();
    QString allowedRoot = QDir::toNativeSeparators(QDir(localAppData).filePath("Packages")).toLower();
    if (!allowedRoot.endsWith(QDir::separator())) {
        allowedRoot.append(QDir::separator());
    }
    return normalized.startsWith(allowedRoot);
}

QString packageRootForFamily(const QString& familyName) {
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (localAppData.isEmpty()) {
        return {};
    }
    return QDir(localAppData).filePath(QString("Packages/%1").arg(familyName));
}

#ifdef Q_OS_WIN
QString registryTypeName(DWORD type) {
    switch (type) {
    case REG_SZ: return "REG_SZ";
    case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
    case REG_MULTI_SZ: return "REG_MULTI_SZ";
    case REG_DWORD: return "REG_DWORD";
    case REG_QWORD: return "REG_QWORD";
    case REG_BINARY: return "REG_BINARY";
    case REG_NONE: return "REG_NONE";
    default: return QString("REG_%1").arg(type);
    }
}

QString bytesToHexPreview(const QByteArray& bytes, int maxBytes = 64) {
    const QByteArray clipped = bytes.left(maxBytes);
    QStringList parts;
    parts.reserve(clipped.size());
    for (unsigned char ch : clipped) {
        parts.append(QString("%1").arg(static_cast<unsigned int>(ch), 2, 16, QLatin1Char('0')).toUpper());
    }
    QString preview = parts.join(' ');
    if (bytes.size() > maxBytes) {
        preview.append(" ...");
    }
    return preview;
}

// Les valeurs composites de Windows.Storage.ApplicationData.LocalSettings sont exposees par
// l'API registre (RegEnumValueW sur le hive settings.dat) avec un type trainant apres la valeur
// brute : [donnee][FILETIME 8 octets] optionnel. Partage entre tryDecodeUtf16SettingPayload
// (texte) et les decodeurs numeriques/booleens ci-dessous pour ne pas dupliquer la conversion
// FILETIME -> ISO8601.
QString appendTrailingFileTimeIfPresent(const QString& base, const QByteArray& data, int valueByteSize) {
    QString decoded = base;
    if (data.size() - valueByteSize == 8) {
        quint64 fileTime = 0;
        std::memcpy(&fileTime, data.constData() + valueByteSize, sizeof(fileTime));
        if (fileTime > 116444736000000000ULL) {
            const qint64 unixMs = static_cast<qint64>((fileTime - 116444736000000000ULL) / 10000ULL);
            decoded.append(QString(" @ %1").arg(QDateTime::fromMSecsSinceEpoch(unixMs, QTimeZone::UTC).toString(Qt::ISODate)));
        }
    }
    return decoded;
}

bool tryDecodeUtf16SettingPayload(const QByteArray& data, QString* preview) {
    if (preview) preview->clear();
    if (data.size() < 2 || (data.size() % 2) != 0) {
        return false;
    }

    const wchar_t* chars = reinterpret_cast<const wchar_t*>(data.constData());
    const int charCount = static_cast<int>(data.size() / sizeof(wchar_t));
    int terminator = -1;
    for (int i = 0; i < charCount; ++i) {
        if (chars[i] == L'\0') {
            terminator = i;
            break;
        }
        if (chars[i] < 0x20 && chars[i] != L'\t' && chars[i] != L'\n' && chars[i] != L'\r') {
            return false;
        }
    }
    if (terminator < 0) {
        return false;
    }

    QString text = QString::fromWCharArray(chars, terminator);
    const int trailingOffset = (terminator + 1) * static_cast<int>(sizeof(wchar_t));
    if (preview) *preview = appendTrailingFileTimeIfPresent(text, data, trailingOffset);
    return true;
}

// Types composites observes sur de vrais hives settings.dat UWP (Notepad) : le registre expose
// le type Windows.Foundation.PropertyType de la valeur WinRT d'origine, decale de 100000000
// (ex: Int32=4 -> 100000004). Ce n'est PAS un type Win32 standard, donc invisible pour
// RegQueryValueEx/REG_DWORD -- sans ce mapping, ces valeurs retombaient sur
// tryDecodeUtf16SettingPayload (une heuristique "texte UTF-16 ?") qui les corrompait
// silencieusement (ex: un Int32 de position de fenetre lu comme 1 caractere Unicode garbled)
// plutot que de simplement echouer proprement vers un hex dump. Seuls les types reellement
// rencontres et verifies octet-a-octet sont geres ici ; les autres continuent de retomber sur
// l'heuristique texte puis le hex dump, comme avant.
constexpr DWORD kAppSettingTypeBase    = 100000000;
constexpr DWORD kAppSettingTypeInt32   = kAppSettingTypeBase + 4;
constexpr DWORD kAppSettingTypeUInt32  = kAppSettingTypeBase + 5;
constexpr DWORD kAppSettingTypeBoolean = kAppSettingTypeBase + 11;

QString registryValuePreview(DWORD type, const QByteArray& data) {
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        if (data.isEmpty()) {
            return {};
        }
        return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data.constData()),
            static_cast<int>(data.size() / sizeof(wchar_t))).remove(QChar::Null);
    }
    if (type == REG_MULTI_SZ) {
        QStringList items;
        const wchar_t* chars = reinterpret_cast<const wchar_t*>(data.constData());
        const int count = static_cast<int>(data.size() / sizeof(wchar_t));
        int start = 0;
        for (int i = 0; i < count; ++i) {
            if (chars[i] == L'\0') {
                if (i > start) {
                    items.append(QString::fromWCharArray(chars + start, i - start));
                }
                start = i + 1;
            }
        }
        return items.join("; ");
    }
    if (type == REG_DWORD && data.size() >= 4) {
        quint32 value = 0;
        std::memcpy(&value, data.constData(), sizeof(value));
        return QString("%1 (0x%2)").arg(value).arg(value, 8, 16, QLatin1Char('0')).toUpper();
    }
    if (type == REG_QWORD && data.size() >= 8) {
        quint64 value = 0;
        std::memcpy(&value, data.constData(), sizeof(value));
        return QString("%1 (0x%2)").arg(value).arg(value, 16, 16, QLatin1Char('0')).toUpper();
    }
    if (type == kAppSettingTypeInt32 && data.size() >= 4) {
        qint32 value = 0;
        std::memcpy(&value, data.constData(), sizeof(value));
        return appendTrailingFileTimeIfPresent(QString::number(value), data, sizeof(value));
    }
    if (type == kAppSettingTypeUInt32 && data.size() >= 4) {
        quint32 value = 0;
        std::memcpy(&value, data.constData(), sizeof(value));
        return appendTrailingFileTimeIfPresent(QString::number(value), data, sizeof(value));
    }
    if (type == kAppSettingTypeBoolean && data.size() >= 1) {
        const bool value = data.at(0) != 0;
        return appendTrailingFileTimeIfPresent(value ? QStringLiteral("true") : QStringLiteral("false"), data, 1);
    }
    QString utf16Preview;
    if (tryDecodeUtf16SettingPayload(data, &utf16Preview)) {
        return utf16Preview;
    }
    return bytesToHexPreview(data);
}

void enumerateRegistryValues(
    HKEY key,
    const QString& keyPath,
    int depth,
    int maxValues,
    QVector<PackageLocalSettingsEntry>* entries) {
    if (!entries || entries->size() >= maxValues || depth > 8) {
        return;
    }

    DWORD valueCount = 0;
    DWORD maxValueNameLen = 0;
    DWORD maxValueDataLen = 0;
    DWORD subkeyCount = 0;
    DWORD maxSubkeyLen = 0;
    if (RegQueryInfoKeyW(
            key,
            nullptr,
            nullptr,
            nullptr,
            &subkeyCount,
            &maxSubkeyLen,
            nullptr,
            &valueCount,
            &maxValueNameLen,
            &maxValueDataLen,
            nullptr,
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    std::vector<wchar_t> valueName(static_cast<size_t>(maxValueNameLen) + 2);
    std::vector<BYTE> valueData(static_cast<size_t>(std::max<DWORD>(maxValueDataLen, 1)));
    for (DWORD i = 0; i < valueCount && entries->size() < maxValues; ++i) {
        DWORD nameLen = static_cast<DWORD>(valueName.size());
        DWORD dataLen = static_cast<DWORD>(valueData.size());
        DWORD type = REG_NONE;
        LONG rc = RegEnumValueW(key, i, valueName.data(), &nameLen, nullptr, &type, valueData.data(), &dataLen);
        if (rc == ERROR_MORE_DATA) {
            valueData.resize(dataLen);
            nameLen = static_cast<DWORD>(valueName.size());
            rc = RegEnumValueW(key, i, valueName.data(), &nameLen, nullptr, &type, valueData.data(), &dataLen);
        }
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        QByteArray data(reinterpret_cast<const char*>(valueData.data()), static_cast<int>(dataLen));
        PackageLocalSettingsEntry entry;
        entry.keyPath = keyPath;
        entry.name = nameLen == 0 ? QStringLiteral("(default)") : QString::fromWCharArray(valueName.data(), static_cast<int>(nameLen));
        entry.type = registryTypeName(type);
        entry.preview = registryValuePreview(type, data);
        entry.dataSizeBytes = dataLen;
        entries->append(entry);
    }

    std::vector<wchar_t> subkeyName(static_cast<size_t>(maxSubkeyLen) + 2);
    for (DWORD i = 0; i < subkeyCount && entries->size() < maxValues; ++i) {
        DWORD nameLen = static_cast<DWORD>(subkeyName.size());
        if (RegEnumKeyExW(key, i, subkeyName.data(), &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(key, subkeyName.data(), 0, KEY_READ, &subkey) != ERROR_SUCCESS || !subkey) {
            continue;
        }
        const QString childName = QString::fromWCharArray(subkeyName.data(), static_cast<int>(nameLen));
        enumerateRegistryValues(subkey, keyPath.isEmpty() ? childName : QString("%1\\%2").arg(keyPath, childName),
            depth + 1, maxValues, entries);
        RegCloseKey(subkey);
    }
}
#endif

} // namespace

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

    const QString root = packageRootForFamily(trimmedFamily);
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

bool inspectPackageLocalSettings(
    const QString& familyName,
    int maxValues,
    QVector<PackageLocalSettingsEntry>* entries,
    QString* settingsPath,
    QString* error) {

    if (entries) entries->clear();
    if (settingsPath) settingsPath->clear();
    if (!entries) {
        return false;
    }

    const QString trimmedFamily = familyName.trimmed();
    if (trimmedFamily.isEmpty()) {
        if (error) *error = "Package family name vide.";
        return false;
    }

    const QString root = packageRootForFamily(trimmedFamily);
    if (root.isEmpty()) {
        if (error) *error = "Variable d'environnement LOCALAPPDATA introuvable.";
        return false;
    }
    const QString path = QDir(root).filePath("Settings/settings.dat");
    if (settingsPath) *settingsPath = path;
    if (!isPathUnderLocalPackages(path)) {
        if (error) *error = "Chemin LocalSettings refusé : doit rester sous %LOCALAPPDATA%\\Packages\\.";
        return false;
    }
    if (!QFileInfo::exists(path)) {
        if (error) *error = QString("Ruche LocalSettings introuvable: %1").arg(path);
        return false;
    }

#ifdef Q_OS_WIN
    HKEY hive = nullptr;
    const LONG rc = RegLoadAppKeyW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        &hive,
        KEY_READ,
        0,
        0);
    if (rc != ERROR_SUCCESS || !hive) {
        if (error) *error = QString("RegLoadAppKeyW a échoué (%1) sur settings.dat.").arg(rc);
        return false;
    }

    const int cap = maxValues > 0 ? std::clamp(maxValues, 1, 1000) : 200;
    enumerateRegistryValues(hive, QString(), 0, cap, entries);
    RegCloseKey(hive);
    return true;
#else
    if (error) *error = "Non supporté sur cette plateforme.";
    return false;
#endif
}

bool patchPackageSaveFileBytes(
    const QString& path,
    const QString& findHex,
    const QString& replaceHex,
    QString* error,
    int* occurrencesFound) {

    if (occurrencesFound) *occurrencesFound = 0;
    if (!isPathUnderLocalPackages(path)) {
        if (error) *error = "Chemin refusé : doit être sous %LOCALAPPDATA%\\Packages\\.";
        return false;
    }

    QByteArray findBytes;
    QByteArray replaceBytes;
    QString parseError;
    if (!decodeHexBytes(findHex, &findBytes, &parseError)) {
        if (error) *error = QString("Séquence recherchée invalide: %1").arg(parseError);
        return false;
    }
    if (!decodeHexBytes(replaceHex, &replaceBytes, &parseError)) {
        if (error) *error = QString("Séquence de remplacement invalide: %1").arg(parseError);
        return false;
    }
    if (findBytes.size() != replaceBytes.size()) {
        if (error) {
            *error = QString("La longueur doit être identique, %1 octets vs %2 octets.")
                .arg(findBytes.size())
                .arg(replaceBytes.size());
        }
        return false;
    }

    QFile file(path);
    if (!file.exists()) {
        if (error) *error = "Fichier introuvable.";
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("Ouverture impossible: %1").arg(file.errorString());
        return false;
    }
    QByteArray raw = file.readAll();
    file.close();

    int occurrences = 0;
    int uniqueIndex = -1;
    int from = 0;
    while (from <= raw.size() - findBytes.size()) {
        const int index = raw.indexOf(findBytes, from);
        if (index < 0) {
            break;
        }
        occurrences += 1;
        uniqueIndex = index;
        from = index + 1;
    }
    if (occurrencesFound) *occurrencesFound = occurrences;

    if (occurrences == 0) {
        if (error) *error = "Séquence introuvable.";
        return false;
    }
    if (occurrences >= 2) {
        if (error) {
            *error = QString("%1 occurrences trouvées, séquence pas assez spécifique -- élargis le contexte autour de la valeur à changer.")
                .arg(occurrences);
        }
        return false;
    }

    raw.replace(uniqueIndex, findBytes.size(), replaceBytes);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QString("Réécriture impossible: %1").arg(file.errorString());
        return false;
    }
    const qint64 written = file.write(raw);
    if (written != raw.size()) {
        if (error) *error = QString("Écriture incomplète: %1/%2 octets.").arg(written).arg(raw.size());
        return false;
    }
    if (!file.flush()) {
        if (error) *error = QString("Flush impossible: %1").arg(file.errorString());
        return false;
    }
    file.close();
    return true;
}

} // namespace killcore
