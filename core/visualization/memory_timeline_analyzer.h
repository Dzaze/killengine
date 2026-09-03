#pragma once

#include "memory_timeline_collector.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

namespace killcore {

/**
 * @brief Pattern détecté dans une série temporelle
 */
struct TimelinePattern {
    enum class Type {
        Unknown,
        Constant,           // Valeur fixe
        StepFunction,       // Changements abrupts
        Linear,             // Progression linéaire
        Cyclic,             // Pattern répétitif
        RandomWalk,         // Marche aléatoire
        Correlated,         // Corrélé avec une autre série
        AntiCheatPattern    // Pattern suspect (anti-cheat)
    };

    Type type;
    uint64_t address;
    double confidence;      // 0.0 - 1.0
    std::string description;
    
    // Métriques spécifiques au pattern
    double correlationScore;
    uint32_t periodMs;      // Pour les patterns cycliques
    double slope;           // Pour les patterns linéaires
    
    TimelinePattern() 
        : type(Type::Unknown)
        , address(0)
        , confidence(0.0)
        , correlationScore(0.0)
        , periodMs(0)
        , slope(0.0) {}
};

/**
 * @brief Corrélation entre deux séries
 */
struct TimelineCorrelation {
    uint64_t addressA;
    uint64_t addressB;
    double pearsonCoefficient;
    double timeLagMs;       // Décalage temporel optimal
    bool isLeading;         // addressA mène addressB
    
    TimelineCorrelation()
        : addressA(0)
        , addressB(0)
        , pearsonCoefficient(0.0)
        , timeLagMs(0.0)
        , isLeading(false) {}
};

/**
 * @brief Analyse comportementale d'une adresse
 */
struct AddressBehaviorProfile {
    uint64_t address;
    
    // Fréquence
    double changesPerSecond;
    double regularityScore;     // 0-1, 1 = très régulier
    
    // Valeurs
    std::vector<uint8_t> minValue;
    std::vector<uint8_t> maxValue;
    std::vector<uint8_t> mostCommonValue;
    uint32_t distinctValueCount;
    
    // Timing
    uint32_t typicalResponseTimeMs;  // Temps de réaction typique
    bool hasBurstBehavior;           // Pics d'activité
    
    AddressBehaviorProfile()
        : address(0)
        , changesPerSecond(0.0)
        , regularityScore(0.0)
        , distinctValueCount(0)
        , typicalResponseTimeMs(0)
        , hasBurstBehavior(false) {}
};

/**
 * @brief Analyseur de séries temporelles mémoire
 * 
 * Fournit des analyses avancées : détection de patterns,
 * corrélations, et profils comportementaux.
 */
class MemoryTimelineAnalyzer {
public:
    MemoryTimelineAnalyzer();
    ~MemoryTimelineAnalyzer();

    // Analyse de patterns
    std::vector<TimelinePattern> detectPatterns(
        const TimelineSeries& series,
        double minConfidence = 0.7
    ) const;

    TimelinePattern detectDominantPattern(
        const TimelineSeries& series
    ) const;

    // Analyse de corrélations
    std::vector<TimelineCorrelation> findCorrelations(
        const std::vector<TimelineSeries>& seriesList,
        double minCorrelation = 0.8
    ) const;

    std::vector<TimelineCorrelation> findCorrelationsWithLag(
        const TimelineSeries& seriesA,
        const TimelineSeries& seriesB,
        int32_t maxLagMs = 1000
    ) const;

    // Profils comportementaux
    AddressBehaviorProfile createBehaviorProfile(
        const TimelineSeries& series
    ) const;

    // Détection anti-cheat
    std::vector<TimelinePattern> detectAntiCheatPatterns(
        const TimelineSeries& series
    ) const;

    bool isLikelyIntegrityCheck(const TimelineSeries& series) const;
    bool isLikelyEncryptedValue(const TimelineSeries& series) const;
    bool isLikelyPointerChasing(const TimelineSeries& series) const;

    // Prédiction
    std::vector<uint8_t> predictNextValue(
        const TimelineSeries& series,
        uint32_t horizonMs = 100
    ) const;

    double predictChangeProbability(
        const TimelineSeries& series,
        uint32_t horizonMs = 100
    ) const;

    // Analyse comparative
    double calculateSimilarity(
        const TimelineSeries& seriesA,
        const TimelineSeries& seriesB
    ) const;

    std::vector<std::pair<uint64_t, double>> findSimilarSeries(
        const TimelineSeries& reference,
        const std::vector<TimelineSeries>& candidates,
        double minSimilarity = 0.8
    ) const;

    // Export d'analyse
    std::string generateAnalysisReport(
        const std::vector<TimelineSeries>& seriesList
    ) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    // Méthodes internes d'analyse
    double calculatePearsonCorrelation(
        const std::vector<double>& valuesA,
        const std::vector<double>& valuesB
    ) const;

    std::vector<double> extractNumericValues(
        const TimelineSeries& series
    ) const;

    bool detectCyclicPattern(
        const TimelineSeries& series,
        uint32_t& outPeriodMs,
        double& outConfidence
    ) const;

    bool detectLinearTrend(
        const TimelineSeries& series,
        double& outSlope,
        double& outConfidence
    ) const;
};

} // namespace killcore
