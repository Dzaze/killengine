#include "memory_timeline_manager.h"
#include "../core/visualization/memory_timeline_analyzer.h"
#include "../core/visualization/memory_timeline_collector.h"
#include "../core/logging/logger.h"
#include "localization/localization.h"
#include <QChar>
#include <QDebug>
#include <QMetaObject>
#include <QPointer>

#include <thread>

namespace killengine {

namespace {
QString bytesToHex(const std::vector<uint8_t>& value) {
    QString hexValue = "0x";
    for (auto it = value.rbegin(); it != value.rend(); ++it) {
        hexValue += QString("%1").arg(*it, 2, 16, QChar('0'));
    }
    return hexValue;
}

bool parseAddress(const QString& addressHex, uint64_t& address) {
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    bool ok = false;
    address = normalized.toULongLong(&ok, 16);
    return ok;
}

QString patternTypeToString(killcore::TimelinePattern::Type type) {
    switch (type) {
        case killcore::TimelinePattern::Type::Constant: return "constant";
        case killcore::TimelinePattern::Type::StepFunction: return "step_function";
        case killcore::TimelinePattern::Type::Linear: return "linear";
        case killcore::TimelinePattern::Type::Cyclic: return "cyclic";
        case killcore::TimelinePattern::Type::RandomWalk: return "random_walk";
        case killcore::TimelinePattern::Type::Correlated: return "correlated";
        case killcore::TimelinePattern::Type::AntiCheatPattern: return "anti_cheat_pattern";
        case killcore::TimelinePattern::Type::Unknown:
        default: return "unknown";
    }
}

QVariantMap patternToVariantMap(const killcore::TimelinePattern& pattern) {
    QVariantMap result;
    result["type"] = patternTypeToString(pattern.type);
    result["address"] = QString("0x%1").arg(pattern.address, 0, 16);
    result["confidence"] = pattern.confidence;
    result["description"] = QString::fromStdString(pattern.description);
    result["correlationScore"] = pattern.correlationScore;
    result["periodMs"] = static_cast<int>(pattern.periodMs);
    result["slope"] = pattern.slope;
    return result;
}

QVariantMap correlationToVariantMap(const killcore::TimelineCorrelation& correlation) {
    QVariantMap result;
    result["addressA"] = QString("0x%1").arg(correlation.addressA, 0, 16);
    result["addressB"] = QString("0x%1").arg(correlation.addressB, 0, 16);
    result["pearsonCoefficient"] = correlation.pearsonCoefficient;
    result["timeLagMs"] = correlation.timeLagMs;
    result["isLeading"] = correlation.isLeading;
    return result;
}
}

class MemoryTimelineManager::Impl {
public:
    std::unique_ptr<killcore::MemoryTimelineCollector> collector;
    std::unique_ptr<killcore::MemoryTimelineAnalyzer> analyzer;
    void* processHandle = nullptr;
    bool paused = false;

    Impl()
        : collector(std::make_unique<killcore::MemoryTimelineCollector>())
        , analyzer(std::make_unique<killcore::MemoryTimelineAnalyzer>()) {}
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
            pointData["valueHex"] = bytesToHex(point.value);
            
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
    // UX-PRODUIT-12 point 4 : compteur incrémental côté collecteur, plus de
    // copie de getSeries() (toutes séries + tous points) juste pour compter.
    stats["totalDataPoints"] = static_cast<qulonglong>(m_impl->collector->totalStoredPointCount());

    return stats;
}

bool MemoryTimelineManager::addAddress(const QString& addressHex, int valueSize) {
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) {
        KE_LOG_WARN() << "Invalid address hex: " << addressHex.toStdString();
        return false;
    }
    
    m_impl->collector->addAddress(address, static_cast<size_t>(valueSize));
    emit addressesChanged();
    return true;
}

bool MemoryTimelineManager::removeAddress(const QString& addressHex) {
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) return false;
    
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

namespace {
QString stopReasonToString(killcore::TimelineStopReason reason) {
    switch (reason) {
        case killcore::TimelineStopReason::DurationReached: return QStringLiteral("duration_reached");
        case killcore::TimelineStopReason::UserStop:
        default: return QStringLiteral("user_stop");
    }
}
} // namespace

void MemoryTimelineManager::stopCollection() {
    m_impl->collector->stopCollection();
    const QString reason = stopReasonToString(m_impl->collector->lastStopReason());
    emit collectingChanged();
    emit collectionFinished(reason);
}

