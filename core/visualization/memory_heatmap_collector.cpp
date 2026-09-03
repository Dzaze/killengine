#include "memory_heatmap_collector.h"
#include "../memory/memory_reader.h"
#include "../logging/logger.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>

namespace killcore {

// Pimpl idiom implementation
class MemoryHeatmapCollector::Impl {
public:
    HeatmapConfig config;
    std::atomic<bool> collecting{false};
    std::atomic<bool> shouldStop{false};
    HANDLE processHandle = nullptr;

    mutable std::mutex regionsMutex;
    std::map<uint64_t, HeatmapRegion> regions;
    // Corrigé (02/09/2026) : hash de contenu par page, SÉPARÉ de
    // HeatmapRegion::intensity. L'ancien code stockait le hash directement
    // dans `intensity` (un champ 0.0-1.0 exposé au frontend pour la
    // visualisation) — la heatmap affichait donc un fragment de hash
    // pseudo-aléatoire, pas une vraie intensité d'accès. Voir updateStats()
    // pour le vrai calcul d'intensité (normalisé sur writeCount).
    std::map<uint64_t, uint64_t> pageHashes;
    // Liste des adresses de page actuellement "vivantes" (committées, protection
    // eligible), reconstruite périodiquement par refreshTrackedPages(). Le
    // contenu (ReadProcessMemory+hash) n'est lu que pour une tranche bornée de
    // cette liste par tick (voir sampleBoundedSlice) — jamais tout d'un coup.
    std::vector<uint64_t> trackedPages;
    size_t roundRobinCursor = 0;
    // Réénumérer les régions (VirtualQueryEx) est bien moins coûteux que lire
    // leur contenu (pas de ReadProcessMemory), mais fait quand même potentiellement
    // des milliers d'appels sur un gros process : throttle à 1 tick sur 10 plutôt
    // que de le refaire à chaque tick.
    int ticksSinceRegionRefresh = 0;
    static constexpr int kRegionRefreshEveryNTicks = 10;

    std::thread collectorThread;
    HeatmapCallback updateCallback;

    HeatmapStats stats{};
    std::chrono::steady_clock::time_point startTime;

    explicit Impl(HeatmapConfig cfg) : config(std::move(cfg)) {}

    ~Impl() {
        // Arrêter proprement la collection
        shouldStop.store(true);
        if (collectorThread.joinable()) {
            collectorThread.join();
        }
    }

    void collectorLoop() {
        while (!shouldStop.load()) {
            auto start = std::chrono::steady_clock::now();

            if (ticksSinceRegionRefresh <= 0) {
                refreshTrackedPages();
                ticksSinceRegionRefresh = kRegionRefreshEveryNTicks;
            }
            --ticksSinceRegionRefresh;

            sampleBoundedSlice();
            updateStats();

            // Appeler le callback si défini
            if (updateCallback) {
                auto topRegions = getTopRegionsInternal(100);
                updateCallback(topRegions);
            }

            // Attendre l'intervalle configuré
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto sleepTime = std::chrono::milliseconds(config.samplingIntervalMs) - elapsed;
            if (sleepTime > std::chrono::milliseconds(0)) {
                std::this_thread::sleep_for(sleepTime);
            }
        }
    }

    // Balaie les métadonnées de régions (pas de lecture de contenu) pour
    // reconstruire la liste des pages éligibles. Corrigé (02/09/2026) : c'est
    // la SEULE partie qui parcourt tout l'espace d'adressage — et elle ne fait
    // aucun ReadProcessMemory, juste des VirtualQueryEx (métadonnées).
    void refreshTrackedPages() {
        if (!processHandle) return;

        std::vector<uint64_t> pages;
        MEMORY_BASIC_INFORMATION mbi;
        uint64_t address = config.minAddress;

        while (address < config.maxAddress && !shouldStop.load()) {
            if (VirtualQueryEx(processHandle, reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
                break;
            }

            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                const uint64_t baseAddr = reinterpret_cast<uint64_t>(mbi.BaseAddress);
                for (uint64_t offset = 0; offset < mbi.RegionSize; offset += config.regionSize) {
                    pages.push_back(baseAddr + offset);
                    if (pages.size() >= config.maxRegions) {
                        break;
                    }
                }
            }

            address = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (pages.size() >= config.maxRegions) {
                break;
            }
        }

        std::lock_guard<std::mutex> lock(regionsMutex);
        trackedPages = std::move(pages);
        if (trackedPages.empty() || roundRobinCursor >= trackedPages.size()) {
            roundRobinCursor = 0;
        }
    }

