// Audit de persistance des profils (.keprofile) -- demande explicite
// propriétaire (liste de 7 chantiers, point 7) : "vérifier que profils,
// Trainer, scripts Lua, CLR targets, patchs et dépendances se rechargent
// correctement... tests unitaires de round-trip JSON, cas de données
// anciennes/manquantes". Portée de ce fichier : core/profiles/profile_store.*
// (targets/patches/autoAsmScripts/luaScripts), qui n'avait jusqu'ici AUCUN
// test dédié -- vérifié avant d'écrire quoi que ce soit (grep sur tests/).
// Le nettoyage des dépendances Trainer mortes (dependsOn) est une logique
// JS séparée (ui/src/stores/trainerDependencies.ts), déjà couverte par
// scripts/test-trainer-dependencies.ps1 -- pas dupliqué ici.

#include "profiles/profile_store.h"
#include "paths/portable_paths.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QJsonDocument>
#include <QMap>
#include <cstring>
#include <limits>

using namespace killcore;

namespace {

// Meme pattern que ScopedUiLanguage (tests/unit/test_localization.cpp) : force
// une racine portable de fixture pour la duree du scope, jamais permanente.
class ScopedPortableRoot {
public:
    explicit ScopedPortableRoot(const QString& root) {
        PortablePaths::setTestRootOverride(root);
    }
    ~ScopedPortableRoot() {
        PortablePaths::setTestRootOverride(QString());
    }
};

// PORT-2c : legacyProfilesDir() pointe vers le vrai emplacement systeme
// (%LOCALAPPDATA%), pas overridable par design (c'est justement l'ancien
// dossier reel qu'on doit lire). Un fichier de fixture y est cree avec un nom
// unique, jamais un profil du propriétaire, et toujours supprime en fin de
// test -- meme pattern que ListRemoveRoundTripUsesRealProfilesDirWithUniqueName
// ci-dessous.
class ScopedLegacyProfileFile {
public:
    explicit ScopedLegacyProfileFile(const QString& path) : m_path(path) {}
    ~ScopedLegacyProfileFile() { QFile::remove(m_path); }

private:
    QString m_path;
};

Locator r2ModuleLocator(uint64_t offset = 0x20) {
    Locator locator;
    locator.module = "game.exe";
    locator.offset = offset;
    locator.lastAddress = 0x1020;
    return locator;
}

ProfileResolutionPlan r2Plan() {
    ProfileResolutionPlan plan;
    plan.discoveryMethod = "Controlled pointer scan";
    plan.expectedBytes = "ef be ad de";
    plan.validationTest = "Check the independent effect after a scene transition";
    plan.executableHash = "hash-v1";
    plan.moduleHashes = {{"game.exe", "hash-v1"}};
    plan.recordedSession = "100:created-1";
    plan.recordedAt = "2026-09-14T10:00:00Z";
    plan.evidenceNote = "Declared observation, not effect certification";
    return plan;
}

ProfileResolutionContext r2Context() {
    return {"game.exe", "hash-v1", "101:created-2", {{"game.exe", "hash-v1"}}};
}

LocatorProbe r2Observed(uint64_t address) {
    LocatorProbe probe;
    probe.resolved = true;
    probe.readable = true;
    probe.address = address;
    probe.bytes = QByteArray::fromHex("efbeadde");
    return probe;
}

QJsonObject r2Diagnose(const ProfileResolutionPlan& plan, const QList<LocatorProbe>& observations,
                       const ProfileResolutionContext& context = r2Context(),
                       const Locator& locator = r2ModuleLocator()) {
    return diagnoseProfileResolution("game.exe", "hash-v1", locator, plan, context, observations);
}

QByteArray r2Pointer(uint64_t pointer, size_t width = 8) {
    QByteArray result(static_cast<qsizetype>(width), '\0');
    std::memcpy(result.data(), &pointer, width);
    return result;
}

LocatorReadBytes r2Reader(const QMap<uint64_t, QByteArray>& memory) {
    return [memory](uint64_t address, size_t count) {
        return memory.value(address).left(static_cast<qsizetype>(count));
    };
}

} // namespace

TEST(ProfileDurability, RoundTripsMetadataForTargetsPatchesAndPointerMap) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    Profile profile;
    profile.executableName = "game.exe";
    ProfileTarget target;
    target.name = "Signature";
    target.locator = r2ModuleLocator();
    target.locator.kind = LocatorKind::PointerChain;
    target.locator.pointerChain = {"game.exe", 0x20, {0x10, 0x8}};
    target.resolutionPlan = r2Plan();
    target.resolutionPlan.alternatives = {r2ModuleLocator(0x80)};
    target.resolutionPlan.alternativeNames = {"Alternative"};
    target.resolutionPlan.baselineObservation = {{"address", "fedcba9876543210"},
        {"observedBytes", "ef be ad de"}, {"effectVerified", false}};
    profile.targets.append(target);
    ProfileCodePatch patch;
    patch.name = "Patch";
    patch.resolutionPlan = target.resolutionPlan;
    profile.patches.append(patch);
    const QString path = directory.filePath("r2.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));
    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.targets.size(), 1);
    ASSERT_EQ(loaded.patches.size(), 1);
    for (const auto& plan : {loaded.targets[0].resolutionPlan, loaded.patches[0].resolutionPlan}) {
        EXPECT_EQ(plan.discoveryMethod, target.resolutionPlan.discoveryMethod);
        EXPECT_EQ(plan.expectedBytes, "ef be ad de");
        EXPECT_EQ(plan.validationTest, target.resolutionPlan.validationTest);
        EXPECT_EQ(plan.executableHash, "hash-v1");
        EXPECT_EQ(plan.recordedSession, "100:created-1");
        EXPECT_EQ(plan.recordedAt, target.resolutionPlan.recordedAt);
        EXPECT_EQ(plan.evidenceNote, target.resolutionPlan.evidenceNote);
        EXPECT_EQ(plan.moduleHashes, target.resolutionPlan.moduleHashes);
        EXPECT_EQ(plan.baselineObservation, target.resolutionPlan.baselineObservation);
        ASSERT_EQ(plan.alternatives.size(), 1);
        EXPECT_EQ(plan.alternatives[0].offset, 0x80u);
        EXPECT_EQ(plan.alternativeNames, QStringList{"Alternative"});
    }
    Profile imported;
    const auto merged = ProfileStore::mergePointerMap(&imported, ProfileStore::exportPointerMap(profile), false);
    ASSERT_EQ(merged.imported, 1);
    EXPECT_EQ(imported.targets[0].resolutionPlan.expectedBytes, "ef be ad de");
}

