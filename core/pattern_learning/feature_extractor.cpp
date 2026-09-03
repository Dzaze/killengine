#include "feature_extractor.h"
#include <map>
#include <set>

namespace killcore {

// ============================================================================
// FeatureExtractor Implementation
// ============================================================================

ExtractedFeatures FeatureExtractor::extract(const std::vector<double>& values) {
    ExtractedFeatures features;
    
    if (values.empty()) {
        return features;
    }
    
    // Basic statistics
    features.mean = calculateMean(values);
    features.variance = calculateVariance(values, features.mean);
    features.stdDev = std::sqrt(features.variance);
    
    auto [minVal, maxVal] = findMinMax(values);
    features.min = minVal;
    features.max = maxVal;
    features.range = maxVal - minVal;
    
    // Entropy and unique values
    features.entropy = calculateEntropy(values);
    features.uniqueValues = static_cast<int>(std::set<double>(values.begin(), values.end()).size());
    
    // Pattern detection
    features.isMonotonic = detectMonotonicity(values);
    features.periodicity = calculatePeriodicity(values);
    features.isPeriodic = features.periodicity > 0.3;
    
    // Binary detection
    std::set<double> uniqueSet(values.begin(), values.end());
    features.isBinary = uniqueSet.size() <= 2;
    if (features.isBinary) {
        int zeroCount = std::count(values.begin(), values.end(), 0.0);
        features.binaryRatio = static_cast<double>(zeroCount) / values.size();
    }
    
    // Range classification
    features.inSmallRange = features.max <= 100.0;
    features.inMediumRange = features.max > 100.0 && features.max <= 1000.0;
    features.inLargeRange = features.max > 1000.0;
    
    return features;
}

ExtractedFeatures FeatureExtractor::extract(const std::vector<double>& values,
                                              const std::vector<int64_t>& timestamps) {
    auto features = extract(values);
    
    if (values.size() < 2 || timestamps.size() < 2) {
        return features;
    }
    
    // Calculate change rate
    double totalChange = 0.0;
    int64_t totalTime = timestamps.back() - timestamps.front();
    
    for (size_t i = 1; i < values.size(); ++i) {
        totalChange += std::abs(values[i] - values[i-1]);
    }
    
    if (totalTime > 0) {
        features.changeRate = totalChange / totalTime;
    }
    
    // Calculate trend
    features.trend = calculateTrend(values);
    
    return features;
}

double FeatureExtractor::calculateEntropy(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    
    // Count frequency of each value
    std::map<double, int> frequency;
    for (double v : values) {
        frequency[v]++;
    }
    
    // Calculate entropy
    double entropy = 0.0;
    double n = static_cast<double>(values.size());
    
    for (const auto& [value, count] : frequency) {
        double p = count / n;
        if (p > 0) {
            entropy -= p * std::log2(p);
        }
    }
    
    return entropy;
}

double FeatureExtractor::calculateTrend(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    
    // Simple linear regression (least squares)
    double n = static_cast<double>(values.size());
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;
    
    for (size_t i = 0; i < values.size(); ++i) {
        double x = static_cast<double>(i);
        double y = values[i];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }
    
    double denominator = n * sumX2 - sumX * sumX;
    if (std::abs(denominator) < 1e-10) return 0.0;
    
    double slope = (n * sumXY - sumX * sumY) / denominator;
    return slope;
}

double FeatureExtractor::calculatePeriodicity(const std::vector<double>& values) {
    if (values.size() < 4) return 0.0;
    
    // Simple autocorrelation-based periodicity detection
    // Look for repeating patterns
    
    size_t n = values.size();
    double mean = calculateMean(values);
    
    // Calculate autocorrelation for different lags
    std::vector<double> autocorr;
    size_t maxLag = std::min(n / 2, size_t(20));
    
    for (size_t lag = 1; lag <= maxLag; ++lag) {
        double sum = 0.0;
        double count = 0.0;
        
        for (size_t i = 0; i + lag < n; ++i) {
            sum += (values[i] - mean) * (values[i + lag] - mean);
            count++;
        }
        
        autocorr.push_back(sum / count);
    }
    
    // Find peaks in autocorrelation
    if (autocorr.size() < 2) return 0.0;
    
    double maxAutocorr = *std::max_element(autocorr.begin(), autocorr.end());
    double minAutocorr = *std::min_element(autocorr.begin(), autocorr.end());
    
    // Periodicity score based on peak strength
    if (maxAutocorr > 0 && minAutocorr < maxAutocorr) {
        return (maxAutocorr - minAutocorr) / maxAutocorr;
    }
    
    return 0.0;
}

bool FeatureExtractor::detectMonotonicity(const std::vector<double>& values) {
    if (values.size() < 2) return false;
    
    bool increasing = true;
    bool decreasing = true;
    
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] < values[i-1]) increasing = false;
        if (values[i] > values[i-1]) decreasing = false;
    }
    
    return increasing || decreasing;
}

std::vector<double> FeatureExtractor::toFeatureVector(const ExtractedFeatures& features) {
    // Normalize features to [0, 1] range for ML
    std::vector<double> vec;
    
    // Statistical features (normalize by typical ranges)
    vec.push_back(std::min(features.mean / 10000.0, 1.0));
    vec.push_back(std::min(features.variance / 1000000.0, 1.0));
    vec.push_back(std::min(features.stdDev / 1000.0, 1.0));
    vec.push_back(std::min(features.range / 10000.0, 1.0));
    
    // Information features
    vec.push_back(std::min(features.entropy / 10.0, 1.0));
    vec.push_back(std::min(static_cast<double>(features.uniqueValues) / 100.0, 1.0));
    
    // Temporal features
    vec.push_back(std::min(features.changeRate * 1000.0, 1.0));
    vec.push_back(std::min(std::abs(features.trend) * 100.0, 1.0));
    
    // Pattern features (binary)
    vec.push_back(features.isMonotonic ? 1.0 : 0.0);
    vec.push_back(features.isPeriodic ? 1.0 : 0.0);
    vec.push_back(features.periodicity);
    
    // Binary features
    vec.push_back(features.isBinary ? 1.0 : 0.0);
    vec.push_back(features.binaryRatio);
    
    // Range features
    vec.push_back(features.inSmallRange ? 1.0 : 0.0);
    vec.push_back(features.inMediumRange ? 1.0 : 0.0);
    vec.push_back(features.inLargeRange ? 1.0 : 0.0);
    
    return vec;
}

// ============================================================================
// Helper Methods
// ============================================================================

double FeatureExtractor::calculateMean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double FeatureExtractor::calculateVariance(const std::vector<double>& values, double mean) {
    if (values.size() < 2) return 0.0;
    
    double sumSquaredDiff = 0.0;
    for (double v : values) {
        double diff = v - mean;
        sumSquaredDiff += diff * diff;
    }
    
    return sumSquaredDiff / (values.size() - 1);
}

std::pair<double, double> FeatureExtractor::findMinMax(const std::vector<double>& values) {
    if (values.empty()) return {0.0, 0.0};
    
    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    return {*minIt, *maxIt};
}

} // namespace killcore
