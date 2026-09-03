#include "pattern_learning_engine.h"
#include "game_profile_database.h"
#include "feature_extractor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <random>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

namespace killcore {

// Déclarations avancées (02/09/2026, Claude) : ces deux fonctions sont
// définies plus bas dans ce fichier mais utilisées par des méthodes définies
// avant leur définition textuelle (EngineDetectionResult::toVariantMap(),
// etc.) — en C++, contrairement aux méthodes de classe, une fonction libre
// doit être déclarée avant son premier usage dans la même unité de
// compilation. Bug pré-existant, jamais détecté car ce fichier n'était pas
// compilé.
static std::string engineTypeToString(EngineType type);
static std::string patternTypeToString(PatternType type);

// ============================================================================
// EngineDetectionResult Implementation
// ============================================================================

QVariantMap EngineDetectionResult::toVariantMap() const {
    QVariantMap map;
    map["type"] = static_cast<int>(type);
    map["typeName"] = QString::fromStdString(engineTypeToString(type));
    map["version"] = QString::fromStdString(version);
    map["confidence"] = confidence;
    map["signature"] = QString::fromStdString(signature);
    return map;
}

EngineDetectionResult EngineDetectionResult::fromVariantMap(const QVariantMap& map) {
    EngineDetectionResult result;
    result.type = static_cast<EngineType>(map.value("type", 0).toInt());
    result.version = map.value("version").toString().toStdString();
    result.confidence = map.value("confidence", 0.0).toDouble();
    result.signature = map.value("signature").toString().toStdString();
    return result;
}

// ============================================================================
// PatternClassification Implementation
// ============================================================================

QVariantMap PatternClassification::toVariantMap() const {
    QVariantMap map;
    map["type"] = static_cast<int>(type);
    map["typeName"] = QString::fromStdString(patternTypeToString(type));
    map["confidence"] = confidence;
    map["suggestedValueType"] = QString::fromStdString(suggestedValueType);
    map["suggestedScale"] = suggestedScale;
    
    QStringList reasons;
    for (const auto& r : reasoning) {
        reasons.append(QString::fromStdString(r));
    }
    map["reasoning"] = reasons;
    return map;
}

// ============================================================================
// GameProfile Implementation
// ============================================================================

QVariantMap GameProfile::toVariantMap() const {
    QVariantMap map;
    map["gameName"] = QString::fromStdString(gameName);
    map["executableName"] = QString::fromStdString(executableName);
    map["engineType"] = static_cast<int>(engineType);
    map["engineTypeName"] = QString::fromStdString(engineTypeToString(engineType));
    map["engineVersion"] = QString::fromStdString(engineVersion);
    map["sessionCount"] = sessionCount;
    map["lastUpdated"] = static_cast<qlonglong>(lastUpdated);
    
    QVariantList offsets;
    for (const auto& ko : knownOffsets) {
        QVariantMap o;
        o["name"] = QString::fromStdString(ko.name);
        o["offset"] = static_cast<qlonglong>(ko.offset);
        o["valueType"] = QString::fromStdString(ko.valueType);
        o["scale"] = ko.scale;
        o["patternType"] = static_cast<int>(ko.patternType);
        o["stabilityScore"] = ko.stabilityScore;
        offsets.append(o);
    }
    map["knownOffsets"] = offsets;
    
    QStringList paths;
    for (const auto& p : successfulPaths) {
        paths.append(QString::fromStdString(p));
    }
    map["successfulPaths"] = paths;
    
    return map;
}

std::optional<GameProfile> GameProfile::fromVariantMap(const QVariantMap& map) {
    GameProfile profile;
    profile.gameName = map.value("gameName").toString().toStdString();
    profile.executableName = map.value("executableName").toString().toStdString();
    profile.engineType = static_cast<EngineType>(map.value("engineType", 0).toInt());
    profile.engineVersion = map.value("engineVersion").toString().toStdString();
    profile.sessionCount = map.value("sessionCount", 0).toInt();
    profile.lastUpdated = map.value("lastUpdated", 0).toLongLong();
    
    QVariantList offsets = map.value("knownOffsets").toList();
    for (const auto& o : offsets) {
        QVariantMap om = o.toMap();
        KnownOffset ko;
        ko.name = om.value("name").toString().toStdString();
        ko.offset = static_cast<uint64_t>(om.value("offset").toLongLong());
        ko.valueType = om.value("valueType").toString().toStdString();
        ko.scale = om.value("scale", 1.0).toDouble();
        ko.patternType = static_cast<PatternType>(om.value("patternType", 0).toInt());
        ko.stabilityScore = om.value("stabilityScore", 0.0).toDouble();
        profile.knownOffsets.push_back(ko);
    }
    
    QStringList paths = map.value("successfulPaths").toStringList();
    for (const auto& p : paths) {
        profile.successfulPaths.push_back(p.toStdString());
    }
    
    return profile;
}

// ============================================================================
// Helper Functions
// ============================================================================

static std::string engineTypeToString(EngineType type) {
    switch (type) {
        case EngineType::Unity: return "Unity";
        case EngineType::UnrealEngine: return "Unreal Engine";
        case EngineType::Godot: return "Godot";
        case EngineType::Custom: return "Custom";
        default: return "Unknown";
    }
}

// Ajoutée (02/09/2026, Claude) : appelée depuis detectEngine() mais jamais
// définie dans le code d'origine — erreur de compilation (identificateur
// introuvable), jamais détectée car ce fichier n'était pas enregistré dans
// core/CMakeLists.txt. Heuristique simple : cherche un motif "X.Y" ou
// "X.Y.Z(.W)" dans une fenêtre bornée après le pattern moteur trouvé — pas
// une garantie (les versions embarquées ne suivent pas toujours ce format),
// mais réel plutôt qu'un stub qui planterait ou inventerait une valeur.
static std::string extractVersionFromMemory(const std::string& memStr, const std::string& afterPattern) {
    const auto pos = memStr.find(afterPattern);
    if (pos == std::string::npos) return {};

    const size_t windowStart = pos;
    const size_t windowEnd = std::min(memStr.size(), pos + afterPattern.size() + 256);
    const std::string window = memStr.substr(windowStart, windowEnd - windowStart);

    static const std::regex versionPattern(R"(\d+\.\d+(\.\d+)?(\.\d+)?)");
    std::smatch match;
    if (std::regex_search(window, match, versionPattern)) {
        return match.str();
    }
    return {};
}

static std::string patternTypeToString(PatternType type) {
    switch (type) {
        case PatternType::ResourceCounter: return "ResourceCounter";
        case PatternType::HealthPool: return "HealthPool";
        case PatternType::StateFlag: return "StateFlag";
        case PatternType::Timer: return "Timer";
        case PatternType::Coordinate: return "Coordinate";
        case PatternType::UIReference: return "UIReference";
        case PatternType::Noise: return "Noise";
        default: return "Unknown";
    }
}

// ============================================================================
// PatternLearningEngine::Impl
// ============================================================================

class PatternLearningEngine::Impl {
public:
    std::unique_ptr<GameProfileDatabase> database;
    std::unique_ptr<FeatureExtractor> featureExtractor;
    bool initialized = false;
    
