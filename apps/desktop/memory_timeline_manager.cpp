#include "memory_timeline_manager.h"
#include "../core/visualization/memory_timeline_collector.h"
#include "../core/logging/logger.h"
#include <QDebug>

namespace killengine {

namespace {
QVariantMap notImplementedResult(const QString& feature) {
    QVariantMap result;
    result["success"] = false;
    result["error"] = QString(
        "%1 non implémenté — MemoryTimelineAnalyzer (core/visualization/"
        "memory_timeline_analyzer.h) déclare l'API mais n'a pas de .cpp. "
        "Voir docs/PHASE_TRACKER.md \"Memory Timeline\"."
    ).arg(feature);
    return result;
}
}

class MemoryTimelineManager::Impl {
public:
    std::unique_ptr<killcore::MemoryTimelineCollector> collector;
    void* processHandle = nullptr;
    bool paused = false;

    Impl() : collector(std::make_unique<killcore::MemoryTimelineCollector>()) {}
};

MemoryTimelineManager::MemoryTimelineManager(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>()) {
    setupCallbacks();
}

MemoryTimelineManager::~MemoryTimelineManager() = default;

void MemoryTimelineManager::setupCallbacks() {
    // Progress callback
    m_impl->collector->setProgressCallback(
        [this](uint32_t percent, const std::string& status) {
            emit collectionProgress(static_cast<int>(percent), QString::fromStdString(status));
        }
    );
    
    // Data callback
    m_impl->collector->setDataCallback(
        [this](const killcore::TimelineDataPoint& point) {
            QVariantMap pointData;
            pointData["timestampMs"] = static_cast<qint64>(point.timestampMs);
            pointData["isValid"] = point.isValid;
            
            // Convertir la valeur en hex string
            QString hexValue = "0x";
            for (auto it = point.value.rbegin(); it != point.value.rend(); ++it) {
                hexValue += QString("%1").arg(*it, 2, 16, QChar('0'));
            }
            pointData["valueHex"] = hexValue;
            
            QString addressHex = QString("0x%1").arg(point.address, 0, 16);
            emit dataPointReceived(addressHex, pointData);
        }
    );
}

bool MemoryTimelineManager::isCollecting() const {
    return m_impl->collector->isCollecting();
}

int MemoryTimelineManager::watchedAddressCount() const {
    return static_cast<int>(m_impl->collector->watchedAddresses().size());
}

QVariantMap MemoryTimelineManager::currentStats() const {
    QVariantMap stats;
    stats["isCollecting"] = isCollecting();
    stats["watchedAddressCount"] = watchedAddressCount();
    
    auto series = m_impl->collector->getSeries();
    int totalPoints = 0;
    for (const auto& [addr, s] : series) {
        totalPoints += static_cast<int>(s.points.size());
    }
    stats["totalDataPoints"] = totalPoints;
    
    return stats;
}

bool MemoryTimelineManager::addAddress(const QString& addressHex, int valueSize) {
    bool ok;
    uint64_t address = addressHex.toULongLong(&ok, 16);
    if (!ok) {
        KE_LOG_WARN() << "Invalid address hex: " << addressHex.toStdString();
        return false;
    }
    
    m_impl->collector->addAddress(address, static_cast<size_t>(valueSize));
    emit addressesChanged();
    return true;
}

bool MemoryTimelineManager::removeAddress(const QString& addressHex) {
    bool ok;
    uint64_t address = addressHex.toULongLong(&ok, 16);
    if (!ok) return false;
    
    m_impl->collector->removeAddress(address);
    emit addressesChanged();
    return true;
}

void MemoryTimelineManager::clearAddresses() {
    m_impl->collector->clearAddresses();
    emit addressesChanged();
}

QVariantList MemoryTimelineManager::getWatchedAddresses() const {
    QVariantList result;
    for (uint64_t addr : m_impl->collector->watchedAddresses()) {
        result.append(QString("0x%1").arg(addr, 0, 16));
    }
    return result;
}

bool MemoryTimelineManager::startCollection() {
    if (!m_impl->processHandle) {
        KE_LOG_WARN() << "No process handle set for timeline collection";
        return false;
    }
    
    if (m_impl->collector->watchedAddresses().empty()) {
        KE_LOG_WARN() << "No addresses to watch";
        return false;
    }
    
    bool started = m_impl->collector->startCollection(m_impl->processHandle);
    if (started) {
        emit collectingChanged();
    }
    return started;
}

void MemoryTimelineManager::stopCollection() {
    m_impl->collector->stopCollection();
    emit collectingChanged();
    emit collectionFinished();
}

void MemoryTimelineManager::pauseCollection() {
    m_impl->paused = true;
    // Note: Le collector n'a pas de pause native, on pourrait l'ajouter
}

void MemoryTimelineManager::resumeCollection() {
    m_impl->paused = false;
}

void MemoryTimelineManager::setSamplingInterval(int intervalMs) {
    auto config = m_impl->collector->config();
    config.samplingIntervalMs = static_cast<uint32_t>(intervalMs);
    m_impl->collector->setConfig(config);
}

void MemoryTimelineManager::setMaxDuration(int durationMs) {
    auto config = m_impl->collector->config();
    config.maxDurationMs = static_cast<uint32_t>(durationMs);
    m_impl->collector->setConfig(config);
}

void MemoryTimelineManager::setTrackOnlyChanges(bool trackOnly) {
    auto config = m_impl->collector->config();
    config.trackOnlyChanges = trackOnly;
    m_impl->collector->setConfig(config);
}

QVariantMap MemoryTimelineManager::getConfig() const {
    auto config = m_impl->collector->config();
    QVariantMap result;
    result["samplingIntervalMs"] = static_cast<int>(config.samplingIntervalMs);
    result["maxDurationMs"] = static_cast<int>(config.maxDurationMs);
    result["maxPointsPerSeries"] = static_cast<int>(config.maxPointsPerSeries);
    result["trackOnlyChanges"] = config.trackOnlyChanges;
    result["calculateStatistics"] = config.calculateStatistics;
    return result;
}

QVariantMap MemoryTimelineManager::getSeriesForAddress(const QString& addressHex) const {
    bool ok;
    uint64_t address = addressHex.toULongLong(&ok, 16);
    if (!ok) return QVariantMap();
    
    auto series = m_impl->collector->getSeriesForAddress(address);
    return seriesToVariantMap(series);
}

QVariantList MemoryTimelineManager::getAllSeries() const {
    QVariantList result;
    auto series = m_impl->collector->getSeries();
    for (const auto& [addr, s] : series) {
        result.append(seriesToVariantMap(s));
    }
    return result;
}

QVariantMap MemoryTimelineManager::getSeriesStats(const QString& addressHex) const {
    bool ok;
    uint64_t address = addressHex.toULongLong(&ok, 16);
    if (!ok) return QVariantMap();
    
    auto series = m_impl->collector->getSeriesForAddress(address);
    
    QVariantMap stats;
    stats["address"] = addressHex;
    stats["valueSize"] = static_cast<int>(series.valueSize);
    stats["changeCount"] = static_cast<int>(series.changeCount);
    stats["firstChangeMs"] = static_cast<qint64>(series.firstChangeMs);
    stats["lastChangeMs"] = static_cast<qint64>(series.lastChangeMs);
    stats["averageIntervalMs"] = series.averageIntervalMs;
    stats["volatilityScore"] = series.volatilityScore;
    
    return stats;
}

QVariantMap MemoryTimelineManager::detectPatterns(const QString& addressHex) {
    Q_UNUSED(addressHex);
    return notImplementedResult("Détection de patterns");
}

QVariantList MemoryTimelineManager::findCorrelations() {
    // TODO: Implémenter
    return QVariantList();
}

QVariantMap MemoryTimelineManager::analyzeBehavior(const QString& addressHex) {
    Q_UNUSED(addressHex);
    return notImplementedResult("Profil comportemental");
}

QVariantList MemoryTimelineManager::findVolatileAddresses(double threshold) {
    auto addresses = m_impl->collector->findVolatileAddresses(threshold);
    QVariantList result;
    for (uint64_t addr : addresses) {
        result.append(QString("0x%1").arg(addr, 0, 16));
    }
    return result;
}

QVariantList MemoryTimelineManager::findStableAddresses(int minDurationMs) {
    auto addresses = m_impl->collector->findStableAddresses(static_cast<uint32_t>(minDurationMs));
    QVariantList result;
    for (uint64_t addr : addresses) {
        result.append(QString("0x%1").arg(addr, 0, 16));
    }
    return result;
}

bool MemoryTimelineManager::exportToJson(const QString& filepath) {
    return m_impl->collector->exportToJson(filepath.toStdString());
}

bool MemoryTimelineManager::exportToCsv(const QString& filepath) {
    return m_impl->collector->exportToCsv(filepath.toStdString());
}

QVariantMap MemoryTimelineManager::generateReport() {
    return notImplementedResult("Génération de rapport");
}

QVariantMap MemoryTimelineManager::predictNextValue(const QString& addressHex) {
    Q_UNUSED(addressHex);
    return notImplementedResult("Prédiction de valeur");
}

void MemoryTimelineManager::setProcessHandle(void* handle) {
    m_impl->processHandle = handle;
}

void* MemoryTimelineManager::processHandle() const {
    return m_impl->processHandle;
}

QVariantMap MemoryTimelineManager::seriesToVariantMap(const killcore::TimelineSeries& series) const {
    QVariantMap result;
    result["address"] = QString("0x%1").arg(series.baseAddress, 0, 16);
    result["valueSize"] = static_cast<int>(series.valueSize);
    result["changeCount"] = static_cast<int>(series.changeCount);
    result["volatilityScore"] = series.volatilityScore;
    result["averageIntervalMs"] = series.averageIntervalMs;
    
    QVariantList points;
    for (const auto& point : series.points) {
        QVariantMap pointData;
        pointData["timestampMs"] = static_cast<qint64>(point.timestampMs);
        pointData["isValid"] = point.isValid;
        
        QString hexValue = "0x";
        for (auto it = point.value.rbegin(); it != point.value.rend(); ++it) {
            hexValue += QString("%1").arg(*it, 2, 16, QChar('0'));
        }
        pointData["valueHex"] = hexValue;
        points.append(pointData);
    }
    result["points"] = points;

    return result;
}

} // namespace killengine
