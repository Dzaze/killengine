#include "memory_timeline_collector.h"
#include "../logging/logger.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

namespace killcore {

class MemoryTimelineCollector::Impl {
public:
    mutable std::mutex m_mutex;
    TimelineCollectorConfig m_config;
    std::unordered_map<uint64_t, TimelineSeries> m_series;
    std::unordered_map<uint64_t, size_t> m_addressSizes;
    
    std::atomic<bool> m_collecting{false};
    std::atomic<bool> m_shouldStop{false};
    std::thread m_collectionThread;
    void* m_processHandle{nullptr};
    
    ProgressCallback m_progressCallback;
    DataCallback m_dataCallback;
    
    std::chrono::steady_clock::time_point m_startTime;

    void collectionLoop() {
        m_startTime = std::chrono::steady_clock::now();
        uint32_t sampleCount = 0;
        
        while (!m_shouldStop.load()) {
            auto loopStart = std::chrono::steady_clock::now();
            
            // Collecter un échantillon pour chaque adresse
            collectSample();
            sampleCount++;
            
            // Vérifier la durée max
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_startTime).count();
            
            if (static_cast<uint32_t>(elapsed) >= m_config.maxDurationMs) {
                KE_LOG_INFO() << "Timeline collector: max duration reached";
                break;
            }
            
            // Progress callback
            if (m_progressCallback) {
                uint32_t percent = static_cast<uint32_t>(
                    (elapsed * 100) / m_config.maxDurationMs);
                m_progressCallback(percent, "Collecting...");
            }
            
            // Attendre l'intervalle suivant
            auto elapsedInLoop = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loopStart).count();
            
            int32_t sleepMs = static_cast<int32_t>(m_config.samplingIntervalMs) - 
                             static_cast<int32_t>(elapsedInLoop);
            if (sleepMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
        }
        
        m_collecting.store(false);
        if (m_progressCallback) {
            m_progressCallback(100, "Complete");
        }
    }

    void collectSample() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        auto now = std::chrono::steady_clock::now();
        uint64_t timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_startTime).count();
        
        for (const auto& [address, valueSize] : m_addressSizes) {
            TimelineDataPoint point;
            point.timestampMs = timestampMs;
            point.address = address;
            point.value.resize(valueSize);
            
            // Lire la mémoire. Corrigé (02/09/2026) : appelait
            // MemoryReader::readMemory(void*, ...), une méthode qui n'existe
            // pas (killcore::MemoryReader est une classe d'instance construite
            // depuis un ProcessHandle&, pas une fonction statique sur un
            // HANDLE brut) — jamais détecté car ce fichier n'était pas encore
            // enregistré dans core/CMakeLists.txt. WinAPI direct pour rester
            // cohérent avec MemoryHeatmapCollector (même pattern HANDLE brut).
            SIZE_T bytesRead = 0;
            point.isValid = ReadProcessMemory(
                m_processHandle,
                reinterpret_cast<LPCVOID>(address),
                point.value.data(),
                valueSize,
                &bytesRead
            ) != 0;

            if (!point.isValid || bytesRead != valueSize) {
                point.isValid = false;
            }
            
            // Stocker le point
            auto& series = m_series[address];
            series.baseAddress = address;
            series.valueSize = valueSize;
            
            // Vérifier si la valeur a changé (si trackOnlyChanges est activé)
            bool shouldStore = true;
            if (m_config.trackOnlyChanges && !series.points.empty()) {
                const auto& lastPoint = series.points.back();
                if (lastPoint.isValid == point.isValid && 
                    lastPoint.value == point.value) {
                    shouldStore = false;
                }
            }
            
            if (shouldStore) {
                // Limiter le nombre de points
                if (series.points.size() >= m_config.maxPointsPerSeries) {
                    series.points.erase(series.points.begin());
                }
                series.points.push_back(point);
                
                // Mettre à jour les statistiques
                if (m_config.calculateStatistics) {
                    updateStatistics(series);
                }
                
                // Callback de données
                if (m_dataCallback) {
                    m_dataCallback(point);
                }
            }
        }
    }

    void updateStatistics(TimelineSeries& series) {
        if (series.points.size() < 2) return;
        
        // Compter les changements
        series.changeCount = static_cast<uint32_t>(series.points.size());
        series.firstChangeMs = series.points.front().timestampMs;
        series.lastChangeMs = series.points.back().timestampMs;
        
        // Calculer l'intervalle moyen
        if (series.points.size() >= 2) {
            uint64_t totalInterval = 0;
            for (size_t i = 1; i < series.points.size(); ++i) {
                totalInterval += series.points[i].timestampMs - series.points[i-1].timestampMs;
            }
            series.averageIntervalMs = static_cast<double>(totalInterval) / (series.points.size() - 1);
        }
        
        // Calculer la volatilité (écart-type normalisé des intervalles)
        if (series.points.size() >= 3 && series.averageIntervalMs > 0) {
            double variance = 0;
            for (size_t i = 1; i < series.points.size(); ++i) {
                double diff = static_cast<double>(series.points[i].timestampMs - 
                    series.points[i-1].timestampMs) - series.averageIntervalMs;
                variance += diff * diff;
            }
            variance /= (series.points.size() - 1);
            double stdDev = std::sqrt(variance);
            series.volatilityScore = std::min(1.0, stdDev / series.averageIntervalMs);
        }
    }
};