TEST(ProfileDurability, LegacyProfileHasNoInventedEvidence) {
    QTemporaryDir directory;
    QFile file(directory.filePath("legacy.keprofile"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"formatVersion":1,"targets":[{"name":"Old","locator":{"kind":"absolute","lastAddress":"1234"}}]})");
    file.close();
    Profile profile;
    ASSERT_TRUE(ProfileStore::load(file.fileName(), &profile));
    ASSERT_EQ(profile.targets.size(), 1);
    const auto& plan = profile.targets[0].resolutionPlan;
    EXPECT_TRUE(plan.expectedBytes.isEmpty());
    EXPECT_TRUE(plan.baselineObservation.isEmpty());
    EXPECT_TRUE(plan.recordedSession.isEmpty());
}

TEST(ProfileDurability, RepeatedModuleRelocationsRecheckConditionsWithoutCertifyingEffect) {
    // Three synthetic sessions, not three real process launches.
    for (uint64_t base : {0x1000ULL, 0x5000ULL, 0x100000000ULL}) {
        const QList<ProcessModuleInfo> modules{{"game.exe", "", base, 0x1000}};
        const auto probe = probeLocator(r2ModuleLocator(), modules, 8, 4,
            r2Reader({{base + 0x20, QByteArray::fromHex("efbeadde")}}));
        ASSERT_TRUE(probe.readable);
        EXPECT_EQ(probe.address, base + 0x20);
        const auto report = r2Diagnose(r2Plan(), {probe});
        EXPECT_EQ(report.value("status").toString(), "conditions_verified");
        EXPECT_TRUE(report.value("conditionsVerified").toBool());
        EXPECT_FALSE(report.value("automaticApplicationAllowed").toBool());
        EXPECT_EQ(report.value("nextAction").toString(), "verify_effect_separately");
    }
}

TEST(ProfileDurability, PointerChainDereferencesBeforeEveryOffsetAfterHeapRelocation) {
    Locator locator = r2ModuleLocator();
    locator.kind = LocatorKind::PointerChain;
    locator.pointerChain = {"game.exe", 0x20, {0x10, 0x8}};
    for (uint64_t heap : {0x2000ULL, 0x9000ULL, 0x100000000ULL}) {
        const auto read = r2Reader({{0x1020, r2Pointer(heap)},
                                   {heap + 0x10, r2Pointer(heap + 0x100)},
                                   {heap + 0x108, QByteArray::fromHex("efbeadde")}});
        const auto probe = probeLocator(locator, {{"game.exe", "", 0x1000, 0x1000}}, 8, 4, read);
        ASSERT_TRUE(probe.readable);
        EXPECT_EQ(probe.address, heap + 0x108);
        EXPECT_EQ(r2Diagnose(r2Plan(), {probe}, r2Context(), locator).value("status").toString(), "conditions_verified");
    }
}

TEST(ProfileDurability, ChangedLayoutProposesAlternativeWithoutReplacingPrimary) {
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x80)};
    auto old = r2Observed(0x1020);
    old.bytes = QByteArray::fromHex("00000000");
    const auto report = r2Diagnose(plan, {old, r2Observed(0x1080)});
    EXPECT_EQ(report.value("status").toString(), "repair_candidate");
    EXPECT_EQ(report.value("candidateAddress").toString(), "1080");
    EXPECT_FALSE(report.value("conditionsVerified").toBool());
    EXPECT_FALSE(report.value("automaticApplicationAllowed").toBool());
    EXPECT_EQ(plan.alternatives[0].offset, 0x80u);
}

TEST(ProfileDurability, TwoMatchingAddressesAreAmbiguousEvenWhenPrimaryMatches) {
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x80)};
    const auto report = r2Diagnose(plan, {r2Observed(0x1020), r2Observed(0x1080)});
    EXPECT_EQ(report.value("status").toString(), "ambiguous");
    EXPECT_EQ(report.value("candidateCount").toInt(), 2);
    EXPECT_TRUE(report.value("candidateAddress").toString().isEmpty());
    EXPECT_FALSE(report.value("conditionsVerified").toBool());
}

TEST(ProfileDurability, DuplicatePathsToSameAddressAreNotAmbiguous) {
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x20)};
    const auto report = r2Diagnose(plan, {r2Observed(0x1020), r2Observed(0x1020)});
    EXPECT_EQ(report.value("candidateCount").toInt(), 1);
    EXPECT_EQ(report.value("status").toString(), "conditions_verified");
}

