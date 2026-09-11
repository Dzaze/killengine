#include "profile_manager.h"

#include "application_controller.h"
#include "code_patch_manager.h"
#include "logging/logger.h"
#include "localization/localization.h"
#include "memory/memory_reader.h"
#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/profile_patch_state.h"
#include "pointer/pointer_chain.h"
#include "pointer/pointer_scanner.h"
#include "process/process_enumerator.h"
#include "profiles/ghidra_bridge.h"
#include "profiles/profile_store.h"
#include "scripting/auto_assembler.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <optional>

namespace killengine {

namespace {

QVariantMap aobPatternQualityToVariantMap(const killcore::AobPatternQuality& quality) {
    QVariantMap item;
    item["score"] = quality.score;
    item["level"] = quality.level;
    item["warning"] = quality.warning;
    item["patternBytes"] = quality.patternBytes;
    item["fixedBytes"] = quality.fixedBytes;
    item["wildcardBytes"] = quality.wildcardBytes;
    item["uniqueFixedBytes"] = quality.uniqueFixedBytes;
    item["fixedRatio"] = quality.fixedRatio;
    item["trainerSafe"] = quality.trainerSafe;
    return item;
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }

    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }

    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok || parsed == 0) {
        return false;
    }

    *address = parsed;
    return true;
}

QString computeExecutableHash(const QString& filePath) {
    if (filePath.isEmpty()) {
        return {};
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

double ratePerSecond(size_t count, qint64 elapsedMs) {
    if (elapsedMs <= 0) {
        return 0.0;
    }
    return static_cast<double>(count) * 1000.0 / static_cast<double>(elapsedMs);
}

QString profileTargetGroupName(QString name) {
    name = name.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("Profil");
    }
    name.replace('_', ' ');
    return name;
}

} // namespace

ProfileManager::ProfileManager(ApplicationController& controller)
    : m_controller(controller) {
}
QVariantMap ProfileManager::saveProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QString& addressHex,
    const QString& valueType,
    const QString& description) {

    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }

    // Trouve le module qui contient l'adresse pour créer le locator
    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_controller.m_pid);
    QString bestModule;
    uint64_t bestBase = 0;
    uint64_t bestSize = 0;

    for (const auto& mod : modules) {
        if (address >= mod.baseAddress && address < mod.baseAddress + mod.size) {
            // Choisit le module le plus petit contenant l'adresse (plus précis)
            if (bestModule.isEmpty() || mod.size < bestSize) {
                bestModule = mod.name;
                bestBase = mod.baseAddress;
                bestSize = mod.size;
            }
        }
    }

    killcore::Locator locator;
    if (!bestModule.isEmpty()) {
        locator.kind = killcore::LocatorKind::ModuleOffset;
        locator.module = bestModule;
        locator.offset = address - bestBase;
        locator.lastAddress = address;
    } else {
        // Fallback : adresse absolue (non stable)
        locator.kind = killcore::LocatorKind::Absolute;
        locator.lastAddress = address;
    }

    // Charge le profil existant ou crée un nouveau
    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (QFile::exists(path)) {
        killcore::ProfileStore::load(path, &profile);
    } else {
        profile.gameName = profileName;
        profile.executableName = m_controller.m_processName;
    }
    if (profile.executableHash.isEmpty()) {
        profile.executableHash = computeExecutableHash(m_controller.m_handle.executablePath());
    }

    // Ajoute ou met à jour la cible
    bool found = false;
    for (auto& target : profile.targets) {
        if (target.name == targetName) {
            target.type = type;
            target.locator = locator;
            target.description = description;
            found = true;
            break;
        }
    }
    if (!found) {
        profile.targets.append({targetName, type, locator, description});
    }

    const bool saved = killcore::ProfileStore::save(profile, path);
    result["success"] = saved;
    result["profileName"] = profileName;
    result["targetName"] = targetName;
    result["locator"] = locator.toString();
    result["locatorKind"] = (locator.kind == killcore::LocatorKind::ModuleOffset) ? "module_offset" : "absolute";
    result["module"] = locator.module;
    result["offset"] = QString::number(locator.offset, 16);
    result["targetCount"] = profile.targets.size();
    if (!saved) {
        result["error"] = KE_TXT("Échec de la sauvegarde du profil.", "Couldn't save the profile.");
    }

    return result;
}

QVariantMap ProfileManager::saveClrFieldProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QString& typeSubstring,
    const QString& identityField,
    const QString& identityValue,
    const QString& targetField,
    const QString& valueType,
    const QString& description) {

    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanTargetName = targetName.trimmed();
    const QString typeFilter = typeSubstring.trimmed();
    const QString idField = identityField.trimmed();
    const QString idValue = identityValue.trimmed();
    const QString field = targetField.trimmed();
    if (cleanProfileName.isEmpty() || cleanTargetName.isEmpty() || typeFilter.isEmpty()
        || idField.isEmpty() || idValue.isEmpty() || field.isEmpty()) {
        result["error"] = KE_TXT("Profil, cible, type CLR, champ identité, valeur identité et champ cible requis.",
            "Profile, target, CLR type, identity field, identity value and target field are required.");
        return result;
    }

    killcore::ValueType type;
    if (!killcore::parseValueType(valueType, &type)) {
        result["error"] = KE_TXT("Type invalide.", "Invalid type.");
        return result;
    }

    QVariantMap locatorProbe = m_controller.findClrObjectsByFieldValue(typeFilter, idField, idValue, 1);
    if (!locatorProbe.value("success").toBool()) {
        const QString error = locatorProbe.value("error").toString();
        result["error"] = error.isEmpty() ? "Locator CLR introuvable. Attache d'abord le helper CLR." : error;
        return result;
    }
    const QVariantMap locatorResult = locatorProbe.value("result").toMap();
    const QVariantList matches = locatorResult.value("matches").toList();
    if (matches.isEmpty()) {
        result["error"] = KE_TXT("Aucun objet CLR ne correspond à ce locator.", "No CLR object matches this locator.");
        return result;
    }

    const QVariantMap firstMatch = matches.first().toMap();
    const QString objectAddressText = firstMatch.value("address").toString();
    uint64_t lastAddress = 0;
    parseHexAddress(objectAddressText, &lastAddress);

    killcore::Locator locator;
    locator.kind = killcore::LocatorKind::ClrField;
    locator.lastAddress = lastAddress;
    locator.clrField.typeSubstring = typeFilter;
    locator.clrField.identityField = idField;
    locator.clrField.identityValue = idValue;
    locator.clrField.targetField = field;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (QFile::exists(path)) {
        killcore::ProfileStore::load(path, &profile);
    } else {
        profile.gameName = cleanProfileName;
        profile.executableName = m_controller.m_processName;
    }
    if (profile.executableHash.isEmpty()) {
        profile.executableHash = computeExecutableHash(m_controller.m_handle.executablePath());
    }

    bool found = false;
    for (auto& target : profile.targets) {
        if (target.name == cleanTargetName) {
            target.type = type;
            target.locator = locator;
            target.description = description;
            found = true;
            break;
        }
    }
    if (!found) {
        profile.targets.append({cleanTargetName, type, locator, description});
    }

    const bool saved = killcore::ProfileStore::save(profile, path);
    result["success"] = saved;
    result["profileName"] = cleanProfileName;
    result["targetName"] = cleanTargetName;
    result["locator"] = locator.toString();
    result["locatorKind"] = "clr_field";
    result["address"] = objectAddressText;
    result["typeSubstring"] = typeFilter;
    result["identityField"] = idField;
    result["identityValue"] = idValue;
    result["fieldName"] = field;
    result["type"] = killcore::valueTypeToString(type);
    result["targetCount"] = profile.targets.size();
    if (!saved) {
        result["error"] = KE_TXT("Échec de la sauvegarde du profil.", "Failed to save the profile.");
    }

    m_controller.appendScanTelemetry(QStringLiteral("clr_inspector_profile_target_save"), {
        {"success", saved},
        {"profileName", cleanProfileName},
        {"targetName", cleanTargetName},
        {"typeSubstring", typeFilter},
        {"identityField", idField},
        {"targetField", field},
        {"error", result.value("error").toString()},
    });

    return result;
}

