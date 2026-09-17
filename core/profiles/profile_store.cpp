#include "profile_store.h"

#include "logging/logger.h"
#include "memory/memory_reader.h"
#include "paths/portable_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

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

QJsonObject resolutionPlanToJson(const ProfileResolutionPlan& plan) {
    QJsonArray alternatives;
    for (const auto& locator : plan.alternatives) alternatives.append(locatorToJson(locator));
    return {{"schemaVersion", 1}, {"invalid", plan.invalid},
            {"discoveryMethod", plan.discoveryMethod}, {"expectedBytes", plan.expectedBytes},
            {"validationTest", plan.validationTest}, {"executableHash", plan.executableHash},
            {"recordedSession", plan.recordedSession}, {"recordedAt", plan.recordedAt},
            {"evidenceNote", plan.evidenceNote}, {"moduleHashes", plan.moduleHashes},
            {"baselineObservation", plan.baselineObservation},
            {"alternativeNames", QJsonArray::fromStringList(plan.alternativeNames)},
            {"alternatives", alternatives}};
}

ProfileResolutionPlan resolutionPlanFromJson(const QJsonValue& value) {
    ProfileResolutionPlan plan;
    if (value.isUndefined()) return plan;
    const QJsonObject json = value.toObject();
    plan.invalid = !value.isObject() || json.value("invalid").toBool()
        || (json.contains("schemaVersion") && json.value("schemaVersion").toInt(-1) != 1);
    for (const QString& key : {QString("discoveryMethod"), QString("expectedBytes"), QString("validationTest"),
                              QString("executableHash"), QString("recordedSession"), QString("recordedAt"), QString("evidenceNote")}) {
        if (json.contains(key) && !json.value(key).isString()) plan.invalid = true;
    }
    if ((json.contains("alternatives") && !json.value("alternatives").isArray())
        || (json.contains("moduleHashes") && !json.value("moduleHashes").isObject())) plan.invalid = true;
    plan.discoveryMethod = json.value("discoveryMethod").toString();
    plan.expectedBytes = json.value("expectedBytes").toString();
    plan.validationTest = json.value("validationTest").toString();
    plan.executableHash = json.value("executableHash").toString();
    plan.recordedSession = json.value("recordedSession").toString();
    plan.recordedAt = json.value("recordedAt").toString();
    plan.evidenceNote = json.value("evidenceNote").toString();
    plan.baselineObservation = json.value("baselineObservation").toObject();
    plan.moduleHashes = json.value("moduleHashes").toObject();
    for (const auto& name : json.value("alternativeNames").toArray()) plan.alternativeNames.append(name.toString());
    for (const auto& item : json.value("alternatives").toArray()) {
        const QString kind = item.toObject().value("kind").toString();
        if (!item.isObject() || (kind != "module_offset" && kind != "pointer_chain"
            && kind != "absolute" && kind != "clr_field")) plan.invalid = true;
        plan.alternatives.append(locatorFromJson(item.toObject()));
        if (plan.alternatives.size() > 8) { plan.invalid = true; break; }
    }
    return plan;
}

QString knowledgeNoteKindToStorageString(KnowledgeNoteKind kind) {
    switch (kind) {
        case KnowledgeNoteKind::ExplainedFailure:         return "explained_failure";
        case KnowledgeNoteKind::SuccessCondition:         return "success_condition";
        case KnowledgeNoteKind::DiscriminatingExperiment: return "discriminating_experiment";
        case KnowledgeNoteKind::Recheck:                  return "recheck";
    }
    return "recheck";
}

QJsonObject knowledgeNoteToJson(const KnowledgeNote& note) {
    return {{"schemaVersion", 1}, {"id", note.id}, {"invalid", note.invalid},
            {"kind", knowledgeNoteKindToStorageString(note.kind)},
            {"description", note.description}, {"experiment", note.experiment},
            {"evidenceNote", note.evidenceNote}, {"sessionId", note.sessionId},
            {"executableHash", note.executableHash}, {"recordedAt", note.recordedAt}};
}

