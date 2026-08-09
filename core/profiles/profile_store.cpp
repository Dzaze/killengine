#include "profile_store.h"

#include "logging/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <nlohmann/json.hpp>

namespace killcore {

namespace {

QString locatorKindToString(LocatorKind kind) {
    switch (kind) {
        case LocatorKind::ModuleOffset: return "module_offset";
        case LocatorKind::Absolute:     return "absolute";
    }
    return "module_offset";
}

LocatorKind stringToLocatorKind(const QString& str) {
    if (str == "absolute") return LocatorKind::Absolute;
    return LocatorKind::ModuleOffset;
}

QJsonObject locatorToJson(const Locator& loc) {
    QJsonObject json;
    json["kind"] = locatorKindToString(loc.kind);
    json["module"] = loc.module;
    json["offset"] = QString::number(loc.offset, 16);
    json["lastAddress"] = QString::number(loc.lastAddress, 16);
    return json;
}

Locator locatorFromJson(const QJsonObject& json) {
    Locator loc;
    loc.kind = stringToLocatorKind(json.value("kind").toString());
    loc.module = json.value("module").toString();
    loc.offset = json.value("offset").toString().toULongLong(nullptr, 16);
    loc.lastAddress = json.value("lastAddress").toString().toULongLong(nullptr, 16);
    return loc;
}

QJsonObject targetToJson(const ProfileTarget& target) {
    QJsonObject json;
    json["name"] = target.name;
    json["type"] = valueTypeToString(target.type);
    json["locator"] = locatorToJson(target.locator);
    if (!target.description.isEmpty()) {
        json["description"] = target.description;
    }
    return json;
}

ProfileTarget targetFromJson(const QJsonObject& json) {
    ProfileTarget target;
    target.name = json.value("name").toString();
    parseValueType(json.value("type").toString("Int32"), &target.type);
    target.locator = locatorFromJson(json.value("locator").toObject());
    target.description = json.value("description").toString();
    return target;
}

} // namespace

// ---------------------------------------------------------------------------
// ProfileStore
// ---------------------------------------------------------------------------

QString ProfileStore::profilesDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base + "/KillEngine/Profiles";
}

bool ProfileStore::ensureProfilesDir() {
    const QString dir = profilesDir();
    QDir d(dir);
    if (d.exists()) return true;
    return d.mkpath(dir);
}

bool ProfileStore::save(const Profile& profile, const QString& filename) {
    if (!ensureProfilesDir()) {
        KE_LOG_ERROR() << "ProfileStore: Cannot create profiles directory";
        return false;
    }

    QJsonObject root;
    root["formatVersion"] = Profile::FORMAT_VERSION;
    root["gameName"] = profile.gameName;
    root["executableName"] = profile.executableName;
    root["executableHash"] = profile.executableHash;

    QJsonArray targetsArray;
    for (const auto& target : profile.targets) {
        targetsArray.append(targetToJson(target));
    }
    root["targets"] = targetsArray;

    QJsonDocument doc(root);

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        KE_LOG_ERROR() << "ProfileStore: Cannot write" << filename.toStdString();
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    KE_LOG_INFO() << "Profile saved: " << filename.toStdString()
                  << " (" << profile.targets.size() << " targets)";
    return true;
}

bool ProfileStore::load(const QString& filename, Profile* profile) {
    if (!profile) return false;

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        KE_LOG_ERROR() << "ProfileStore: Cannot read" << filename.toStdString();
        return false;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        KE_LOG_ERROR() << "ProfileStore: JSON parse error: " << parseError.errorString().toStdString();
        return false;
    }

    const QJsonObject root = doc.object();
    profile->gameName = root.value("gameName").toString();
    profile->executableName = root.value("executableName").toString();
    profile->executableHash = root.value("executableHash").toString();

    profile->targets.clear();
    const QJsonArray targetsArray = root.value("targets").toArray();
    for (const auto& item : targetsArray) {
        profile->targets.append(targetFromJson(item.toObject()));
    }

    KE_LOG_INFO() << "Profile loaded: " << filename.toStdString()
                  << " (" << profile->targets.size() << " targets)";
    return true;
}

QStringList ProfileStore::listProfiles() {
    QStringList result;
    QDir dir(profilesDir());
    if (!dir.exists()) return result;

    const QStringList filters = QStringList() << "*.keprofile";
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files);
    for (const auto& info : entries) {
        result.append(info.completeBaseName());
    }

    return result;
}

QString ProfileStore::profilePath(const QString& profileName) {
    return profilesDir() + "/" + profileName + ".keprofile";
}

bool ProfileStore::remove(const QString& profileName) {
    const QString path = profilePath(profileName);
    QFile file(path);
    return file.remove();
}

} // namespace killcore