QVariantList ProfileManager::listProfiles() {
    QVariantList result;
    const auto names = killcore::ProfileStore::listProfiles();
    for (const auto& name : names) {
        QVariantMap entry;
        entry["name"] = name;

        // Charge les infos de base
        killcore::Profile profile;
        if (killcore::ProfileStore::load(killcore::ProfileStore::profilePath(name), &profile)) {
            entry["gameName"] = profile.gameName;
            entry["executableName"] = profile.executableName;
            entry["targetCount"] = profile.targets.size();
            entry["patchCount"] = profile.patches.size();
        }
        result.append(entry);
    }
    return result;
}

QVariantMap ProfileManager::loadProfile(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable ou illisible.", "Profile not found or unreadable.");
        return result;
    }

    result["success"] = true;
    result["profileName"] = profileName;
    result["gameName"] = profile.gameName;
    result["executableName"] = profile.executableName;

    QVariantList targetsList;
    for (const auto& target : profile.targets) {
        QVariantMap targetEntry;
        targetEntry["name"] = target.name;
        targetEntry["type"] = killcore::valueTypeToString(target.type);
        targetEntry["locator"] = target.locator.toString();
        targetEntry["description"] = target.description;
        targetEntry["ghidraSymbol"] = target.ghidraSymbol;
        targetEntry["ghidraNote"] = target.ghidraNote;
        QVariantList dependencies;
        for (const auto& dependency : target.dependsOn) {
            dependencies.append(dependency);
        }
        targetEntry["dependsOn"] = dependencies;
        switch (target.locator.kind) {
            case killcore::LocatorKind::Absolute:
                targetEntry["locatorKind"] = "absolute";
                break;
            case killcore::LocatorKind::ModuleOffset:
                targetEntry["locatorKind"] = "module_offset";
                break;
            case killcore::LocatorKind::PointerChain:
                targetEntry["locatorKind"] = "pointer_chain";
                break;
            case killcore::LocatorKind::ClrField:
                targetEntry["locatorKind"] = "clr_field";
                targetEntry["clrTypeSubstring"] = target.locator.clrField.typeSubstring;
                targetEntry["clrIdentityField"] = target.locator.clrField.identityField;
                targetEntry["clrIdentityValue"] = target.locator.clrField.identityValue;
                targetEntry["clrFieldName"] = target.locator.clrField.targetField;
                break;
        }
        targetsList.append(targetEntry);
    }
    result["targets"] = targetsList;
    result["targetCount"] = targetsList.size();

    QVariantList patchesList;
    for (const auto& patch : profile.patches) {
        QVariantMap patchEntry;
        patchEntry["name"] = patch.name;
        patchEntry["module"] = patch.module;
        patchEntry["moduleOffset"] = QString::number(patch.moduleOffset, 16).toUpper();
        patchEntry["aobPattern"] = patch.aobPattern;
        patchEntry["patchBytes"] = patch.patchBytes;
        patchEntry["originalBytes"] = patch.originalBytes;
        patchEntry["disassembly"] = patch.disassembly;
        patchEntry["riskLevel"] = patch.riskLevel;
        patchEntry["description"] = patch.description;
        patchEntry["ghidraSymbol"] = patch.ghidraSymbol;
        patchEntry["ghidraNote"] = patch.ghidraNote;
        patchEntry["signatureScore"] = patch.signatureScore;
        patchEntry["signatureLevel"] = patch.signatureLevel;
        patchEntry["signatureWarning"] = patch.signatureWarning;
        patchEntry["signatureFixedBytes"] = patch.signatureFixedBytes;
        patchEntry["signatureWildcardBytes"] = patch.signatureWildcardBytes;
        patchEntry["signatureUniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
        patchEntry["signatureFixedRatio"] = patch.signatureFixedRatio;
        patchEntry["trainerSafe"] = patch.trainerSafe;
        patchEntry["signatureMatches"] = patch.signatureMatches;
        patchesList.append(patchEntry);
    }
    result["patches"] = patchesList;
    result["patchCount"] = patchesList.size();

    QVariantList autoAsmScriptsList;
    for (const auto& script : profile.autoAsmScripts) {
        QVariantMap scriptEntry;
        scriptEntry["name"] = script.name;
        scriptEntry["scriptText"] = script.scriptText;
        scriptEntry["description"] = script.description;
        scriptEntry["riskLevel"] = script.riskLevel;
        autoAsmScriptsList.append(scriptEntry);
    }
    result["autoAsmScripts"] = autoAsmScriptsList;
    result["autoAsmScriptCount"] = autoAsmScriptsList.size();

    QVariantList luaScriptsList;
    for (const auto& script : profile.luaScripts) {
        QVariantMap scriptEntry;
        scriptEntry["name"] = script.name;
        scriptEntry["scriptText"] = script.scriptText;
        scriptEntry["description"] = script.description;
        scriptEntry["savedAtEpochMs"] = QString::number(script.savedAtEpochMs);
        scriptEntry["savedAt"] = script.savedAtEpochMs != 0
            ? QDateTime::fromMSecsSinceEpoch(script.savedAtEpochMs).toString(Qt::ISODate)
            : QString();
        luaScriptsList.append(scriptEntry);
    }
    result["luaScripts"] = luaScriptsList;
    result["luaScriptCount"] = luaScriptsList.size();

    return result;
}

bool ProfileManager::deleteProfile(const QString& profileName) {
    return killcore::ProfileStore::remove(profileName);
}

QVariantMap ProfileManager::resolveProfileTarget(const QString& profileName, const QString& targetName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    for (const auto& target : profile.targets) {
        if (target.name == targetName) {
            if (target.locator.kind == killcore::LocatorKind::ClrField) {
                const auto& clr = target.locator.clrField;
                QVariantMap locatorProbe = m_controller.findClrObjectsByFieldValue(clr.typeSubstring, clr.identityField, clr.identityValue, 1);
                if (!locatorProbe.value("success").toBool()) {
                    const QString error = locatorProbe.value("error").toString();
                    result["error"] = error.isEmpty() ? KE_TXT("Impossible de résoudre le locator CLR.", "Unable to resolve the CLR locator.") : error;
                    return result;
                }
                const QVariantMap locatorResult = locatorProbe.value("result").toMap();
                const QVariantList matches = locatorResult.value("matches").toList();
                if (matches.isEmpty()) {
                    result["error"] = KE_TXT("Aucun objet CLR ne correspond à ce locator.", "No CLR object matches this locator.");
                    return result;
                }

                const QVariantMap match = matches.first().toMap();
                result["success"] = true;
                result["address"] = match.value("address").toString();
                result["type"] = killcore::valueTypeToString(target.type);
                result["locator"] = target.locator.toString();
                result["locatorKind"] = "clr_field";
                result["clrTypeSubstring"] = clr.typeSubstring;
                result["clrIdentityField"] = clr.identityField;
                result["clrIdentityValue"] = clr.identityValue;
                result["clrFieldName"] = clr.targetField;
                result["matchesReturned"] = locatorResult.value("matchesReturned").toInt();
                result["typeMatches"] = locatorResult.value("typeMatches").toInt();
                return result;
            }

            uint64_t address = 0;
            if (killcore::resolveLocatorAddress(m_controller.m_handle, target.locator, &address)) {
                result["success"] = true;
                result["address"] = QString::number(address, 16);
                result["type"] = killcore::valueTypeToString(target.type);
                result["locator"] = target.locator.toString();
                result["locatorKind"] = target.locator.kind == killcore::LocatorKind::PointerChain
                    ? "pointer_chain"
                    : (target.locator.kind == killcore::LocatorKind::Absolute ? "absolute" : "module_offset");
                return result;
            } else {
                result["error"] = KE_TXT("Impossible de résoudre le locator. Le module est peut-être absent.",
                    "Unable to resolve the locator. The module may be missing.");
                return result;
            }
        }
    }

    result["error"] = KE_TXT("Cible introuvable dans le profil.", "Target not found in the profile.");
    return result;
}

