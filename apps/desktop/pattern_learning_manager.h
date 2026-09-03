#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <memory>

namespace killcore {
    class PatternLearningEngine;
}

namespace killengine {

// ============================================================================
// Pattern Learning Manager (Qt Bridge)
// ============================================================================

class PatternLearningManager : public QObject {
    Q_OBJECT

public:
    explicit PatternLearningManager(QObject* parent = nullptr);
    ~PatternLearningManager();

    // Initialization
    Q_INVOKABLE bool initialize();
    Q_INVOKABLE bool isInitialized() const;
    Q_INVOKABLE QVariantMap getStatistics() const;

    // Engine Detection
    Q_INVOKABLE QVariantMap detectEngine(const QVariantList& moduleNames,
                                         const QVariantMap& memorySample);

    // Pattern Classification
    Q_INVOKABLE QVariantMap classifyPattern(const QString& addressHex,
                                            const QVariantList& valueHistory,
                                            const QVariantList& timestamps);

    // Profile Management
    Q_INVOKABLE QVariantMap loadProfile(const QString& gameName);
    Q_INVOKABLE bool saveProfile(const QVariantMap& profile);
    Q_INVOKABLE QVariantList listKnownGames();
    Q_INVOKABLE bool deleteProfile(const QString& gameName);

    // Session Recording
    Q_INVOKABLE void recordSession(const QVariantMap& session);
    Q_INVOKABLE QVariantList suggestResolutionPaths(const QString& gameName,
                                                     int targetType);

    // Contextual Suggestions
    Q_INVOKABLE QVariantList suggestValueTypes(int engineType, int patternType);
    Q_INVOKABLE double getTypeSuccessRate(const QString& gameName,
                                          const QString& valueType);

    // Clustering
    Q_INVOKABLE QVariantList clusterAddresses(const QVariantList& addresses,
                                              const QVariantList& features);

    // Real-time pattern tracking
    Q_INVOKABLE void startPatternTracking(const QString& addressHex,
                                          const QString& valueType);
    Q_INVOKABLE void stopPatternTracking(const QString& addressHex);
    Q_INVOKABLE void recordValue(const QString& addressHex, double value);
    Q_INVOKABLE QVariantMap getPatternAnalysis(const QString& addressHex);

    // Batch operations
    Q_INVOKABLE QVariantMap analyzeCandidates(const QVariantList& candidates);
    Q_INVOKABLE QVariantList getTopSuggestions(const QString& gameName,
                                                int patternType,
                                                int count);

signals:
    void engineDetected(const QString& gameName, const QVariantMap& engineInfo);
    void patternClassified(const QString& address, const QVariantMap& classification);
    void suggestionReady(const QString& context, const QStringList& suggestions);
    void trackingUpdated(const QString& addressHex, const QVariantMap& analysis);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killengine