    // Engine detection signatures
    struct EngineSignature {
        EngineType type;
        std::vector<std::string> modulePatterns;
        std::vector<std::string> memoryPatterns;
        double baseConfidence;
    };
    
    std::vector<EngineSignature> engineSignatures = {
        {
            EngineType::Unity,
            {"UnityPlayer.dll", "mono.dll", "mono-2.0-bdwgc.dll"},
            {"UnityEngine", "MonoBehaviour", "GameObject"},
            0.85
        },
        {
            EngineType::UnrealEngine,
            {"UE4Editor.dll", "UnrealEditor.dll", "Core.dll"},
            {"UEngine", "UObject", "AActor"},
            0.90
        },
        {
            EngineType::Godot,
            {"godot.dll", "godot-cpp.dll"},
            {"GDScript", "Node", "SceneTree"},
            0.80
        }
    };
    
    // Pattern classification rules
    struct PatternRule {
        PatternType type;
        std::function<bool(const std::vector<double>&, const std::vector<int64_t>&)> matcher;
        std::string suggestedValueType;
        double suggestedScale;
    };
    
    std::vector<PatternRule> patternRules;
    
    Impl() {
        initializePatternRules();
    }
    
    void initializePatternRules() {
        // Resource counter: values that increase/decrease gradually
        patternRules.push_back({
            PatternType::ResourceCounter,
            [](const std::vector<double>& values, const std::vector<int64_t>&) {
                if (values.size() < 3) return false;
                // Check for gradual changes (not instant jumps)
                int gradualChanges = 0;
                for (size_t i = 1; i < values.size(); ++i) {
                    double diff = std::abs(values[i] - values[i-1]);
                    if (diff > 0 && diff < values[i-1] * 0.5) gradualChanges++;
                }
                return gradualChanges >= static_cast<int>(values.size() * 0.3);
            },
            "Int32", 1.0
        });
        
        // Health pool: values between 0-100 or 0-1000, decreasing then stable
        patternRules.push_back({
            PatternType::HealthPool,
            [](const std::vector<double>& values, const std::vector<int64_t>&) {
                if (values.empty()) return false;
                double maxVal = *std::max_element(values.begin(), values.end());
                return (maxVal <= 100.0 || (maxVal <= 1000.0 && maxVal > 100.0));
            },
            "Float32", 1.0
        });
        
        // State flag: binary values (0/1 or very few distinct values)
        patternRules.push_back({
            PatternType::StateFlag,
            [](const std::vector<double>& values, const std::vector<int64_t>&) {
                std::set<double> uniqueValues(values.begin(), values.end());
                return uniqueValues.size() <= 3;
            },
            "Int32", 1.0
        });
        
        // Timer: monotonically increasing
        patternRules.push_back({
            PatternType::Timer,
            [](const std::vector<double>& values, const std::vector<int64_t>&) {
                if (values.size() < 2) return false;
                int increasing = 0;
                for (size_t i = 1; i < values.size(); ++i) {
                    if (values[i] >= values[i-1]) increasing++;
                }
                return increasing >= static_cast<int>(values.size() * 0.9);
            },
            "Int32", 1.0
        });
    }
};

// ============================================================================
// PatternLearningEngine Implementation
// ============================================================================

PatternLearningEngine::PatternLearningEngine(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

PatternLearningEngine::~PatternLearningEngine() = default;

bool PatternLearningEngine::initialize(const std::string& dbPath) {
    m_impl->database = std::make_unique<GameProfileDatabase>();
    if (!m_impl->database->open(dbPath)) {
        return false;
    }
    
    m_impl->featureExtractor = std::make_unique<FeatureExtractor>();
    m_impl->initialized = true;
    return true;
}

bool PatternLearningEngine::isInitialized() const {
    return m_impl->initialized;
}

EngineDetectionResult PatternLearningEngine::detectEngine(
    const std::vector<std::string>& moduleNames,
    const std::vector<uint8_t>& memorySample) {
    
    EngineDetectionResult result;
    result.type = EngineType::Unknown;
    result.confidence = 0.0;
    
    if (!m_impl->initialized) return result;
    
    // Check module names against signatures
    for (const auto& sig : m_impl->engineSignatures) {
        int matches = 0;
        for (const auto& pattern : sig.modulePatterns) {
            for (const auto& module : moduleNames) {
                if (module.find(pattern) != std::string::npos) {
                    matches++;
                    result.signature = pattern;
                    break;
                }
            }
        }
        
        if (matches > 0) {
            double matchRatio = static_cast<double>(matches) / sig.modulePatterns.size();
            double confidence = sig.baseConfidence * (0.5 + 0.5 * matchRatio);
            
            if (confidence > result.confidence) {
                result.type = sig.type;
                result.confidence = confidence;
            }
        }
    }
    
    // Check memory patterns
    if (result.type != EngineType::Unknown && !memorySample.empty()) {
        std::string memStr(reinterpret_cast<const char*>(memorySample.data()),
                          std::min(memorySample.size(), size_t(4096)));
        
        for (const auto& sig : m_impl->engineSignatures) {
            if (sig.type != result.type) continue;
            
            for (const auto& memPattern : sig.memoryPatterns) {
                if (memStr.find(memPattern) != std::string::npos) {
                    result.confidence = std::min(0.99, result.confidence + 0.05);
                    result.version = extractVersionFromMemory(memStr, memPattern);
                    break;
                }
            }
        }
    }
    
    emit engineDetected(QString::fromStdString(result.signature), result.toVariantMap());
    return result;
}

PatternClassification PatternLearningEngine::classifyPattern(
    uint64_t address,
    const std::vector<double>& valueHistory,
    const std::vector<int64_t>& timestamps) {
    
    PatternClassification classification;
    classification.type = PatternType::Unknown;
    classification.confidence = 0.0;
    classification.suggestedValueType = "Int32";
    classification.suggestedScale = 1.0;
    
    if (!m_impl->initialized || valueHistory.empty()) return classification;
    
    // Try each pattern rule
    double bestConfidence = 0.0;
    for (const auto& rule : m_impl->patternRules) {
        if (rule.matcher(valueHistory, timestamps)) {
            // Calculate confidence based on how well it matches
            double confidence = 0.7; // Base confidence
            
            // Boost confidence for clear patterns
            if (rule.type == PatternType::StateFlag && valueHistory.size() >= 5) {
                std::set<double> unique(valueHistory.begin(), valueHistory.end());
                if (unique.size() == 2) confidence = 0.95;
            }
            
            if (confidence > bestConfidence) {
                bestConfidence = confidence;
                classification.type = rule.type;
                classification.confidence = confidence;
                classification.suggestedValueType = rule.suggestedValueType;
                classification.suggestedScale = rule.suggestedScale;
                classification.reasoning.push_back("Pattern matches " + patternTypeToString(rule.type));
            }
        }
    }
    
    // Extract additional features
    auto features = m_impl->featureExtractor->extract(valueHistory);
    classification.reasoning.push_back("Entropy: " + std::to_string(features.entropy));
    classification.reasoning.push_back("Variance: " + std::to_string(features.variance));
    
    emit patternClassified(QString::number(address, 16), classification.toVariantMap());
    return classification;
}

std::optional<GameProfile> PatternLearningEngine::loadProfile(const std::string& gameName) {
    if (!m_impl->initialized) return std::nullopt;
    return m_impl->database->loadProfile(gameName);
}

bool PatternLearningEngine::saveProfile(const GameProfile& profile) {
    if (!m_impl->initialized) return false;
    return m_impl->database->saveProfile(profile);
}

std::vector<std::string> PatternLearningEngine::listKnownGames() const {
    if (!m_impl->initialized) return {};
    return m_impl->database->listGames();
}

bool PatternLearningEngine::deleteProfile(const std::string& gameName) {
    if (!m_impl->initialized) return false;
    return m_impl->database->deleteProfile(gameName);
}

void PatternLearningEngine::recordSession(const LearningSession& session) {
    if (!m_impl->initialized) return;
    
    // Update profile with session data
    auto profileOpt = loadProfile(session.gameName);
    GameProfile profile;
    
    if (profileOpt) {
        profile = *profileOpt;
    } else {
        profile.gameName = session.gameName;
    }
    
    profile.sessionCount++;
    profile.lastUpdated = session.timestamp;
    
    if (session.wasSuccessful && !session.resolutionPath.empty()) {
        // Add unique resolution path
        auto it = std::find(profile.successfulPaths.begin(), 
                           profile.successfulPaths.end(), 
                           session.resolutionPath);
        if (it == profile.successfulPaths.end()) {
            profile.successfulPaths.push_back(session.resolutionPath);
        }
    }
    
    saveProfile(profile);
    
    // Record session in database
    m_impl->database->recordSession(session);
}

std::vector<std::string> PatternLearningEngine::suggestResolutionPaths(
    const std::string& gameName,
    PatternType targetType) {
    
    if (!m_impl->initialized) return {};
    
    auto profileOpt = loadProfile(gameName);
    if (!profileOpt) return {};
    
    return profileOpt->successfulPaths;
}

std::vector<std::string> PatternLearningEngine::suggestValueTypes(
    EngineType engine,
    PatternType pattern) {
    
    std::vector<std::string> suggestions;
    
    // Engine-specific suggestions
    switch (engine) {
        case EngineType::Unity:
            if (pattern == PatternType::ResourceCounter) {
                suggestions = {"Int32", "Int32x100", "Float32"};
            } else if (pattern == PatternType::HealthPool) {
                suggestions = {"Float32", "Int32"};
            }
            break;
            
        case EngineType::UnrealEngine:
            if (pattern == PatternType::ResourceCounter) {
                suggestions = {"Int32", "Float32"};
            } else if (pattern == PatternType::HealthPool) {
                suggestions = {"Float32", "Int32x10"};
            }
            break;
            
        default:
            suggestions = {"Int32", "Float32", "Int64"};
    }
    
    QStringList qSuggestions;
    for (const auto& s : suggestions) {
        qSuggestions.append(QString::fromStdString(s));
    }
    emit suggestionReady("value_types", qSuggestions);
    
    return suggestions;
}

double PatternLearningEngine::getTypeSuccessRate(
    const std::string& gameName,
    const std::string& valueType) {
    
    if (!m_impl->initialized) return 0.0;
    return m_impl->database->getSuccessRate(gameName, valueType);
}

std::vector<PatternLearningEngine::Cluster> PatternLearningEngine::clusterAddresses(
    const std::vector<uint64_t>& addresses,
    const std::vector<std::vector<double>>& features) {
    
    std::vector<Cluster> clusters;
    if (addresses.empty() || features.empty()) return clusters;
    
    // Simple k-means clustering (k=3 for Resource/Health/Other)
    const int k = 3;
    std::vector<int> assignments(addresses.size(), 0);
    
    // Initialize centroids randomly
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, addresses.size() - 1);
    
    std::vector<std::vector<double>> centroids;
    for (int i = 0; i < k; ++i) {
        centroids.push_back(features[dis(gen)]);
    }
    
    // Iterate
    for (int iter = 0; iter < 10; ++iter) {
        // Assign to nearest centroid
        for (size_t i = 0; i < addresses.size(); ++i) {
            double minDist = std::numeric_limits<double>::max();
            for (int c = 0; c < k; ++c) {
                double dist = 0;
                for (size_t f = 0; f < features[i].size() && f < centroids[c].size(); ++f) {
                    dist += std::pow(features[i][f] - centroids[c][f], 2);
                }
                if (dist < minDist) {
                    minDist = dist;
                    assignments[i] = c;
                }
            }
        }
        
        // Update centroids
        for (int c = 0; c < k; ++c) {
            std::vector<double> newCentroid(features[0].size(), 0);
            int count = 0;
            for (size_t i = 0; i < addresses.size(); ++i) {
                if (assignments[i] == c) {
                    for (size_t f = 0; f < features[i].size(); ++f) {
                        newCentroid[f] += features[i][f];
                    }
                    count++;
                }
            }
            if (count > 0) {
                for (auto& v : newCentroid) v /= count;
                centroids[c] = newCentroid;
            }
        }
    }
    
    // Build clusters
    for (int c = 0; c < k; ++c) {
        Cluster cluster;
        cluster.id = c;
        
        // Determine dominant type
        std::map<PatternType, int> typeCounts;
        for (size_t i = 0; i < addresses.size(); ++i) {
            if (assignments[i] == c) {
                cluster.addresses.push_back(addresses[i]);
            }
        }
        
        // Label based on size
        if (cluster.addresses.size() < addresses.size() / 6) {
            cluster.label = "Small Cluster " + std::to_string(c);
        } else if (cluster.addresses.size() < addresses.size() / 3) {
            cluster.label = "Medium Cluster " + std::to_string(c);
        } else {
            cluster.label = "Large Cluster " + std::to_string(c);
        }
        
        clusters.push_back(cluster);
    }
    
    return clusters;
}

QVariantMap PatternLearningEngine::getStatistics() const {
    QVariantMap stats;
    if (!m_impl->initialized) return stats;
    
    stats["initialized"] = true;
    stats["knownGames"] = static_cast<int>(listKnownGames().size());
    stats["engineSignaturesLoaded"] = static_cast<int>(m_impl->engineSignatures.size());
    stats["patternRulesLoaded"] = static_cast<int>(m_impl->patternRules.size());
    
    return stats;
}

} // namespace killcore