QVariantMap ProfileManager::comparePointerMapAcrossRestart(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    auto locatorKindLabel = [](killcore::LocatorKind kind) -> QString {
        switch (kind) {
            case killcore::LocatorKind::ModuleOffset: return "module_offset";
            case killcore::LocatorKind::Absolute:     return "absolute";
            case killcore::LocatorKind::PointerChain: return "pointer_chain";
            case killcore::LocatorKind::ClrField:     return "clr_field";
        }
        return "unknown";
    };

    QVariantList entries;
    int validCount = 0;
    int invalidCount = 0;
    int unsupportedCount = 0;

    for (const auto& target : profile.targets) {
        QVariantMap entry;
        entry["targetName"] = target.name;
        entry["locatorKind"] = locatorKindLabel(target.locator.kind);
        entry["previousAddress"] = QString::number(target.locator.lastAddress, 16);

        uint64_t address = 0;
        if (killcore::resolveLocatorAddress(m_controller.m_handle, target.locator, &address)) {
            entry["status"] = "valid";
            entry["address"] = QString::number(address, 16);
            ++validCount;
        } else {
            entry["status"] = "invalid";
            entry["address"] = "";
            ++invalidCount;
        }
        entries.append(entry);
    }

    result["success"] = true;
    result["profileName"] = profileName;
    result["results"] = entries;
    result["validCount"] = validCount;
    result["invalidCount"] = invalidCount;
    result["unsupportedCount"] = unsupportedCount;
    result["error"] = "";
    return result;
}

