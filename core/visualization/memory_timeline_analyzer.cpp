#include "memory_timeline_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace killcore {
namespace {

constexpr double kEpsilon = 0.000001;

uint64_t decodeLittleEndian(const std::vector<uint8_t>& bytes) {
    uint64_t value = 0;
    const size_t count = std::min<size_t>(bytes.size(), sizeof(uint64_t));
    for (size_t i = 0; i < count; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

std::vector<uint8_t> encodeLittleEndian(uint64_t value, size_t size) {
    std::vector<uint8_t> bytes(std::min<size_t>(size, sizeof(uint64_t)), 0);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
    }
    return bytes;
}

bool sameBytes(const TimelineDataPoint& a, const TimelineDataPoint& b) {
    return a.isValid == b.isValid && a.value == b.value;
}

std::vector<const TimelineDataPoint*> validPoints(const TimelineSeries& series) {
    std::vector<const TimelineDataPoint*> points;
    points.reserve(series.points.size());
    for (const auto& point : series.points) {
        if (point.isValid && !point.value.empty() && point.value.size() <= sizeof(uint64_t)) {
            points.push_back(&point);
        }
    }
    return points;
}

std::vector<uint64_t> changeIntervals(const TimelineSeries& series) {
    std::vector<uint64_t> intervals;
    const auto points = validPoints(series);
    if (points.size() < 2) {
        return intervals;
    }

    for (size_t i = 1; i < points.size(); ++i) {
        if (!sameBytes(*points[i - 1], *points[i])) {
            intervals.push_back(points[i]->timestampMs - points[i - 1]->timestampMs);
        }
    }
    return intervals;
}

double mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double stddev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double avg = mean(values);
    double variance = 0.0;
    for (const double value : values) {
        const double diff = value - avg;
        variance += diff * diff;
    }
    variance /= static_cast<double>(values.size());
    return std::sqrt(variance);
}

std::string typeToString(TimelinePattern::Type type) {
    switch (type) {
        case TimelinePattern::Type::Constant: return "constant";
        case TimelinePattern::Type::StepFunction: return "step_function";
        case TimelinePattern::Type::Linear: return "linear";
        case TimelinePattern::Type::Cyclic: return "cyclic";
        case TimelinePattern::Type::RandomWalk: return "random_walk";
        case TimelinePattern::Type::Correlated: return "correlated";
        case TimelinePattern::Type::AntiCheatPattern: return "anti_cheat_pattern";
        case TimelinePattern::Type::Unknown:
        default: return "unknown";
    }
}

TimelinePattern makePattern(
    TimelinePattern::Type type,
    uint64_t address,
    double confidence,
    const std::string& description) {
    TimelinePattern pattern;
    pattern.type = type;
    pattern.address = address;
    pattern.confidence = std::clamp(confidence, 0.0, 1.0);
    pattern.description = description;
    return pattern;
}

} // namespace

class MemoryTimelineAnalyzer::Impl {
public:
    static std::vector<double> timestampsAsDouble(const TimelineSeries& series) {
        std::vector<double> timestamps;
        for (const auto* point : validPoints(series)) {
            timestamps.push_back(static_cast<double>(point->timestampMs));
        }
        return timestamps;
    }
};

MemoryTimelineAnalyzer::MemoryTimelineAnalyzer()
    : m_impl(std::make_unique<Impl>()) {
}

MemoryTimelineAnalyzer::~MemoryTimelineAnalyzer() = default;

std::vector<TimelinePattern> MemoryTimelineAnalyzer::detectPatterns(
    const TimelineSeries& series,
    double minConfidence) const {
    std::vector<TimelinePattern> patterns;

    const auto dominant = detectDominantPattern(series);
    if (dominant.confidence >= minConfidence) {
        patterns.push_back(dominant);
    }

    for (auto pattern : detectAntiCheatPatterns(series)) {
        if (pattern.confidence >= minConfidence) {
            patterns.push_back(std::move(pattern));
        }
    }

    return patterns;
}