KnowledgeNote knowledgeNoteFromJson(const QJsonValue& value) {
    KnowledgeNote note;
    if (!value.isObject()) { note.invalid = true; return note; }
    const QJsonObject json = value.toObject();
    note.invalid = json.value("invalid").toBool()
        || (json.contains("schemaVersion") && json.value("schemaVersion").toInt(-1) != 1);
    for (const QString& key : {QString("id"), QString("description"), QString("experiment"),
                              QString("evidenceNote"), QString("sessionId"),
                              QString("executableHash"), QString("recordedAt")}) {
        if (json.contains(key) && !json.value(key).isString()) note.invalid = true;
    }
    note.id = json.value("id").toString();
    if (!knowledgeNoteKindFromString(json.value("kind").toString(), &note.kind)) note.invalid = true;
    note.description = json.value("description").toString();
    note.experiment = json.value("experiment").toString();
    note.evidenceNote = json.value("evidenceNote").toString();
    note.sessionId = json.value("sessionId").toString();
    note.executableHash = json.value("executableHash").toString();
    note.recordedAt = json.value("recordedAt").toString();
    return note;
}

bool parseExpectedBytes(const QString& text, QByteArray* bytes) {
    QString compact = text;
    compact.remove(QRegularExpression("\\s"));
    if (compact.isEmpty()) { bytes->clear(); return true; }
    static const QRegularExpression hex("^[0-9a-fA-F]+$");
    if (compact.size() > 128 || compact.size() % 2 != 0 || !hex.match(compact).hasMatch()) return false;
    *bytes = QByteArray::fromHex(compact.toLatin1());
    return true;
}

QString resolutionModule(const Locator& locator) {
    if (locator.kind == LocatorKind::Absolute || locator.kind == LocatorKind::ClrField) return {};
    return (locator.kind == LocatorKind::PointerChain
        ? locator.pointerChain.module : locator.module).toLower();
}

Locator patchLocator(const ProfileCodePatch& patch) {
    Locator locator;
    locator.kind = LocatorKind::ModuleOffset;
    locator.module = patch.module;
    locator.offset = patch.moduleOffset;
    return locator;
}

// File fingerprints are bounded: unknown is preferable to stalling the GUI or
// accepting a truncated hash. Cache lifetime is one diagnostic request only.
QString resolutionFileHash(const QString& path, QElapsedTimer& timer, qint64& budget) {
    QFile file(path);
    if (path.isEmpty() || timer.elapsed() > 1500 || !file.open(QIODevice::ReadOnly)
        || file.size() > 64 * 1024 * 1024 || file.size() > budget) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (timer.elapsed() > 1500) return {};
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty() || block.size() > budget) return {};
        budget -= block.size();
        hash.addData(block);
    }
    if (file.error() != QFileDevice::NoError) return {};
    return QString::fromLatin1(hash.result().toHex());
}

ProfileResolutionContext resolutionContext(const ProcessHandle& process,
                                           const QList<ProcessModuleInfo>& modules,
                                           const QSet<QString>& requiredModules) {
    ProfileResolutionContext context;
    context.executableName = process.executableName();
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process.rawHandle(), &created, &exited, &kernel, &user)) {
        const quint64 stamp = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        context.session = QString("%1:%2").arg(process.pid()).arg(stamp);
    }
#endif
    QElapsedTimer timer;
    timer.start();
    qint64 budget = 128 * 1024 * 1024;
    const QString executablePath = process.executablePath();
    context.executableHash = resolutionFileHash(executablePath, timer, budget);
    for (const auto& module : modules) {
        const QString name = module.name.toLower();
        if (!requiredModules.contains(name)) continue;
        const QString hash = module.path.compare(executablePath, Qt::CaseInsensitive) == 0
            ? context.executableHash : resolutionFileHash(module.path, timer, budget);
        context.moduleHashes[name] = hash;
    }
    return context;
}

