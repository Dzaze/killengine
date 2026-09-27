#include "candidate_comparison_collector.h"
#include "candidate_comparison_decoder.h"
#include "../logging/logger.h"

#include <QHash>
#include <QSet>

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>

namespace killcore {

namespace {

constexpr uint32_t kMinIntervalMs = 50;
constexpr uint32_t kMaxDurationMs = 120000;
constexpr size_t kMinSeries = 2;
constexpr size_t kMaxSeries = 6;
constexpr size_t kMaxPointsPerSeries = 2401; // 6 x 2401 = 14406, borne de la fiche
constexpr size_t kMaxTourTimings = 2401;
constexpr uint32_t kMaxConsecutiveInvalidTours = 5;
constexpr size_t kMaxMarkers = 100;
constexpr int kMaxMarkerTextLength = 500;
constexpr int kComparisonExportSchemaVersion = 1;

uint64_t msSince(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point now) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

const char* stopReasonToExportString(ComparisonStopReason reason) {
    switch (reason) {
        case ComparisonStopReason::UserStop: return "user_stop";
        case ComparisonStopReason::DurationReached: return "duration_reached";
        case ComparisonStopReason::TargetLost: return "target_lost";
    }
    return "user_stop";
}

/// Pearson standard. Retourne NaN si l'une des deux séries a une variance
/// nulle (corrélation non définie, jamais approximée à 0 ou 1).
double pearsonCorrelation(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = a.size();
    double meanA = 0.0;
    double meanB = 0.0;
    for (size_t i = 0; i < n; ++i) {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= static_cast<double>(n);
    meanB /= static_cast<double>(n);

    double numerator = 0.0;
    double denomA = 0.0;
    double denomB = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        numerator += da * db;
        denomA += da * da;
        denomB += db * db;
    }
    if (denomA <= 0.0 || denomB <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return numerator / std::sqrt(denomA * denomB);
}

} // namespace

class CandidateComparisonCollector::Impl {
public:
    mutable std::mutex m_mutex;
    std::vector<ComparisonSeriesConfig> m_seriesConfigs;
    uint32_t m_intervalMs{100};
    uint32_t m_maxDurationMs{30000};

    std::atomic<bool> m_collecting{false};
    std::atomic<bool> m_shouldStop{false};
    std::thread m_collectionThread;
    void* m_processHandle{nullptr};

    std::mutex m_stopMutex;
    std::condition_variable m_stopCv;
    std::atomic<int> m_stopReason{static_cast<int>(ComparisonStopReason::UserStop)};
    std::atomic<uint32_t> m_skippedTicks{0};

    QHash<QString, std::vector<ComparisonPoint>> m_pointsBySeries;
    std::vector<ComparisonTourTiming> m_tourTimings;
    std::vector<ComparisonMarker> m_markers;
    uint32_t m_nextMarkerId{1};

    std::chrono::steady_clock::time_point m_startTime;

    void collectionLoop() {
        auto nextTickDeadline = m_startTime;
        uint32_t batchId = 0;
        uint32_t consecutiveInvalidTours = 0;

        while (!m_shouldStop.load()) {
            const uint64_t tourStartMs = msSince(m_startTime, std::chrono::steady_clock::now());
            const bool anyValid = collectTour(batchId, tourStartMs);
            const uint64_t tourEndMs = msSince(m_startTime, std::chrono::steady_clock::now());
            recordTourTiming(batchId, tourStartMs, tourEndMs);
            ++batchId;

            if (anyValid) {
                consecutiveInvalidTours = 0;
            } else {
                ++consecutiveInvalidTours;
                if (consecutiveInvalidTours >= kMaxConsecutiveInvalidTours) {
                    KE_LOG_WARN() << "Candidate comparison: target lost (no valid read for "
                                  << kMaxConsecutiveInvalidTours << " consecutive tours)";
                    m_stopReason.store(static_cast<int>(ComparisonStopReason::TargetLost));
                    break;
                }
            }

            if (tourEndMs >= m_maxDurationMs) {
                m_stopReason.store(static_cast<int>(ComparisonStopReason::DurationReached));
                break;
            }

            // Avance le rendez-vous suivant d'un pas d'intervalle. Si un ou
            // plusieurs pas sont déjà dépassés (tour trop lent), les sauter
            // et le compter plutôt que de lancer des lectures concurrentes
            // de rattrapage (exigence explicite de la fiche).
            nextTickDeadline += std::chrono::milliseconds(m_intervalMs);
            const auto now = std::chrono::steady_clock::now();
            while (nextTickDeadline <= now) {
                nextTickDeadline += std::chrono::milliseconds(m_intervalMs);
                m_skippedTicks.fetch_add(1, std::memory_order_relaxed);
            }

            std::unique_lock<std::mutex> stopLock(m_stopMutex);
            m_stopCv.wait_until(stopLock, nextTickDeadline, [this] { return m_shouldStop.load(); });
        }

        m_collecting.store(false);
    }

