#include "profiles/profile_store.h"
#include "process/process_handle.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using killcore::KnowledgeNote;
using killcore::KnowledgeNoteKind;
using killcore::Profile;
using killcore::ProfileCodePatch;
using killcore::ProfileStore;
using killcore::ProfileTarget;
using killcore::evaluateKnowledgeNotes;
using killcore::knowledgeNoteKindFromString;
using killcore::knowledgeNoteKindToString;

namespace {

Profile profileWithOneTarget() {
    Profile profile;
    profile.gameName = "R4 Fixture";
    profile.executableName = "R4Fixture.exe";
    ProfileTarget target;
    target.name = "Health";
    profile.targets.append(target);
    return profile;
}

QJsonObject validOptions(const QString& kind = "recheck", const QString& description = "à vérifier") {
    return QJsonObject{{"kind", kind}, {"description", description}, {"experiment", ""}, {"evidenceNote", ""}};
}

} // namespace

TEST(KnowledgeNoteKindStrings, RoundTripsAllKnownKinds) {
    const QList<KnowledgeNoteKind> kinds = {
        KnowledgeNoteKind::ExplainedFailure,
        KnowledgeNoteKind::SuccessCondition,
        KnowledgeNoteKind::DiscriminatingExperiment,
        KnowledgeNoteKind::Recheck,
    };
    for (KnowledgeNoteKind kind : kinds) {
        const QString text = knowledgeNoteKindToString(kind);
        KnowledgeNoteKind parsed;
        ASSERT_TRUE(knowledgeNoteKindFromString(text, &parsed));
        EXPECT_EQ(parsed, kind);
    }
}

TEST(KnowledgeNoteKindStrings, UnknownStringIsRejectedNotDefaulted) {
    KnowledgeNoteKind parsed = KnowledgeNoteKind::SuccessCondition; // sentinel, must stay untouched
    EXPECT_FALSE(knowledgeNoteKindFromString("not_a_real_kind", &parsed));
    EXPECT_EQ(parsed, KnowledgeNoteKind::SuccessCondition);
}

TEST(ProfileKnowledgeNote, AddWithoutAttachedProcessSucceedsButStaysUnversioned) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess; // default-constructed: isValid() == false
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("explained_failure", "Bulles Solitaire : checksum jamais trouvé côté save local"),
        noProcess, &error);
    ASSERT_FALSE(id.isEmpty()) << error.toStdString();
    ASSERT_EQ(profile.targets[0].knowledgeNotes.size(), 1);
    const auto& note = profile.targets[0].knowledgeNotes.first();
    EXPECT_EQ(note.id, id);
    EXPECT_EQ(note.kind, KnowledgeNoteKind::ExplainedFailure);
    EXPECT_TRUE(note.executableHash.isEmpty());
    EXPECT_TRUE(note.sessionId.isEmpty());
    EXPECT_FALSE(note.recordedAt.isEmpty());
}

TEST(ProfileKnowledgeNote, RejectsUnknownKind) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("not_a_kind", "peu importe"), noProcess, &error);
    EXPECT_TRUE(id.isEmpty());
    EXPECT_EQ(error, "invalid_kind");
}

TEST(ProfileKnowledgeNote, RejectsEmptyDescription) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("recheck", "   "), noProcess, &error);
    EXPECT_TRUE(id.isEmpty());
    EXPECT_EQ(error, "invalid_note");
}

TEST(ProfileKnowledgeNote, RejectsUnknownEntry) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "DoesNotExist",
        validOptions(), noProcess, &error);
    EXPECT_TRUE(id.isEmpty());
    EXPECT_EQ(error, "entry_not_found");
}

TEST(ProfileKnowledgeNote, RejectsInvalidEntryKind) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "not_a_kind_either", "Health",
        validOptions(), noProcess, &error);
    EXPECT_TRUE(id.isEmpty());
    EXPECT_EQ(error, "invalid_entry_kind");
}

TEST(ProfileKnowledgeNote, CapsAtMaximumNotesPerEntry) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    for (int i = 0; i < ProfileStore::kMaxKnowledgeNotesPerEntry; ++i) {
        const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
            validOptions("recheck", QString("note %1").arg(i)), noProcess, &error);
        ASSERT_FALSE(id.isEmpty()) << i << " " << error.toStdString();
    }
    ASSERT_EQ(profile.targets[0].knowledgeNotes.size(), ProfileStore::kMaxKnowledgeNotesPerEntry);
    const QString overflowId = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("recheck", "one too many"), noProcess, &error);
    EXPECT_TRUE(overflowId.isEmpty());
    EXPECT_EQ(error, "too_many_notes");
    EXPECT_EQ(profile.targets[0].knowledgeNotes.size(), ProfileStore::kMaxKnowledgeNotesPerEntry);
}

TEST(ProfileKnowledgeNote, WorksOnPatchesToo) {
    Profile profile;
    ProfileCodePatch patch;
    patch.name = "SpeedPatch";
    profile.patches.append(patch);
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "patch", "SpeedPatch",
        validOptions("success_condition", "tient tant que le module n'est pas rechargé"), noProcess, &error);
    ASSERT_FALSE(id.isEmpty()) << error.toStdString();
    ASSERT_EQ(profile.patches[0].knowledgeNotes.size(), 1);
}