QJsonObject targetToJson(const ProfileTarget& target) {
    QJsonObject json;
    json["name"] = target.name;
    json["type"] = valueTypeToString(target.type);
    json["locator"] = locatorToJson(target.locator);
    json["resolutionPlan"] = resolutionPlanToJson(target.resolutionPlan);
    if (!target.knowledgeNotes.isEmpty()) {
        QJsonArray notes;
        for (const auto& note : target.knowledgeNotes) notes.append(knowledgeNoteToJson(note));
        json["knowledgeNotes"] = notes;
    }
    if (!target.description.isEmpty()) {
        json["description"] = target.description;
    }
    if (!target.ghidraSymbol.isEmpty()) {
        json["ghidraSymbol"] = target.ghidraSymbol;
    }
    if (!target.ghidraNote.isEmpty()) {
        json["ghidraNote"] = target.ghidraNote;
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
    json["resolutionPlan"] = resolutionPlanToJson(patch.resolutionPlan);
    if (!patch.knowledgeNotes.isEmpty()) {
        QJsonArray notes;
        for (const auto& note : patch.knowledgeNotes) notes.append(knowledgeNoteToJson(note));
        json["knowledgeNotes"] = notes;
    }
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
    if (!patch.ghidraSymbol.isEmpty()) {
        json["ghidraSymbol"] = patch.ghidraSymbol;
    }
    if (!patch.ghidraNote.isEmpty()) {
        json["ghidraNote"] = patch.ghidraNote;
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
    target.resolutionPlan = resolutionPlanFromJson(json.value("resolutionPlan"));
    for (const auto& item : json.value("knowledgeNotes").toArray()) {
        if (target.knowledgeNotes.size() >= ProfileStore::kMaxKnowledgeNotesPerEntry) break;
        target.knowledgeNotes.append(knowledgeNoteFromJson(item));
    }
    target.description = json.value("description").toString();
    target.ghidraSymbol = json.value("ghidraSymbol").toString();
    target.ghidraNote = json.value("ghidraNote").toString();
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
    patch.resolutionPlan = resolutionPlanFromJson(json.value("resolutionPlan"));
    for (const auto& item : json.value("knowledgeNotes").toArray()) {
        if (patch.knowledgeNotes.size() >= ProfileStore::kMaxKnowledgeNotesPerEntry) break;
        patch.knowledgeNotes.append(knowledgeNoteFromJson(item));
    }
    patch.originalBytes = json.value("originalBytes").toString();
    patch.disassembly = json.value("disassembly").toString();
    patch.riskLevel = json.value("riskLevel").toString();
    patch.description = json.value("description").toString();
    patch.ghidraSymbol = json.value("ghidraSymbol").toString();
    patch.ghidraNote = json.value("ghidraNote").toString();
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

ProcessIdentity currentProcessIdentity(const ProcessHandle& process) {
    ProcessIdentity identity;
    if (!process.isValid()) return identity;
    const ProfileResolutionContext context = resolutionContext(process, {}, {});
    identity.executableHash = context.executableHash;
    identity.sessionId = context.session;
    return identity;
}

QString knowledgeNoteKindToString(KnowledgeNoteKind kind) {
    return knowledgeNoteKindToStorageString(kind);
}

bool knowledgeNoteKindFromString(const QString& text, KnowledgeNoteKind* kind) {
    if (text == "explained_failure") { *kind = KnowledgeNoteKind::ExplainedFailure; return true; }
    if (text == "success_condition") { *kind = KnowledgeNoteKind::SuccessCondition; return true; }
    if (text == "discriminating_experiment") { *kind = KnowledgeNoteKind::DiscriminatingExperiment; return true; }
    if (text == "recheck") { *kind = KnowledgeNoteKind::Recheck; return true; }
    return false;
}

QJsonObject evaluateKnowledgeNotes(const QList<KnowledgeNote>& notes, const QString& currentExecutableHash) {
    QJsonArray valid, stale, unversioned;
    for (const auto& note : notes) {
        const QJsonObject json = knowledgeNoteToJson(note);
        if (note.executableHash.isEmpty()) {
            unversioned.append(json);
        } else if (!currentExecutableHash.isEmpty()
                   && note.executableHash.compare(currentExecutableHash, Qt::CaseInsensitive) != 0) {
            stale.append(json);
        } else {
            valid.append(json);
        }
    }
    return {{"valid", valid}, {"stale", stale}, {"unversioned", unversioned}};
}

// ---------------------------------------------------------------------------
// ProfileStore
// ---------------------------------------------------------------------------

QString ProfileStore::profilesDir() {
    // PORT-2b (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : ancien emplacement
    // QStandardPaths::GenericDataLocation (%LOCALAPPDATA% sur Windows, pas
    // %ProgramData% -- confusion vecue et corrigee pendant AM-4) cassait le
    // mode portable, comme P1-P6 avant lui. Deplace vers "data/profiles" via
    // le resolveur commun killcore::PortablePaths ; voir legacyProfilesDir()
    // pour l'ancien chemin, utilise uniquement par importLegacyProfiles().
    return PortablePaths::ensureSubdir("data/profiles");
}

bool ProfileStore::ensureProfilesDir() {
    const QString dir = profilesDir();
    QDir d(dir);
    if (d.exists()) return true;
    return d.mkpath(dir);
}

QString ProfileStore::legacyProfilesDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base + "/KillEngine/Profiles";
}

QList<ProfileStore::LegacyImportResult> ProfileStore::importLegacyProfiles() {
    QList<LegacyImportResult> results;

    const QDir legacyDir(legacyProfilesDir());
    if (!legacyDir.exists()) {
        return results; // Rien a importer -- pas une erreur.
    }

    if (!ensureProfilesDir()) {
        LegacyImportResult failure;
        failure.error = QStringLiteral("Impossible de créer le dossier de profils portable.");
        results.append(failure);
        return results;
    }

    const QStringList legacyFiles = legacyDir.entryList(QStringList() << "*.keprofile",
                                                          QDir::Files, QDir::Name);
    for (const QString& fileName : legacyFiles) {
        LegacyImportResult result;
        result.fileName = fileName;

        const QString sourcePath = legacyDir.filePath(fileName);
        const QString destinationPath = QDir(profilesDir()).filePath(fileName);

        if (QFile::exists(destinationPath)) {
            // Ne jamais ecraser un profil portable deja present -- une
            // collision de nom est signalee, pas resolue automatiquement.
            result.skippedExisting = true;
            results.append(result);
            continue;
        }

        Profile scratch;
        if (!ProfileStore::load(sourcePath, &scratch)) {
            // Ne se charge pas comme un Profile valide : signale, jamais copie.
            result.invalid = true;
            results.append(result);
            continue;
        }

        if (!QFile::copy(sourcePath, destinationPath)) {
            // Copie (jamais deplace) : la source reste intacte meme en cas
            // d'echec, et un echec sur un fichier n'interrompt pas les suivants.
            result.error = QStringLiteral("Échec de la copie vers le dossier portable.");
            results.append(result);
            continue;
        }

        result.imported = true;
        results.append(result);
    }

    return results;
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

    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        KE_LOG_ERROR() << "ProfileStore: Cannot write" << filename.toStdString();
        return false;
    }

    const QByteArray encoded = doc.toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        KE_LOG_ERROR() << "ProfileStore: Cannot commit " << filename.toStdString();
        return false;
    }

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

QJsonObject diagnoseProfileResolution(const QString& expectedExecutable,
                                     const QString& legacyExecutableHash,
                                     const Locator& primary,
                                     const ProfileResolutionPlan& plan,
                                     const ProfileResolutionContext& context,
                                     const QList<LocatorProbe>& observations) {
    QJsonObject result{{"status", "unverified"}, {"conditionsVerified", false},
                       {"automaticApplicationAllowed", false}, {"candidateAddress", ""},
                       {"requiredTest", plan.validationTest}, {"plan", resolutionPlanToJson(plan)}};
    auto finish = [&](const QString& status, const QString& nextAction) {
        result["status"] = status;
        result["nextAction"] = nextAction;
        return result;
    };
    QByteArray expected;
    if (plan.invalid || !parseExpectedBytes(plan.expectedBytes, &expected) || plan.alternatives.size() > 8
        || observations.size() != plan.alternatives.size() + 1) {
        return finish("invalid_conditions", "edit_conditions");
    }
    if (!expectedExecutable.isEmpty()
        && context.executableName.compare(expectedExecutable, Qt::CaseInsensitive) != 0) {
        return finish("wrong_process", "attach_expected_process");
    }
    const QString expectedHash = plan.executableHash.isEmpty() ? legacyExecutableHash : plan.executableHash;
    const bool versionKnown = !expectedHash.isEmpty() && !context.executableHash.isEmpty();
    const bool versionChanged = versionKnown
        && expectedHash.compare(context.executableHash, Qt::CaseInsensitive) != 0;
    result["executableVersionChanged"] = versionChanged;
    result["versionKnown"] = versionKnown;
    result["sessionChanged"] = !plan.recordedSession.isEmpty() && !context.session.isEmpty()
        && plan.recordedSession != context.session;
    QList<Locator> locators{primary};
    locators.append(plan.alternatives);
    QJsonArray candidates;
    QSet<quint64> matchingAddresses;
    int selected = -1;
    bool incomplete = false;
    int readable = 0;
    for (qsizetype i = 0; i < observations.size(); ++i) {
        const auto& observation = observations[i];
        const bool matches = observation.readable && observation.resolved && observation.address != 0
            && (expected.isEmpty() || observation.bytes == expected);
        const QString code = observation.errorCode;
        incomplete = incomplete || code == "unsupported_locator" || code == "ambiguous_module"
            || code == "invalid_probe" || code == "pointer_unreadable" || code == "address_unreadable";
        if (observation.readable) ++readable;
        if (matches) {
            matchingAddresses.insert(observation.address);
            if (selected < 0) selected = static_cast<int>(i);
        }
        QJsonObject candidate{{"index", static_cast<int>(i)}, {"locator", locators[i].toString()},
                              {"address", observation.resolved ? QString::number(observation.address, 16) : QString()},
                              {"readable", observation.readable}, {"matchesConditions", matches && !expected.isEmpty()},
                              {"observedBytes", QString::fromLatin1(observation.bytes.toHex(' '))},
                              {"errorCode", code}};
        candidates.append(candidate);
    }
    result["candidates"] = candidates;
    result["candidateCount"] = matchingAddresses.size();
    if (matchingAddresses.size() > 1) return finish("ambiguous", "disambiguate_candidates");
    if (incomplete) return finish("inconclusive", "retry_read_or_inspect_manually");
    if (selected < 0) return finish(readable > 0 ? "conditions_mismatch" : "missing", "rediscover_target");
    result["candidateAddress"] = QString::number(observations[selected].address, 16);
    result["candidateIndex"] = selected;
    result["addressMoved"] = primary.lastAddress != 0 && primary.lastAddress != observations[selected].address;
    const QString moduleName = resolutionModule(locators[selected]);
    const QString savedModuleHash = plan.moduleHashes.value(moduleName).toString();
    const QString currentModuleHash = context.moduleHashes.value(moduleName).toString();
    const bool moduleKnown = !savedModuleHash.isEmpty() && !currentModuleHash.isEmpty();
    const bool moduleChanged = moduleKnown && savedModuleHash != currentModuleHash;
    result["moduleVersionChanged"] = moduleChanged;
    if (versionChanged || moduleChanged) return finish("version_changed", "revalidate_on_current_version");
    if (selected != 0) return finish("repair_candidate", "test_alternative_before_replacing");
    if (primary.kind == LocatorKind::Absolute
        && (plan.recordedSession.isEmpty() || context.session.isEmpty() || plan.recordedSession != context.session)) {
        return finish("session_changed", "rediscover_absolute_address");
    }
    if (expectedExecutable.isEmpty() || expected.isEmpty() || !versionKnown || (!moduleName.isEmpty() && !moduleKnown)) {
        return finish("unverified", "record_and_check_stable_conditions");
    }
    result["conditionsVerified"] = true;
    return finish("conditions_verified", "verify_effect_separately");
}

QJsonObject ProfileStore::diagnose(const Profile& profile, const ProcessHandle& process) {
    if (!process.isValid()) return {{"success", false}, {"errorCode", "no_process"}};
    if (profile.targets.size() + profile.patches.size() > 256) {
        return {{"success", false}, {"errorCode", "too_many_entries"}};
    }
    QElapsedTimer diagnosticTimer;
    diagnosticTimer.start();
    const auto modules = ProcessEnumerator::enumerateModules(process.pid());
    QSet<QString> requiredModules;
    auto collect = [&](const Locator& locator, const ProfileResolutionPlan& plan) {
        requiredModules.insert(resolutionModule(locator));
        for (const auto& alternative : plan.alternatives) requiredModules.insert(resolutionModule(alternative));
    };
    for (const auto& target : profile.targets) collect(target.locator, target.resolutionPlan);
    for (const auto& patch : profile.patches) collect(patchLocator(patch), patch.resolutionPlan);
    const auto context = resolutionContext(process, modules, requiredModules);
    MemoryReader reader(process);
    const size_t pointerBytes = process.architecture() == Architecture::x86 ? 4 : 8;
    const LocatorReadBytes read = [&](uint64_t address, size_t count) {
        if (diagnosticTimer.elapsed() > 2000) return QByteArray();
        const auto result = reader.read(address, count);
        return result.success && !result.partial ? result.data : QByteArray();
    };
    QJsonArray entries;
    auto inspect = [&](const QString& kind, const QString& name, const Locator& locator,
                       const ProfileResolutionPlan& plan, size_t valueBytes) {
        QByteArray expected;
        QList<LocatorProbe> observations;
        if (parseExpectedBytes(plan.expectedBytes, &expected) && plan.alternatives.size() <= 8) {
            const size_t count = expected.isEmpty() ? std::clamp<size_t>(valueBytes, 1, 64) : expected.size();
            observations.append(probeLocator(locator, modules, pointerBytes, count, read));
            for (const auto& alternative : plan.alternatives) {
                observations.append(probeLocator(alternative, modules, pointerBytes, count, read));
            }
        }
        auto item = diagnoseProfileResolution(profile.executableName, profile.executableHash,
                                             locator, plan, context, observations);
        item["entryKind"] = kind;
        item["name"] = name;
        entries.append(item);
    };
    for (const auto& target : profile.targets) {
        inspect("target", target.name, target.locator, target.resolutionPlan, valueTypeSize(target.type));
    }
    for (const auto& patch : profile.patches) {
        auto plan = patch.resolutionPlan;
        if (plan.expectedBytes.isEmpty()) plan.expectedBytes = patch.originalBytes;
        inspect("patch", patch.name, patchLocator(patch), plan, 1);
    }
    return {{"success", true}, {"entries", entries}, {"session", context.session},
            {"executableName", context.executableName}, {"executableHash", context.executableHash},
            {"observedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"readBudgetExceeded", diagnosticTimer.elapsed() > 2000},
            {"automaticApplicationAllowed", false}, {"scope", "stored_locators_only"}};
}

bool ProfileStore::setResolutionPlan(Profile* profile, const QString& entryKind,
                                     const QString& entryName, const QJsonObject& options,
                                     const ProcessHandle& process, QString* error) {
    auto fail = [&](const QString& code) { if (error) *error = code; return false; };
    if (!profile || !process.isValid()) return fail("no_process");
    if (profile->executableName.isEmpty()
        || profile->executableName.compare(process.executableName(), Qt::CaseInsensitive) != 0) {
        return fail("wrong_process");
    }
    ProfileResolutionPlan* destination = nullptr;
    Locator primary;
    ValueType primaryType{ValueType::Int32};
    if (entryKind == "target") {
        for (auto& target : profile->targets) {
            if (target.name != entryName) continue;
            if (destination) return fail("ambiguous_entry_name");
            destination = &target.resolutionPlan;
            primary = target.locator;
            primaryType = target.type;
        }
    } else if (entryKind == "patch") {
        for (auto& patch : profile->patches) {
            if (patch.name != entryName) continue;
            if (destination) return fail("ambiguous_entry_name");
            destination = &patch.resolutionPlan;
            primary = patchLocator(patch);
        }
    } else return fail("invalid_entry_kind");
    if (!destination) return fail("entry_not_found");
    ProfileResolutionPlan plan;
    plan.discoveryMethod = options.value("discoveryMethod").toString().trimmed();
    plan.validationTest = options.value("validationTest").toString().trimmed();
    plan.evidenceNote = options.value("evidenceNote").toString().trimmed();
    QByteArray expected;
    if (!options.value("expectedBytes").isString()
        || !parseExpectedBytes(options.value("expectedBytes").toString(), &expected)) return fail("invalid_conditions");
    if (plan.discoveryMethod.isEmpty() || plan.discoveryMethod.size() > 512
        || plan.validationTest.isEmpty() || plan.validationTest.size() > 2048
        || plan.evidenceNote.size() > 2048) return fail("invalid_plan");
    plan.expectedBytes = QString::fromLatin1(expected.toHex(' '));
    if (!options.value("alternativeNames").isArray()) return fail("invalid_alternatives");
    const auto names = options.value("alternativeNames").toArray();
    if (names.size() > 8) return fail("too_many_alternatives");
    QSet<QString> seen;
    for (const auto& value : names) {
        if (!value.isString() || value.toString().isEmpty() || value.toString() == entryName
            || seen.contains(value.toString())) return fail("invalid_alternatives");
        const QString name = value.toString();
        seen.insert(name);
        plan.alternativeNames.append(name);
        int matches = 0;
        Locator alternative;
        if (entryKind == "target") {
            for (const auto& target : profile->targets) {
                if (target.name == name) {
                    if (target.type != primaryType) return fail("alternative_type_mismatch");
                    alternative = target.locator;
                    ++matches;
                }
            }
        } else {
            for (const auto& patch : profile->patches) {
                if (patch.name == name) { alternative = patchLocator(patch); ++matches; }
            }
        }
        if (matches != 1) return fail("alternative_not_unique");
        plan.alternatives.append(alternative);
    }
    const auto modules = ProcessEnumerator::enumerateModules(process.pid());
    QSet<QString> requiredModules{resolutionModule(primary)};
    for (const auto& locator : plan.alternatives) requiredModules.insert(resolutionModule(locator));
    const auto context = resolutionContext(process, modules, requiredModules);
    plan.executableHash = context.executableHash;
    plan.moduleHashes = context.moduleHashes;
    plan.recordedSession = context.session;
    plan.recordedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    MemoryReader reader(process);
    const auto observation = probeLocator(primary, modules,
        process.architecture() == Architecture::x86 ? 4 : 8,
        expected.isEmpty() ? 1 : static_cast<size_t>(expected.size()),
        [&](uint64_t address, size_t count) {
            const auto result = reader.read(address, count);
            return result.success && !result.partial ? result.data : QByteArray();
        });
    plan.baselineObservation = {{"address", QString::number(observation.address, 16)},
        {"readable", observation.readable}, {"observedBytes", QString::fromLatin1(observation.bytes.toHex(' '))},
        {"errorCode", observation.errorCode}, {"source", "memory_read"}, {"effectVerified", false}};
    // The note is declared provenance, not evidence that an effect was observed.
    *destination = plan;
    if (error) error->clear();
    return true;
}

QString ProfileStore::addKnowledgeNote(Profile* profile, const QString& entryKind,
                                       const QString& entryName, const QJsonObject& options,
                                       const ProcessHandle& process, QString* error) {
    auto fail = [&](const QString& code) { if (error) *error = code; return QString(); };
    if (!profile) return fail("no_profile");
    QList<KnowledgeNote>* destination = nullptr;
    if (entryKind == "target") {
        for (auto& target : profile->targets) {
            if (target.name != entryName) continue;
            if (destination) return fail("ambiguous_entry_name");
            destination = &target.knowledgeNotes;
        }
    } else if (entryKind == "patch") {
        for (auto& patch : profile->patches) {
            if (patch.name != entryName) continue;
            if (destination) return fail("ambiguous_entry_name");
            destination = &patch.knowledgeNotes;
        }
    } else return fail("invalid_entry_kind");
    if (!destination) return fail("entry_not_found");
    if (destination->size() >= kMaxKnowledgeNotesPerEntry) return fail("too_many_notes");

    KnowledgeNote note;
    if (!knowledgeNoteKindFromString(options.value("kind").toString(), &note.kind)) return fail("invalid_kind");
    note.description = options.value("description").toString().trimmed();
    note.experiment = options.value("experiment").toString().trimmed();
    note.evidenceNote = options.value("evidenceNote").toString().trimmed();
    if (note.description.isEmpty() || note.description.size() > 2048
        || note.experiment.size() > 128 || note.evidenceNote.size() > 2048) return fail("invalid_note");

    // A version-scoped note is strictly more useful than an unversioned one (it can be
    // contradicted later by evaluateKnowledgeNotes), but recording knowledge without an
    // attached process must still succeed -- a lesson learned after a session ended is
    // real, just honestly unversioned rather than falsely tied to no process.
    if (process.isValid()) {
        const auto context = resolutionContext(process, {}, {});
        note.executableHash = context.executableHash;
        note.sessionId = context.session;
    }
    note.recordedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    note.id = QString("K-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    destination->append(note);
    if (error) error->clear();
    return note.id;
}

QJsonObject ProfileStore::getKnowledgeNotes(const Profile& profile, const QString& entryKind,
                                            const QString& entryName, const ProcessHandle& process) {
    const QList<KnowledgeNote>* source = nullptr;
    if (entryKind == "target") {
        for (const auto& target : profile.targets) {
            if (target.name != entryName) continue;
            if (source) return {{"success", false}, {"errorCode", "ambiguous_entry_name"}};
            source = &target.knowledgeNotes;
        }
    } else if (entryKind == "patch") {
        for (const auto& patch : profile.patches) {
            if (patch.name != entryName) continue;
            if (source) return {{"success", false}, {"errorCode", "ambiguous_entry_name"}};
            source = &patch.knowledgeNotes;
        }
    } else return {{"success", false}, {"errorCode", "invalid_entry_kind"}};
    if (!source) return {{"success", false}, {"errorCode", "entry_not_found"}};

    QString currentExecutableHash;
    if (process.isValid()) currentExecutableHash = resolutionContext(process, {}, {}).executableHash;
    QJsonObject result = evaluateKnowledgeNotes(*source, currentExecutableHash);
    result["success"] = true;
    result["versionEvaluated"] = !currentExecutableHash.isEmpty();
    return result;
}

} // namespace killcore
