#include "candidate_comparison_manager.h"
#include "../core/visualization/candidate_comparison_collector.h"
#include "../core/visualization/candidate_comparison_decoder.h"
#include "../core/logging/logger.h"
#include "localization/localization.h"

#include <QMetaObject>
#include <QPointer>

#include <thread>

namespace killengine {

namespace {

QString stopReasonToString(killcore::ComparisonStopReason reason) {
    switch (reason) {
        case killcore::ComparisonStopReason::UserStop: return QStringLiteral("user_stop");
        case killcore::ComparisonStopReason::DurationReached: return QStringLiteral("duration_reached");
        case killcore::ComparisonStopReason::TargetLost: return QStringLiteral("target_lost");
    }
    return QStringLiteral("user_stop");
}

bool parseAddress(const QString& addressHex, uint64_t* out) {
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    bool ok = false;
    *out = normalized.toULongLong(&ok, 16);
    return ok;
}

QVariantMap pointToVariantMap(const killcore::ComparisonPoint& point, killcore::ValueType type, double factor) {
    QVariantMap result;
    result["batchId"] = static_cast<qulonglong>(point.batchId);
    result["timestampMs"] = static_cast<qulonglong>(point.timestampMs);
    result["isValid"] = point.isValid;
    if (!point.isValid) {
        return result;
    }
    const auto decoded = killcore::decodeComparisonValue(point.rawBytes, type);
    result["ok"] = decoded.ok;
    if (!decoded.ok) {
        return result;
    }
    result["exactValueText"] = decoded.exactValueText;
    result["numericValue"] = decoded.numericValue;
    result["isNaN"] = decoded.isNaN;
    result["isInfinite"] = decoded.isInfinite;
    bool exact = false;
    result["scaledValueText"] = killcore::formatScaledValueText(decoded, type, factor, &exact);
    result["scaledValueExact"] = exact;
    return result;
}

QVariantMap correlationToVariantMap(const killcore::ComparisonPairwiseCorrelation& c) {
    QVariantMap result;
    result["seriesIdA"] = c.seriesIdA;
    result["seriesIdB"] = c.seriesIdB;
    result["computable"] = c.computable;
    result["coefficient"] = c.coefficient;
    result["pairCount"] = c.pairCount;
    result["reason"] = c.reason;
    return result;
}

QVariantMap markerToVariantMap(const killcore::ComparisonMarker& marker) {
    QVariantMap result;
    result["id"] = static_cast<qulonglong>(marker.id);
    result["timestampMs"] = static_cast<qulonglong>(marker.timestampMs);
    result["text"] = marker.text;
    return result;
}

} // namespace

class CandidateComparisonManager::Impl {
public:
    std::unique_ptr<killcore::CandidateComparisonCollector> collector;
    void* processHandle{nullptr};