TimelinePattern MemoryTimelineAnalyzer::detectDominantPattern(const TimelineSeries& series) const {
    const auto values = extractNumericValues(series);
    if (values.size() < 2) {
        return makePattern(TimelinePattern::Type::Unknown, series.baseAddress, 0.0, "not enough valid points");
    }

    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    if (std::abs(*maxIt - *minIt) <= kEpsilon) {
        return makePattern(TimelinePattern::Type::Constant, series.baseAddress, 1.0, "value stays constant");
    }

    uint32_t periodMs = 0;
    double cyclicConfidence = 0.0;
    if (detectCyclicPattern(series, periodMs, cyclicConfidence) && cyclicConfidence >= 0.72) {
        auto pattern = makePattern(TimelinePattern::Type::Cyclic, series.baseAddress, cyclicConfidence, "repeating value sequence");
        pattern.periodMs = periodMs;
        return pattern;
    }

    double slope = 0.0;
    double linearConfidence = 0.0;
    if (detectLinearTrend(series, slope, linearConfidence) && linearConfidence >= 0.70) {
        auto pattern = makePattern(TimelinePattern::Type::Linear, series.baseAddress, linearConfidence, "linear trend");
        pattern.slope = slope;
        return pattern;
    }

    std::set<double> distinct(values.begin(), values.end());
    size_t changes = 0;
    for (size_t i = 1; i < values.size(); ++i) {
        if (std::abs(values[i] - values[i - 1]) > kEpsilon) {
            ++changes;
        }
    }
    if (distinct.size() <= std::max<size_t>(3, values.size() / 4) && changes <= values.size() / 2) {
        return makePattern(TimelinePattern::Type::StepFunction, series.baseAddress, 0.75, "small number of abrupt value levels");
    }

    const double volatility = stddev(values) / std::max(1.0, std::abs(mean(values)));
    const double confidence = std::clamp(volatility, 0.35, 1.0);
    return makePattern(TimelinePattern::Type::RandomWalk, series.baseAddress, confidence, "irregular value changes");
}

std::vector<TimelineCorrelation> MemoryTimelineAnalyzer::findCorrelations(
    const std::vector<TimelineSeries>& seriesList,
    double minCorrelation) const {
    std::vector<TimelineCorrelation> correlations;
    for (size_t i = 0; i < seriesList.size(); ++i) {
        for (size_t j = i + 1; j < seriesList.size(); ++j) {
            const auto valuesA = extractNumericValues(seriesList[i]);
            const auto valuesB = extractNumericValues(seriesList[j]);
            const size_t count = std::min(valuesA.size(), valuesB.size());
            if (count < 3) {
                continue;
            }

            std::vector<double> a(valuesA.begin(), valuesA.begin() + static_cast<std::ptrdiff_t>(count));
            std::vector<double> b(valuesB.begin(), valuesB.begin() + static_cast<std::ptrdiff_t>(count));
            const double coefficient = calculatePearsonCorrelation(a, b);
            if (std::abs(coefficient) >= minCorrelation) {
                TimelineCorrelation correlation;
                correlation.addressA = seriesList[i].baseAddress;
                correlation.addressB = seriesList[j].baseAddress;
                correlation.pearsonCoefficient = coefficient;
                correlations.push_back(correlation);
            }
        }
    }
    return correlations;
}

std::vector<TimelineCorrelation> MemoryTimelineAnalyzer::findCorrelationsWithLag(
    const TimelineSeries& seriesA,
    const TimelineSeries& seriesB,
    int32_t maxLagMs) const {
    std::vector<TimelineCorrelation> correlations;
    const auto valuesA = extractNumericValues(seriesA);
    const auto valuesB = extractNumericValues(seriesB);
    const auto timestampsA = Impl::timestampsAsDouble(seriesA);
    const auto timestampsB = Impl::timestampsAsDouble(seriesB);
    const size_t count = std::min(valuesA.size(), valuesB.size());
    if (count < 3) {
        return correlations;
    }

    const int maxLagPoints = static_cast<int>(std::min<size_t>(count / 2, 16));
    for (int lag = -maxLagPoints; lag <= maxLagPoints; ++lag) {
        std::vector<double> a;
        std::vector<double> b;
        std::vector<double> lagSamples;
        for (size_t i = 0; i < count; ++i) {
            const int j = static_cast<int>(i) + lag;
            if (j < 0 || j >= static_cast<int>(count)) {
                continue;
            }
            const double lagMs = timestampsB[static_cast<size_t>(j)] - timestampsA[i];
            if (std::abs(lagMs) > static_cast<double>(maxLagMs)) {
                continue;
            }
            a.push_back(valuesA[i]);
            b.push_back(valuesB[static_cast<size_t>(j)]);
            lagSamples.push_back(lagMs);
        }
        if (a.size() < 3) {
            continue;
        }

        const double coefficient = calculatePearsonCorrelation(a, b);
        if (std::abs(coefficient) >= 0.8) {
            TimelineCorrelation correlation;
            correlation.addressA = seriesA.baseAddress;
            correlation.addressB = seriesB.baseAddress;
            correlation.pearsonCoefficient = coefficient;
            correlation.timeLagMs = mean(lagSamples);
            correlation.isLeading = correlation.timeLagMs > 0.0;
            correlations.push_back(correlation);
        }
    }

    std::sort(correlations.begin(), correlations.end(), [](const auto& a, const auto& b) {
        return std::abs(a.pearsonCoefficient) > std::abs(b.pearsonCoefficient);
    });
    return correlations;
}

