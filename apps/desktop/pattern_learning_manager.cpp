#include "pattern_learning_manager.h"
#include "pattern_learning/pattern_learning_engine.h"
#include <QCoreApplication>
#include <QDir>
#include <QUuid>
#include <QDateTime>
#include <map>

namespace killengine {

// ============================================================================
// PatternLearningManager::Impl
// ============================================================================

class PatternLearningManager::Impl {
public:
    std::unique_ptr<killcore::PatternLearningEngine> engine;
    
    // Real-time tracking state
    struct TrackingState {
        std::vector<double> values;
        std::vector<int64_t> timestamps;
        std::string valueType;
        int64_t startTime;
    };
    std::map<std::string, TrackingState> tracking;
    
    QString getDatabasePath() {
        // Portabilite reelle (13/09/2026, docs/PORTABILITY_ROADMAP.md candidat P5) :
        // AppDataLocation (%APPDATA%) casse le mode portable comme P1/P2/P3/P4 --
        // deplacer/copier le dossier de l'app perdait silencieusement la base
        // Pattern Learning. Redirige vers un dossier relatif a l'executable.
        QString dataDir = QDir(QCoreApplication::applicationDirPath()).filePath("data");
        QDir().mkpath(dataDir);
        // ".json", pas ".db" : GameProfileDatabase est en JSON-in-file
        // (02/09/2026, Claude) — voir core/pattern_learning/game_profile_database.cpp.
        return dataDir + "/pattern_learning.json";
    }
};

// ============================================================================
// PatternLearningManager Implementation
// ============================================================================

PatternLearningManager::PatternLearningManager(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

PatternLearningManager::~PatternLearningManager() = default;

bool PatternLearningManager::initialize() {
    m_impl->engine = std::make_unique<killcore::PatternLearningEngine>(this);
    
    QString dbPath = m_impl->getDatabasePath();
    bool success = m_impl->engine->initialize(dbPath.toStdString());
    
    if (success) {
        // Connect signals
        connect(m_impl->engine.get(), &killcore::PatternLearningEngine::engineDetected,
                this, &PatternLearningManager::engineDetected);
        connect(m_impl->engine.get(), &killcore::PatternLearningEngine::patternClassified,
                this, &PatternLearningManager::patternClassified);
        connect(m_impl->engine.get(), &killcore::PatternLearningEngine::suggestionReady,
                this, &PatternLearningManager::suggestionReady);
    }
    
    return success;
}

bool PatternLearningManager::isInitialized() const {
    return m_impl->engine && m_impl->engine->isInitialized();
}

QVariantMap PatternLearningManager::getStatistics() const {
    if (!m_impl->engine) return QVariantMap();
    return m_impl->engine->getStatistics();
}

QVariantMap PatternLearningManager::detectEngine(const QVariantList& moduleNames,
                                                const QVariantMap& memorySample) {
    if (!m_impl->engine) return QVariantMap();
    
    // Convert module names
    std::vector<std::string> modules;
    for (const auto& m : moduleNames) {
        modules.push_back(m.toString().toStdString());
    }
    
    // Convert memory sample (if provided)
    std::vector<uint8_t> memory;
    if (memorySample.contains("data")) {
        QByteArray data = memorySample["data"].toByteArray();
        memory.assign(data.begin(), data.end());
    }
    
    auto result = m_impl->engine->detectEngine(modules, memory);
    return result.toVariantMap();
}

QVariantMap PatternLearningManager::classifyPattern(const QString& addressHex,
                                                   const QVariantList& valueHistory,
                                                   const QVariantList& timestamps) {
    if (!m_impl->engine) return QVariantMap();
    
    // Parse address
    bool ok;
    uint64_t address = addressHex.toULongLong(&ok, 16);
    if (!ok) return QVariantMap();
    
    // Convert value history
    std::vector<double> values;
    for (const auto& v : valueHistory) {
        values.push_back(v.toDouble());
    }
    
    // Convert timestamps
    std::vector<int64_t> times;
    for (const auto& t : timestamps) {
        times.push_back(t.toLongLong());
    }
    
    auto result = m_impl->engine->classifyPattern(address, values, times);
    return result.toVariantMap();
}

QVariantMap PatternLearningManager::loadProfile(const QString& gameName) {
    if (!m_impl->engine) return QVariantMap();
    
    auto profile = m_impl->engine->loadProfile(gameName.toStdString());
    if (profile) {
        return profile->toVariantMap();
    }
    return QVariantMap();
}

bool PatternLearningManager::saveProfile(const QVariantMap& profile) {
    if (!m_impl->engine) return false;
    
    auto p = killcore::GameProfile::fromVariantMap(profile);
    if (p) {
        return m_impl->engine->saveProfile(*p);
    }
    return false;
}

QVariantList PatternLearningManager::listKnownGames() {
    QVariantList games;
    if (!m_impl->engine) return games;
    
    auto list = m_impl->engine->listKnownGames();
    for (const auto& g : list) {
        games.append(QString::fromStdString(g));
    }
    return games;
}

bool PatternLearningManager::deleteProfile(const QString& gameName) {
    if (!m_impl->engine) return false;
    return m_impl->engine->deleteProfile(gameName.toStdString());
}

void PatternLearningManager::recordSession(const QVariantMap& session) {
    if (!m_impl->engine) return;
    
    killcore::LearningSession ls;
    ls.sessionId = session.value("sessionId", QUuid::createUuid().toString()).toString().toStdString();
    ls.gameName = session["gameName"].toString().toStdString();
    ls.timestamp = session.value("timestamp", QDateTime::currentDateTime().toMSecsSinceEpoch()).toLongLong();
    ls.resolutionPath = session["resolutionPath"].toString().toStdString();
    ls.wasSuccessful = session["wasSuccessful"].toBool();
    ls.durationMs = session["durationMs"].toLongLong();
    
    // Parse discovered patterns. Corrigé (02/09/2026, Claude) : cette boucle
    // ne faisait rien (commentaire "Would need to convert..." sans corps) —
    // discoveredPatterns n'était donc jamais réellement transmis à
    // GameProfileDatabase::recordSession, qui s'en sert pour alimenter le
    // taux de succès par type de valeur.
    QVariantList patterns = session["discoveredPatterns"].toList();
    for (const auto& p : patterns) {
        const QVariantMap pm = p.toMap();
        killcore::PatternClassification pc;
        pc.type = static_cast<killcore::PatternType>(pm.value("type", 0).toInt());
        pc.confidence = pm.value("confidence", 0.0).toDouble();
        pc.suggestedValueType = pm.value("suggestedValueType").toString().toStdString();
        pc.suggestedScale = pm.value("suggestedScale", 1.0).toDouble();
        const QStringList reasons = pm.value("reasoning").toStringList();
        for (const auto& r : reasons) {
            pc.reasoning.push_back(r.toStdString());
        }
        ls.discoveredPatterns.push_back(pc);
    }

    m_impl->engine->recordSession(ls);
}

QVariantList PatternLearningManager::suggestResolutionPaths(const QString& gameName,
                                                           int targetType) {
    QVariantList paths;
    if (!m_impl->engine) return paths;
    
    auto suggestions = m_impl->engine->suggestResolutionPaths(
        gameName.toStdString(),
        static_cast<killcore::PatternType>(targetType)
    );
    
    for (const auto& p : suggestions) {
        paths.append(QString::fromStdString(p));
    }
    return paths;
}

QVariantList PatternLearningManager::suggestValueTypes(int engineType, int patternType) {
    QVariantList types;
    if (!m_impl->engine) return types;
    
    auto suggestions = m_impl->engine->suggestValueTypes(
        static_cast<killcore::EngineType>(engineType),
        static_cast<killcore::PatternType>(patternType)
    );
    
    for (const auto& t : suggestions) {
        types.append(QString::fromStdString(t));
    }
    return types;
}

double PatternLearningManager::getTypeSuccessRate(const QString& gameName,
                                                 const QString& valueType) {
    if (!m_impl->engine) return 0.0;
    return m_impl->engine->getTypeSuccessRate(
        gameName.toStdString(),
        valueType.toStdString()
    );
}

QVariantList PatternLearningManager::clusterAddresses(const QVariantList& addresses,
                                                     const QVariantList& features) {
    QVariantList clusters;
    if (!m_impl->engine) return clusters;
    
    // Convert addresses
    std::vector<uint64_t> addrs;
    for (const auto& a : addresses) {
        bool ok;
        addrs.push_back(a.toString().toULongLong(&ok, 16));
    }
    
    // Convert features (list of feature vectors)
    std::vector<std::vector<double>> feats;
    for (const auto& f : features) {
        std::vector<double> vec;
        for (const auto& v : f.toList()) {
            vec.push_back(v.toDouble());
        }
        feats.push_back(vec);
    }
    
    auto result = m_impl->engine->clusterAddresses(addrs, feats);
    
    for (const auto& c : result) {
        QVariantMap cluster;
        cluster["id"] = c.id;
        cluster["label"] = QString::fromStdString(c.label);
        
        QVariantList addrs;
        for (uint64_t a : c.addresses) {
            addrs.append(QString::number(a, 16));
        }
        cluster["addresses"] = addrs;
        cluster["dominantType"] = static_cast<int>(c.dominantType);
        
        clusters.append(cluster);
    }
    
    return clusters;
}

void PatternLearningManager::startPatternTracking(const QString& addressHex,
                                                 const QString& valueType) {
    std::string addr = addressHex.toStdString();
    
    Impl::TrackingState state;
    state.valueType = valueType.toStdString();
    state.startTime = QDateTime::currentDateTime().toMSecsSinceEpoch();
    
    m_impl->tracking[addr] = std::move(state);
}

void PatternLearningManager::stopPatternTracking(const QString& addressHex) {
    m_impl->tracking.erase(addressHex.toStdString());
}

void PatternLearningManager::recordValue(const QString& addressHex, double value) {
    std::string addr = addressHex.toStdString();
    auto it = m_impl->tracking.find(addr);
    if (it == m_impl->tracking.end()) return;
    
    it->second.values.push_back(value);
    it->second.timestamps.push_back(QDateTime::currentDateTime().toMSecsSinceEpoch());
    
    // Keep only last 100 values
    if (it->second.values.size() > 100) {
        it->second.values.erase(it->second.values.begin());
        it->second.timestamps.erase(it->second.timestamps.begin());
    }
    
    // Emit update if we have enough data
    if (it->second.values.size() >= 5) {
        emit trackingUpdated(addressHex, getPatternAnalysis(addressHex));
    }
}

QVariantMap PatternLearningManager::getPatternAnalysis(const QString& addressHex) {
    QVariantMap analysis;
    
    std::string addr = addressHex.toStdString();
    auto it = m_impl->tracking.find(addr);
    if (it == m_impl->tracking.end()) return analysis;
    
    analysis["address"] = addressHex;
    analysis["sampleCount"] = static_cast<int>(it->second.values.size());
    analysis["valueType"] = QString::fromStdString(it->second.valueType);
    
    if (it->second.values.size() >= 3 && m_impl->engine) {
        auto classification = m_impl->engine->classifyPattern(
            std::stoull(addr, nullptr, 16),
            it->second.values,
            it->second.timestamps
        );
        analysis["classification"] = classification.toVariantMap();
    }
    
    return analysis;
}

QVariantMap PatternLearningManager::analyzeCandidates(const QVariantList& candidates) {
    QVariantMap result;
    if (!m_impl->engine) return result;
    
    // Group candidates by pattern type
    std::map<int, int> typeCounts;
    std::map<int, QVariantList> typeCandidates;
    
    for (const auto& c : candidates) {
        QVariantMap candidate = c.toMap();
        QString address = candidate["address"].toString();
        QVariantList history = candidate["history"].toList();
        
        if (history.isEmpty()) continue;
        
        // Convert history
        std::vector<double> values;
        for (const auto& h : history) {
            values.push_back(h.toDouble());
        }
        
        // Classify
        bool ok;
        uint64_t addr = address.toULongLong(&ok, 16);
        if (!ok) continue;
        
        auto classification = m_impl->engine->classifyPattern(addr, values, {});
        
        int type = static_cast<int>(classification.type);
        typeCounts[type]++;
        typeCandidates[type].append(candidate);
    }
    
    // Build result
    result["totalAnalyzed"] = static_cast<int>(candidates.size());
    
    QVariantMap distribution;
    for (const auto& [type, count] : typeCounts) {
        distribution[QString::number(type)] = count;
    }
    result["typeDistribution"] = distribution;
    
    // Find dominant type
    int dominantType = 0;
    int maxCount = 0;
    for (const auto& [type, count] : typeCounts) {
        if (count > maxCount) {
            maxCount = count;
            dominantType = type;
        }
    }
    result["dominantType"] = dominantType;
    result["dominantTypeCount"] = maxCount;
    result["groupedCandidates"] = typeCandidates[dominantType];
    
    return result;
}

QVariantList PatternLearningManager::getTopSuggestions(const QString& gameName,
                                                      int patternType,
                                                      int count) {
    QVariantList suggestions;
    if (!m_impl->engine) return suggestions;
    
    // Load profile
    auto profile = m_impl->engine->loadProfile(gameName.toStdString());
    if (!profile) return suggestions;
    
    // Filter offsets by pattern type
    std::vector<killcore::KnownOffset> matchingOffsets;
    for (const auto& ko : profile->knownOffsets) {
        if (static_cast<int>(ko.patternType) == patternType) {
            matchingOffsets.push_back(ko);
        }
    }
    
    // Sort by stability score
    std::sort(matchingOffsets.begin(), matchingOffsets.end(),
              [](const auto& a, const auto& b) {
                  return a.stabilityScore > b.stabilityScore;
              });
    
    // Return top N
    int limit = std::min(count, static_cast<int>(matchingOffsets.size()));
    for (int i = 0; i < limit; ++i) {
        QVariantMap offset;
        offset["name"] = QString::fromStdString(matchingOffsets[i].name);
        offset["address"] = QString::number(matchingOffsets[i].offset, 16);
        offset["valueType"] = QString::fromStdString(matchingOffsets[i].valueType);
        offset["scale"] = matchingOffsets[i].scale;
        offset["stabilityScore"] = matchingOffsets[i].stabilityScore;
        suggestions.append(offset);
    }
    
    return suggestions;
}

} // namespace killengine