TEST(ProfileKnowledgeNote, PersistsAcrossSaveAndLoad) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = directory.filePath("r4.keprofile");

    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    const QString id = ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("discriminating_experiment", "changer le max sans toucher la valeur actuelle a tranché"),
        noProcess, &error);
    ASSERT_FALSE(id.isEmpty()) << error.toStdString();
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.targets.size(), 1);
    ASSERT_EQ(loaded.targets[0].knowledgeNotes.size(), 1);
    const auto& note = loaded.targets[0].knowledgeNotes.first();
    EXPECT_EQ(note.id, id);
    EXPECT_EQ(note.kind, KnowledgeNoteKind::DiscriminatingExperiment);
    EXPECT_EQ(note.description, "changer le max sans toucher la valeur actuelle a tranché");
    EXPECT_FALSE(note.invalid);
}

TEST(ProfileKnowledgeNote, MalformedImportedNoteIsFlaggedInvalidNotDropped) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    for (const auto& malformed : {QJsonValue("just a string, not an object"),
                                  QJsonValue(QJsonObject{{"description", 123}}),
                                  QJsonValue(QJsonObject{{"schemaVersion", 99}, {"description", "ok text"}})}) {
        QFile file(directory.filePath("malformed.keprofile"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        QJsonObject target{{"name", "Health"}, {"knowledgeNotes", QJsonArray{malformed}}};
        file.write(QJsonDocument(QJsonObject{{"targets", QJsonArray{target}}}).toJson());
        file.close();

        Profile loaded;
        ASSERT_TRUE(ProfileStore::load(file.fileName(), &loaded));
        ASSERT_EQ(loaded.targets.size(), 1);
        ASSERT_EQ(loaded.targets[0].knowledgeNotes.size(), 1);
        EXPECT_TRUE(loaded.targets[0].knowledgeNotes.first().invalid);

        // Re-saving must not silently drop the invalid marker.
        ASSERT_TRUE(ProfileStore::save(loaded, file.fileName()));
        ASSERT_TRUE(ProfileStore::load(file.fileName(), &loaded));
        ASSERT_EQ(loaded.targets[0].knowledgeNotes.size(), 1);
        EXPECT_TRUE(loaded.targets[0].knowledgeNotes.first().invalid);
    }
}

TEST(EvaluateKnowledgeNotes, SplitsValidStaleAndUnversioned) {
    KnowledgeNote current;
    current.id = "K-current";
    current.executableHash = "AAA111";

    KnowledgeNote outdated;
    outdated.id = "K-outdated";
    outdated.executableHash = "BBB222";

    KnowledgeNote noVersion;
    noVersion.id = "K-unversioned";
    noVersion.executableHash = "";

    const QJsonObject result = evaluateKnowledgeNotes({current, outdated, noVersion}, "aaa111");
    ASSERT_EQ(result.value("valid").toArray().size(), 1);
    EXPECT_EQ(result.value("valid").toArray().first().toObject().value("id").toString(), "K-current");
    ASSERT_EQ(result.value("stale").toArray().size(), 1);
    EXPECT_EQ(result.value("stale").toArray().first().toObject().value("id").toString(), "K-outdated");
    ASSERT_EQ(result.value("unversioned").toArray().size(), 1);
    EXPECT_EQ(result.value("unversioned").toArray().first().toObject().value("id").toString(), "K-unversioned");
}

TEST(EvaluateKnowledgeNotes, EmptyCurrentHashLeavesVersionedNotesUnevaluatedAsValid) {
    // Without an attached process, we cannot claim a note is stale -- that would be a
    // false contradiction, not an honest "unknown". Only an explicit hash mismatch may
    // move a note to "stale".
    KnowledgeNote versioned;
    versioned.id = "K-1";
    versioned.executableHash = "AAA111";

    const QJsonObject result = evaluateKnowledgeNotes({versioned}, "");
    EXPECT_EQ(result.value("valid").toArray().size(), 1);
    EXPECT_EQ(result.value("stale").toArray().size(), 0);
}

TEST(EvaluateKnowledgeNotes, HashComparisonIsCaseInsensitive) {
    KnowledgeNote note;
    note.id = "K-1";
    note.executableHash = "ABCDEF";
    const QJsonObject result = evaluateKnowledgeNotes({note}, "abcdef");
    EXPECT_EQ(result.value("valid").toArray().size(), 1);
    EXPECT_EQ(result.value("stale").toArray().size(), 0);
}

TEST(ProfileKnowledgeNote, GetKnowledgeNotesReturnsSplitForExistingEntry) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    QString error;
    ASSERT_FALSE(ProfileStore::addKnowledgeNote(&profile, "target", "Health",
        validOptions("recheck", "sans process attaché"), noProcess, &error).isEmpty());

    const QJsonObject result = ProfileStore::getKnowledgeNotes(profile, "target", "Health", noProcess);
    EXPECT_TRUE(result.value("success").toBool());
    EXPECT_FALSE(result.value("versionEvaluated").toBool());
    EXPECT_EQ(result.value("unversioned").toArray().size(), 1);
}

TEST(ProfileKnowledgeNote, GetKnowledgeNotesFailsForUnknownEntry) {
    Profile profile = profileWithOneTarget();
    killcore::ProcessHandle noProcess;
    const QJsonObject result = ProfileStore::getKnowledgeNotes(profile, "target", "DoesNotExist", noProcess);
    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_EQ(result.value("errorCode").toString(), "entry_not_found");
}