AddressBehaviorProfile MemoryTimelineAnalyzer::createBehaviorProfile(const TimelineSeries& series) const {
    AddressBehaviorProfile profile;
    profile.address = series.baseAddress;

    const auto points = validPoints(series);
    if (points.empty()) {
        return profile;
    }

    auto minPoint = points.front();
    auto maxPoint = points.front();
    std::map<std::vector<uint8_t>, uint32_t> counts;
    for (const auto* point : points) {
        if (decodeLittleEndian(point->value) < decodeLittleEndian(minPoint->value)) {
            minPoint = point;
        }
        if (decodeLittleEndian(point->value) > decodeLittleEndian(maxPoint->value)) {
            maxPoint = point;
        }
        ++counts[point->value];
    }
    profile.minValue = minPoint->value;
    profile.maxValue = maxPoint->value;
    profile.distinctValueCount = static_cast<uint32_t>(counts.size());
    profile.mostCommonValue = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    })->first;

    const auto intervals64 = changeIntervals(series);
    if (!intervals64.empty()) {
        std::vector<double> intervals;
        intervals.reserve(intervals64.size());
        for (const auto interval : intervals64) {
            intervals.push_back(static_cast<double>(interval));
        }

        const double durationSeconds = std::max(
            0.001,
            static_cast<double>(points.back()->timestampMs - points.front()->timestampMs) / 1000.0);
        profile.changesPerSecond = static_cast<double>(intervals.size()) / durationSeconds;

        const double avgInterval = mean(intervals);
        const double intervalStdDev = stddev(intervals);
        profile.regularityScore = avgInterval > 0.0
            ? std::clamp(1.0 - (intervalStdDev / avgInterval), 0.0, 1.0)
            : 0.0;
        profile.typicalResponseTimeMs = static_cast<uint32_t>(std::round(avgInterval));
        const auto minInterval = *std::min_element(intervals.begin(), intervals.end());
        profile.hasBurstBehavior = avgInterval > 0.0 && minInterval < avgInterval * 0.35 && intervals.size() >= 4;
    }

    return profile;
}

std::vector<TimelinePattern> MemoryTimelineAnalyzer::detectAntiCheatPatterns(const TimelineSeries& series) const {
    std::vector<TimelinePattern> patterns;
    if (isLikelyIntegrityCheck(series)) {
        patterns.push_back(makePattern(
            TimelinePattern::Type::AntiCheatPattern,
            series.baseAddress,
            0.78,
            "regular checksum-like updates"));
    }
    if (isLikelyEncryptedValue(series)) {
        patterns.push_back(makePattern(
            TimelinePattern::Type::AntiCheatPattern,
            series.baseAddress,
            0.74,
            "high-entropy value changes"));
    }
    if (isLikelyPointerChasing(series)) {
        patterns.push_back(makePattern(
            TimelinePattern::Type::AntiCheatPattern,
            series.baseAddress,
            0.70,
            "pointer-like address churn"));
    }
    return patterns;
}

bool MemoryTimelineAnalyzer::isLikelyIntegrityCheck(const TimelineSeries& series) const {
    const auto intervals64 = changeIntervals(series);
    if (intervals64.size() < 5 || (series.valueSize != 4 && series.valueSize != 8)) {
        return false;
    }
    std::vector<double> intervals;
    for (const auto interval : intervals64) {
        intervals.push_back(static_cast<double>(interval));
    }
    const double avg = mean(intervals);
    return avg > 0.0 && (stddev(intervals) / avg) < 0.20;
}

bool MemoryTimelineAnalyzer::isLikelyEncryptedValue(const TimelineSeries& series) const {
    const auto values = extractNumericValues(series);
    if (values.size() < 8) {
        return false;
    }
    std::set<double> distinct(values.begin(), values.end());
    if (distinct.size() < values.size() * 0.75) {
        return false;
    }

    std::vector<double> deltas;
    for (size_t i = 1; i < values.size(); ++i) {
        deltas.push_back(std::abs(values[i] - values[i - 1]));
    }
    return mean(deltas) > std::max(32.0, std::abs(mean(values)) * 0.10);
}

