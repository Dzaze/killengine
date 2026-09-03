#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <functional>

namespace killcore {

/**
 * @brief Point de données temporel pour une adresse mémoire
 */
struct TimelineDataPoint {
    uint64_t timestampMs;           // Timestamp relatif au début de la capture
    uint64_t address;               // Adresse mémoire
    std::vector<uint8_t> value;     // Valeur brute (taille variable selon le type)
    bool isValid;                   // La lecture a réussi
    
    TimelineDataPoint() : timestampMs(0), address(0), isValid(false) {}
};

/**
 * @brief Série temporelle complète pour une adresse
 */
struct TimelineSeries {
    uint64_t baseAddress;
    size_t valueSize;
    std::vector<TimelineDataPoint> points;
    
    // Statistiques calculées
    uint64_t firstChangeMs;
    uint64_t lastChangeMs;
    uint32_t changeCount;
    double averageIntervalMs;
    double volatilityScore;         // 0.0 = stable, 1.0 = très volatile
    
    TimelineSeries() : baseAddress(0), valueSize(0), firstChangeMs(0), 
                       lastChangeMs(0), changeCount(0), averageIntervalMs(0), 
                       volatilityScore(0.0) {}
};

/**
 * @brief Configuration de la collecte de timeline
 */
struct TimelineCollectorConfig {
    uint32_t samplingIntervalMs;    // Intervalle entre les échantillons
    uint32_t maxDurationMs;         // Durée max de la capture
    size_t maxPointsPerSeries;      // Limite pour éviter la saturation mémoire
    bool trackOnlyChanges;          // Ne stocker que les valeurs qui changent
    bool calculateStatistics;       // Calculer les stats en temps réel
    
    TimelineCollectorConfig() 
        : samplingIntervalMs(100)
        , maxDurationMs(60000)
        , maxPointsPerSeries(10000)
        , trackOnlyChanges(true)
        , calculateStatistics(true) {}
};

/**
 * @brief Collecteur de données temporelles mémoire
 * 
 * Capture l'évolution des valeurs mémoire au fil du temps
 * pour détecter des patterns, des cycles, ou des corrélations.
 */
class MemoryTimelineCollector {
public:
    using ProgressCallback = std::function<void(uint32_t percent, const std::string& status)>;
    using DataCallback = std::function<void(const TimelineDataPoint& point)>;

    MemoryTimelineCollector();
    ~MemoryTimelineCollector();

    // Configuration
    void setConfig(const TimelineCollectorConfig& config);
    TimelineCollectorConfig config() const;

    // Gestion des adresses à surveiller
    void addAddress(uint64_t address, size_t valueSize);
    void removeAddress(uint64_t address);
    void clearAddresses();
    std::vector<uint64_t> watchedAddresses() const;

    // Contrôle de la collecte
    bool startCollection(void* processHandle);
    void stopCollection();
    bool isCollecting() const;

    // Callbacks
    void setProgressCallback(ProgressCallback callback);
    void setDataCallback(DataCallback callback);

    // Résultats
    std::unordered_map<uint64_t, TimelineSeries> getSeries() const;
    TimelineSeries getSeriesForAddress(uint64_t address) const;
    
    // Export
    bool exportToJson(const std::string& filepath) const;
    bool exportToCsv(const std::string& filepath) const;

    // Analyse rapide
    std::vector<uint64_t> findVolatileAddresses(double threshold = 0.5) const;
    std::vector<uint64_t> findStableAddresses(uint32_t minDurationMs = 5000) const;
    std::vector<uint64_t> findCyclicalAddresses(double correlationThreshold = 0.7) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killcore
