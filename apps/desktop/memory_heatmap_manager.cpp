#include "memory_heatmap_manager.h"
#include "../core/visualization/memory_heatmap_collector.h"
#include "../core/logging/logger.h"
#include <QVariant>
#include <QString>

namespace killengine {

class MemoryHeatmapManager::Impl {
public:
    std::unique_ptr<killcore::MemoryHeatmapCollector> collector;
    std::function<void(const QVariantList&)> updateCallback;
};

MemoryHeatmapManager::MemoryHeatmapManager(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>()) {
}

MemoryHeatmapManager::~MemoryHeatmapManager() = default;

killcore::HeatmapConfig MemoryHeatmapManager::convertConfig(const QVariantMap& options) const {
    killcore::HeatmapConfig config;
    
    if (options.contains("samplingIntervalMs")) {
        config.samplingIntervalMs = options["samplingIntervalMs"].toUInt();
    }
    if (options.contains("regionSize")) {
        config.regionSize = options["regionSize"].toUInt();
    }
    if (options.contains("maxRegions")) {
        config.maxRegions = options["maxRegions"].toUInt();
    }
    if (options.contains("trackReads")) {
        config.trackReads = options["trackReads"].toBool();
    }
    if (options.contains("trackWrites")) {
        config.trackWrites = options["trackWrites"].toBool();
    }
    if (options.contains("trackExecutions")) {
        config.trackExecutions = options["trackExecutions"].toBool();
    }
    if (options.contains("minAddress")) {
        config.minAddress = options["minAddress"].toULongLong();
    }
    if (options.contains("maxAddress")) {
        config.maxAddress = options["maxAddress"].toULongLong();
    }
    
    return config;
}

bool MemoryHeatmapManager::startHeatmapCollection(quint64 processHandle, const QVariantMap& options) {
    if (m_impl->collector && m_impl->collector->isCollecting()) {
        emit heatmapError("Collection déjà en cours");
        return false;
    }
    
    auto config = convertConfig(options);
    m_impl->collector = std::make_unique<killcore::MemoryHeatmapCollector>(config);
    
    // Configurer le callback de mise à jour
    if (m_impl->updateCallback) {
        m_impl->collector->setUpdateCallback([this](const std::vector<killcore::HeatmapRegion>& regions) {
            QVariantList list;
            for (const auto& region : regions) {
                QVariantMap map;
                map["baseAddress"] = QString("0x%1").arg(region.baseAddress, 0, 16);
                map["size"] = static_cast<qint64>(region.size);
                map["accessCount"] = static_cast<int>(region.accessCount);
                map["writeCount"] = static_cast<int>(region.writeCount);
                map["readCount"] = static_cast<int>(region.readCount);
                map["intensity"] = region.intensity;
                list.append(map);
            }
            m_impl->updateCallback(list);
            emit regionsUpdated(list);
        });
    }
    
    void* handle = reinterpret_cast<void*>(processHandle);
    bool success = m_impl->collector->startCollection(handle);
    
    if (success) {
        emit collectingChanged(true);
        KE_LOG_INFO() << "MemoryHeatmapManager: Collection started";
    } else {
        emit heatmapError("Échec du démarrage de la collecte");
    }
    
    return success;
}

void MemoryHeatmapManager::stopHeatmapCollection() {
    if (m_impl->collector) {
        m_impl->collector->stopCollection();
        emit collectingChanged(false);
        KE_LOG_INFO() << "MemoryHeatmapManager: Collection stopped";
    }
}

bool MemoryHeatmapManager::isCollecting() const {
    return m_impl->collector && m_impl->collector->isCollecting();
}

QVariantList MemoryHeatmapManager::getTopHeatmapRegions(int maxRegions) const {
    QVariantList result;
    
    if (!m_impl->collector) {
        return result;
    }
    
    auto regions = m_impl->collector->getTopRegions(static_cast<uint32_t>(maxRegions));
    
    for (const auto& region : regions) {
        QVariantMap map;
        map["baseAddress"] = QString("0x%1").arg(region.baseAddress, 0, 16).toUpper();
        map["size"] = static_cast<qint64>(region.size);
        map["accessCount"] = static_cast<int>(region.accessCount);
        map["writeCount"] = static_cast<int>(region.writeCount);
        map["readCount"] = static_cast<int>(region.readCount);
        map["intensity"] = region.intensity;
        result.append(map);
    }
    
    return result;
}

QVariantList MemoryHeatmapManager::getHeatmapRegionsInRange(const QString& startHex, const QString& endHex) const {
    QVariantList result;
    
    if (!m_impl->collector) {
        return result;
    }
    
    bool ok;
    uint64_t start = startHex.toULongLong(&ok, 16);
    if (!ok) {
        // Erreur silencieuse en const - le caller doit valider
        return result;
    }
    
    uint64_t end = endHex.toULongLong(&ok, 16);
    if (!ok) {
        // Erreur silencieuse en const - le caller doit valider
        return result;
    }
    
    auto regions = m_impl->collector->getRegionsInRange(start, end);
    
    for (const auto& region : regions) {
        QVariantMap map;
        map["baseAddress"] = QString("0x%1").arg(region.baseAddress, 0, 16).toUpper();
        map["size"] = static_cast<qint64>(region.size);
        map["accessCount"] = static_cast<int>(region.accessCount);
        map["writeCount"] = static_cast<int>(region.writeCount);
        map["readCount"] = static_cast<int>(region.readCount);
        map["intensity"] = region.intensity;
        result.append(map);
    }
    
    return result;
}

QVariantMap MemoryHeatmapManager::getHeatmapStats() const {
    QVariantMap result;
    
    if (!m_impl->collector) {
        result["totalRegions"] = 0;
        result["activeRegions"] = 0;
        result["totalAccesses"] = 0;
        result["totalWrites"] = 0;
        result["totalReads"] = 0;
        result["averageIntensity"] = 0.0;
        result["collecting"] = false;
        return result;
    }
    
    auto stats = m_impl->collector->getStats();
    
    result["totalRegions"] = static_cast<qint64>(stats.totalRegions);
    result["activeRegions"] = static_cast<qint64>(stats.activeRegions);
    result["totalAccesses"] = static_cast<qint64>(stats.totalAccesses);
    result["totalWrites"] = static_cast<qint64>(stats.totalWrites);
    result["totalReads"] = static_cast<qint64>(stats.totalReads);
    result["averageIntensity"] = stats.averageIntensity;
    result["collecting"] = isCollecting();
    
    return result;
}

void MemoryHeatmapManager::resetHeatmap() {
    if (m_impl->collector) {
        m_impl->collector->reset();
        emit statsChanged(getHeatmapStats());
    }
}

QVariantList MemoryHeatmapManager::generateHeatmapGrid(int width, int height) const {
    QVariantList result;
    
    if (!m_impl->collector) {
        return result;
    }
    
    auto grid = m_impl->collector->generateGrid(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    
    for (float intensity : grid) {
        result.append(intensity);
    }
    
    return result;
}

QString MemoryHeatmapManager::exportHeatmapToJson() const {
    if (!m_impl->collector) {
        return "{}";
    }
    
    return QString::fromStdString(m_impl->collector->exportToJson());
}

void MemoryHeatmapManager::setUpdateCallback(std::function<void(const QVariantList&)> callback) {
    m_impl->updateCallback = std::move(callback);
}

} // namespace killengine
