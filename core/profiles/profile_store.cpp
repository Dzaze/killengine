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
        case LocatorKind::PointerChain: return "pointer_chain";
    }
    return "module_offset";
}

LocatorKind stringToLocatorKind(const QString& str) {
    if (str == "absolute") return LocatorKind::Absolute;
    if (str == "pointer_chain") return LocatorKind::PointerChain;
    return LocatorKind::ModuleOffset;
}

QJsonObject pointerChainToJson(const PointerChain& chain) {
    QJsonObject json;
    json["module"] = chain.module;
    json["baseOffset"] = QString::number(chain.baseOffset, 16);
    QJsonArray offsetsArray;
    for (uint64_t off : chain.offsets) {
        offsetsArray.append(QString::number(off, 16));
    }
    json["offsets"] = offsetsArray;
    return json;
}

PointerChain pointerChainFromJson(const QJsonObject& json) {
    PointerChain chain;
    chain.module = json.value("module").toString();
    chain.baseOffset = json.value("baseOffset").toString().toULongLong(nullptr, 16);
    const QJsonArray offsetsArray = json.value("offsets").toArray();
    for (const auto& item : offsetsArray) {
        chain.offsets.append(item.toString().toULongLong(nullptr, 16));
    }
    return chain;
}

QJsonObject locatorToJson(const Locator& loc) {
    QJsonObject json;
    json["kind"] = locatorKindToString(loc.kind);
    json["module"] = loc.module;
    json["offset"] = QString::number(loc.offset, 16);
    json["lastAddress"] = QString::number(loc.lastAddress, 16);
    if (loc.kind == LocatorKind::PointerChain) {
        json["pointerChain"] = pointerChainToJson(loc.pointerChain);
    }
    return json;
}

Locator locatorFromJson(const QJsonObject& json) {
    Locator loc;
    loc.kind = stringToLocatorKind(json.value("kind").toString());
    loc.module = json.value("module").toString();
    loc.offset = json.value("offset").toString().toULongLong(nullptr, 16);
    loc.lastAddress = json.value("lastAddress").toString().toULongLong(nullptr, 16);
    if (loc.kind == LocatorKind::PointerChain) {
        loc.pointerChain = pointerChainFromJson(json.value("pointerChain").toObject());
    }
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

QJsonObject patchToJson(const ProfileCodePatch& patch) {
    QJsonObject json;
    json["name"] = patch.name;
    json["module"] = patch.module;
    json["moduleOffset"] = QString::number(patch.moduleOffset, 16);
    json["aobPattern"] = patch.aobPattern;
    json["patchBytes"] = patch.patchBytes;
    if (!patch.originalBytes.isEmpty()) {
        json["originalBytes"] = patch.originalBytes;
    }
    if (!patch.disassembly.isEmpty()) {
        json["disassembly"] = patch.disassembly;
    }
    if (!patch.riskLevel.isEmpty()) {
        json["riskLevel"] = patch.riskLevel;
    }
    if (!patch.description.isEmpty()) {
        json["description"] = patch.description;
    }
    if (patch.signatureScore > 0 || !patch.signatureLevel.isEmpty()) {
        QJsonObject quality;
        quality["score"] = patch.signatureScore;
        quality["level"] = patch.signatureLevel;
        quality["warning"] = patch.signatureWarning;
        quality["fixedBytes"] = patch.signatureFixedBytes;
        quality["wildcardBytes"] = patch.signatureWildcardBytes;
        quality["uniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
        quality["fixedRatio"] = patch.signatureFixedRatio;
        quality["trainerSafe"] = patch.trainerSafe;
        quality["matches"] = patch.signatureMatches;
        json["signatureQuality"] = quality;
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

ProfileCodePatch patchFromJson(const QJsonObject& json) {
    ProfileCodePatch patch;
    patch.name = json.value("name").toString();
    patch.module = json.value("module").toString();
    patch.moduleOffset = json.value("moduleOffset").toString().toULongLong(nullptr, 16);
    patch.aobPattern = json.value("aobPattern").toString();
    patch.patchBytes = json.value("patchBytes").toString();
    patch.originalBytes = json.value("originalBytes").toString();
    patch.disassembly = json.value("disassembly").toString();
    patch.riskLevel = json.value("riskLevel").toString();
    patch.description = json.value("description").toString();
    const QJsonObject quality = json.value("signatureQuality").toObject();
    patch.signatureScore = quality.value("score").toInt();
    patch.signatureLevel = quality.value("level").toString();
    patch.signatureWarning = quality.value("warning").toString();
    patch.signatureFixedBytes = quality.value("fixedBytes").toInt();
    patch.signatureWildcardBytes = quality.value("wildcardBytes").toInt();
    patch.signatureUniqueFixedBytes = quality.value("uniqueFixedBytes").toInt();
    patch.signatureFixedRatio = quality.value("fixedRatio").toDouble();
    patch.trainerSafe = quality.value("trainerSafe").toBool(false);
    patch.signatureMatches = quality.value("matches").toInt();
    return patch;
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

    QJsonArray patchesArray;
    for (const auto& patch : profile.patches) {
        patchesArray.append(patchToJson(patch));
    }
    root["patches"] = patchesArray;

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

    profile->patches.clear();
    const QJsonArray patchesArray = root.value("patches").toArray();
    for (const auto& item : patchesArray) {
        profile->patches.append(patchFromJson(item.toObject()));
    }

    KE_LOG_INFO() << "Profile loaded: " << filename.toStdString()
                  << " (" << profile->targets.size() << " targets, "
                  << profile->patches.size() << " patches)";
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