TEST(ProfileDurability, MissingAllStoredPathsIsExplicitlyIrrecoverableWithinScope) {
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x80)};
    LocatorProbe missing;
    missing.errorCode = "module_missing";
    const auto report = r2Diagnose(plan, {missing, missing});
    EXPECT_EQ(report.value("status").toString(), "missing");
    EXPECT_EQ(report.value("nextAction").toString(), "rediscover_target");
    EXPECT_FALSE(report.value("automaticApplicationAllowed").toBool());
}

TEST(ProfileDurability, UnreadableAlternativeCannotProveUniqueness) {
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x80)};
    LocatorProbe unknown;
    unknown.errorCode = "pointer_unreadable";
    const auto report = r2Diagnose(plan, {r2Observed(0x1020), unknown});
    EXPECT_EQ(report.value("status").toString(), "inconclusive");
    EXPECT_FALSE(report.value("conditionsVerified").toBool());
}

TEST(ProfileDurability, ExecutableAndModuleVersionChangesRequireRevalidation) {
    auto context = r2Context();
    context.executableHash = "hash-v2";
    auto report = r2Diagnose(r2Plan(), {r2Observed(0x1020)}, context);
    EXPECT_EQ(report.value("status").toString(), "version_changed");
    EXPECT_TRUE(report.value("executableVersionChanged").toBool());
    context = r2Context();
    context.moduleHashes["game.exe"] = "module-v2";
    report = r2Diagnose(r2Plan(), {r2Observed(0x1020)}, context);
    EXPECT_EQ(report.value("status").toString(), "version_changed");
    EXPECT_TRUE(report.value("moduleVersionChanged").toBool());
}

TEST(ProfileDurability, MissingHashesAndMissingConditionsNeverBecomeVerified) {
    auto context = r2Context();
    context.executableHash.clear();
    EXPECT_EQ(r2Diagnose(r2Plan(), {r2Observed(0x1020)}, context).value("status").toString(), "unverified");
    context = r2Context();
    context.moduleHashes = {};
    EXPECT_EQ(r2Diagnose(r2Plan(), {r2Observed(0x1020)}, context).value("status").toString(), "unverified");
    auto plan = r2Plan();
    plan.expectedBytes.clear();
    EXPECT_EQ(r2Diagnose(plan, {r2Observed(0x1020)}).value("status").toString(), "unverified");
}

TEST(ProfileDurability, AbsoluteAddressReusedByNewSessionDoesNotBecomeValid) {
    auto locator = r2ModuleLocator();
    locator.kind = LocatorKind::Absolute;
    locator.module.clear();
    const auto report = r2Diagnose(r2Plan(), {r2Observed(locator.lastAddress)}, r2Context(), locator);
    EXPECT_EQ(report.value("status").toString(), "session_changed");
    EXPECT_FALSE(report.value("conditionsVerified").toBool());
}

TEST(ProfileDurability, WrongExecutableIsRejectedEvenWithMatchingBytes) {
    auto context = r2Context();
    context.executableName = "other.exe";
    EXPECT_EQ(r2Diagnose(r2Plan(), {r2Observed(0x1020)}, context).value("status").toString(), "wrong_process");
}

TEST(ProfileDurability, RejectsMalformedConditionsAndTruncatedObservationSets) {
    for (const QString& invalid : {QString("GG"), QString("0"), QString("??"), QString(130, 'a')}) {
        auto plan = r2Plan();
        plan.expectedBytes = invalid;
        EXPECT_EQ(r2Diagnose(plan, {r2Observed(0x1020)}).value("status").toString(), "invalid_conditions");
    }
    auto plan = r2Plan();
    plan.alternatives = {r2ModuleLocator(0x80)};
    EXPECT_EQ(r2Diagnose(plan, {r2Observed(0x1020)}).value("status").toString(), "invalid_conditions");
}

TEST(ProfileDurability, LocatorBoundsPreventOverflowAndOutOfModuleReads) {
    int reads = 0;
    const LocatorReadBytes read = [&](uint64_t, size_t) { ++reads; return QByteArray(); };
    auto locator = r2ModuleLocator(0x1000);
    auto probe = probeLocator(locator, {{"game.exe", "", 0x1000, 0x1000}}, 8, 4, read);
    EXPECT_EQ(probe.errorCode, "module_offset_out_of_range");
    locator.offset = 0x20;
    probe = probeLocator(locator, {{"game.exe", "", std::numeric_limits<uint64_t>::max() - 0x10, 0x1000}}, 8, 4, read);
    EXPECT_EQ(probe.errorCode, "module_offset_out_of_range");
    EXPECT_EQ(reads, 0);
}

TEST(ProfileDurability, ModuleOffsetZeroAndHomonymousModulesAreHandled) {
    auto locator = r2ModuleLocator(0);
    const auto read = r2Reader({{0x1000, QByteArray::fromHex("efbeadde")}});
    EXPECT_TRUE(probeLocator(locator, {{"GAME.EXE", "", 0x1000, 0x1000}}, 8, 4, read).readable);
    const auto ambiguous = probeLocator(locator,
        {{"GAME.EXE", "", 0x1000, 0x1000}, {"game.exe", "", 0x2000, 0x1000}}, 8, 4, read);
    EXPECT_EQ(ambiguous.errorCode, "ambiguous_module");
}