    Impl() : collector(std::make_unique<killcore::CandidateComparisonCollector>()) {}
};

CandidateComparisonManager::CandidateComparisonManager(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>()) {
    // AUDIT-PIPE-A2 : source unique de la notification de fin, câblée une
    // seule fois ici plutôt que dupliquée dans stopCollection()/
    // stopCollectionAsync() -- voir CandidateComparisonCollector::
    // setFinishedCallback pour la garantie "exactement une fois, toute voie
    // de sortie confondue". Appelé depuis le thread de capture : jamais
    // toucher Qt directement, toujours marshaler via QueuedConnection, et ne
    // capturer `this` qu'au travers d'un QPointer (le manager peut être
    // détruit pendant qu'une capture encore active tourne en arrière-plan).
    QPointer<CandidateComparisonManager> self(this);
    m_impl->collector->setFinishedCallback([self](killcore::ComparisonStopReason reason) {
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(self.data(), [self, reason]() {
            if (!self) {
                return;
            }
            emit self->comparisonFinished(stopReasonToString(reason));
        }, Qt::QueuedConnection);
    });
}

CandidateComparisonManager::~CandidateComparisonManager() = default;

void CandidateComparisonManager::setProcessHandle(void* handle) {
    m_impl->processHandle = handle;
}

void* CandidateComparisonManager::processHandle() const {
    return m_impl->processHandle;
}

QVariantMap CandidateComparisonManager::startComparison(const QVariantList& series, const QVariantMap& options) {
    QVariantMap result;

    std::vector<killcore::ComparisonSeriesConfig> configs;
    configs.reserve(static_cast<size_t>(series.size()));
    for (const auto& entry : series) {
        const QVariantMap map = entry.toMap();
        killcore::ComparisonSeriesConfig cfg;
        cfg.id = map.value("id").toString();
        cfg.label = map.value("label").toString();
        if (!parseAddress(map.value("address").toString(), &cfg.address)) {
            result["success"] = false;
            result["error"] = KE_TXT("Adresse invalide dans la sélection de comparaison.",
                                      "Invalid address in the comparison selection.");
            return result;
        }
        if (!killcore::parseValueType(map.value("type").toString(), &cfg.type)) {
            result["success"] = false;
            result["error"] = KE_TXT("Type de valeur invalide dans la sélection de comparaison.",
                                      "Invalid value type in the comparison selection.");
            return result;
        }
        cfg.factor = map.value("factor", 1.0).toDouble();
        if (!(cfg.factor > 0.0)) {
            cfg.factor = 1.0;
        }
        configs.push_back(cfg);
    }

    const uint32_t intervalMs = static_cast<uint32_t>(options.value("intervalMs", 100).toUInt());
    const uint32_t maxDurationMs = static_cast<uint32_t>(options.value("maxDurationMs", 30000).toUInt());

    if (!m_impl->collector->configure(configs, intervalMs, maxDurationMs)) {
        result["success"] = false;
        result["error"] = KE_TXT(
            "Configuration de comparaison invalide (2 à 6 séries, identifiants uniques requis, ou capture déjà active).",
            "Invalid comparison configuration (2 to 6 series, unique ids required, or a capture is already active).");
        return result;
    }

    if (!m_impl->processHandle) {
        result["success"] = false;
        result["error"] = KE_TXT("Aucun processus attaché.", "No process attached.");
        return result;
    }

    const bool started = m_impl->collector->startCollection(m_impl->processHandle);
    result["success"] = started;
    if (!started) {
        result["error"] = KE_TXT("Impossible de démarrer la capture de comparaison.",
                                  "Unable to start the comparison capture.");
    }
    return result;
}

void CandidateComparisonManager::stopCollection() {
    m_impl->collector->stopCollection();
}

void CandidateComparisonManager::stopCollectionAsync() {
    // Même motif que MemoryTimelineManager::stopCollectionAsync : ne jamais
    // bloquer le thread Qt sur le join() du thread de capture -- utilisé par
    // la garde attach/detach qui tourne directement sur le thread Qt.
    // AUDIT-PIPE-A2 : plus besoin d'émettre ici -- le callback de fin câblé
    // dans le constructeur (setFinishedCallback) s'en charge déjà, depuis le
    // thread de capture lui-même, pour cette voie comme pour toutes les
    // autres (stop synchrone, sortie naturelle).
    if (!m_impl->collector->isCollecting()) {
        return;
    }
    auto* collector = m_impl->collector.get();
    std::thread([collector]() {
        collector->stopCollection();
    }).detach();
}

bool CandidateComparisonManager::isCollecting() const {
    return m_impl->collector->isCollecting();
}

QVariantMap CandidateComparisonManager::getStatus() const {
    QVariantMap result;
    result["collecting"] = m_impl->collector->isCollecting();
    result["stopReason"] = stopReasonToString(m_impl->collector->lastStopReason());
    result["skippedTicks"] = static_cast<qulonglong>(m_impl->collector->skippedTickCount());
    result["tourCount"] = static_cast<qulonglong>(m_impl->collector->tourTimings().size());

    QVariantList seriesList;
    for (const auto& cfg : m_impl->collector->seriesConfigs()) {
        QVariantMap s;
        s["id"] = cfg.id;
        s["address"] = QString("0x%1").arg(cfg.address, 0, 16);
        s["type"] = killcore::valueTypeToString(cfg.type);
        s["factor"] = cfg.factor;
        s["label"] = cfg.label;
        s["pointCount"] = static_cast<qulonglong>(m_impl->collector->pointCountForSeries(cfg.id));
        seriesList.append(s);
    }
    result["series"] = seriesList;

    QVariantList markerList;
    for (const auto& marker : m_impl->collector->markers()) {
        markerList.append(markerToVariantMap(marker));
    }
    result["markers"] = markerList;
    return result;
}

QVariantMap CandidateComparisonManager::getSamples(const QString& seriesId, int offset, int limit) const {
    QVariantMap result;
    killcore::ValueType type = killcore::ValueType::Int32;
    double factor = 1.0;
    bool found = false;
    for (const auto& cfg : m_impl->collector->seriesConfigs()) {
        if (cfg.id == seriesId) {
            type = cfg.type;
            factor = cfg.factor;
            found = true;
            break;
        }
    }
    if (!found) {
        result["success"] = false;
        result["error"] = KE_TXT("Série de comparaison inconnue.", "Unknown comparison series.");
        return result;
    }

    const size_t safeOffset = offset < 0 ? 0 : static_cast<size_t>(offset);
    const size_t safeLimit = limit < 0 ? 0 : static_cast<size_t>(limit);
    const auto points = m_impl->collector->pointsForSeries(seriesId, safeOffset, safeLimit);

    QVariantList pointList;
    pointList.reserve(static_cast<qsizetype>(points.size()));
    for (const auto& point : points) {
        pointList.append(pointToVariantMap(point, type, factor));
    }

    result["success"] = true;
    result["points"] = pointList;
    result["totalCount"] = static_cast<qulonglong>(m_impl->collector->pointCountForSeries(seriesId));
    return result;
}

QVariantMap CandidateComparisonManager::getCorrelations() const {
    QVariantMap result;
    QVariantList list;
    for (const auto& c : m_impl->collector->correlations()) {
        list.append(correlationToVariantMap(c));
    }
    result["correlations"] = list;
    return result;
}

QVariantMap CandidateComparisonManager::addMarker(const QString& text) {
    QVariantMap result;
    killcore::ComparisonMarker marker;
    const bool added = m_impl->collector->addMarker(text, &marker);
    result["success"] = added;
    if (added) {
        result["id"] = static_cast<qulonglong>(marker.id);
        result["timestampMs"] = static_cast<qulonglong>(marker.timestampMs);
    } else {
        result["error"] = KE_TXT(
            "Repère refusé (capture arrêtée, texte vide/trop long, ou 100 repères déjà atteints).",
            "Marker refused (capture stopped, empty/too long text, or 100 markers already reached).");
    }
    return result;
}

bool CandidateComparisonManager::exportToJson(const QString& filepath) const {
    return m_impl->collector->exportToJson(filepath.toStdString());
}

} // namespace killengine