    /// Retourne true si au moins une série a été lue avec succès ce tour.
    bool collectTour(uint32_t batchId, uint64_t /*tourStartMs*/) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool anyValid = false;
        for (const auto& cfg : m_seriesConfigs) {
            ComparisonPoint point;
            point.batchId = batchId;
            point.timestampMs = msSince(m_startTime, std::chrono::steady_clock::now());

            const size_t sz = valueTypeSize(cfg.type);
            std::vector<uint8_t> buffer(sz);
            SIZE_T bytesRead = 0;
            const bool readOk = ReadProcessMemory(
                m_processHandle,
                reinterpret_cast<LPCVOID>(cfg.address),
                buffer.data(),
                sz,
                &bytesRead) != 0 && bytesRead == sz;

            point.isValid = readOk;
            if (readOk) {
                point.rawBytes = std::move(buffer);
                anyValid = true;
            }

            auto& points = m_pointsBySeries[cfg.id];
            if (static_cast<size_t>(points.size()) >= kMaxPointsPerSeries) {
                points.erase(points.begin());
            }
            points.push_back(std::move(point));
        }
        return anyValid;
    }

    void recordTourTiming(uint32_t batchId, uint64_t startMs, uint64_t endMs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_tourTimings.size() >= kMaxTourTimings) {
            m_tourTimings.erase(m_tourTimings.begin());
        }
        m_tourTimings.push_back({batchId, startMs, endMs});
    }
};

CandidateComparisonCollector::CandidateComparisonCollector() : m_impl(std::make_unique<Impl>()) {}

CandidateComparisonCollector::~CandidateComparisonCollector() {
    stopCollection();
}

bool CandidateComparisonCollector::configure(
    const std::vector<ComparisonSeriesConfig>& series, uint32_t intervalMs, uint32_t maxDurationMs) {
    if (m_impl->m_collecting.load()) {
        KE_LOG_WARN() << "Candidate comparison: cannot reconfigure while a capture is active";
        return false;
    }
    if (series.size() < kMinSeries || series.size() > kMaxSeries) {
        return false;
    }
    QSet<QString> seenIds;
    for (const auto& s : series) {
        if (s.id.isEmpty() || seenIds.contains(s.id)) {
            return false;
        }
        seenIds.insert(s.id);
    }

    const uint32_t clampedInterval = std::max<uint32_t>(kMinIntervalMs, intervalMs);
    const uint32_t clampedDuration =
        std::min<uint32_t>(kMaxDurationMs, std::max<uint32_t>(clampedInterval, maxDurationMs));

    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_seriesConfigs = series;
    m_impl->m_intervalMs = clampedInterval;
    m_impl->m_maxDurationMs = clampedDuration;
    m_impl->m_pointsBySeries.clear();
    m_impl->m_tourTimings.clear();
    m_impl->m_markers.clear();
    m_impl->m_nextMarkerId = 1;
    m_impl->m_skippedTicks.store(0);
    return true;
}

bool CandidateComparisonCollector::startCollection(void* processHandle) {
    if (m_impl->m_collecting.load()) {
        KE_LOG_WARN() << "Candidate comparison collector already running";
        return false;
    }
    if (!processHandle) {
        KE_LOG_ERROR() << "Candidate comparison: invalid process handle";
        return false;
    }
    if (m_impl->m_seriesConfigs.size() < kMinSeries) {
        KE_LOG_WARN() << "Candidate comparison: not configured (need configure() first)";
        return false;
    }

    // Même piège que MemoryTimelineCollector (memory_timeline_collector.cpp) :
    // un thread fini mais jamais joint reste "joinable" -- le réassigner sans
    // join() appelle std::terminate().
    if (m_impl->m_collectionThread.joinable()) {
        m_impl->m_collectionThread.join();
    }

    m_impl->m_processHandle = processHandle;
    m_impl->m_shouldStop.store(false);
    m_impl->m_stopReason.store(static_cast<int>(ComparisonStopReason::UserStop));

    // Fixé ici (thread appelant), pas au premier tour de collectionLoop() :
    // un appelant qui observe m_collecting==true (ci-dessous) et enchaîne
    // aussitôt sur addMarker() doit voir un m_startTime déjà valide -- sinon
    // msSince() calcule depuis l'epoch de steady_clock (valeur énorme, ex.
    // l'uptime système), bug réel trouvé par
    // AddMarkerAcceptedWhileCollectingWithIncrementingIds avant ce correctif.
    // Le verrou assure la visibilité vers addMarker()/markers(), qui
    // acquièrent le même mutex.
    {
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        m_impl->m_startTime = std::chrono::steady_clock::now();
    }
    m_impl->m_collecting.store(true);

    m_impl->m_collectionThread = std::thread(&Impl::collectionLoop, m_impl.get());
    return true;
}