TEST(ProfileDurability, PointerWidthNullOverflowAndShortReadsAreRejected) {
    auto locator = r2ModuleLocator();
    locator.kind = LocatorKind::PointerChain;
    locator.pointerChain = {"game.exe", 0x20, {0x10}};
    const QList<ProcessModuleInfo> modules{{"game.exe", "", 0x1000, 0x1000}};
    auto probe = probeLocator(locator, modules, 4, 4, r2Reader({{0x1020, r2Pointer(0xfffffff8, 4)}}));
    EXPECT_EQ(probe.errorCode, "address_overflow");
    probe = probeLocator(locator, modules, 8, 4, r2Reader({{0x1020, r2Pointer(0)}}));
    EXPECT_EQ(probe.errorCode, "null_pointer");
    probe = probeLocator(locator, modules, 8, 4, r2Reader({{0x1020, QByteArray(3, '\0')}}));
    EXPECT_EQ(probe.errorCode, "pointer_unreadable");
    probe = probeLocator(r2ModuleLocator(), modules, 8, 4, r2Reader({{0x1020, QByteArray(3, '\0')}}));
    EXPECT_FALSE(probe.readable);
    EXPECT_EQ(probe.errorCode, "address_unreadable");
}

TEST(ProfileDurability, ClrProbeIsExplicitlyUnsupportedWithoutMemoryAccess) {
    Locator locator;
    locator.kind = LocatorKind::ClrField;
    int reads = 0;
    const auto probe = probeLocator(locator, {}, 8, 4, [&](uint64_t, size_t) { ++reads; return QByteArray(); });
    EXPECT_EQ(probe.errorCode, "unsupported_locator");
    EXPECT_EQ(reads, 0);
}