MemoryTimelineCollector::MemoryTimelineCollector() 
    : m_impl(std::make_unique<Impl>()) {}

MemoryTimelineCollector::~MemoryTimelineCollector() {
    stopCollection();
}

void MemoryTimelineCollector::setConfig(const TimelineCollectorConfig& config) {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_config = config;
}

TimelineCollectorConfig MemoryTimelineCollector::config() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_config;
}

void MemoryTimelineCollector::addAddress(uint64_t address, size_t valueSize) {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_addressSizes[address] = valueSize;
    
    // Initialiser la série si elle n'existe pas
    if (m_impl->m_series.find(address) == m_impl->m_series.end()) {
        TimelineSeries series;
        series.baseAddress = address;
        series.valueSize = valueSize;
        m_impl->m_series[address] = std::move(series);
    }
}

void MemoryTimelineCollector::removeAddress(uint64_t address) {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_addressSizes.erase(address);
    m_impl->m_series.erase(address);
}

void MemoryTimelineCollector::clearAddresses() {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    m_impl->m_addressSizes.clear();
    m_impl->m_series.clear();
}

std::vector<uint64_t> MemoryTimelineCollector::watchedAddresses() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    std::vector<uint64_t> addresses;
    addresses.reserve(m_impl->m_addressSizes.size());
    for (const auto& [addr, _] : m_impl->m_addressSizes) {
        addresses.push_back(addr);
    }
    return addresses;
}

bool MemoryTimelineCollector::startCollection(void* processHandle) {
    if (m_impl->m_collecting.load()) {
        KE_LOG_WARN() << "Timeline collector already running";
        return false;
    }
    
    if (!processHandle) {
        KE_LOG_ERROR() << "Invalid process handle";
        return false;
    }
    
    if (m_impl->m_addressSizes.empty()) {
        KE_LOG_WARN() << "No addresses to watch";
        return false;
    }

    // Corrigé (02/09/2026, crash reproduit en live — std::terminate) : si une
    // collecte précédente s'est terminée naturellement (maxDurationMs atteint,
    // collectionLoop() retourne de lui-même), le thread est fini mais reste
    // "joinable" tant que personne n'a appelé join()/detach() dessus. Ré-
    // assigner m_collectionThread à un nouveau std::thread SANS le joindre
    // d'abord appelle std::terminate() — comportement standard de
    // std::thread::operator=(std::thread&&) sur un thread encore joinable,
    // pas une race ni un pointeur invalide. isCollecting()/m_collecting ne
    // suffit pas à le détecter (déjà remis à false par collectionLoop()).
    if (m_impl->m_collectionThread.joinable()) {
        m_impl->m_collectionThread.join();
    }

    m_impl->m_processHandle = processHandle;
    m_impl->m_shouldStop.store(false);
    m_impl->m_collecting.store(true);

    m_impl->m_collectionThread = std::thread(&Impl::collectionLoop, m_impl.get());
    
    KE_LOG_INFO() << "Timeline collector started with " << m_impl->m_addressSizes.size() 
                  << " addresses";
    return true;
}

void MemoryTimelineCollector::stopCollection() {
    // Corrigé (02/09/2026, même cause que le fix dans startCollection ci-dessus) :
    // l'ancien "if (!m_collecting.load()) return;" sortait AVANT de joindre le
    // thread quand collectionLoop() avait déjà mis m_collecting à false tout
    // seul (durée max atteinte) — le thread restait joinable indéfiniment.
    // ~MemoryTimelineCollector() appelle stopCollection() ; détruire un
    // std::thread encore joinable appelle std::terminate(). Toujours tenter
    // le join si joinable, peu importe l'état de m_collecting.
    m_impl->m_shouldStop.store(true);

    if (m_impl->m_collectionThread.joinable()) {
        m_impl->m_collectionThread.join();
    }

    m_impl->m_collecting.store(false);
    KE_LOG_INFO() << "Timeline collector stopped";
}

