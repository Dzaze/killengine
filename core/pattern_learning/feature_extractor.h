#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace killcore {

// ============================================================================
// Feature Extraction for Pattern Learning
// ============================================================================

struct ExtractedFeatures {
    // Statistical features
    double mean = 0.0;
    double variance = 0.0;
    double stdDev = 0.0;
    double min = 0.0;
    double max = 0.0;
    double range = 0.0;
    
    // Entropy and information
    double entropy = 0.0;
    int uniqueValues = 0;
    
    // Temporal features
    double changeRate = 0.0;  // Average change per time unit
    double trend = 0.0;       // Linear trend slope
    
    // Pattern features
    bool isMonotonic = false;
    bool isPeriodic = false;
    double periodicity = 0.0;
    
    // Binary features
    bool isBinary = false;
    double binaryRatio = 0.0;  // Ratio of 0/1 values
    
    // Range features
    bool inSmallRange = false;   // 0-100
    bool inMediumRange = false;  // 0-1000
    bool inLargeRange = false;   // > 1000
};

class FeatureExtractor {
public:
    FeatureExtractor() = default;
    ~FeatureExtractor() = default;

    // Main extraction method
    ExtractedFeatures extract(const std::vector<double>& values);
    
    // Extract with timestamps for temporal features
    ExtractedFeatures extract(const std::vector<double>& values,
                             const std::vector<int64_t>& timestamps);

    // Individual feature calculations
    static double calculateEntropy(const std::vector<double>& values);
    static double calculateTrend(const std::vector<double>& values);
    static double calculatePeriodicity(const std::vector<double>& values);
    static bool detectMonotonicity(const std::vector<double>& values);
    
    // Feature vector for ML (normalized)
    std::vector<double> toFeatureVector(const ExtractedFeatures& features);

private:
    // Helper methods
    static double calculateMean(const std::vector<double>& values);
    static double calculateVariance(const std::vector<double>& values, double mean);
    static std::pair<double, double> findMinMax(const std::vector<double>& values);
};

} // namespace killcore