TEST(ProfileDurability, MalformedImportedMetadataCannotSilentlyDropConditions) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto& malformed : {QJsonValue("invalid"), QJsonValue(QJsonObject{{"expectedBytes", 123}}),
                                  QJsonValue(QJsonObject{{"schemaVersion", 99}}),
                                  QJsonValue(QJsonObject{{"alternatives", "not an array"}})}) {
        QFile file(directory.filePath("malformed.keprofile"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QJsonObject target{{"name", "Target"}, {"resolutionPlan", malformed}};
        file.write(QJsonDocument(QJsonObject{{"targets", QJsonArray{target}}}).toJson());
        file.close();
        Profile loaded;
        ASSERT_TRUE(ProfileStore::load(file.fileName(), &loaded));
        ASSERT_EQ(loaded.targets.size(), 1);
        EXPECT_TRUE(loaded.targets[0].resolutionPlan.invalid);
        EXPECT_EQ(r2Diagnose(loaded.targets[0].resolutionPlan, {r2Observed(0x1020)}).value("status").toString(), "invalid_conditions");
        // Re-saving must not silently remove the invalid marker.
        ASSERT_TRUE(ProfileStore::save(loaded, file.fileName()));
        ASSERT_TRUE(ProfileStore::load(file.fileName(), &loaded));
        EXPECT_TRUE(loaded.targets[0].resolutionPlan.invalid);
    }
}

TEST(ProfileDurability, ReadableButChangedBytesAreNotSuccessfulResolutionEvidence) {
    auto observation = r2Observed(0x1020);
    observation.bytes = QByteArray::fromHex("00000000");
    const auto report = r2Diagnose(r2Plan(), {observation});
    EXPECT_EQ(report.value("status").toString(), "conditions_mismatch");
    EXPECT_FALSE(report.value("conditionsVerified").toBool());
}

TEST(ProfileDurability, ExcessiveChainAndAlternativeCountsStayBounded) {
    auto locator = r2ModuleLocator();
    locator.kind = LocatorKind::PointerChain;
    locator.pointerChain = {"game.exe", 0x20, QList<uint64_t>(17, 0x10)};
    int reads = 0;
    const auto probe = probeLocator(locator, {{"game.exe", "", 0x1000, 0x1000}}, 8, 4,
        [&](uint64_t, size_t) { ++reads; return QByteArray(); });
    EXPECT_EQ(probe.errorCode, "invalid_pointer_chain");
    EXPECT_EQ(reads, 0);
    auto plan = r2Plan();
    plan.alternatives = QList<Locator>(9, r2ModuleLocator());
    const auto report = r2Diagnose(plan, QList<LocatorProbe>(10, r2Observed(0x1020)));
    EXPECT_EQ(report.value("status").toString(), "invalid_conditions");
}

namespace {

Profile buildFullProfile() {
    Profile profile;
    profile.gameName = "KillEngine Test Target";
    profile.executableName = "KillEngineTestTarget.exe";
    profile.executableHash = "deadbeefcafef00d";

    ProfileTarget moduleOffsetTarget;
    moduleOffsetTarget.name = "Money";
    moduleOffsetTarget.type = ValueType::Int32;
    moduleOffsetTarget.locator.kind = LocatorKind::ModuleOffset;
    moduleOffsetTarget.locator.module = "KillEngineTestTarget.exe";
    moduleOffsetTarget.locator.offset = 0x3F2A0;
    moduleOffsetTarget.locator.lastAddress = 0x7FF612345678ULL;
    moduleOffsetTarget.description = "Money counter";
    moduleOffsetTarget.ghidraSymbol = "PlayerMoney";
    moduleOffsetTarget.ghidraNote = "Imported symbol";
    profile.targets.append(moduleOffsetTarget);

    ProfileTarget absoluteTarget;
    absoluteTarget.name = "DebugOnly";
    absoluteTarget.type = ValueType::Float32;
    absoluteTarget.locator.kind = LocatorKind::Absolute;
    absoluteTarget.locator.lastAddress = 0x7FF6AAAABBBBULL;
    profile.targets.append(absoluteTarget);

    ProfileTarget pointerChainTarget;
    pointerChainTarget.name = "PlayerHealth";
    pointerChainTarget.type = ValueType::Int32;
    pointerChainTarget.locator.kind = LocatorKind::PointerChain;
    pointerChainTarget.locator.pointerChain.module = "KillEngineTestTarget.exe";
    pointerChainTarget.locator.pointerChain.baseOffset = 0x12345;
    pointerChainTarget.locator.pointerChain.offsets = {0x10, 0x28, 0x8};
    pointerChainTarget.dependsOn = {"Money", "ManagedHealth"};
    profile.targets.append(pointerChainTarget);

    ProfileTarget clrFieldTarget;
    clrFieldTarget.name = "ManagedHealth";
    clrFieldTarget.type = ValueType::Int32;
    clrFieldTarget.locator.kind = LocatorKind::ClrField;
    clrFieldTarget.locator.clrField.typeSubstring = "Player";
    clrFieldTarget.locator.clrField.identityField = "Id";
    clrFieldTarget.locator.clrField.identityValue = "42";
    clrFieldTarget.locator.clrField.targetField = "Health";
    profile.targets.append(clrFieldTarget);

    ProfileCodePatch fullPatch;
    fullPatch.name = "Infinite Ammo";
    fullPatch.module = "KillEngineTestTarget.exe";
    fullPatch.moduleOffset = 0x5234;
    fullPatch.aobPattern = "48 83 EC ??";
    fullPatch.patchBytes = "90 90 90 90";
    fullPatch.originalBytes = "48 83 EC 28";
    fullPatch.disassembly = "sub rsp, 28";
    fullPatch.riskLevel = "medium";
    fullPatch.description = "NOPs the ammo decrement";
    fullPatch.ghidraSymbol = "ApplyAmmoCost";
    fullPatch.ghidraNote = "Named in Ghidra";
    fullPatch.signatureScore = 87;
    fullPatch.signatureLevel = "good";
    fullPatch.signatureWarning = "";
    fullPatch.signatureFixedBytes = 6;
    fullPatch.signatureWildcardBytes = 1;
    fullPatch.signatureUniqueFixedBytes = 6;
    fullPatch.signatureFixedRatio = 0.857;
    fullPatch.trainerSafe = true;
    fullPatch.signatureMatches = 1;
    profile.patches.append(fullPatch);

    ProfileCodePatch minimalPatch;
    minimalPatch.name = "Minimal";
    minimalPatch.module = "KillEngineTestTarget.exe";
    minimalPatch.moduleOffset = 0x1000;
    minimalPatch.aobPattern = "90";
    minimalPatch.patchBytes = "CC";
    profile.patches.append(minimalPatch);

    ProfileAutoAsmScript asmScript;
    asmScript.name = "Trampoline";
    asmScript.scriptText = "alloc(newmem, 256)\nnewmem:\nmov [rax+8], 1\n";
    asmScript.description = "Test trampoline";
    asmScript.riskLevel = "high";
    profile.autoAsmScripts.append(asmScript);

    ProfileLuaScript luaScript;
    luaScript.name = "Ping helper";
    luaScript.scriptText = "local ke = require('killengine')\nprint(ke.ping('hi'))\n";
    luaScript.description = "Smoke test script";
    luaScript.savedAtEpochMs = 1735000000000LL;
    profile.luaScripts.append(luaScript);

    return profile;
}

} // namespace

TEST(ProfileStore, RoundTripsFullProfileExactly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("full.keprofile");

    const Profile original = buildFullProfile();
    ASSERT_TRUE(ProfileStore::save(original, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));

    EXPECT_EQ(loaded.gameName, original.gameName);
    EXPECT_EQ(loaded.executableName, original.executableName);
    EXPECT_EQ(loaded.executableHash, original.executableHash);

    ASSERT_EQ(loaded.targets.size(), original.targets.size());
    for (int i = 0; i < original.targets.size(); ++i) {
        const auto& a = original.targets[i];
        const auto& b = loaded.targets[i];
        EXPECT_EQ(b.name, a.name) << i;
        EXPECT_EQ(static_cast<int>(b.type), static_cast<int>(a.type)) << i;
        EXPECT_EQ(b.description, a.description) << i;
        EXPECT_EQ(static_cast<int>(b.locator.kind), static_cast<int>(a.locator.kind)) << i;
        EXPECT_EQ(b.locator.module, a.locator.module) << i;
        EXPECT_EQ(b.locator.offset, a.locator.offset) << i;
        EXPECT_EQ(b.locator.lastAddress, a.locator.lastAddress) << i;
        EXPECT_EQ(b.locator.pointerChain.module, a.locator.pointerChain.module) << i;
        EXPECT_EQ(b.locator.pointerChain.baseOffset, a.locator.pointerChain.baseOffset) << i;
        EXPECT_EQ(b.locator.pointerChain.offsets, a.locator.pointerChain.offsets) << i;
        EXPECT_EQ(b.locator.clrField.typeSubstring, a.locator.clrField.typeSubstring) << i;
        EXPECT_EQ(b.locator.clrField.identityField, a.locator.clrField.identityField) << i;
        EXPECT_EQ(b.locator.clrField.identityValue, a.locator.clrField.identityValue) << i;
        EXPECT_EQ(b.locator.clrField.targetField, a.locator.clrField.targetField) << i;
        EXPECT_EQ(b.dependsOn, a.dependsOn) << i;
        EXPECT_EQ(b.ghidraSymbol, a.ghidraSymbol) << i;
        EXPECT_EQ(b.ghidraNote, a.ghidraNote) << i;
    }

    ASSERT_EQ(loaded.patches.size(), original.patches.size());
    const auto& origFullPatch = original.patches[0];
    const auto& loadedFullPatch = loaded.patches[0];
    EXPECT_EQ(loadedFullPatch.name, origFullPatch.name);
    EXPECT_EQ(loadedFullPatch.module, origFullPatch.module);
    EXPECT_EQ(loadedFullPatch.moduleOffset, origFullPatch.moduleOffset);
    EXPECT_EQ(loadedFullPatch.aobPattern, origFullPatch.aobPattern);
    EXPECT_EQ(loadedFullPatch.patchBytes, origFullPatch.patchBytes);
    EXPECT_EQ(loadedFullPatch.originalBytes, origFullPatch.originalBytes);
    EXPECT_EQ(loadedFullPatch.disassembly, origFullPatch.disassembly);
    EXPECT_EQ(loadedFullPatch.riskLevel, origFullPatch.riskLevel);
    EXPECT_EQ(loadedFullPatch.description, origFullPatch.description);
    EXPECT_EQ(loadedFullPatch.ghidraSymbol, origFullPatch.ghidraSymbol);
    EXPECT_EQ(loadedFullPatch.ghidraNote, origFullPatch.ghidraNote);
    EXPECT_EQ(loadedFullPatch.signatureScore, origFullPatch.signatureScore);
    EXPECT_EQ(loadedFullPatch.signatureLevel, origFullPatch.signatureLevel);
    EXPECT_EQ(loadedFullPatch.signatureFixedBytes, origFullPatch.signatureFixedBytes);
    EXPECT_EQ(loadedFullPatch.signatureWildcardBytes, origFullPatch.signatureWildcardBytes);
    EXPECT_EQ(loadedFullPatch.signatureUniqueFixedBytes, origFullPatch.signatureUniqueFixedBytes);
    EXPECT_DOUBLE_EQ(loadedFullPatch.signatureFixedRatio, origFullPatch.signatureFixedRatio);
    EXPECT_EQ(loadedFullPatch.trainerSafe, origFullPatch.trainerSafe);
    EXPECT_EQ(loadedFullPatch.signatureMatches, origFullPatch.signatureMatches);

    ASSERT_EQ(loaded.autoAsmScripts.size(), original.autoAsmScripts.size());
    EXPECT_EQ(loaded.autoAsmScripts[0].name, original.autoAsmScripts[0].name);
    EXPECT_EQ(loaded.autoAsmScripts[0].scriptText, original.autoAsmScripts[0].scriptText);
    EXPECT_EQ(loaded.autoAsmScripts[0].description, original.autoAsmScripts[0].description);
    EXPECT_EQ(loaded.autoAsmScripts[0].riskLevel, original.autoAsmScripts[0].riskLevel);

    ASSERT_EQ(loaded.luaScripts.size(), original.luaScripts.size());
    EXPECT_EQ(loaded.luaScripts[0].name, original.luaScripts[0].name);
    EXPECT_EQ(loaded.luaScripts[0].scriptText, original.luaScripts[0].scriptText);
    EXPECT_EQ(loaded.luaScripts[0].description, original.luaScripts[0].description);
    EXPECT_EQ(loaded.luaScripts[0].savedAtEpochMs, original.luaScripts[0].savedAtEpochMs);
}

TEST(ProfileStore, MinimalPatchRoundTripsWithZeroedSignatureQuality) {
    // patchToJson() n'ecrit "signatureQuality" que si score>0 ou level non
    // vide (voir profile_store.cpp) -- verifie que patchFromJson() ne plante
    // pas et retombe sur des zeros/vide propres quand la cle est absente.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("minimal.keprofile");

    Profile original;
    original.gameName = "G";
    original.executableName = "g.exe";
    ProfileCodePatch minimal;
    minimal.name = "Minimal";
    minimal.module = "g.exe";
    minimal.moduleOffset = 0x10;
    minimal.aobPattern = "90";
    minimal.patchBytes = "CC";
    original.patches.append(minimal);

    ASSERT_TRUE(ProfileStore::save(original, path));
    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));

    ASSERT_EQ(loaded.patches.size(), 1);
    EXPECT_EQ(loaded.patches[0].signatureScore, 0);
    EXPECT_TRUE(loaded.patches[0].signatureLevel.isEmpty());
    EXPECT_FALSE(loaded.patches[0].trainerSafe);
    EXPECT_DOUBLE_EQ(loaded.patches[0].signatureFixedRatio, 0.0);
}

