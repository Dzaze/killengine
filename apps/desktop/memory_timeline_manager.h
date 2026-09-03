#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <memory>
#include <functional>

namespace killcore {
    class MemoryTimelineCollector;
    class MemoryTimelineAnalyzer;
    struct TimelineSeries;
}

namespace killengine {

/**
 * @brief Manager Qt pour le Memory Timeline Visualizer
 * 
 * Pont entre le core C++ et l'UI Vue pour la visualisation
 * temporelle des données mémoire.
 */
class MemoryTimelineManager : public QObject {
    Q_OBJECT

    // Propriétés exposées à QML/Vue
    Q_PROPERTY(bool isCollecting READ isCollecting NOTIFY collectingChanged)
    Q_PROPERTY(int watchedAddressCount READ watchedAddressCount NOTIFY addressesChanged)
    Q_PROPERTY(QVariantMap currentStats READ currentStats NOTIFY statsChanged)

public:
    explicit MemoryTimelineManager(QObject* parent = nullptr);
    ~MemoryTimelineManager();

    // Getters pour propriétés
    bool isCollecting() const;
    int watchedAddressCount() const;
    QVariantMap currentStats() const;

    // Gestion des adresses
    Q_INVOKABLE bool addAddress(const QString& addressHex, int valueSize);
    Q_INVOKABLE bool removeAddress(const QString& addressHex);
    Q_INVOKABLE void clearAddresses();
    Q_INVOKABLE QVariantList getWatchedAddresses() const;

    // Contrôle de la collecte
    Q_INVOKABLE bool startCollection();
    Q_INVOKABLE void stopCollection();
    Q_INVOKABLE void pauseCollection();
    Q_INVOKABLE void resumeCollection();

    // Configuration
    Q_INVOKABLE void setSamplingInterval(int intervalMs);
    Q_INVOKABLE void setMaxDuration(int durationMs);
    Q_INVOKABLE void setTrackOnlyChanges(bool trackOnly);
    Q_INVOKABLE QVariantMap getConfig() const;

    // Résultats
    Q_INVOKABLE QVariantMap getSeriesForAddress(const QString& addressHex) const;
    Q_INVOKABLE QVariantList getAllSeries() const;
    Q_INVOKABLE QVariantMap getSeriesStats(const QString& addressHex) const;

    // Analyses simples / corrélations rapides.
    Q_INVOKABLE QVariantList findCorrelations();
    Q_INVOKABLE QVariantList findVolatileAddresses(double threshold);
    Q_INVOKABLE QVariantList findStableAddresses(int minDurationMs);

    // Export
    Q_INVOKABLE bool exportToJson(const QString& filepath);
    Q_INVOKABLE bool exportToCsv(const QString& filepath);

    // Analyses avancées portées par MemoryTimelineAnalyzer.
    Q_INVOKABLE QVariantMap detectPatterns(const QString& addressHex);
    Q_INVOKABLE QVariantMap analyzeBehavior(const QString& addressHex);
    Q_INVOKABLE QVariantMap predictNextValue(const QString& addressHex);
    Q_INVOKABLE QVariantMap generateReport();

    // Définition du process handle (appelé par ApplicationController)
    void setProcessHandle(void* handle);
    void* processHandle() const;

signals:
    void collectingChanged();
    void addressesChanged();
    void statsChanged();
    void dataPointReceived(const QString& addressHex, const QVariantMap& pointData);
    void collectionProgress(int percent, const QString& status);
    void collectionFinished();
    void patternDetected(const QString& addressHex, const QVariantMap& patternData);
    void correlationFound(const QVariantMap& correlationData);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    void setupCallbacks();
    QVariantMap seriesToVariantMap(const killcore::TimelineSeries& series) const;
};

} // namespace killengine
