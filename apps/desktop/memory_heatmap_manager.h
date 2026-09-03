#pragma once

#include <QObject>
#include <QVariantMap>
#include <QVariantList>
#include <memory>
#include <functional>

// Forward declaration
namespace killcore {
    class MemoryHeatmapCollector;
    struct HeatmapConfig;
}

namespace killengine {

/**
 * @brief Manager Qt pour le Memory Heatmap
 * 
 * Cette classe fait le pont entre le collecteur C++ et l'interface QML/Vue.
 * Elle expose les méthodes nécessaires via Q_INVOKABLE.
 */
class MemoryHeatmapManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool collecting READ isCollecting NOTIFY collectingChanged)
    Q_PROPERTY(QVariantMap stats READ getStats NOTIFY statsChanged)

public:
    explicit MemoryHeatmapManager(QObject* parent = nullptr);
    ~MemoryHeatmapManager() override;

    // Non-copyable
    MemoryHeatmapManager(const MemoryHeatmapManager&) = delete;
    MemoryHeatmapManager& operator=(const MemoryHeatmapManager&) = delete;

    /**
     * @brief Démarre la collecte de heatmap sur le processus attaché
     * @param processHandle Handle natif du processus (void* casté en quint64)
     * @param options Options de configuration (intervalle, taille région, etc.)
     * @return true si démarré avec succès
     */
    Q_INVOKABLE bool startHeatmapCollection(quint64 processHandle, const QVariantMap& options = QVariantMap());

    /**
     * @brief Arrête la collecte
     */
    Q_INVOKABLE void stopHeatmapCollection();

    /**
     * @brief Vérifie si la collecte est active
     */
    Q_INVOKABLE bool isCollecting() const;

    /**
     * @brief Récupère les régions les plus actives
     * @param maxRegions Nombre maximum de régions
     * @return Liste des régions
     */
    Q_INVOKABLE QVariantList getTopHeatmapRegions(int maxRegions = 100) const;

    /**
     * @brief Récupère les régions dans une plage d'adresses
     */
    Q_INVOKABLE QVariantList getHeatmapRegionsInRange(const QString& startHex, const QString& endHex) const;

    /**
     * @brief Récupère les statistiques globales
     */
    Q_INVOKABLE QVariantMap getHeatmapStats() const;

    /**
     * @brief Récupère les statistiques (pour la propriété QML)
     */
    QVariantMap getStats() const { return getHeatmapStats(); }

    /**
     * @brief Réinitialise les données collectées
     */
    Q_INVOKABLE void resetHeatmap();

    /**
     * @brief Génère une grille 2D pour visualisation
     * @param width Largeur de la grille
     * @param height Hauteur de la grille
     * @return Liste d'intensités (0.0 - 1.0)
     */
    Q_INVOKABLE QVariantList generateHeatmapGrid(int width, int height) const;

    /**
     * @brief Exporte les données au format JSON
     */
    Q_INVOKABLE QString exportHeatmapToJson() const;

    /**
     * @brief Configure le callback de mise à jour en temps réel
     * @param callback Fonction à appeler à chaque mise à jour
     */
    void setUpdateCallback(std::function<void(const QVariantList&)> callback);

signals:
    void collectingChanged(bool collecting);
    void statsChanged(const QVariantMap& stats);
    void regionsUpdated(const QVariantList& regions);
    void heatmapError(const QString& error);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    // Convertit la configuration Qt en configuration C++
    killcore::HeatmapConfig convertConfig(const QVariantMap& options) const;
};

} // namespace killengine