TEST(ProfileStore, LoadsOldProfileMissingNewerFieldsAsEmptyNotCrash) {
    // Simule un .keprofile ecrit avant que patches/autoAsmScripts/luaScripts
    // n'existent (PHASE 19/79/K) : seuls formatVersion/gameName/
    // executableName/targets sont presents. load() doit reussir et rendre
    // des listes vides plutot que d'echouer ou de dereferencer du JSON absent.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("old.keprofile");

    const QString oldJson = QStringLiteral(
        "{\"formatVersion\":1,\"gameName\":\"Old Game\",\"executableName\":\"old.exe\","
        "\"targets\":[{\"name\":\"Score\",\"type\":\"Int32\",\"locator\":{\"kind\":\"module_offset\","
        "\"module\":\"old.exe\",\"offset\":\"100\",\"lastAddress\":\"0\"}}]}");

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    {
        QTextStream stream(&file);
        stream << oldJson;
    }
    file.close();

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    EXPECT_EQ(loaded.gameName, QStringLiteral("Old Game"));
    ASSERT_EQ(loaded.targets.size(), 1);
    EXPECT_EQ(loaded.targets[0].name, QStringLiteral("Score"));
    EXPECT_EQ(loaded.targets[0].locator.offset, 0x100ULL);
    EXPECT_TRUE(loaded.patches.isEmpty());
    EXPECT_TRUE(loaded.autoAsmScripts.isEmpty());
    EXPECT_TRUE(loaded.luaScripts.isEmpty());
    EXPECT_TRUE(loaded.targets[0].dependsOn.isEmpty());
}