    // Lit+hash au plus config.maxPagesPerTick pages, en reprenant où le tick
    // précédent s'est arrêté (round-robin) — un balayage complet du process
    // s'étale sur plusieurs ticks au lieu de tout tenter en un seul.
    void sampleBoundedSlice() {
        if (!processHandle || (!config.trackReads && !config.trackWrites)) {
            return;
        }

        std::vector<uint64_t> slice;
        {
            std::lock_guard<std::mutex> lock(regionsMutex);
            if (trackedPages.empty()) {
                return;
            }
            const size_t budget = std::min<size_t>(config.maxPagesPerTick, trackedPages.size());
            slice.reserve(budget);
            for (size_t i = 0; i < budget; ++i) {
                slice.push_back(trackedPages[roundRobinCursor]);
                roundRobinCursor = (roundRobinCursor + 1) % trackedPages.size();
            }
        }

        for (const uint64_t pageAddr : slice) {
            if (shouldStop.load()) break;
            samplePage(pageAddr);
        }
    }

    void samplePage(uint64_t pageAddr) {
        std::vector<uint8_t> buffer(config.regionSize);
        SIZE_T bytesRead = 0;

        if (!ReadProcessMemory(processHandle, reinterpret_cast<LPCVOID>(pageAddr),
                                buffer.data(), config.regionSize, &bytesRead) || bytesRead == 0) {
            // Page devenue inaccessible (libérée/protection changée) : laissée
            // telle quelle, refreshTrackedPages() la retirera à son prochain
            // passage si la région a réellement disparu.
            return;
        }

        const uint64_t hash = computeHash(buffer.data(), bytesRead);

        std::lock_guard<std::mutex> lock(regionsMutex);

        const auto hashIt = pageHashes.find(pageAddr);
        const bool hadPreviousHash = hashIt != pageHashes.end();
        const uint64_t previousHash = hadPreviousHash ? hashIt->second : 0;
        pageHashes[pageAddr] = hash;

        auto it = regions.find(pageAddr);
        if (it == regions.end()) {
            if (regions.size() >= config.maxRegions) {
                return;
            }
            HeatmapRegion region{};
            region.baseAddress = pageAddr;
            region.size = static_cast<uint64_t>(bytesRead);
            region.lastAccess = std::chrono::steady_clock::now();
            it = regions.emplace(pageAddr, region).first;
        }

        // Corrigé (02/09/2026) : readCount/writeCount respectent maintenant
        // trackReads/trackWrites (ignorés auparavant), et readCount s'incrémente
        // à CHAQUE échantillon (pas seulement à la création de la région).
        if (config.trackReads) {
            it->second.readCount++;
            it->second.accessCount++;
        }
        if (config.trackWrites && hadPreviousHash && previousHash != hash) {
            it->second.writeCount++;
            if (!config.trackReads) {
                it->second.accessCount++;
            }
            it->second.lastAccess = std::chrono::steady_clock::now();
        }
    }

    uint64_t computeHash(const uint8_t* data, SIZE_T size) {
        // FNV-1a hash simple
        uint64_t hash = 14695981039346656037ULL;
        const uint64_t prime = 1099511628211ULL;
        
        // Échantillonner tous les 8 octets pour la performance
        for (SIZE_T i = 0; i < size; i += 8) {
            hash ^= data[i];
            hash *= prime;
        }
        
        return hash;
    }
    