void CandidateComparisonCollector::stopCollection() {
    m_impl->m_shouldStop.store(true);
    m_impl->m_stopCv.notify_one();

    if (m_impl->m_collectionThread.joinable()) {
        m_impl->m_collectionThread.join();
    }
    m_impl->m_collecting.store(false);
}

bool CandidateComparisonCollector::isCollecting() const {
    return m_impl->m_collecting.load();
}

ComparisonStopReason CandidateComparisonCollector::lastStopReason() const {
    return static_cast<ComparisonStopReason>(m_impl->m_stopReason.load());
}

uint32_t CandidateComparisonCollector::skippedTickCount() const {
    return m_impl->m_skippedTicks.load();
}

std::vector<ComparisonSeriesConfig> CandidateComparisonCollector::seriesConfigs() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_seriesConfigs;
}

std::vector<ComparisonPoint> CandidateComparisonCollector::pointsForSeries(
    const QString& seriesId, size_t offset, size_t limit) const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    const auto it = m_impl->m_pointsBySeries.find(seriesId);
    if (it == m_impl->m_pointsBySeries.end() || offset >= it->size()) {
        return {};
    }
    const size_t end = std::min(it->size(), offset + limit);
    return std::vector<ComparisonPoint>(it->begin() + static_cast<ptrdiff_t>(offset), it->begin() + static_cast<ptrdiff_t>(end));
}

size_t CandidateComparisonCollector::pointCountForSeries(const QString& seriesId) const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    const auto it = m_impl->m_pointsBySeries.find(seriesId);
    return it == m_impl->m_pointsBySeries.end() ? 0 : it->size();
}

std::vector<ComparisonTourTiming> CandidateComparisonCollector::tourTimings() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_tourTimings;
}

std::vector<ComparisonPairwiseCorrelation> CandidateComparisonCollector::correlations() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return correlationsLocked();
}

std::vector<ComparisonPairwiseCorrelation> CandidateComparisonCollector::correlationsLocked() const {
    std::vector<ComparisonPairwiseCorrelation> out;
    const auto& configs = m_impl->m_seriesConfigs;

    for (size_t i = 0; i < configs.size(); ++i) {
        for (size_t j = i + 1; j < configs.size(); ++j) {
            ComparisonPairwiseCorrelation entry;
            entry.seriesIdA = configs[i].id;
            entry.seriesIdB = configs[j].id;

            const auto itA = m_impl->m_pointsBySeries.find(configs[i].id);
            const auto itB = m_impl->m_pointsBySeries.find(configs[j].id);
            const size_t n = (itA == m_impl->m_pointsBySeries.end() || itB == m_impl->m_pointsBySeries.end())
                ? 0
                : std::min(itA->size(), itB->size());

            std::vector<double> a;
            std::vector<double> b;
            // Alignement par construction : les deux séries reçoivent
            // exactement un point par tour, dans le même ordre -- même
            // indice == même batchId, jamais un appariement par indice
            // "au hasard" comme MemoryTimelineAnalyzer::findCorrelations.
            for (size_t k = 0; k < n; ++k) {
                const auto& pa = (*itA)[k];
                const auto& pb = (*itB)[k];
                if (!pa.isValid || !pb.isValid) continue;
                const auto da = decodeComparisonValue(pa.rawBytes, configs[i].type);
                const auto db = decodeComparisonValue(pb.rawBytes, configs[j].type);
                if (!da.ok || !db.ok || da.isNaN || da.isInfinite || db.isNaN || db.isInfinite) continue;
                a.push_back(da.numericValue);
                b.push_back(db.numericValue);
            }

            entry.pairCount = static_cast<int>(a.size());
            if (a.size() < 10) {
                entry.computable = false;
                entry.reason = QStringLiteral("Moins de 10 paires valides communes.");
            } else {
                const double coeff = pearsonCorrelation(a, b);
                if (std::isnan(coeff)) {
                    entry.computable = false;
                    entry.reason = QStringLiteral("Variance nulle sur au moins une série.");
                } else {
                    entry.computable = true;
                    entry.coefficient = coeff;
                }
            }
            out.push_back(entry);
        }
    }
    return out;
}

