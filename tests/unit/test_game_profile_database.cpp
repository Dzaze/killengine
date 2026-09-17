// PROPOSITIONS-1 #2 (Pattern Learning) — tests de GameProfileDatabase après
// réécriture SQLite -> JSON-in-file (02/09/2026, l'original dépendait d'une
// bibliothèque absente du projet, jamais testé ni même compilé avant).

#include "pattern_learning/game_profile_database.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

using killcore::GameProfile;
using killcore::GameProfileDatabase;
using killcore::KnownOffset;
using killcore::LearningSession;
using killcore::PatternClassification;
using killcore::PatternType;
using killcore::EngineType;

namespace {

QString dbPathIn(const QTemporaryDir& dir) {
    return dir.filePath("pattern_learning.json");
}

GameProfile makeProfile(const std::string& name) {
    GameProfile profile;
    profile.gameName = name;
    profile.executableName = name + ".exe";
    profile.engineType = EngineType::Unity;
    profile.engineVersion = "2022.3";
    profile.sessionCount = 1;
    profile.lastUpdated = 1000;

    KnownOffset offset;
    offset.name = "gold";
    offset.offset = 0x1000;
    offset.valueType = "Int32";
    offset.scale = 1.0;
    offset.patternType = PatternType::ResourceCounter;
    offset.stabilityScore = 0.9;
    profile.knownOffsets.push_back(offset);
    profile.successfulPaths.push_back("base+0x10->0x20");
    return profile;
}

} // namespace

TEST(GameProfileDatabaseTest, OpenCreatesEmptyDatabase) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    EXPECT_TRUE(db.open(dbPathIn(dir).toStdString()));
    EXPECT_TRUE(db.isOpen());
    EXPECT_TRUE(db.listGames().empty());
}

TEST(GameProfileDatabaseTest, SaveAndLoadProfileRoundTrips) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    const auto profile = makeProfile("StarCraft2");
    EXPECT_TRUE(db.saveProfile(profile));

    const auto loaded = db.loadProfile("StarCraft2");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->executableName, "StarCraft2.exe");
    EXPECT_EQ(loaded->engineType, EngineType::Unity);
    ASSERT_EQ(loaded->knownOffsets.size(), 1u);
    EXPECT_EQ(loaded->knownOffsets[0].name, "gold");
    EXPECT_EQ(loaded->knownOffsets[0].offset, 0x1000u);
    ASSERT_EQ(loaded->successfulPaths.size(), 1u);
    EXPECT_EQ(loaded->successfulPaths[0], "base+0x10->0x20");
}

TEST(GameProfileDatabaseTest, ProfilePersistsAcrossReopen) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dbPathIn(dir).toStdString();

    {
        GameProfileDatabase db;
        ASSERT_TRUE(db.open(path));
        ASSERT_TRUE(db.saveProfile(makeProfile("Solitaire")));
    }
    {
        GameProfileDatabase db;
        ASSERT_TRUE(db.open(path));
        const auto loaded = db.loadProfile("Solitaire");
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->gameName, "Solitaire");
    }
}

TEST(GameProfileDatabaseTest, LoadUnknownProfileReturnsNullopt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));
    EXPECT_FALSE(db.loadProfile("DoesNotExist").has_value());
}

TEST(GameProfileDatabaseTest, DeleteProfileRemovesIt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));
    ASSERT_TRUE(db.saveProfile(makeProfile("Temp")));

    EXPECT_TRUE(db.deleteProfile("Temp"));
    EXPECT_FALSE(db.loadProfile("Temp").has_value());
    EXPECT_FALSE(db.deleteProfile("Temp")); // Deja supprime : deuxieme suppression echoue proprement.
}

TEST(GameProfileDatabaseTest, ListGamesSortedByLastUpdatedDescending) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    auto older = makeProfile("Older");
    older.lastUpdated = 100;
    auto newer = makeProfile("Newer");
    newer.lastUpdated = 999;

    ASSERT_TRUE(db.saveProfile(older));
    ASSERT_TRUE(db.saveProfile(newer));

    const auto games = db.listGames();
    ASSERT_EQ(games.size(), 2u);
    EXPECT_EQ(games[0], "Newer");
    EXPECT_EQ(games[1], "Older");
}

TEST(GameProfileDatabaseTest, RecordAndRetrieveSessions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    LearningSession session;
    session.sessionId = "s1";
    session.gameName = "StarCraft2";
    session.timestamp = 500;
    session.resolutionPath = "base+0x8";
    session.wasSuccessful = true;
    session.durationMs = 1234;

    PatternClassification pattern;
    pattern.type = PatternType::ResourceCounter;
    pattern.confidence = 0.9;
    pattern.suggestedValueType = "Int32";
    session.discoveredPatterns.push_back(pattern);

    EXPECT_TRUE(db.recordSession(session));
    EXPECT_EQ(db.getSessionCount("StarCraft2"), 1);

    const auto sessions = db.getSessions("StarCraft2", 10);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].sessionId, "s1");
    EXPECT_TRUE(sessions[0].wasSuccessful);
    EXPECT_EQ(sessions[0].durationMs, 1234);
}