TEST(ProfileStore, ExportPointerMapIncludesOnlyPointerChainsWithMetadata) {
    const Profile profile = buildFullProfile();

    const QJsonObject pointerMap = ProfileStore::exportPointerMap(profile);

    EXPECT_EQ(pointerMap.value("format").toString(), QStringLiteral("killengine.pointer_map"));
    EXPECT_EQ(pointerMap.value("formatVersion").toInt(), 1);
    EXPECT_EQ(pointerMap.value("sourceGameName").toString(), profile.gameName);
    const QJsonArray targets = pointerMap.value("targets").toArray();
    ASSERT_EQ(targets.size(), 1);

    const QJsonObject target = targets[0].toObject();
    EXPECT_EQ(target.value("name").toString(), QStringLiteral("PlayerHealth"));
    EXPECT_EQ(target.value("locator").toObject().value("kind").toString(), QStringLiteral("pointer_chain"));
    const QJsonArray dependencies = target.value("dependsOn").toArray();
    ASSERT_EQ(dependencies.size(), 2);
    EXPECT_EQ(dependencies[0].toString(), QStringLiteral("Money"));
    EXPECT_EQ(dependencies[1].toString(), QStringLiteral("ManagedHealth"));
}

TEST(ProfileStore, MergePointerMapSkipsDuplicatesUnlessReplaceRequested) {
    const Profile source = buildFullProfile();
    const QJsonObject pointerMap = ProfileStore::exportPointerMap(source);

    Profile destination;
    destination.gameName = "Destination";
    destination.executableName = "dest.exe";

    auto firstImport = ProfileStore::mergePointerMap(&destination, pointerMap, false);
    EXPECT_EQ(firstImport.imported, 1);
    EXPECT_EQ(firstImport.replaced, 0);
    EXPECT_EQ(firstImport.skipped, 0);
    ASSERT_EQ(destination.targets.size(), 1);
    EXPECT_EQ(destination.targets[0].name, QStringLiteral("PlayerHealth"));
    EXPECT_EQ(destination.targets[0].dependsOn, QStringList({QStringLiteral("Money"), QStringLiteral("ManagedHealth")}));

    auto duplicateImport = ProfileStore::mergePointerMap(&destination, pointerMap, false);
    EXPECT_EQ(duplicateImport.imported, 0);
    EXPECT_EQ(duplicateImport.skipped, 1);
    ASSERT_EQ(destination.targets.size(), 1);

    QJsonObject replacementMap = pointerMap;
    QJsonArray targets = replacementMap.value("targets").toArray();
    QJsonObject replacementTarget = targets[0].toObject();
    replacementTarget["description"] = "Replacement";
    targets[0] = replacementTarget;
    replacementMap["targets"] = targets;

    auto replacementImport = ProfileStore::mergePointerMap(&destination, replacementMap, true);
    EXPECT_EQ(replacementImport.imported, 1);
    EXPECT_EQ(replacementImport.replaced, 1);
    EXPECT_EQ(replacementImport.skipped, 0);
    ASSERT_EQ(destination.targets.size(), 1);
    EXPECT_EQ(destination.targets[0].description, QStringLiteral("Replacement"));
}

TEST(ProfileStore, LoadFailsCleanlyOnMalformedJson) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("broken.keprofile");

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    {
        QTextStream stream(&file);
        stream << "{ this is not valid json";
    }
    file.close();

    Profile loaded;
    EXPECT_FALSE(ProfileStore::load(path, &loaded));
}

TEST(ProfileStore, LoadFailsCleanlyOnMissingFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Profile loaded;
    EXPECT_FALSE(ProfileStore::load(dir.filePath("does-not-exist.keprofile"), &loaded));
}

TEST(ProfileStore, LoadReturnsFalseWithNullOutputPointer) {
    EXPECT_FALSE(ProfileStore::load("irrelevant.keprofile", nullptr));
}

TEST(ProfileStore, ListRemoveRoundTripUsesRealProfilesDirWithUniqueName) {
    // listProfiles()/remove()/profilePath() travaillent sur le dossier reel
    // (<dossier de l'executable>\data\profiles depuis PORT-2b, 17/09/2026 --
    // avant, %LOCALAPPDATA%\KillEngine\Profiles), contrairement a save/load
    // qui acceptent un chemin complet arbitraire (teste ci-dessus via
    // QTemporaryDir). Nom unique + nettoyage explicite pour ne pas laisser de
    // profil residuel sous le dossier de sortie du build.
    const QString uniqueName = QStringLiteral("killengine_test_profile_store_%1")
        .arg(QDateTime::currentMSecsSinceEpoch());

    ASSERT_TRUE(ProfileStore::ensureProfilesDir());
    Profile profile;
    profile.gameName = "Unique Test Profile";
    profile.executableName = "unique.exe";
    ASSERT_TRUE(ProfileStore::save(profile, ProfileStore::profilePath(uniqueName)));

    EXPECT_TRUE(ProfileStore::listProfiles().contains(uniqueName));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(ProfileStore::profilePath(uniqueName), &loaded));
    EXPECT_EQ(loaded.gameName, profile.gameName);

    EXPECT_TRUE(ProfileStore::remove(uniqueName));
    EXPECT_FALSE(ProfileStore::listProfiles().contains(uniqueName));
}

