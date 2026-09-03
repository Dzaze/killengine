#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <QObject>
#include <QVariantMap>
#include <QVariantList>

namespace killcore {

// Forward declarations
class GameProfileDatabase;
class FeatureExtractor;

// ============================================================================
// Engine Detection Result
// ============================================================================

enum class EngineType {
    Unknown,
    Unity,
    UnrealEngine,
    Godot,
    Custom
};

struct EngineDetectionResult {
    EngineType type = EngineType::Unknown;
    std::string version;
    double confidence = 0.0;
    std::string signature;
    
    QVariantMap toVariantMap() const;
    static EngineDetectionResult fromVariantMap(const QVariantMap& map);
};

// ============================================================================
// Pattern Classification
// ============================================================================

enum class PatternType {
    Unknown,
    ResourceCounter,      // Ressources (minerals, gold, etc.)
    HealthPool,           // Santé/vie
    StateFlag,            // États binaires (ON/OFF)
    Timer,                // Compteurs temps
    Coordinate,           // Position X/Y/Z
    UIReference,          // Référence UI (affichage seul)
    Noise                 // Bruit à ignorer
};

struct PatternClassification {
    PatternType type = PatternType::Unknown;
    double confidence = 0.0;
    std::string suggestedValueType;
    double suggestedScale = 1.0;
    std::vector<std::string> reasoning;
    
    QVariantMap toVariantMap() const;
};

// ============================================================================
// Game Profile
// ============================================================================

struct KnownOffset {
    std::string name;
    uint64_t offset = 0;
    std::string valueType;
    double scale = 1.0;
    PatternType patternType = PatternType::Unknown;
    double stabilityScore = 0.0;
};

struct GameProfile {
    std::string gameName;
    std::string executableName;
    EngineType engineType = EngineType::Unknown;
    std::string engineVersion;
    std::vector<KnownOffset> knownOffsets;
    std::vector<std::string> successfulPaths;
    int sessionCount = 0;
    int64_t lastUpdated = 0;
    
    QVariantMap toVariantMap() const;
    static std::optional<GameProfile> fromVariantMap(const QVariantMap& map);
};

// ============================================================================
// Learning Session
// ============================================================================

struct LearningSession {
    std::string sessionId;
    std::string gameName;
    int64_t timestamp = 0;
    std::vector<PatternClassification> discoveredPatterns;
    std::string resolutionPath;
    bool wasSuccessful = false;
    int64_t durationMs = 0;
};

// ============================================================================
// Pattern Learning Engine
// ============================================================================

class PatternLearningEngine : public QObject {
    Q_OBJECT

public:
    explicit PatternLearningEngine(QObject* parent = nullptr);
    ~PatternLearningEngine();

    // Initialization
    bool initialize(const std::string& dbPath);
    bool isInitialized() const;

    // Engine Detection
    EngineDetectionResult detectEngine(const std::vector<std::string>& moduleNames,
                                       const std::vector<uint8_t>& memorySample);

    // Pattern Classification
    PatternClassification classifyPattern(uint64_t address,
                                          const std::vector<double>& valueHistory,
                                          const std::vector<int64_t>& timestamps);

    // Profile Management
    std::optional<GameProfile> loadProfile(const std::string& gameName);
    bool saveProfile(const GameProfile& profile);
    std::vector<std::string> listKnownGames() const;
    bool deleteProfile(const std::string& gameName);

    // Learning from Sessions
    void recordSession(const LearningSession& session);
    std::vector<std::string> suggestResolutionPaths(const std::string& gameName,
                                                   PatternType targetType);

    // Contextual Suggestions
    std::vector<std::string> suggestValueTypes(EngineType engine, PatternType pattern);
    double getTypeSuccessRate(const std::string& gameName, 
                              const std::string& valueType);

    // Clustering
    struct Cluster {
        int id = 0;
        std::string label;
        std::vector<uint64_t> addresses;
        PatternType dominantType = PatternType::Unknown;
    };
    std::vector<Cluster> clusterAddresses(const std::vector<uint64_t>& addresses,
                                          const std::vector<std::vector<double>>& features);

    // Statistics
    QVariantMap getStatistics() const;

signals:
    void engineDetected(const QString& gameName, const QVariantMap& engineInfo);
    void patternClassified(const QString& address, const QVariantMap& classification);
    void suggestionReady(const QString& context, const QStringList& suggestions);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killcore