bool CandidateComparisonCollector::addMarker(const QString& text, ComparisonMarker* out) {
    if (!m_impl->m_collecting.load()) {
        return false;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxMarkerTextLength) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    if (m_impl->m_markers.size() >= kMaxMarkers) {
        return false;
    }
    ComparisonMarker marker;
    marker.id = m_impl->m_nextMarkerId++;
    marker.timestampMs = msSince(m_impl->m_startTime, std::chrono::steady_clock::now());
    marker.text = trimmed;
    m_impl->m_markers.push_back(marker);
    if (out) {
        *out = marker;
    }
    return true;
}

std::vector<ComparisonMarker> CandidateComparisonCollector::markers() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_markers;
}

bool CandidateComparisonCollector::exportToJson(const std::string& filepath) const {
    try {
        nlohmann::json j;
        j["kind"] = "candidate_comparison_export";
        j["schemaVersion"] = kComparisonExportSchemaVersion;

        std::lock_guard<std::mutex> lock(m_impl->m_mutex);

        j["capture"]["intervalMs"] = m_impl->m_intervalMs;
        j["capture"]["maxDurationMs"] = m_impl->m_maxDurationMs;
        j["capture"]["collecting"] = m_impl->m_collecting.load();
        j["capture"]["stopReason"] = stopReasonToExportString(
            static_cast<ComparisonStopReason>(m_impl->m_stopReason.load()));
        j["capture"]["skippedTicks"] = m_impl->m_skippedTicks.load();

        for (const auto& cfg : m_impl->m_seriesConfigs) {
            nlohmann::json seriesJson;
            seriesJson["id"] = cfg.id.toStdString();
            seriesJson["address"] = QString("0x%1").arg(cfg.address, 0, 16).toStdString();
            seriesJson["type"] = valueTypeToString(cfg.type).toStdString();
            seriesJson["factor"] = cfg.factor;
            seriesJson["label"] = cfg.label.toStdString();
            seriesJson["points"] = nlohmann::json::array();

            const auto it = m_impl->m_pointsBySeries.find(cfg.id);
            if (it != m_impl->m_pointsBySeries.end()) {
                for (const auto& point : *it) {
                    nlohmann::json pointJson;
                    pointJson["batchId"] = point.batchId;
                    pointJson["timestampMs"] = point.timestampMs;
                    pointJson["isValid"] = point.isValid;
                    if (point.isValid) {
                        const auto decoded = decodeComparisonValue(point.rawBytes, cfg.type);
                        pointJson["ok"] = decoded.ok;
                        if (decoded.ok) {
                            pointJson["exactValueText"] = decoded.exactValueText.toStdString();
                            pointJson["numericValue"] = decoded.numericValue;
                            pointJson["isNaN"] = decoded.isNaN;
                            pointJson["isInfinite"] = decoded.isInfinite;
                            bool exact = false;
                            pointJson["scaledValueText"] =
                                formatScaledValueText(decoded, cfg.type, cfg.factor, &exact).toStdString();
                            pointJson["scaledValueExact"] = exact;
                        }
                    }
                    seriesJson["points"].push_back(pointJson);
                }
            }
            j["series"].push_back(seriesJson);
        }

        j["tourTimings"] = nlohmann::json::array();
        for (const auto& timing : m_impl->m_tourTimings) {
            nlohmann::json t;
            t["batchId"] = timing.batchId;
            t["tourStartMs"] = timing.tourStartMs;
            t["tourEndMs"] = timing.tourEndMs;
            j["tourTimings"].push_back(t);
        }

        j["correlations"] = nlohmann::json::array();
        for (const auto& c : correlationsLocked()) {
            nlohmann::json entry;
            entry["seriesIdA"] = c.seriesIdA.toStdString();
            entry["seriesIdB"] = c.seriesIdB.toStdString();
            entry["computable"] = c.computable;
            entry["coefficient"] = c.coefficient;
            entry["pairCount"] = c.pairCount;
            entry["reason"] = c.reason.toStdString();
            j["correlations"].push_back(entry);
        }

        j["markers"] = nlohmann::json::array();
        for (const auto& marker : m_impl->m_markers) {
            nlohmann::json m;
            m["id"] = marker.id;
            m["timestampMs"] = marker.timestampMs;
            m["text"] = marker.text.toStdString();
            j["markers"].push_back(m);
        }

        j["method"]["correlation"] = "pearson_paired_by_batch_id";
        j["method"]["minPairsForComputable"] = 10;

        std::ofstream file(filepath);
        file << j.dump(2);
        return file.good();
    } catch (const std::exception& e) {
        KE_LOG_ERROR() << "Failed to export candidate comparison to JSON: " << e.what();
        return false;
    }
}

} // namespace killcore