TEST(GameProfileDatabaseTest, GetSessionsRespectsLimitAndOrder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    for (int i = 0; i < 5; ++i) {
        LearningSession session;
        session.sessionId = "s" + std::to_string(i);
        session.gameName = "Game";
        session.timestamp = i * 100;
        db.recordSession(session);
    }

    const auto sessions = db.getSessions("Game", 2);
    ASSERT_EQ(sessions.size(), 2u);
    // Plus recent d'abord.
    EXPECT_EQ(sessions[0].sessionId, "s4");
    EXPECT_EQ(sessions[1].sessionId, "s3");
}

TEST(GameProfileDatabaseTest, SuccessRateComputedFromRecordedSessions) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    PatternClassification pattern;
    pattern.suggestedValueType = "Int32";

    LearningSession success;
    success.gameName = "Game";
    success.wasSuccessful = true;
    success.discoveredPatterns.push_back(pattern);
    db.recordSession(success);

    LearningSession failure;
    failure.gameName = "Game";
    failure.wasSuccessful = false;
    failure.discoveredPatterns.push_back(pattern);
    db.recordSession(failure);

    EXPECT_DOUBLE_EQ(db.getSuccessRate("Game", "Int32"), 0.5);
}

TEST(GameProfileDatabaseTest, SuccessRateUnknownTypeReturnsZero) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));
    EXPECT_DOUBLE_EQ(db.getSuccessRate("Game", "Unknown"), 0.0);
}

TEST(GameProfileDatabaseTest, ClearOldSessionsRemovesOnlyOlderOnes) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GameProfileDatabase db;
    ASSERT_TRUE(db.open(dbPathIn(dir).toStdString()));

    LearningSession oldSession;
    oldSession.sessionId = "old";
    oldSession.gameName = "Game";
    oldSession.timestamp = 100;
    db.recordSession(oldSession);

    LearningSession newSession;
    newSession.sessionId = "new";
    newSession.gameName = "Game";
    newSession.timestamp = 999999;
    db.recordSession(newSession);

    EXPECT_TRUE(db.clearOldSessions(1000));
    EXPECT_EQ(db.getSessionCount("Game"), 1);
    const auto remaining = db.getSessions("Game", 10);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0].sessionId, "new");
}

TEST(GameProfileDatabaseTest, OperationsFailWhenNotOpen) {
    GameProfileDatabase db;
    EXPECT_FALSE(db.isOpen());
    EXPECT_FALSE(db.saveProfile(makeProfile("X")));
    EXPECT_FALSE(db.loadProfile("X").has_value());
    EXPECT_FALSE(db.recordSession(LearningSession{}));
    EXPECT_TRUE(db.listGames().empty());
}

// PORT-3c (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : un JSON illisible ne
// doit jamais être silencieusement remplacé par une base vide qui masquerait
// la corruption -- open() doit échouer, pas réussir avec 0 profil.
TEST(GameProfileDatabaseTest, OpenFailsOnCorruptJsonInsteadOfSilentlyEmptyBase) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dbPathIn(dir);

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("{ this is not valid JSON");
    file.close();

    GameProfileDatabase db;
    EXPECT_FALSE(db.open(path.toStdString()));
    EXPECT_FALSE(db.isOpen());
}

// PORT-3c : un refus d'écriture (simulé par un fichier en lecture seule,
// jamais un vrai remplissage de disque ni un changement des droits d'un
// dossier utilisateur partagé) ne doit ni annoncer un faux succès ni
// endommager la dernière version valide sur disque.
TEST(GameProfileDatabaseTest, SaveFailureLeavesPreviousValidFileIntact) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dbPathIn(dir);

    GameProfileDatabase db;
    ASSERT_TRUE(db.open(path.toStdString()));
    ASSERT_TRUE(db.saveProfile(makeProfile("SurvivingGame")));

    ASSERT_TRUE(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                            | QFileDevice::ReadGroup | QFileDevice::ReadOther));

    const bool saveSucceeded = db.saveProfile(makeProfile("ShouldNeverPersist"));

    // Restaure les droits avant toute assertion supplémentaire : ne jamais
    // laisser un fichier en lecture seule empêcher le nettoyage de
    // QTemporaryDir, même si une assertion ci-dessous échoue.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ReadUser | QFileDevice::WriteUser
                                | QFileDevice::ReadGroup | QFileDevice::ReadOther);

    EXPECT_FALSE(saveSucceeded);

    GameProfileDatabase reopened;
    ASSERT_TRUE(reopened.open(path.toStdString()));
    const auto surviving = reopened.loadProfile("SurvivingGame");
    ASSERT_TRUE(surviving.has_value());
    EXPECT_EQ(surviving->gameName, "SurvivingGame");
    EXPECT_FALSE(reopened.loadProfile("ShouldNeverPersist").has_value());
}