QVariantMap ProfileManager::exportPointerMap(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    if (cleanProfileName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil requis.", "Profile name required.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    const QJsonObject pointerMap = killcore::ProfileStore::exportPointerMap(profile);
    const QJsonDocument doc(pointerMap);
    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["targetCount"] = pointerMap.value("targetCount").toInt();
    result["json"] = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    return result;
}

QVariantMap ProfileManager::importPointerMap(
    const QString& profileName,
    const QString& pointerMapJson,
    const QVariantMap& options) {

    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    if (cleanProfileName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil requis.", "Profile name required.");
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(pointerMapJson.trimmed().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result["error"] = KE_TXT("JSON pointer map invalide : %1", "Invalid pointer map JSON: %1").arg(parseError.errorString());
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    const bool profileExists = killcore::ProfileStore::load(path, &profile);
    if (!profileExists) {
        const QJsonObject root = doc.object();
        profile.gameName = cleanProfileName;
        profile.executableName = root.value("sourceExecutableName").toString(m_controller.m_processName);
        profile.executableHash = root.value("sourceExecutableHash").toString();
    }

    const bool replaceExisting = options.value("replaceExisting", false).toBool();
    const auto importResult = killcore::ProfileStore::mergePointerMap(&profile, doc.object(), replaceExisting);
    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    QVariantList messages;
    for (const auto& message : importResult.messages) {
        messages.append(message);
    }
    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["isNewProfile"] = !profileExists;
    result["imported"] = importResult.imported;
    result["replaced"] = importResult.replaced;
    result["skipped"] = importResult.skipped;
    result["messages"] = messages;
    result["targetCount"] = profile.targets.size();
    return result;
}

QVariantMap ProfileManager::setProfileTargetDependencies(
    const QString& profileName,
    const QString& targetName,
    const QVariantList& dependencyNames) {

    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanTargetName = targetName.trimmed();
    if (cleanProfileName.isEmpty() || cleanTargetName.isEmpty()) {
        result["error"] = KE_TXT("Profil et cible requis.", "Profile and target required.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    QStringList cleanDependencies;
    for (const auto& item : dependencyNames) {
        const QString dependency = item.toString().trimmed();
        if (!dependency.isEmpty()
            && dependency.compare(cleanTargetName, Qt::CaseInsensitive) != 0
            && !cleanDependencies.contains(dependency)) {
            cleanDependencies.append(dependency);
        }
    }

    for (auto& target : profile.targets) {
        if (target.name.compare(cleanTargetName, Qt::CaseInsensitive) == 0) {
            target.dependsOn = cleanDependencies;
            if (!killcore::ProfileStore::save(profile, path)) {
                result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
                return result;
            }

            result["success"] = true;
            result["profileName"] = cleanProfileName;
            result["targetName"] = target.name;
            QVariantList dependencies;
            for (const auto& dependency : cleanDependencies) {
                dependencies.append(dependency);
            }
            result["dependsOn"] = dependencies;
            result["dependencyCount"] = cleanDependencies.size();
            return result;
        }
    }

    result["error"] = KE_TXT("Cible introuvable dans le profil.", "Target not found in the profile.");
    return result;
}

QVariantMap ProfileManager::exportGhidraArtifacts(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    if (cleanProfileName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil requis.", "Profile name required.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    const QJsonObject exportJson = killcore::exportGhidraArtifacts(profile);
    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["artifactCount"] = exportJson.value("artifactCount").toInt();
    result["json"] = QString::fromUtf8(QJsonDocument(exportJson).toJson(QJsonDocument::Indented));
    result["pythonScript"] = killcore::generateGhidraImportScript(exportJson);
    return result;
}

QVariantMap ProfileManager::importGhidraSymbols(const QString& profileName, const QString& symbolsText) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    if (cleanProfileName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil requis.", "Profile name required.");
        return result;
    }
    if (symbolsText.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Export Ghidra vide.", "Empty Ghidra export.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    killcore::GhidraSymbolImportResult importResult;
    QString error;
    if (!killcore::importGhidraSymbols(&profile, symbolsText.toUtf8(), &importResult, &error)) {
        result["error"] = error;
        return result;
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    QVariantList messages;
    for (const auto& message : importResult.messages) {
        messages.append(message);
    }
    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["symbolsRead"] = importResult.symbolsRead;
    result["targetsUpdated"] = importResult.targetsUpdated;
    result["patchesUpdated"] = importResult.patchesUpdated;
    result["unmatched"] = importResult.unmatched;
    result["messages"] = messages;
    return result;
}

QVariantMap ProfileManager::activateProfileTarget(const QString& profileName, const QString& targetName) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    for (const auto& target : profile.targets) {
        if (target.name == targetName) {
            if (target.locator.kind == killcore::LocatorKind::ClrField) {
                const auto& locator = target.locator.clrField;
                auto locatorResult = m_controller.findClrObjectsByFieldValue(
                    locator.typeSubstring,
                    locator.identityField,
                    locator.identityValue,
                    1);
                const QVariantMap payload = locatorResult.value("result").toMap();
                const QVariantList matches = payload.value("matches").toList();
                if (!locatorResult.value("success").toBool() || matches.isEmpty()) {
                    result["error"] = locatorResult.value("error").toString().isEmpty()
                        ? KE_TXT("Impossible de retrouver l'objet CLR par son champ d'identité.", "Unable to find the CLR object by its identity field.")
                        : locatorResult.value("error").toString();
                    result["locatorKind"] = "clr_field";
                    return result;
                }

                uint64_t objectAddress = 0;
                const QString objectAddressText = matches.first().toMap().value("address").toString();
                if (!parseHexAddress(objectAddressText, &objectAddress)) {
                    result["error"] = KE_TXT("Le locator CLR a renvoyé une adresse objet invalide.", "The CLR locator returned an invalid object address.");
                    result["locatorKind"] = "clr_field";
                    return result;
                }

                const ApplicationController::ActiveProfileTarget active{
                    profileName,
                    target.name,
                    profileTargetGroupName(target.name),
                    objectAddress,
                    target.type,
                    killcore::LocatorKind::ClrField,
                    locator.typeSubstring,
                    locator.identityField,
                    locator.identityValue,
                    locator.targetField,
                };

                bool updated = false;
                for (auto& existing : m_controller.m_activeProfileTargets) {
                    if (existing.profileName == active.profileName && existing.targetName == active.targetName) {
                        existing = active;
                        updated = true;
                        break;
                    }
                }
                if (!updated) {
                    m_controller.m_activeProfileTargets.append(active);
                }

                result["success"] = true;
                result["profileName"] = profileName;
                result["targetName"] = target.name;
                result["groupName"] = active.groupName;
                result["address"] = QString::number(objectAddress, 16);
                result["type"] = killcore::valueTypeToString(target.type);
                result["locatorKind"] = "clr_field";
                result["clrTypeSubstring"] = locator.typeSubstring;
                result["clrIdentityField"] = locator.identityField;
                result["clrIdentityValue"] = locator.identityValue;
                result["clrFieldName"] = locator.targetField;
                result["activeTargetCount"] = m_controller.m_activeProfileTargets.size();
                result["message"] = KE_TXT("« %1 » activé pour l'Assistant via CLR : objet 0x%2, champ %3.",
                                        "\"%1\" activated for the Assistant via CLR: object 0x%2, field %3.")
                                        .arg(target.name, QString::number(objectAddress, 16), locator.targetField);
                return result;
            }

            uint64_t address = 0;
            if (!killcore::resolveLocatorAddress(m_controller.m_handle, target.locator, &address)) {
                result["error"] = KE_TXT("Impossible d'activer cette cible. Le module est peut-être absent.",
                    "Unable to activate this target. The module may be missing.");
                return result;
            }

            const ApplicationController::ActiveProfileTarget active{
                profileName,
                target.name,
                profileTargetGroupName(target.name),
                address,
                target.type,
                target.locator.kind,
            };

            bool updated = false;
            for (auto& existing : m_controller.m_activeProfileTargets) {
                if (existing.profileName == active.profileName && existing.targetName == active.targetName) {
                    existing = active;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                m_controller.m_activeProfileTargets.append(active);
            }

            bool autoWriteTargetUpdated = false;
            for (auto& existing : m_controller.m_lastAutoWriteTargets) {
                if (existing.address == active.address) {
                    existing.type = active.type;
                    autoWriteTargetUpdated = true;
                    break;
                }
            }
            if (!autoWriteTargetUpdated) {
                m_controller.m_lastAutoWriteTargets.append({active.address, active.type});
            }

            result["success"] = true;
            result["profileName"] = profileName;
            result["targetName"] = target.name;
            result["groupName"] = active.groupName;
            result["address"] = QString::number(address, 16);
            result["type"] = killcore::valueTypeToString(target.type);
            result["activeTargetCount"] = m_controller.m_activeProfileTargets.size();
            result["message"] = KE_TXT("« %1 » activé pour l'Assistant à l'adresse 0x%2.",
                                    "\"%1\" activated for the Assistant at address 0x%2.")
                                    .arg(target.name, QString::number(address, 16));
            return result;
        }
    }

    result["error"] = KE_TXT("Cible introuvable dans le profil.", "Target not found in the profile.");
    return result;
}

QVariantMap ProfileManager::saveProfileCodePatch(
    const QString& profileName,
    const QString& patchName,
    const QString& addressHex,
    const QString& aobPattern,
    const QString& patchBytes,
    const QVariantMap& metadata) {

    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanPatchName = patchName.trimmed();
    if (cleanProfileName.isEmpty() || cleanPatchName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil ou de patch vide.", "Empty profile or patch name.");
        return result;
    }

    uint64_t address = 0;
    if (!parseHexAddress(addressHex, &address)) {
        result["error"] = KE_TXT("Adresse patch invalide.", "Invalid patch address.");
        return result;
    }

    const auto parsedPattern = killcore::parseAobPattern(aobPattern);
    if (!parsedPattern.isValid()) {
        result["error"] = parsedPattern.error;
        return result;
    }
    const auto patternQuality = killcore::evaluateAobPatternQuality(parsedPattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(patternQuality);
    result["signatureRisk"] = patternQuality.level;

    const bool allowWeakAob = metadata.value("allowWeakAob", false).toBool();
    if (!allowWeakAob && (patternQuality.fixedBytes < 3 || patternQuality.score < 35)) {
        result["errorCode"] = QStringLiteral("aob_signature_too_weak");
        result["error"] = KE_TXT("Signature AOB trop faible pour Trainer (%1/100, %2 octet(s) fixe(s)). Allonge la signature ou ajoute des octets fixes.",
                              "AOB signature too weak for the Trainer (%1/100, %2 fixed byte(s)). Lengthen the signature or add more fixed bytes.")
                              .arg(patternQuality.score)
                              .arg(patternQuality.fixedBytes);
        result["signatureWarning"] = patternQuality.warning;
        m_controller.appendScanTelemetry("trainer_patch_save_blocked", result);
        return result;
    }

    killcore::AobScanOptions uniquenessOptions;
    uniquenessOptions.executableOnly = true;
    uniquenessOptions.imageOnly = true;
    uniquenessOptions.maxResults = 2;
    const auto uniquenessScan = killcore::scanAobPattern(m_controller.m_handle, parsedPattern, uniquenessOptions);
    result["signatureMatches"] = uniquenessScan.matchesFound;
    if (!uniquenessScan.success || uniquenessScan.matchesFound != 1) {
        if (uniquenessScan.matchesFound == 0) {
            result["errorCode"] = QStringLiteral("aob_signature_not_found");
            result["error"] = KE_TXT("Signature AOB introuvable dans le code image exécutable.",
                "AOB signature not found in the executable image code.");
        } else {
            result["errorCode"] = QStringLiteral("aob_signature_not_unique");
            result["error"] = KE_TXT("Signature AOB non unique (%1 matches). Sauvegarde Trainer bloquée.",
                "AOB signature not unique (%1 matches). Trainer save blocked.").arg(uniquenessScan.matchesFound);
        }
        m_controller.appendScanTelemetry("trainer_patch_save_blocked", result);
        return result;
    }

    const auto parsedPatch = killcore::parsePatchBytes(patchBytes);
    if (!parsedPatch.isValid()) {
        result["error"] = parsedPatch.error;
        return result;
    }

    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_controller.m_pid);
    QString moduleName;
    uint64_t moduleOffset = 0;
    uint64_t bestSize = 0;
    for (const auto& module : modules) {
        if (address >= module.baseAddress && address < module.baseAddress + module.size) {
            if (moduleName.isEmpty() || module.size < bestSize) {
                moduleName = module.name;
                moduleOffset = address - module.baseAddress;
                bestSize = module.size;
            }
        }
    }

    QString originalBytes = metadata.value("originalBytes").toString().trimmed();
    if (originalBytes.isEmpty()) {
        killcore::MemoryReader reader(m_controller.m_handle);
        const auto read = reader.readChunked(address, static_cast<size_t>(parsedPatch.bytes.size()), 4096);
        if (read.success || read.partial) {
            originalBytes = QString::fromLatin1(read.data.toHex(' ').toUpper());
        }
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        profile.gameName = cleanProfileName;
        profile.executableName = m_controller.m_processName;
    }
    // Hash calculé une seule fois (a la creation du profil, ou a la
    // migration d'un profil existant qui n'en avait pas encore un) plutot
    // qu'a chaque sauvegarde : sert de reference "version pour laquelle ce
    // profil a ete fait", pas de suivre la derniere version utilisee.
    if (profile.executableHash.isEmpty()) {
        profile.executableHash = computeExecutableHash(m_controller.m_handle.executablePath());
    }

    killcore::ProfileCodePatch patch;
    patch.name = cleanPatchName;
    patch.module = moduleName;
    patch.moduleOffset = moduleOffset;
    patch.aobPattern = aobPattern.trimmed();
    patch.patchBytes = patchBytes.trimmed();
    patch.originalBytes = originalBytes;
    patch.disassembly = metadata.value("disassembly").toString();
    patch.riskLevel = metadata.value("riskLevel").toString();
    patch.description = metadata.value("description").toString();
    patch.signatureScore = patternQuality.score;
    patch.signatureLevel = patternQuality.level;
    patch.signatureWarning = patternQuality.warning;
    patch.signatureFixedBytes = patternQuality.fixedBytes;
    patch.signatureWildcardBytes = patternQuality.wildcardBytes;
    patch.signatureUniqueFixedBytes = patternQuality.uniqueFixedBytes;
    patch.signatureFixedRatio = patternQuality.fixedRatio;
    patch.trainerSafe = patternQuality.trainerSafe;
    patch.signatureMatches = uniquenessScan.matchesFound;

    bool replaced = false;
    for (auto& existing : profile.patches) {
        if (existing.name == patch.name) {
            patch.ghidraSymbol = existing.ghidraSymbol;
            patch.ghidraNote = existing.ghidraNote;
            existing = patch;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profile.patches.append(patch);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["patchName"] = cleanPatchName;
    result["module"] = moduleName;
    result["moduleOffset"] = QString::number(moduleOffset, 16).toUpper();
    result["patchCount"] = profile.patches.size();
    result["replaced"] = replaced;
    result["signatureMatches"] = uniquenessScan.matchesFound;
    result["signatureWarning"] = patternQuality.warning;
    m_controller.appendScanTelemetry("trainer_patch_saved", result);
    return result;
}

QVariantMap ProfileManager::applyProfileCodePatch(const QString& profileName, const QString& patchName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["patchName"] = patchName;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    std::optional<killcore::ProfileCodePatch> patch;
    for (const auto& item : profile.patches) {
        if (item.name == patchName) {
            patch = item;
            break;
        }
    }
    if (!patch) {
        result["error"] = KE_TXT("Patch introuvable dans le profil.", "Patch not found in the profile.");
        return result;
    }

    // Cause la plus probable d'une signature AOB qui ne matche plus rien :
    // le jeu a été mis à jour depuis que ce patch a été sauvegardé. Comparer
    // le hash de l'exécutable attaché à celui enregistré permet de le dire
    // avec certitude plutôt que de laisser un "signature introuvable" nu que
    // l'utilisateur ne peut pas distinguer d'un bug du côté de l'outil.
    const bool hasKnownExecutableHash = !profile.executableHash.isEmpty();
    const QString currentExecutableHash = hasKnownExecutableHash
        ? computeExecutableHash(m_controller.m_handle.executablePath())
        : QString();
    const bool executableVersionMismatch = hasKnownExecutableHash
        && !currentExecutableHash.isEmpty()
        && currentExecutableHash != profile.executableHash;
    result["executableVersionMismatch"] = executableVersionMismatch;

    const auto pattern = killcore::parseAobPattern(patch->aobPattern);
    if (!pattern.isValid()) {
        result["error"] = pattern.error;
        return result;
    }
    const auto quality = killcore::evaluateAobPatternQuality(pattern);
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    if (quality.fixedBytes < 3 || quality.score < 35) {
        result["errorCode"] = QStringLiteral("aob_signature_too_weak");
        result["error"] = KE_TXT("Patch bloqué : signature AOB trop faible (%1/100, %2 octet(s) fixe(s)).",
                              "Patch blocked: AOB signature too weak (%1/100, %2 fixed byte(s)).")
                              .arg(quality.score)
                              .arg(quality.fixedBytes);
        result["signatureWarning"] = quality.warning;
        m_controller.appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = true;
    options.imageOnly = true;
    options.maxResults = 100;
    const auto scan = killcore::scanAobPattern(m_controller.m_handle, pattern, options);
    if (!scan.success || scan.matches.isEmpty()) {
        if (executableVersionMismatch) {
            result["errorCode"] = QStringLiteral("aob_signature_not_found_version_mismatch");
            result["error"] = KE_TXT(
                "Signature AOB introuvable — et ce patch a été enregistré pour une version différente "
                "de l'exécutable (l'empreinte du fichier ne correspond pas à celle attachée "
                "actuellement). C'est très probablement pourquoi : une mise à jour du jeu a changé "
                "les octets autour de cette instruction. Recapture-la depuis \"Écrit par\" sur cette version.",
                "AOB signature not found — and this patch was saved for a different version "
                "of the executable (the file's fingerprint doesn't match the one currently attached). "
                "This is very likely why: a game update changed the bytes around this instruction. "
                "Recapture it from \"Written by\" on this version.");
        } else {
            result["errorCode"] = QStringLiteral("aob_signature_not_found");
            result["error"] = scan.error.isEmpty() ? KE_TXT("Signature AOB introuvable.", "AOB signature not found.") : scan.error;
        }
        m_controller.appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }
    result["signatureMatches"] = scan.matchesFound;
    if (scan.matchesFound != 1) {
        result["errorCode"] = QStringLiteral("aob_signature_not_unique");
        result["error"] = KE_TXT("Patch bloqué : signature AOB non unique (%1 matches). Regénère une signature plus spécifique.",
                              "Patch blocked: AOB signature not unique (%1 matches). Regenerate a more specific signature.")
                              .arg(scan.matchesFound);
        m_controller.appendScanTelemetry("trainer_patch_apply_blocked", result);
        return result;
    }

    const auto modules = killcore::ProcessEnumerator::enumerateModules(m_controller.m_pid);
    uint64_t selectedAddress = scan.matches.first().address;
    bool selectedModuleMatch = false;
    for (const auto& match : scan.matches) {
        for (const auto& module : modules) {
            if (match.address >= module.baseAddress && match.address < module.baseAddress + module.size
                && module.name.compare(patch->module, Qt::CaseInsensitive) == 0) {
                selectedAddress = match.address;
                selectedModuleMatch = true;
                break;
            }
        }
        if (selectedModuleMatch) {
            break;
        }
    }

    result = m_controller.applyCodePatch(QString::number(selectedAddress, 16), patch->patchBytes, {{"verify", true}});
    result["profileName"] = profileName;
    result["patchName"] = patchName;
    result["matchedAddress"] = QString::number(selectedAddress, 16).toUpper();
    result["matchCount"] = scan.matches.size();
    result["aobPattern"] = patch->aobPattern;
    result["signatureQuality"] = aobPatternQualityToVariantMap(quality);
    result["signatureRisk"] = quality.level;
    result["signatureMatches"] = scan.matchesFound;
    // La signature a matché quand même : pas bloquant, mais vaut la peine
    // d'être su (ex: une mise à jour mineure du jeu qui n'a pas touché ce
    // code précis — le patch est probablement toujours valide, mais moins
    // certain qu'un hash identique).
    result["executableVersionMismatch"] = executableVersionMismatch;
    if (executableVersionMismatch) {
        result["executableVersionWarning"] = KE_TXT(
            "Exécutable d'une version différente de celle où ce patch a été "
            "enregistré — la signature a quand même matché, mais vérifie le "
            "résultat avant de t'y fier pleinement.",
            "Executable of a different version than the one this patch was "
            "saved for — the signature still matched, but check the result "
            "before fully trusting it.");
    }
    m_controller.appendScanTelemetry(result.value("success").toBool() ? "trainer_patch_apply" : "trainer_patch_apply_failed", result);
    return result;
}

QVariantMap ProfileManager::restoreProfileCodePatch(const QString& profileName, const QString& patchName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["patchName"] = patchName;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    for (const auto& patch : profile.patches) {
        if (patch.name != patchName) {
            continue;
        }
        QList<killcore::AobPattern> restorePatterns;
        const auto originalPattern = killcore::parseAobPattern(patch.aobPattern);
        if (originalPattern.isValid()) {
            restorePatterns.append(originalPattern);
        }
        const auto activePattern = killcore::parseAobPattern(patch.patchBytes);
        if (activePattern.isValid()) {
            restorePatterns.append(activePattern);
        }
        if (restorePatterns.isEmpty()) {
            result["error"] = originalPattern.error.isEmpty() ? activePattern.error : originalPattern.error;
            return result;
        }
        killcore::AobScanOptions options;
        options.executableOnly = true;
        options.imageOnly = true;
        options.maxResults = 100;
        for (const auto& restorePattern : restorePatterns) {
            const auto scan = killcore::scanAobPattern(m_controller.m_handle, restorePattern, options);
            for (const auto& match : scan.matches) {
                if (m_controller.m_codePatchManager->isCodePatchActive(match.address)) {
                    result = m_controller.restoreCodePatch(QString::number(match.address, 16));
                    result["profileName"] = profileName;
                    result["patchName"] = patchName;
                    result["matchedAddress"] = QString::number(match.address, 16).toUpper();
                    return result;
                }
            }
        }
        result["error"] = KE_TXT("Patch non actif dans la session courante.", "Patch not active in the current session.");
        return result;
    }

    result["error"] = KE_TXT("Patch introuvable dans le profil.", "Patch not found in the profile.");
    return result;
}

QVariantMap ProfileManager::applyAllProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    QVariantList patchResults;
    int applied = 0;
    int alreadyActive = 0;
    for (const auto& patch : profile.patches) {
        QVariantMap patchResult = applyProfileCodePatch(profileName, patch.name);
        if (patchResult.value("success").toBool()) {
            ++applied;
        } else if (patchResult.value("active").toBool()) {
            ++alreadyActive;
            patchResult["alreadyActive"] = true;
        }
        patchResults.append(patchResult);
    }

    result["success"] = applied + alreadyActive == profile.patches.size();
    result["applied"] = applied;
    result["alreadyActive"] = alreadyActive;
    result["total"] = profile.patches.size();
    result["results"] = patchResults;
    if (profile.patches.isEmpty()) {
        result["error"] = KE_TXT("Aucun patch trainer dans ce profil.", "No trainer patch in this profile.");
    } else if (!result.value("success").toBool()) {
        result["error"] = KE_TXT("Application partielle : %1 appliqué(s), %2 déjà actif(s), %3 total.",
                              "Partial application: %1 applied, %2 already active, %3 total.")
                              .arg(applied)
                              .arg(alreadyActive)
                              .arg(profile.patches.size());
    }
    return result;
}

QVariantMap ProfileManager::restoreAllProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    QVariantList patchResults;
    int restored = 0;
    int alreadyInactive = 0;
    for (const auto& patch : profile.patches) {
        QVariantMap patchResult = restoreProfileCodePatch(profileName, patch.name);
        patchResults.append(patchResult);
        if (patchResult.value("success").toBool()) {
            ++restored;
        } else if (patchResult.value("error").toString().contains("non actif", Qt::CaseInsensitive)) {
            ++alreadyInactive;
        }
    }

    result["success"] = restored + alreadyInactive == profile.patches.size();
    result["restored"] = restored;
    result["alreadyInactive"] = alreadyInactive;
    result["total"] = profile.patches.size();
    result["results"] = patchResults;
    if (profile.patches.isEmpty()) {
        result["error"] = KE_TXT("Aucun patch trainer dans ce profil.", "No trainer patch in this profile.");
    } else if (!result.value("success").toBool()) {
        result["error"] = KE_TXT("Restauration partielle : %1 restauré(s), %2 déjà inactif(s), %3 total.",
                              "Partial restore: %1 restored, %2 already inactive, %3 total.")
                              .arg(restored)
                              .arg(alreadyInactive)
                              .arg(profile.patches.size());
    }
    return result;
}

QVariantMap ProfileManager::inspectProfileCodePatches(const QString& profileName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    killcore::AobScanOptions options;
    options.executableOnly = true;
    options.imageOnly = true;
    options.maxResults = 100;

    QVariantList states;
    int originalCount = 0;
    int activeCount = 0;
    int ambiguousCount = 0;
    int missingCount = 0;
    int invalidCount = 0;

    for (const auto& patch : profile.patches) {
        QVariantMap state;
        state["profileName"] = profileName;
        state["patchName"] = patch.name;
        state["module"] = patch.module;
        state["moduleOffset"] = QString::number(patch.moduleOffset, 16).toUpper();
        state["signatureScore"] = patch.signatureScore;
        state["signatureLevel"] = patch.signatureLevel;
        state["signatureWarning"] = patch.signatureWarning;
        state["signatureFixedBytes"] = patch.signatureFixedBytes;
        state["signatureWildcardBytes"] = patch.signatureWildcardBytes;
        state["signatureUniqueFixedBytes"] = patch.signatureUniqueFixedBytes;
        state["signatureFixedRatio"] = patch.signatureFixedRatio;
        state["trainerSafe"] = patch.trainerSafe;
        state["savedSignatureMatches"] = patch.signatureMatches;
        state["success"] = true;
        state["active"] = false;

        const auto originalPattern = killcore::parseAobPattern(patch.aobPattern);
        const auto patchedPattern = killcore::parseAobPattern(patch.patchBytes);
        if (!originalPattern.isValid() && !patchedPattern.isValid()) {
            state["success"] = false;
            state["status"] = "invalid";
            state["error"] = originalPattern.error.isEmpty() ? patchedPattern.error : originalPattern.error;
            ++invalidCount;
            states.append(state);
            continue;
        }

        int originalMatches = 0;
        int patchedMatches = 0;
        QString originalAddress;
        QString patchedAddress;

        if (originalPattern.isValid()) {
            const auto scan = killcore::scanAobPattern(m_controller.m_handle, originalPattern, options);
            originalMatches = scan.matches.size();
            if (!scan.matches.isEmpty()) {
                originalAddress = QString::number(scan.matches.first().address, 16).toUpper();
            }
        }

        if (patchedPattern.isValid()) {
            const auto scan = killcore::scanAobPattern(m_controller.m_handle, patchedPattern, options);
            patchedMatches = scan.matches.size();
            if (!scan.matches.isEmpty()) {
                patchedAddress = QString::number(scan.matches.first().address, 16).toUpper();
            }
        }

        state["originalMatches"] = originalMatches;
        state["patchedMatches"] = patchedMatches;
        state["matchedAddress"] = patchedAddress.isEmpty() ? originalAddress : patchedAddress;

        const bool sessionActive = !patchedAddress.isEmpty() && m_controller.m_codePatchManager->isCodePatchActive(patchedAddress.toULongLong(nullptr, 16));
        const auto memoryState = killcore::classifyProfilePatchMemoryState(
            originalMatches,
            patchedMatches,
            sessionActive,
            originalPattern.isValid() || patchedPattern.isValid());
        state["status"] = memoryState.status;
        state["success"] = memoryState.success;
        state["active"] = memoryState.active;

        if (memoryState.status == "active") {
            ++activeCount;
        } else if (memoryState.status == "original") {
            ++originalCount;
        } else if (memoryState.status == "missing") {
            state["error"] = KE_TXT("Signature originale et patchée introuvables.", "Original and patched signatures not found.");
            ++missingCount;
        } else {
            state["warning"] = KE_TXT("Signature non unique ou état mixte.", "Signature not unique or mixed state.");
            ++ambiguousCount;
        }

        states.append(state);
    }

    result["success"] = invalidCount == 0 && missingCount == 0;
    result["states"] = states;
    result["total"] = profile.patches.size();
    result["original"] = originalCount;
    result["active"] = activeCount;
    result["ambiguous"] = ambiguousCount;
    result["missing"] = missingCount;
    result["invalid"] = invalidCount;
    if (profile.patches.isEmpty()) {
        result["error"] = KE_TXT("Aucun patch trainer dans ce profil.", "No trainer patch in this profile.");
    } else if (!result.value("success").toBool()) {
        result["error"] = KE_TXT("Inspection : %1 actif(s), %2 original(aux), %3 ambigu(s), %4 introuvable(s), %5 invalide(s).",
                              "Inspection: %1 active, %2 original, %3 ambiguous, %4 not found, %5 invalid.")
                              .arg(activeCount)
                              .arg(originalCount)
                              .arg(ambiguousCount)
                              .arg(missingCount)
                              .arg(invalidCount);
    }
    return result;
}

QVariantMap ProfileManager::saveProfileAutoAsmScript(
    const QString& profileName,
    const QString& scriptName,
    const QString& scriptText,
    const QVariantMap& metadata) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanScriptName = scriptName.trimmed();
    if (cleanProfileName.isEmpty() || cleanScriptName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil ou de script vide.", "Empty profile or script name.");
        return result;
    }
    if (scriptText.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Script vide.", "Empty script.");
        return result;
    }

    // Refuse de sauvegarder un script qui ne parse meme pas — evite de
    // stocker un texte casse qu'on ne pourra jamais rejouer plus tard.
    const auto parsed = killcore::parseAutoAsmScript(scriptText);
    if (!parsed.success) {
        result["error"] = parsed.error;
        result["errorLine"] = parsed.errorLine;
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        profile.gameName = cleanProfileName;
        profile.executableName = m_controller.m_processName;
    }
    if (profile.executableHash.isEmpty() && m_controller.m_handle.isValid()) {
        profile.executableHash = computeExecutableHash(m_controller.m_handle.executablePath());
    }

    killcore::ProfileAutoAsmScript script;
    script.name = cleanScriptName;
    script.scriptText = scriptText;
    script.description = metadata.value("description").toString();
    script.riskLevel = metadata.value("riskLevel").toString();

    bool replaced = false;
    for (auto& existing : profile.autoAsmScripts) {
        if (existing.name == script.name) {
            existing = script;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profile.autoAsmScripts.append(script);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["scriptName"] = cleanScriptName;
    result["scriptCount"] = profile.autoAsmScripts.size();
    result["replaced"] = replaced;
    m_controller.appendScanTelemetry("auto_asm_script_saved", result);
    return result;
}

QVariantMap ProfileManager::applyProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["scriptName"] = scriptName;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    for (const auto& script : profile.autoAsmScripts) {
        if (script.name == scriptName) {
            // Reutilise tel quel le chemin d'execution manuel — meme garde-fou
            // "un seul script actif a la fois", meme suivi de restauration.
            return m_controller.executeAutoAssemblerScript(script.scriptText);
        }
    }

    result["error"] = KE_TXT("Script auto-assembler introuvable dans ce profil.", "Auto-assembler script not found in this profile.");
    return result;
}

QVariantMap ProfileManager::deleteProfileAutoAsmScript(const QString& profileName, const QString& scriptName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["scriptName"] = scriptName;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    const int before = profile.autoAsmScripts.size();
    profile.autoAsmScripts.removeIf([&](const killcore::ProfileAutoAsmScript& script) {
        return script.name == scriptName;
    });
    if (profile.autoAsmScripts.size() == before) {
        result["error"] = KE_TXT("Script auto-assembler introuvable dans ce profil.", "Auto-assembler script not found in this profile.");
        return result;
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["scriptCount"] = profile.autoAsmScripts.size();
    return result;
}


namespace {

killcore::PointerChain variantMapToPointerChain(const QVariantMap& chainMap, QString* error = nullptr) {
    killcore::PointerChain chain;
    chain.module = chainMap.value("module").toString();
    if (chain.module.isEmpty()) {
        if (error) *error = KE_TXT("Chaîne invalide : module manquant.", "Invalid chain: missing module.");
        return chain;
    }

    const QString baseOffsetHex = chainMap.value("baseOffset").toString();
    bool ok = false;
    chain.baseOffset = baseOffsetHex.toULongLong(&ok, 16);
    if (!ok) {
        if (error) *error = KE_TXT("Chaîne invalide : baseOffset hex invalide.", "Invalid chain: invalid hex baseOffset.");
        return chain;
    }

    const QVariantList offsets = chainMap.value("offsets").toList();
    for (const auto& offsetVar : offsets) {
        const uint64_t off = offsetVar.toString().toULongLong(&ok, 16);
        if (!ok) {
            if (error) *error = KE_TXT("Chaîne invalide : offset hex invalide.", "Invalid chain: invalid hex offset.");
            return chain;
        }
        chain.offsets.append(off);
    }

    return chain;
}

QVariantMap pointerChainToVariantMap(const killcore::PointerChain& chain) {
    QVariantMap result;
    result["module"] = chain.module;
    result["baseOffset"] = QString::number(chain.baseOffset, 16);
    QVariantList offsets;
    for (uint64_t offset : chain.offsets) {
        offsets.append(QString::number(offset, 16));
    }
    result["offsets"] = offsets;
    result["depth"] = chain.depth();
    result["label"] = chain.toString();
    return result;
}

} // namespace

QVariantMap ProfileManager::scanPointerChains(
    const QString& addressHex,
    const QVariantMap& scanOptions) {

    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attache.", "No process attached.");
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(addressHex, &targetAddress)) {
        result["error"] = KE_TXT("Adresse cible invalide.", "Invalid target address.");
        return result;
    }

    killcore::PointerScanOptions options;
    options.maxDepth = scanOptions.value("maxDepth", 3).toInt();
    options.maxOffset = scanOptions.value("maxOffset", 0x1000).toULongLong();
    options.maxResults = static_cast<size_t>(scanOptions.value("maxResults", 100).toULongLong());
    options.onlyModuleBase = scanOptions.value("onlyModuleBase", true).toBool();
    options.alignment = static_cast<size_t>(scanOptions.value("alignment", 8).toULongLong());

    const QVariant baseModulesVar = scanOptions.value("baseModules");
    if (baseModulesVar.isValid() && baseModulesVar.canConvert<QVariantList>()) {
        for (const auto& mod : baseModulesVar.toList()) {
            options.baseModules.append(mod.toString());
        }
    }

    QElapsedTimer timer;
    timer.start();

    auto scanResult = killcore::scanForPointerChains(m_controller.m_handle, targetAddress, options);

    result["success"] = scanResult.success;
    result["partial"] = scanResult.partial;
    result["cancelled"] = scanResult.cancelled;
    result["pointersScanned"] = static_cast<qulonglong>(scanResult.pointersScanned);
    result["bytesScanned"] = static_cast<qulonglong>(scanResult.bytesScanned);
    result["elapsedMs"] = static_cast<qulonglong>(timer.elapsed());
    result["chainCount"] = static_cast<int>(scanResult.chains.size());

    if (!scanResult.errorMessage.isEmpty()) {
        result["error"] = scanResult.errorMessage;
    }

    QVariantList chainsList;
    for (const auto& chain : scanResult.chains) {
        chainsList.append(pointerChainToVariantMap(chain));
    }
    result["chains"] = chainsList;

    KE_LOG_INFO() << "scanPointerChains: target=0x" << QString::number(targetAddress, 16).toStdString()
                  << " chains=" << scanResult.chains.size()
                  << " elapsed=" << timer.elapsed() << "ms";
    m_controller.appendScanTelemetry("pointer_scan", {
        {"targetAddress", QString::number(targetAddress, 16)},
        {"maxDepth", options.maxDepth},
        {"maxOffset", static_cast<qulonglong>(options.maxOffset)},
        {"maxResults", static_cast<qulonglong>(options.maxResults)},
        {"onlyModuleBase", options.onlyModuleBase},
        {"alignment", static_cast<qulonglong>(options.alignment)},
        {"baseModules", options.baseModules},
        {"success", result.value("success")},
        {"partial", result.value("partial")},
        {"cancelled", result.value("cancelled")},
        {"pointersScanned", result.value("pointersScanned")},
        {"bytesScanned", result.value("bytesScanned")},
        {"chainCount", result.value("chainCount")},
        {"elapsedMs", result.value("elapsedMs")},
        {"pointersPerSecond", ratePerSecond(scanResult.pointersScanned, timer.elapsed())},
        {"bytesPerSecond", ratePerSecond(scanResult.bytesScanned, timer.elapsed())},
        {"error", result.value("error")},
    });

    return result;
}

QVariantMap ProfileManager::resolvePointerChain(const QVariantMap& chainMap) {
    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    QString parseError;
    const auto chain = variantMapToPointerChain(chainMap, &parseError);
    if (!parseError.isEmpty()) {
        result["error"] = parseError;
        return result;
    }

    const auto resolveResult = killcore::resolvePointerChain(m_controller.m_handle, chain);
    if (!resolveResult.success) {
        result["error"] = resolveResult.errorMessage.isEmpty()
            ? KE_TXT("Résolution de chaîne échouée.", "Chain resolution failed.")
            : resolveResult.errorMessage;
        return result;
    }

    result["success"] = true;
    result["finalAddress"] = QString::number(resolveResult.finalAddress, 16);

    QVariantList steps;
    for (uint64_t addr : resolveResult.intermediateAddresses) {
        steps.append(QString::number(addr, 16));
    }
    result["steps"] = steps;

    return result;
}

QVariantMap ProfileManager::suggestStableLocatorForAddress(
    const QString& addressHex,
    const QVariantMap& options) {

    QVariantMap result;
    result["success"] = false;
    result["chainCount"] = 0;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    uint64_t targetAddress = 0;
    if (!parseHexAddress(addressHex, &targetAddress)) {
        result["error"] = KE_TXT("Adresse invalide.", "Invalid address.");
        return result;
    }

    // Bornes volontairement modestes : cette methode est pensee pour un appel
    // explicite juste apres une ecriture confirmee, pas pour tourner en fond.
    QVariantMap scanOptions;
    scanOptions["maxDepth"] = options.value("maxDepth", 3);
    scanOptions["maxOffset"] = options.value("maxOffset", 0x1000);
    scanOptions["maxResults"] = options.value("maxResults", 5);
    scanOptions["onlyModuleBase"] = options.value("onlyModuleBase", true);
    scanOptions["alignment"] = options.value("alignment", 8);
    if (options.contains("baseModules")) {
        scanOptions["baseModules"] = options.value("baseModules");
    }

    const QVariantMap scanResult = scanPointerChains(addressHex, scanOptions);
    result["success"] = scanResult.value("success").toBool();
    result["elapsedMs"] = scanResult.value("elapsedMs");
    if (!scanResult.value("error").toString().isEmpty()) {
        result["error"] = scanResult.value("error");
    }
    if (!result["success"].toBool()) {
        return result;
    }

    const QVariantList chains = scanResult.value("chains").toList();
    result["chainCount"] = chains.size();
    if (chains.isEmpty()) {
        result["message"] = QStringLiteral(
            "Aucune chaine de pointeurs stable trouvee vers 0x%1 dans les bornes explorees. "
            "L'adresse absolue devra etre re-recherchee si le processus redemarre.")
            .arg(QString::number(targetAddress, 16).toUpper());
        return result;
    }

    // Meilleure chaine = la plus courte (moins de niveaux = plus stable, plus
    // rapide a resoudre) ; a profondeur egale, le plus petit offset de base.
    QVariantMap best = chains.first().toMap();
    for (const auto& candidate : chains) {
        const QVariantMap chain = candidate.toMap();
        const int candidateDepth = chain.value("depth").toInt();
        const int bestDepth = best.value("depth").toInt();
        if (candidateDepth < bestDepth) {
            best = chain;
        } else if (candidateDepth == bestDepth) {
            const qulonglong candidateBase = chain.value("baseOffset").toString().toULongLong(nullptr, 16);
            const qulonglong bestBase = best.value("baseOffset").toString().toULongLong(nullptr, 16);
            if (candidateBase < bestBase) {
                best = chain;
            }
        }
    }

    result["bestChain"] = best;
    result["message"] = QStringLiteral(
        "%1 chaine(s) de pointeurs stable(s) trouvee(s) vers 0x%2. "
        "Meilleure option : %3 (profondeur %4). Sauvegarde-la dans un profil pour qu'elle survive a un redemarrage.")
        .arg(chains.size())
        .arg(QString::number(targetAddress, 16).toUpper())
        .arg(best.value("label").toString())
        .arg(best.value("depth").toInt());

    m_controller.appendScanTelemetry("suggest_stable_locator", {
        {"targetAddress", QString::number(targetAddress, 16)},
        {"success", result.value("success")},
        {"chainCount", result.value("chainCount")},
        {"bestDepth", best.value("depth")},
        {"elapsedMs", result.value("elapsedMs")},
    });

    return result;
}


QVariantMap ProfileManager::savePointerChainProfileTarget(
    const QString& profileName,
    const QString& targetName,
    const QVariantMap& chainMap,
    const QString& valueType,
    const QString& description) {

    QVariantMap result;
    result["success"] = false;

    if (!m_controller.m_handle.isValid()) {
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    QString parseError;
    const auto chain = variantMapToPointerChain(chainMap, &parseError);
    if (!parseError.isEmpty()) {
        result["error"] = parseError;
        return result;
    }

    // Valider la chaîne avant de la sauvegarder.
    const auto resolveCheck = killcore::resolvePointerChain(m_controller.m_handle, chain);
    if (!resolveCheck.success) {
        result["error"] = KE_TXT("La chaîne ne se résout pas : %1", "The chain doesn't resolve: %1").arg(resolveCheck.errorMessage);
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    bool isNewProfile = true;
    if (killcore::ProfileStore::load(path, &profile)) {
        isNewProfile = false;
    } else {
        profile.gameName = profileName;
        profile.executableName = m_controller.m_processName;
    }

    killcore::ValueType type = killcore::ValueType::Int32;
    killcore::parseValueType(valueType, &type);

    // Cherche une cible existante avec le même nom pour la remplacer.
    bool replaced = false;
    for (auto& existing : profile.targets) {
        if (existing.name == targetName) {
            existing.type = type;
            existing.locator.kind = killcore::LocatorKind::PointerChain;
            existing.locator.pointerChain = chain;
            existing.locator.lastAddress = resolveCheck.finalAddress;
            existing.description = description;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        killcore::ProfileTarget target;
        target.name = targetName;
        target.type = type;
        target.locator.kind = killcore::LocatorKind::PointerChain;
        target.locator.pointerChain = chain;
        target.locator.lastAddress = resolveCheck.finalAddress;
        target.description = description;
        profile.targets.append(target);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["profileName"] = profileName;
    result["targetName"] = targetName;
    result["resolvedAddress"] = QString::number(resolveCheck.finalAddress, 16);
    result["isNewProfile"] = isNewProfile;
    result["chainLabel"] = chain.toString();
    result["message"] = KE_TXT("Cible « %1 » sauvegardée avec chaîne de pointeurs (résout à 0x%2).",
                            "Target \"%1\" saved with a pointer chain (resolves to 0x%2).")
                            .arg(targetName, QString::number(resolveCheck.finalAddress, 16));

    KE_LOG_INFO() << "savePointerChainProfileTarget: profile=" << profileName.toStdString()
                  << " target=" << targetName.toStdString()
                  << " addr=0x" << QString::number(resolveCheck.finalAddress, 16).toStdString();

    return result;
}


QVariantMap ProfileManager::saveProfileLuaScript(
    const QString& profileName,
    const QString& scriptName,
    const QString& scriptText,
    const QVariantMap& metadata) {
    QVariantMap result;
    result["success"] = false;

    const QString cleanProfileName = profileName.trimmed();
    const QString cleanScriptName = scriptName.trimmed();
    if (cleanProfileName.isEmpty() || cleanScriptName.isEmpty()) {
        result["error"] = KE_TXT("Nom de profil ou de script vide.", "Empty profile or script name.");
        return result;
    }
    if (scriptText.trimmed().isEmpty()) {
        result["error"] = KE_TXT("Script vide.", "Empty script.");
        return result;
    }

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(cleanProfileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        profile.gameName = cleanProfileName;
        profile.executableName = m_controller.m_processName;
    }
    if (profile.executableHash.isEmpty() && m_controller.m_handle.isValid()) {
        profile.executableHash = computeExecutableHash(m_controller.m_handle.executablePath());
    }

    killcore::ProfileLuaScript script;
    script.name = cleanScriptName;
    script.scriptText = scriptText;
    script.description = metadata.value("description").toString();
    script.savedAtEpochMs = QDateTime::currentMSecsSinceEpoch();

    bool replaced = false;
    for (auto& existing : profile.luaScripts) {
        if (existing.name == script.name) {
            existing = script;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        profile.luaScripts.append(script);
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["profileName"] = cleanProfileName;
    result["scriptName"] = cleanScriptName;
    result["scriptCount"] = profile.luaScripts.size();
    result["replaced"] = replaced;
    m_controller.appendScanTelemetry("lua_script_saved", result);
    return result;
}

QVariantMap ProfileManager::deleteProfileLuaScript(const QString& profileName, const QString& scriptName) {
    QVariantMap result;
    result["success"] = false;
    result["profileName"] = profileName;
    result["scriptName"] = scriptName;

    killcore::Profile profile;
    const QString path = killcore::ProfileStore::profilePath(profileName);
    if (!killcore::ProfileStore::load(path, &profile)) {
        result["error"] = KE_TXT("Profil introuvable.", "Profile not found.");
        return result;
    }

    const int before = profile.luaScripts.size();
    profile.luaScripts.removeIf([&](const killcore::ProfileLuaScript& script) {
        return script.name == scriptName;
    });
    if (profile.luaScripts.size() == before) {
        result["error"] = KE_TXT("Script Lua introuvable dans ce profil.", "Lua script not found in this profile.");
        return result;
    }

    if (!killcore::ProfileStore::save(profile, path)) {
        result["error"] = KE_TXT("Impossible de sauvegarder le profil.", "Couldn't save the profile.");
        return result;
    }

    result["success"] = true;
    result["scriptCount"] = profile.luaScripts.size();
    return result;
}


} // namespace killengine
