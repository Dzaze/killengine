#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <map>
#include <mutex>

namespace killcore {

/**
 * @brief Structure représentant une région mémoire avec son activité
 */
struct HeatmapRegion {
    uint64_t baseAddress;
    uint64_t size;
    uint32_t accessCount;
    uint32_t writeCount;
    uint32_t readCount;
    float intensity;  // 0.0 - 1.0
    std::chrono::steady_clock::time_point lastAccess;
};

/**
 * @brief Configuration de la collecte heatmap
 */
struct HeatmapConfig {
    uint32_t samplingIntervalMs = 100;      // Intervalle d'échantillonnage
    uint32_t regionSize = 4096;             // Taille d'une région (4KB par défaut)
    uint32_t maxRegions = 10000;            // Nombre max de régions
    bool trackReads = true;                 // Tracker les lectures
    bool trackWrites = true;                // Tracker les écritures
    bool trackExecutions = false;           // Non implémenté par ce collecteur passif (hash-diff par
                                             // page) : détecter une exécution demande une instrumentation
                                             // active (breakpoint matériel / page guard), pas juste relire
                                             // la mémoire. Conservé pour compat API future, ignoré pour l'instant.
    uint64_t minAddress = 0x10000;          // Adresse minimale
    uint64_t maxAddress = 0x7FFFFFFFFFFF;   // Adresse maximale (user space x64)
    // Corrigé (02/09/2026) : borne le nombre de pages relues+hashées PAR TICK,
    // quelle que soit la taille du process cible. Sans ça, sampleMemoryActivity()
    // relisait TOUTE la mémoire committée (potentiellement plusieurs Go sur un
    // vrai jeu) à chaque tick de samplingIntervalMs (100ms par défaut) — chaque
    // page = un ReadProcessMemory, donc des centaines de milliers de syscalls
    // par tick sur une cible réelle, largement hors budget. Le round-robin sur
    // maxPagesPerTick étale le balayage complet sur plusieurs ticks au lieu
    // d'essayer (et d'échouer) à tout faire en un seul. 4096 pages = 16 Mo par
    // tick à 4K, ce qui reste largement sous la seconde même dans le pire cas.
    uint32_t maxPagesPerTick = 4096;
};

/**
 * @brief Statistiques de la heatmap
 */
struct HeatmapStats {
    uint64_t totalRegions;
    uint64_t activeRegions;
    uint64_t totalAccesses;
    uint64_t totalWrites;
    uint64_t totalReads;
    float averageIntensity;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point lastUpdate;
};

/**
 * @brief Collecteur de données pour la heatmap mémoire
 * 
 * Cette classe échantillonne l'activité mémoire d'un processus cible
 * et construit une carte de chaleur des accès.
 */
class MemoryHeatmapCollector {
public:
    using HeatmapCallback = std::function<void(const std::vector<HeatmapRegion>&)>;

    explicit MemoryHeatmapCollector(HeatmapConfig config = {});
    ~MemoryHeatmapCollector();

    // Non-copyable
    MemoryHeatmapCollector(const MemoryHeatmapCollector&) = delete;
    MemoryHeatmapCollector& operator=(const MemoryHeatmapCollector&) = delete;

    /**
     * @brief Démarre la collecte sur un processus
     * @param processHandle Handle du processus cible
     * @return true si démarré avec succès
     */
    bool startCollection(void* processHandle);

    /**
     * @brief Arrête la collecte
     */
    void stopCollection();

    /**
     * @brief Vérifie si la collecte est active
     */
    bool isCollecting() const;

    /**
     * @brief Récupère les régions actives (top N par intensité)
     * @param maxRegions Nombre maximum de régions à retourner
     * @return Liste des régions triées par intensité décroissante
     */
    std::vector<HeatmapRegion> getTopRegions(uint32_t maxRegions = 100) const;

    /**
     * @brief Récupère les régions dans une plage d'adresses
     */
    std::vector<HeatmapRegion> getRegionsInRange(uint64_t start, uint64_t end) const;

    /**
     * @brief Récupère les statistiques globales
     */
    HeatmapStats getStats() const;

    /**
     * @brief Réinitialise les données collectées
     */
    void reset();

    /**
     * @brief Définit un callback pour les mises à jour en temps réel
     */
    void setUpdateCallback(HeatmapCallback callback);

    /**
     * @brief Génère une grille 2D pour la visualisation
     * @param width Largeur de la grille
     * @param height Hauteur de la grille
     * @return Grille d'intensités (0.0 - 1.0)
     */
    std::vector<float> generateGrid(uint32_t width, uint32_t height) const;

    /**
     * @brief Exporte les données au format JSON
     */
    std::string exportToJson() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killcore