bool MemoryTimelineAnalyzer::isLikelyPointerChasing(const TimelineSeries& series) const {
    if (series.valueSize != 8) {
        return false;
    }
    const auto values = extractNumericValues(series);
    if (values.size() < 5) {
        return false;
    }

    size_t pointerLike = 0;
    std::set<double> distinct(values.begin(), values.end());
    for (const auto value : values) {
        if (value >= 0x10000 && value <= 0x00007fffffffffffULL) {
            ++pointerLike;
        }
    }
    return pointerLike >= values.size() * 0.8 && distinct.size() >= values.size() / 2;
}

std::vector<uint8_t> MemoryTimelineAnalyzer::predictNextValue(
    const TimelineSeries& series,
    uint32_t horizonMs) const {
    const auto values = extractNumericValues(series);
    const auto points = validPoints(series);
    if (values.empty() || points.empty()) {
        return {};
    }
    if (values.size() == 1) {
        return points.back()->value;
    }

    double slope = 0.0;
    double confidence = 0.0;
    double predicted = values.back();
    if (detectLinearTrend(series, slope, confidence) && confidence >= 0.60) {
        predicted = values.back() + slope * static_cast<double>(horizonMs);
    } else {
        uint32_t periodMs = 0;
        double cyclicConfidence = 0.0;
        if (detectCyclicPattern(series, periodMs, cyclicConfidence) && cyclicConfidence >= 0.70 && periodMs > 0) {
            const uint64_t targetTime = points.back()->timestampMs + horizonMs;
            const uint64_t relative = targetTime % periodMs;
            const auto best = std::min_element(points.begin(), points.end(), [relative, periodMs](const auto* a, const auto* b) {
                const auto da = std::abs(static_cast<int64_t>(a->timestampMs % periodMs) - static_cast<int64_t>(relative));
                const auto db = std::abs(static_cast<int64_t>(b->timestampMs % periodMs) - static_cast<int64_t>(relative));
                return da < db;
            });
            if (best != points.end()) {
                return (*best)->value;
            }
        }
    }

    const double maxForSize = series.valueSize >= sizeof(uint64_t)
        ? static_cast<double>(std::numeric_limits<uint64_t>::max())
        : static_cast<double>((uint64_t{1} << (series.valueSize * 8)) - 1);
    const uint64_t encoded = static_cast<uint64_t>(std::llround(std::clamp(predicted, 0.0, maxForSize)));
    return encodeLittleEndian(encoded, series.valueSize);
}

double MemoryTimelineAnalyzer::predictChangeProbability(
    const TimelineSeries& series,
    uint32_t horizonMs) const {
    const auto intervals64 = changeIntervals(series);
    if (intervals64.empty()) {
        return 0.0;
    }
    std::vector<double> intervals;
    for (const auto interval : intervals64) {
        intervals.push_back(static_cast<double>(interval));
    }
    const double avgInterval = mean(intervals);
    if (avgInterval <= 0.0) {
        return 1.0;
    }
    return std::clamp(static_cast<double>(horizonMs) / avgInterval, 0.0, 1.0);
}

double MemoryTimelineAnalyzer::calculateSimilarity(
    const TimelineSeries& seriesA,
    const TimelineSeries& seriesB) const {
    const auto valuesA = extractNumericValues(seriesA);
    const auto valuesB = extractNumericValues(seriesB);
    const size_t count = std::min(valuesA.size(), valuesB.size());
    if (count < 2) {
        return 0.0;
    }
    std::vector<double> a(valuesA.begin(), valuesA.begin() + static_cast<std::ptrdiff_t>(count));
    std::vector<double> b(valuesB.begin(), valuesB.begin() + static_cast<std::ptrdiff_t>(count));
    return std::abs(calculatePearsonCorrelation(a, b));
}