    void updateStats() {
        std::lock_guard<std::mutex> lock(regionsMutex);

        // Corrigé (02/09/2026) : `intensity` est maintenant un vrai signal
        // d'activité — writeCount normalisé contre la région la plus active
        // vue jusqu'ici (donc toujours dans [0,1], la région la plus "chaude"
        // valant ~1.0) — au lieu d'un fragment de hash sans rapport avec
        // l'activité réelle.
        uint32_t maxWriteCount = 0;
        for (const auto& [addr, region] : regions) {
            maxWriteCount = std::max(maxWriteCount, region.writeCount);
        }
        const float normalizer = maxWriteCount > 0 ? static_cast<float>(maxWriteCount) : 1.0f;

        stats.totalRegions = regions.size();
        stats.activeRegions = 0;
        stats.totalAccesses = 0;
        stats.totalWrites = 0;
        stats.totalReads = 0;

        auto now = std::chrono::steady_clock::now();
        auto activeThreshold = std::chrono::seconds(5);

        float totalIntensity = 0.0f;

        for (auto& [addr, region] : regions) {
            region.intensity = static_cast<float>(region.writeCount) / normalizer;

            if (now - region.lastAccess < activeThreshold) {
                stats.activeRegions++;
            }
            stats.totalAccesses += region.accessCount;
            stats.totalWrites += region.writeCount;
            stats.totalReads += region.readCount;
            totalIntensity += region.intensity;
        }

        stats.averageIntensity = regions.empty() ? 0.0f : totalIntensity / regions.size();
        stats.lastUpdate = now;
    }
    
