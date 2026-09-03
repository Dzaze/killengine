#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include "pattern_learning_engine.h"

// Forward declaration for SQLite
struct sqlite3;

namespace killcore {

// ============================================================================
// Game Profile Database
// ============================================================================

class GameProfileDatabase {
public:
    GameProfileDatabase();
    ~GameProfileDatabase();

    // Database lifecycle
    bool open(const std::string& dbPath);
    void close();
    bool isOpen() const;

    // Profile operations
    bool saveProfile(const GameProfile& profile);
    std::optional<GameProfile> loadProfile(const std::string& gameName);
    bool deleteProfile(const std::string& gameName);
    std::vector<std::string> listGames();

    // Session recording
    bool recordSession(const LearningSession& session);
    std::vector<LearningSession> getSessions(const std::string& gameName, int limit = 100);

    // Statistics
    double getSuccessRate(const std::string& gameName, const std::string& valueType);
    int getSessionCount(const std::string& gameName);

    // Maintenance
    bool vacuum();
    bool clearOldSessions(int64_t olderThanTimestamp);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killcore