std::vector<std::pair<uint64_t, double>> MemoryTimelineAnalyzer::findSimilarSeries(
    const TimelineSeries& reference,
    const std::vector<TimelineSeries>& candidates,
    double minSimilarity) const {
    std::vector<std::pair<uint64_t, double>> result;
    for (const auto& candidate : candidates) {
        const double similarity = calculateSimilarity(reference, candidate);
        if (similarity >= minSimilarity) {
            result.emplace_back(candidate.baseAddress, similarity);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    return result;
}

std::string MemoryTimelineAnalyzer::generateAnalysisReport(
    const std::vector<TimelineSeries>& seriesList) const {
    std::ostringstream out;
    out << "Memory Timeline Analysis\n";
    out << "Series count: " << seriesList.size() << "\n";

    for (const auto& series : seriesList) {
        const auto pattern = detectDominantPattern(series);
        const auto profile = createBehaviorProfile(series);
        out << "0x" << std::hex << series.baseAddress << std::dec
            << " points=" << series.points.size()
            << " changes=" << profile.distinctValueCount
            << " pattern=" << typeToString(pattern.type)
            << " confidence=" << std::fixed << std::setprecision(2) << pattern.confidence
            << " cps=" << std::fixed << std::setprecision(2) << profile.changesPerSecond
            << "\n";
    }
    return out.str();
}

double MemoryTimelineAnalyzer::calculatePearsonCorrelation(
    const std::vector<double>& valuesA,
    const std::vector<double>& valuesB) const {
    const size_t count = std::min(valuesA.size(), valuesB.size());
    if (count < 2) {
        return 0.0;
    }

    const double meanA = std::accumulate(valuesA.begin(), valuesA.begin() + static_cast<std::ptrdiff_t>(count), 0.0) / static_cast<double>(count);
    const double meanB = std::accumulate(valuesB.begin(), valuesB.begin() + static_cast<std::ptrdiff_t>(count), 0.0) / static_cast<double>(count);

    double numerator = 0.0;
    double denomA = 0.0;
    double denomB = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double da = valuesA[i] - meanA;
        const double db = valuesB[i] - meanB;
        numerator += da * db;
        denomA += da * da;
        denomB += db * db;
    }
    if (denomA <= kEpsilon || denomB <= kEpsilon) {
        return 0.0;
    }
    return numerator / std::sqrt(denomA * denomB);
}

std::vector<double> MemoryTimelineAnalyzer::extractNumericValues(const TimelineSeries& series) const {
    std::vector<double> values;
    for (const auto* point : validPoints(series)) {
        values.push_back(static_cast<double>(decodeLittleEndian(point->value)));
    }
    return values;
}

bool MemoryTimelineAnalyzer::detectCyclicPattern(
    const TimelineSeries& series,
    uint32_t& outPeriodMs,
    double& outConfidence) const {
    const auto points = validPoints(series);
    if (points.size() < 6) {
        return false;
    }

    double bestScore = 0.0;
    size_t bestLag = 0;
    for (size_t lag = 1; lag <= points.size() / 2; ++lag) {
        size_t matches = 0;
        for (size_t i = lag; i < points.size(); ++i) {
            if (points[i]->value == points[i - lag]->value) {
                ++matches;
            }
        }
        const double score = static_cast<double>(matches) / static_cast<double>(points.size() - lag);
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }

    if (bestLag == 0 || bestScore < 0.60) {
        return false;
    }

    uint64_t totalPeriod = 0;
    size_t periodSamples = 0;
    for (size_t i = bestLag; i < points.size(); ++i) {
        totalPeriod += points[i]->timestampMs - points[i - bestLag]->timestampMs;
        ++periodSamples;
    }
    outPeriodMs = periodSamples > 0
        ? static_cast<uint32_t>(totalPeriod / periodSamples)
        : 0;
    outConfidence = bestScore;
    return true;
}

bool MemoryTimelineAnalyzer::detectLinearTrend(
    const TimelineSeries& series,
    double& outSlope,
    double& outConfidence) const {
    const auto values = extractNumericValues(series);
    const auto timestamps = Impl::timestampsAsDouble(series);
    const size_t count = std::min(values.size(), timestamps.size());
    if (count < 3) {
        return false;
    }

    const double xMean = std::accumulate(timestamps.begin(), timestamps.begin() + static_cast<std::ptrdiff_t>(count), 0.0) / static_cast<double>(count);
    const double yMean = std::accumulate(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count), 0.0) / static_cast<double>(count);

    double numerator = 0.0;
    double denominator = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double dx = timestamps[i] - xMean;
        numerator += dx * (values[i] - yMean);
        denominator += dx * dx;
    }
    if (denominator <= kEpsilon) {
        return false;
    }

    outSlope = numerator / denominator;
    if (std::abs(outSlope) <= kEpsilon) {
        return false;
    }

    double ssResidual = 0.0;
    double ssTotal = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double predicted = yMean + outSlope * (timestamps[i] - xMean);
        const double residual = values[i] - predicted;
        ssResidual += residual * residual;
        const double centered = values[i] - yMean;
        ssTotal += centered * centered;
    }
    outConfidence = ssTotal > kEpsilon ? std::clamp(1.0 - ssResidual / ssTotal, 0.0, 1.0) : 0.0;
    return outConfidence > 0.0;
}

} // namespace killcore