    std::vector<HeatmapRegion> getTopRegionsInternal(uint32_t maxCount) const {
        std::lock_guard<std::mutex> lock(regionsMutex);
        
        std::vector<HeatmapRegion> result;
        result.reserve(std::min<size_t>(maxCount, regions.size()));
        
        for (const auto& [addr, region] : regions) {
            result.push_back(region);
        }
        
        // Trier par nombre d'accès décroissant
        std::sort(result.begin(), result.end(), [](const HeatmapRegion& a, const HeatmapRegion& b) {
            return a.accessCount > b.accessCount;
        });
        
        if (result.size() > maxCount) {
            result.resize(maxCount);
        }
        
        return result;
    }
};

MemoryHeatmapCollector::MemoryHeatmapCollector(HeatmapConfig config)
    : m_impl(std::make_unique<Impl>(std::move(config))) {
}

MemoryHeatmapCollector::~MemoryHeatmapCollector() = default;

bool MemoryHeatmapCollector::startCollection(void* processHandle) {
    if (m_impl->collecting.load()) {
        return false;
    }
    
    m_impl->processHandle = processHandle;
    m_impl->shouldStop.store(false);
    m_impl->collecting.store(true);
    m_impl->startTime = std::chrono::steady_clock::now();
    m_impl->stats.startTime = m_impl->startTime;
    
    // Capturer une copie du pointeur impl pour éviter la référence à this
    auto* implPtr = m_impl.get();
    m_impl->collectorThread = std::thread([implPtr]() {
        implPtr->collectorLoop();
    });
    
    KE_LOG_INFO() << "MemoryHeatmapCollector: Collection started";
    return true;
}

void MemoryHeatmapCollector::stopCollection() {
    if (!m_impl->collecting.load()) {
        return;
    }
    
    m_impl->shouldStop.store(true);
    
    if (m_impl->collectorThread.joinable()) {
        m_impl->collectorThread.join();
    }
    
    m_impl->collecting.store(false);
    m_impl->processHandle = nullptr;
    
    KE_LOG_INFO() << "MemoryHeatmapCollector: Collection stopped";
}

bool MemoryHeatmapCollector::isCollecting() const {
    return m_impl->collecting.load();
}

std::vector<HeatmapRegion> MemoryHeatmapCollector::getTopRegions(uint32_t maxRegions) const {
    return m_impl->getTopRegionsInternal(maxRegions);
}

std::vector<HeatmapRegion> MemoryHeatmapCollector::getRegionsInRange(uint64_t start, uint64_t end) const {
    std::lock_guard<std::mutex> lock(m_impl->regionsMutex);
    
    std::vector<HeatmapRegion> result;
    
    auto it = m_impl->regions.lower_bound(start);
    while (it != m_impl->regions.end() && it->first < end) {
        result.push_back(it->second);
        ++it;
    }
    
    return result;
}

HeatmapStats MemoryHeatmapCollector::getStats() const {
    std::lock_guard<std::mutex> lock(m_impl->regionsMutex);
    return m_impl->stats;
}

void MemoryHeatmapCollector::reset() {
    std::lock_guard<std::mutex> lock(m_impl->regionsMutex);
    m_impl->regions.clear();
    m_impl->pageHashes.clear();
    m_impl->trackedPages.clear();
    m_impl->roundRobinCursor = 0;
    m_impl->ticksSinceRegionRefresh = 0;
    m_impl->stats = HeatmapStats{};
}

void MemoryHeatmapCollector::setUpdateCallback(HeatmapCallback callback) {
    m_impl->updateCallback = std::move(callback);
}

std::vector<float> MemoryHeatmapCollector::generateGrid(uint32_t width, uint32_t height) const {
    std::vector<float> grid(width * height, 0.0f);
    
    std::lock_guard<std::mutex> lock(m_impl->regionsMutex);
    
    if (m_impl->regions.empty()) {
        return grid;
    }
    
    // Trouver les bornes d'adresses
    uint64_t minAddr = m_impl->regions.begin()->first;
    uint64_t maxAddr = m_impl->regions.rbegin()->first;
    uint64_t addrRange = maxAddr - minAddr;
    
    if (addrRange == 0) {
        return grid;
    }
    
    // Mapper les régions sur la grille
    for (const auto& [addr, region] : m_impl->regions) {
        // Normaliser l'adresse en coordonnées de grille
        float normalizedPos = static_cast<float>(addr - minAddr) / static_cast<float>(addrRange);
        uint32_t gridPos = static_cast<uint32_t>(normalizedPos * width * height);
        gridPos = std::min(gridPos, width * height - 1);
        
        // Intensité déjà normalisée par updateStats() (writeCount relatif au
        // max observé) — cohérent avec getTopHeatmapRegions/getHeatmapStats,
        // au lieu de recalculer une métrique différente ici.
        grid[gridPos] = std::max(grid[gridPos], region.intensity);
    }
    
    return grid;
}

std::string MemoryHeatmapCollector::exportToJson() const {
    std::lock_guard<std::mutex> lock(m_impl->regionsMutex);
    
    std::ostringstream json;
    json << "{\n";
    json << "  \"stats\": {\n";
    json << "    \"totalRegions\": " << m_impl->stats.totalRegions << ",\n";
    json << "    \"activeRegions\": " << m_impl->stats.activeRegions << ",\n";
    json << "    \"totalAccesses\": " << m_impl->stats.totalAccesses << ",\n";
    json << "    \"totalWrites\": " << m_impl->stats.totalWrites << ",\n";
    json << "    \"totalReads\": " << m_impl->stats.totalReads << ",\n";
    json << "    \"averageIntensity\": " << m_impl->stats.averageIntensity << "\n";
    json << "  },\n";
    json << "  \"regions\": [\n";
    
    bool first = true;
    for (const auto& [addr, region] : m_impl->regions) {
        if (!first) json << ",\n";
        first = false;
        
        json << "    {\n";
        json << "      \"baseAddress\": \"0x" << std::hex << region.baseAddress << std::dec << "\",\n";
        json << "      \"size\": " << region.size << ",\n";
        json << "      \"accessCount\": " << region.accessCount << ",\n";
        json << "      \"writeCount\": " << region.writeCount << ",\n";
        json << "      \"readCount\": " << region.readCount << ",\n";
        json << "      \"intensity\": " << region.intensity << "\n";
        json << "    }";
    }
    
    json << "\n  ]\n";
    json << "}\n";
    
    return json.str();
}

} // namespace killcore