bool MemoryTimelineCollector::isCollecting() const {
    return m_impl->m_collecting.load();
}

void MemoryTimelineCollector::setProgressCallback(ProgressCallback callback) {
    m_impl->m_progressCallback = callback;
}

void MemoryTimelineCollector::setDataCallback(DataCallback callback) {
    m_impl->m_dataCallback = callback;
}

std::unordered_map<uint64_t, TimelineSeries> MemoryTimelineCollector::getSeries() const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    return m_impl->m_series;
}

TimelineSeries MemoryTimelineCollector::getSeriesForAddress(uint64_t address) const {
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    auto it = m_impl->m_series.find(address);
    if (it != m_impl->m_series.end()) {
        return it->second;
    }
    return TimelineSeries();
}

bool MemoryTimelineCollector::exportToJson(const std::string& filepath) const {
    try {
        nlohmann::json j;
        
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        
        j["config"]["samplingIntervalMs"] = m_impl->m_config.samplingIntervalMs;
        j["config"]["maxDurationMs"] = m_impl->m_config.maxDurationMs;
        j["config"]["trackOnlyChanges"] = m_impl->m_config.trackOnlyChanges;
        
        for (const auto& [address, series] : m_impl->m_series) {
            nlohmann::json seriesJson;
            seriesJson["address"] = address;
            seriesJson["valueSize"] = series.valueSize;
            seriesJson["changeCount"] = series.changeCount;
            seriesJson["volatilityScore"] = series.volatilityScore;
            seriesJson["averageIntervalMs"] = series.averageIntervalMs;
            
            for (const auto& point : series.points) {
                nlohmann::json pointJson;
                pointJson["timestampMs"] = point.timestampMs;
                pointJson["isValid"] = point.isValid;
                
                // Convertir la valeur en hex
                std::stringstream hexValue;
                hexValue << "0x";
                for (auto it = point.value.rbegin(); it != point.value.rend(); ++it) {
                    hexValue << std::hex << std::setw(2) << std::setfill('0') 
                             << static_cast<int>(*it);
                }
                pointJson["valueHex"] = hexValue.str();
                
                seriesJson["points"].push_back(pointJson);
            }
            
            j["series"].push_back(seriesJson);
        }
        
        std::ofstream file(filepath);
        file << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        KE_LOG_ERROR() << "Failed to export timeline to JSON: " << e.what();
        return false;
    }
}

bool MemoryTimelineCollector::exportToCsv(const std::string& filepath) const {
    try {
        std::ofstream file(filepath);
        file << "address,timestampMs,valueHex,isValid\n";
        
        std::lock_guard<std::mutex> lock(m_impl->m_mutex);
        
        for (const auto& [address, series] : m_impl->m_series) {
            for (const auto& point : series.points) {
                std::stringstream hexValue;
                hexValue << "0x";
                for (auto it = point.value.rbegin(); it != point.value.rend(); ++it) {
                    hexValue << std::hex << std::setw(2) << std::setfill('0') 
                             << static_cast<int>(*it);
                }
                
                file << address << "," << point.timestampMs << ","
                     << hexValue.str() << "," << (point.isValid ? "1" : "0") << "\n";
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        KE_LOG_ERROR() << "Failed to export timeline to CSV: " << e.what();
        return false;
    }
}

std::vector<uint64_t> MemoryTimelineCollector::findVolatileAddresses(double threshold) const {
    std::vector<uint64_t> result;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    
    for (const auto& [address, series] : m_impl->m_series) {
        if (series.volatilityScore >= threshold) {
            result.push_back(address);
        }
    }
    
    return result;
}

std::vector<uint64_t> MemoryTimelineCollector::findStableAddresses(uint32_t minDurationMs) const {
    std::vector<uint64_t> result;
    std::lock_guard<std::mutex> lock(m_impl->m_mutex);
    
    for (const auto& [address, series] : m_impl->m_series) {
        uint32_t duration = static_cast<uint32_t>(series.lastChangeMs - series.firstChangeMs);
        if (duration >= minDurationMs && series.changeCount <= 2) {
            result.push_back(address);
        }
    }
    
    return result;
}

} // namespace killcore
