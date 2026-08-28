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

#include <algorithm>

namespace killcore {

namespace {

QString locatorKindToString(LocatorKind kind) {
    switch (kind) {
        case LocatorKind::ModuleOffset: return "module_offset";
        case LocatorKind::Absolute:     return "absolute";
        case LocatorKind::PointerChain: return "pointer_chain";
        case LocatorKind::ClrField:     return "clr_field";
    }
    return "module_offset";
}

LocatorKind stringToLocatorKind(const QString& str) {
    if (str == "absolute") return LocatorKind::Absolute;
    if (str == "pointer_chain") return LocatorKind::PointerChain;
    if (str == "clr_field") return LocatorKind::ClrField;
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

QJsonObject clrFieldLocatorToJson(const ClrFieldLocator& locator) {
    QJsonObject json;
    json["typeSubstring"] = locator.typeSubstring;
    json["identityField"] = locator.identityField;
    json["identityValue"] = locator.identityValue;
    json["targetField"] = locator.targetField;
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

ClrFieldLocator clrFieldLocatorFromJson(const QJsonObject& json) {
    ClrFieldLocator locator;
    locator.typeSubstring = json.value("typeSubstring").toString();
    locator.identityField = json.value("identityField").toString();
    locator.identityValue = json.value("identityValue").toString();
    locator.targetField = json.value("targetField").toString();
    return locator;
}

QJsonObject locatorToJson(const Locator& loc) {
    QJsonObject json;
    json["kind"] = locatorKindToString(loc.kind);
    json["module"] = loc.module;
    json["offset"] = QString::number(loc.offset, 16);
    json["lastAddress"] = QString::number(loc.lastAddress, 16);
    if (loc.kind == LocatorKind::PointerChain) {
        json["pointerChain"] = pointerChainToJson(loc.pointerChain);
    } else if (loc.kind == LocatorKind::ClrField) {
        json["clrField"] = clrFieldLocatorToJson(loc.clrField);
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
    } else if (loc.kind == LocatorKind::ClrField) {
        loc.clrField = clrFieldLocatorFromJson(json.value("clrField").toObject());
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
    if (!target.dependsOn.isEmpty()) {
        QJsonArray dependsOnArray;
        for (const auto& dependency : target.dependsOn) {
            if (!dependency.trimmed().isEmpty()) {
                dependsOnArray.append(dependency.trimmed());
            }
        }
        if (!dependsOnArray.isEmpty()) {
            json["dependsOn"] = dependsOnArray;
        }
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

QJsonObject autoAsmScriptToJson(const ProfileAutoAsmScript& script) {
    QJsonObject json;
    json["name"] = script.name;
    json["scriptText"] = script.scriptText;
    if (!script.description.isEmpty()) {
        json["description"] = script.description;
    }
    if (!script.riskLevel.isEmpty()) {
        json["riskLevel"] = script.riskLevel;
    }
    return json;
}

ProfileAutoAsmScript autoAsmScriptFromJson(const QJsonObject& json) {
    ProfileAutoAsmScript script;
    script.name = json.value("name").toString();
    script.scriptText = json.value("scriptText").toString();
    script.description = json.value("description").toString();
    script.riskLevel = json.value("riskLevel").toString();
    return script;
}

QJsonObject luaScriptToJson(const ProfileLuaScript& script) {
    QJsonObject json;
    json["name"] = script.name;
    json["scriptText"] = script.scriptText;
    if (!script.description.isEmpty()) {
        json["description"] = script.description;
    }
    if (script.savedAtEpochMs != 0) {
        json["savedAtEpochMs"] = QString::number(script.savedAtEpochMs);
    }
    return json;
}

ProfileLuaScript luaScriptFromJson(const QJsonObject& json) {
    ProfileLuaScript script;
    script.name = json.value("name").toString();
    script.scriptText = json.value("scriptText").toString();
    script.description = json.value("description").toString();
    script.savedAtEpochMs = json.value("savedAtEpochMs").toString().toLongLong();
    return script;
}

ProfileTarget targetFromJson(const QJsonObject& json) {
    ProfileTarget target;
    target.name = json.value("name").toString();
    parseValueType(json.value("type").toString("Int32"), &target.type);
    target.locator = locatorFromJson(json.value("locator").toObject());
    target.description = json.value("description").toString();
    const QJsonArray dependsOnArray = json.value("dependsOn").toArray();
    for (const auto& item : dependsOnArray) {
        const QString dependency = item.isString()
            ? item.toString().trimmed()
            : QString::number(item.toInt()).trimmed();
        if (!dependency.isEmpty() && !target.dependsOn.contains(dependency)) {
            target.dependsOn.append(dependency);
        }
    }
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

    QJsonArray autoAsmScriptsArray;
    for (const auto& script : profile.autoAsmScripts) {
        autoAsmScriptsArray.append(autoAsmScriptToJson(script));
    }
    root["autoAsmScripts"] = autoAsmScriptsArray;

    QJsonArray luaScriptsArray;
    for (const auto& script : profile.luaScripts) {
        luaScriptsArray.append(luaScriptToJson(script));
    }
    root["luaScripts"] = luaScriptsArray;

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

    profile->autoAsmScripts.clear();
    const QJsonArray autoAsmScriptsArray = root.value("autoAsmScripts").toArray();
    for (const auto& item : autoAsmScriptsArray) {
        profile->autoAsmScripts.append(autoAsmScriptFromJson(item.toObject()));
    }

    profile->luaScripts.clear();
    const QJsonArray luaScriptsArray = root.value("luaScripts").toArray();
    for (const auto& item : luaScriptsArray) {
        profile->luaScripts.append(luaScriptFromJson(item.toObject()));
    }

    KE_LOG_INFO() << "Profile loaded: " << filename.toStdString()
                  << " (" << profile->targets.size() << " targets, "
                  << profile->patches.size() << " patches, "
                  << profile->autoAsmScripts.size() << " auto-asm scripts, "
                  << profile->luaScripts.size() << " lua scripts)";
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

QJsonObject ProfileStore::exportPointerMap(const Profile& profile) {
    QJsonArray targetsArray;
    for (const auto& target : profile.targets) {
        if (target.locator.kind == LocatorKind::PointerChain) {
            targetsArray.append(targetToJson(target));
        }
    }

    QJsonObject root;
    root["format"] = "killengine.pointer_map";
    root["formatVersion"] = 1;
    root["sourceGameName"] = profile.gameName;
    root["sourceExecutableName"] = profile.executableName;
    root["sourceExecutableHash"] = profile.executableHash;
    root["targetCount"] = targetsArray.size();
    root["targets"] = targetsArray;
    return root;
}

ProfileStore::PointerMapImportResult ProfileStore::mergePointerMap(
    Profile* profile,
    const QJsonObject& pointerMap,
    bool replaceExisting) {

    PointerMapImportResult result;
    if (!profile) {
        result.messages.append("Profil de destination nul.");
        return result;
    }

    const QJsonArray targetsArray = pointerMap.value("targets").toArray();
    for (const auto& item : targetsArray) {
        const ProfileTarget incoming = targetFromJson(item.toObject());
        if (incoming.name.trimmed().isEmpty()) {
            ++result.skipped;
            result.messages.append("Cible ignoree : nom vide.");
            continue;
        }
        if (incoming.locator.kind != LocatorKind::PointerChain || !incoming.locator.pointerChain.isValid()) {
            ++result.skipped;
            result.messages.append(QString("Cible ignoree : %1 n'est pas une chaine de pointeurs valide.").arg(incoming.name));
            continue;
        }

        auto existingIt = std::find_if(profile->targets.begin(), profile->targets.end(), [&](const ProfileTarget& target) {
            return target.name.compare(incoming.name, Qt::CaseInsensitive) == 0;
        });
        if (existingIt != profile->targets.end()) {
            if (!replaceExisting) {
                ++result.skipped;
                result.messages.append(QString("Cible ignoree : %1 existe deja.").arg(incoming.name));
                continue;
            }
            *existingIt = incoming;
            ++result.replaced;
            ++result.imported;
            continue;
        }

        profile->targets.append(incoming);
        ++result.imported;
    }

    return result;
}

} // namespace killcore