// PORT-2c (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : reprise explicite et non
// destructive des profils encore sous l'ancien emplacement systeme.
TEST(ProfileStore, ImportLegacyProfilesCopiesValidSkipsCollisionAndFlagsInvalid) {
    QDir().mkpath(ProfileStore::legacyProfilesDir());

    const QString suffix = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString validName = "killengine_test_legacy_valid_" + suffix;
    const QString collisionName = "killengine_test_legacy_collision_" + suffix;
    const QString invalidName = "killengine_test_legacy_invalid_" + suffix;

    const QString validLegacyPath = QDir(ProfileStore::legacyProfilesDir()).filePath(validName + ".keprofile");
    const QString collisionLegacyPath = QDir(ProfileStore::legacyProfilesDir()).filePath(collisionName + ".keprofile");
    const QString invalidLegacyPath = QDir(ProfileStore::legacyProfilesDir()).filePath(invalidName + ".keprofile");

    ScopedLegacyProfileFile cleanupValid(validLegacyPath);
    ScopedLegacyProfileFile cleanupCollision(collisionLegacyPath);
    ScopedLegacyProfileFile cleanupInvalid(invalidLegacyPath);

    Profile validLegacyProfile;
    validLegacyProfile.gameName = "Legacy Valid";
    validLegacyProfile.executableName = "legacy.exe";
    ASSERT_TRUE(ProfileStore::save(validLegacyProfile, validLegacyPath));

    Profile collisionLegacyProfile;
    collisionLegacyProfile.gameName = "Legacy Incoming (must not win)";
    ASSERT_TRUE(ProfileStore::save(collisionLegacyProfile, collisionLegacyPath));

    QFile invalidFile(invalidLegacyPath);
    ASSERT_TRUE(invalidFile.open(QIODevice::WriteOnly | QIODevice::Text));
    invalidFile.write("this is not valid profile JSON {{{");
    invalidFile.close();

    QTemporaryDir portableRootDir;
    ASSERT_TRUE(portableRootDir.isValid());
    ScopedPortableRoot scopedRoot(portableRootDir.path());

    // Un profil portable du meme nom existe deja *avant* l'import -- ne doit
    // jamais etre ecrase par la version legacy homonyme.
    const QString collisionPortablePath = QDir(ProfileStore::profilesDir()).filePath(collisionName + ".keprofile");
    Profile existingPortableProfile;
    existingPortableProfile.gameName = "Portable Original (must survive)";
    ASSERT_TRUE(ProfileStore::save(existingPortableProfile, collisionPortablePath));

    const QList<ProfileStore::LegacyImportResult> results = ProfileStore::importLegacyProfiles();

    auto findResult = [&results](const QString& fileName) -> const ProfileStore::LegacyImportResult* {
        for (const auto& r : results) {
            if (r.fileName == fileName) return &r;
        }
        return nullptr;
    };

    const auto* validResult = findResult(validName + ".keprofile");
    ASSERT_NE(validResult, nullptr);
    EXPECT_TRUE(validResult->imported);
    EXPECT_FALSE(validResult->skippedExisting);
    EXPECT_FALSE(validResult->invalid);

    const auto* collisionResult = findResult(collisionName + ".keprofile");
    ASSERT_NE(collisionResult, nullptr);
    EXPECT_FALSE(collisionResult->imported);
    EXPECT_TRUE(collisionResult->skippedExisting);

    const auto* invalidResult = findResult(invalidName + ".keprofile");
    ASSERT_NE(invalidResult, nullptr);
    EXPECT_FALSE(invalidResult->imported);
    EXPECT_TRUE(invalidResult->invalid);

    // Copie reelle, contenu correct, jamais un deplacement.
    const QString validPortablePath = QDir(ProfileStore::profilesDir()).filePath(validName + ".keprofile");
    Profile importedProfile;
    ASSERT_TRUE(ProfileStore::load(validPortablePath, &importedProfile));
    EXPECT_EQ(importedProfile.gameName, "Legacy Valid");
    EXPECT_TRUE(QFile::exists(validLegacyPath)); // la source n'a jamais ete supprimee

    // Le profil portable pre-existant n'a pas ete ecrase par l'homonyme legacy.
    Profile survivingProfile;
    ASSERT_TRUE(ProfileStore::load(collisionPortablePath, &survivingProfile));
    EXPECT_EQ(survivingProfile.gameName, "Portable Original (must survive)");

    // Le fichier invalide n'a jamais ete copie.
    const QString invalidPortablePath = QDir(ProfileStore::profilesDir()).filePath(invalidName + ".keprofile");
    EXPECT_FALSE(QFile::exists(invalidPortablePath));
}

TEST(ProfileStore, ImportLegacyProfilesReturnsEmptyWhenNothingToImportFromLegacyDir) {
    // legacyProfilesDir() existe presque certainement deja sur la machine de
    // test (vrais profils du propriétaire) -- ce test ne verifie donc pas
    // l'absence du dossier, seulement qu'un nom qui n'existe pas cote legacy
    // n'apparait jamais dans les resultats.
    const QString suffix = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString absentName = "killengine_test_legacy_absent_" + suffix + ".keprofile";

    QTemporaryDir portableRootDir;
    ASSERT_TRUE(portableRootDir.isValid());
    ScopedPortableRoot scopedRoot(portableRootDir.path());

    const QList<ProfileStore::LegacyImportResult> results = ProfileStore::importLegacyProfiles();
    for (const auto& r : results) {
        EXPECT_NE(r.fileName, absentName);
    }
}