void MemoryTimelineManager::stopCollectionAsync() {
    // UX-PRODUIT-12 : ne jamais bloquer le thread Qt sur le join() du thread
    // de collecte (jusqu'à un intervalle de sampling avant le fix côté
    // collecteur) -- utilisé par la garde attach/detach qui tourne, elle,
    // directement sur le thread Qt. L'arrêt réel est lancé sur un thread
    // séparé ; le résultat est marshalé en retour comme les scans async.
    if (!m_impl->collector->isCollecting()) {
        return;
    }
    auto* collector = m_impl->collector.get();
    QPointer<MemoryTimelineManager> self(this);
    std::thread([self, collector]() {
        collector->stopCollection();
        const auto reason = collector->lastStopReason();
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, reason]() {
            if (!self) {
                return;
            }
            emit self->collectingChanged();
            emit self->collectionFinished(stopReasonToString(reason));
        }, Qt::QueuedConnection);
    }).detach();
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
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) return QVariantMap();
    
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
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) return QVariantMap();
    
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
    QVariantMap result;
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) {
        result["success"] = false;
        result["error"] = KE_TXT("Adresse invalide", "Invalid address");
        return result;
    }

    const auto series = m_impl->collector->getSeriesForAddress(address);
    const auto patterns = m_impl->analyzer->detectPatterns(series);
    QVariantList patternList;
    for (const auto& pattern : patterns) {
        patternList.append(patternToVariantMap(pattern));
    }

    result["success"] = true;
    result["address"] = QString("0x%1").arg(address, 0, 16);
    result["patterns"] = patternList;
    return result;
}

QVariantList MemoryTimelineManager::findCorrelations() {
    std::vector<killcore::TimelineSeries> seriesList;
    const auto allSeries = m_impl->collector->getSeries();
    seriesList.reserve(allSeries.size());
    for (const auto& [address, series] : allSeries) {
        Q_UNUSED(address);
        seriesList.push_back(series);
    }

    QVariantList result;
    for (const auto& correlation : m_impl->analyzer->findCorrelations(seriesList)) {
        result.append(correlationToVariantMap(correlation));
    }
    return result;
}

QVariantMap MemoryTimelineManager::analyzeBehavior(const QString& addressHex) {
    QVariantMap result;
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) {
        result["success"] = false;
        result["error"] = KE_TXT("Adresse invalide", "Invalid address");
        return result;
    }

    const auto series = m_impl->collector->getSeriesForAddress(address);
    const auto profile = m_impl->analyzer->createBehaviorProfile(series);

    QVariantMap behavior;
    behavior["address"] = QString("0x%1").arg(profile.address, 0, 16);
    behavior["changesPerSecond"] = profile.changesPerSecond;
    behavior["regularityScore"] = profile.regularityScore;
    behavior["minValueHex"] = bytesToHex(profile.minValue);
    behavior["maxValueHex"] = bytesToHex(profile.maxValue);
    behavior["mostCommonValueHex"] = bytesToHex(profile.mostCommonValue);
    behavior["distinctValueCount"] = static_cast<int>(profile.distinctValueCount);
    behavior["typicalResponseTimeMs"] = static_cast<int>(profile.typicalResponseTimeMs);
    behavior["hasBurstBehavior"] = profile.hasBurstBehavior;

    result["success"] = true;
    result["behavior"] = behavior;
    return result;
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
    std::vector<killcore::TimelineSeries> seriesList;
    const auto allSeries = m_impl->collector->getSeries();
    seriesList.reserve(allSeries.size());
    for (const auto& [address, series] : allSeries) {
        Q_UNUSED(address);
        seriesList.push_back(series);
    }

    QVariantMap result;
    result["success"] = true;
    result["report"] = QString::fromStdString(m_impl->analyzer->generateAnalysisReport(seriesList));
    return result;
}

QVariantMap MemoryTimelineManager::predictNextValue(const QString& addressHex) {
    QVariantMap result;
    uint64_t address = 0;
    if (!parseAddress(addressHex, address)) {
        result["success"] = false;
        result["error"] = KE_TXT("Adresse invalide", "Invalid address");
        return result;
    }

    const auto series = m_impl->collector->getSeriesForAddress(address);
    const auto prediction = m_impl->analyzer->predictNextValue(series);
    result["success"] = true;
    result["address"] = QString("0x%1").arg(address, 0, 16);
    result["valueHex"] = bytesToHex(prediction);
    result["changeProbability"] = m_impl->analyzer->predictChangeProbability(series);
    return result;
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
        
        pointData["valueHex"] = bytesToHex(point.value);
        points.append(pointData);
    }
    result["points"] = points;

    return result;
}

} // namespace killengine
