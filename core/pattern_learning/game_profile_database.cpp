#include "game_profile_database.h"

#include "logging/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QVariantMap>
#include <QVariantList>

#include <algorithm>
#include <map>

// Réécrit en JSON-in-file (02/09/2026, Claude) : la version d'origine
// utilisait SQLite (#include <sqlite3.h>), une dépendance qui n'existe nulle
// part ailleurs dans ce projet (pas dans le CMake, pas vendorée) — jamais
// détecté car ce fichier n'était pas non plus enregistré dans
// core/CMakeLists.txt. Reste cohérent avec le reste du codebase
// (profile_store.cpp et consorts, QJsonDocument/QFile) plutôt que d'ajouter
// la première dépendance SQL du projet. API publique (game_profile_database.h)
// inchangée — aucun appelant (pattern_learning_engine.cpp,
// pattern_learning_manager.cpp) n'a besoin d'être modifié.
//
// Format du fichier JSON (un seul document) :
//   { "profiles": { "<gameName>": <GameProfile::toVariantMap()> },
//     "sessions": [ <LearningSession...> ],
//     "successRates": { "<gameName>|<valueType>": {successCount,totalCount} } }
//
// successRates complète au passage un trou de l'original : la table SQL
// type_success_rates existait dans le schéma mais rien n'écrivait jamais
// dedans (getSuccessRate() aurait toujours renvoyé 0.0) — recordSession()
// alimente maintenant ce compteur pour chaque pattern découvert.

namespace killcore {

namespace {

PatternClassification patternClassificationFromVariantMap(const QVariantMap& map) {
    PatternClassification p;
    p.type = static_cast<PatternType>(map.value("type", 0).toInt());
    p.confidence = map.value("confidence", 0.0).toDouble();
    p.suggestedValueType = map.value("suggestedValueType").toString().toStdString();
    p.suggestedScale = map.value("suggestedScale", 1.0).toDouble();
    const QStringList reasons = map.value("reasoning").toStringList();
    for (const auto& r : reasons) {
        p.reasoning.push_back(r.toStdString());
    }
    return p;
}

QVariantMap learningSessionToVariantMap(const LearningSession& s) {
    QVariantMap map;
    map["sessionId"] = QString::fromStdString(s.sessionId);
    map["gameName"] = QString::fromStdString(s.gameName);
    map["timestamp"] = static_cast<qlonglong>(s.timestamp);
    QVariantList patterns;
    for (const auto& p : s.discoveredPatterns) {
        patterns.append(p.toVariantMap());
    }
    map["discoveredPatterns"] = patterns;
    map["resolutionPath"] = QString::fromStdString(s.resolutionPath);
    map["wasSuccessful"] = s.wasSuccessful;
    map["durationMs"] = static_cast<qlonglong>(s.durationMs);
    return map;
}

LearningSession learningSessionFromVariantMap(const QVariantMap& map) {
    LearningSession s;
    s.sessionId = map.value("sessionId").toString().toStdString();
    s.gameName = map.value("gameName").toString().toStdString();
    s.timestamp = map.value("timestamp", 0).toLongLong();
    const QVariantList patterns = map.value("discoveredPatterns").toList();
    for (const auto& p : patterns) {
        s.discoveredPatterns.push_back(patternClassificationFromVariantMap(p.toMap()));
    }
    s.resolutionPath = map.value("resolutionPath").toString().toStdString();
    s.wasSuccessful = map.value("wasSuccessful", false).toBool();
    s.durationMs = map.value("durationMs", 0).toLongLong();
    return s;
}

} // namespace

class GameProfileDatabase::Impl {
public:
    struct SuccessCounter {
        int successCount = 0;
        int totalCount = 0;
    };

    QString filePath;
    bool opened = false;
    std::map<std::string, GameProfile> profiles;       // clé = gameName
    std::vector<LearningSession> sessions;              // ordre d'insertion
    std::map<std::string, SuccessCounter> successRates; // clé = "gameName|valueType"

    static std::string successKey(const std::string& gameName, const std::string& valueType) {
        return gameName + "|" + valueType;
    }

    bool load() {
        profiles.clear();
        sessions.clear();
        successRates.clear();

        QFile file(filePath);
        if (!file.exists()) {
            return true; // Pas encore de fichier : base vide valide (premier lancement).
        }
        if (!file.open(QIODevice::ReadOnly)) {
            KE_LOG_ERROR() << "GameProfileDatabase: cannot open for reading: " << filePath.toStdString();
            return false;
        }
        const QByteArray raw = file.readAll();
        file.close();
        if (raw.trimmed().isEmpty()) {
            return true;
        }

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            // PORT-3c : ne jamais remplacer silencieusement un JSON illisible
            // par une base vide -- open() propage cet échec à l'appelant
            // (PatternLearningEngine::initialize() renvoie false) au lieu de
            // réussir avec une base vide qui masquerait la corruption.
            KE_LOG_ERROR() << "GameProfileDatabase: invalid JSON in " << filePath.toStdString()
                           << ": " << err.errorString().toStdString();
            return false;
        }
        const QVariantMap root = doc.object().toVariantMap();

        const QVariantMap profilesMap = root.value("profiles").toMap();
        for (auto it = profilesMap.constBegin(); it != profilesMap.constEnd(); ++it) {
            const auto profile = GameProfile::fromVariantMap(it.value().toMap());
            if (profile) {
                profiles[it.key().toStdString()] = *profile;
            }
        }

        const QVariantList sessionsList = root.value("sessions").toList();
        for (const auto& s : sessionsList) {
            sessions.push_back(learningSessionFromVariantMap(s.toMap()));
        }

        const QVariantMap ratesMap = root.value("successRates").toMap();
        for (auto it = ratesMap.constBegin(); it != ratesMap.constEnd(); ++it) {
            const QVariantMap rm = it.value().toMap();
            SuccessCounter counter;
            counter.successCount = rm.value("successCount", 0).toInt();
            counter.totalCount = rm.value("totalCount", 0).toInt();
            successRates[it.key().toStdString()] = counter;
        }

        return true;
    }

    bool persist() const {
        QVariantMap profilesMap;
        for (const auto& entry : profiles) {
            profilesMap[QString::fromStdString(entry.first)] = entry.second.toVariantMap();
        }

        QVariantList sessionsList;
        for (const auto& s : sessions) {
            sessionsList.append(learningSessionToVariantMap(s));
        }

        QVariantMap ratesMap;
        for (const auto& entry : successRates) {
            QVariantMap rm;
            rm["successCount"] = entry.second.successCount;
            rm["totalCount"] = entry.second.totalCount;
            ratesMap[QString::fromStdString(entry.first)] = rm;
        }

        QVariantMap root;
        root["profiles"] = profilesMap;
        root["sessions"] = sessionsList;
        root["successRates"] = ratesMap;

        // PORT-3c (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : QFile en
        // WriteOnly|Truncate écrase le fichier existant AVANT que le contenu
        // ne soit écrit -- une écriture qui échoue ensuite (disque plein,
        // process tué) perdait la dernière base valide, sans aucun moyen de
        // la récupérer. QSaveFile écrit dans un fichier temporaire et ne
        // remplace l'original qu'au commit() réussi (même patron que
        // core/profiles/profile_store.cpp) : un échec laisse l'ancien fichier
        // intact. `written == encoded.size()` (pas seulement `>= 0`) détecte
        // aussi une écriture partielle que write() rapporterait comme un
        // nombre positif mais incomplet.
        QDir().mkpath(QFileInfo(filePath).absolutePath());
        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            KE_LOG_ERROR() << "GameProfileDatabase: cannot open for writing: " << filePath.toStdString();
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromVariant(root);
        const QByteArray encoded = doc.toJson(QJsonDocument::Indented);
        const qint64 written = file.write(encoded);
        if (written != encoded.size()) {
            KE_LOG_ERROR() << "GameProfileDatabase: partial write (" << written << "/" << encoded.size()
                           << " bytes): " << filePath.toStdString();
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            KE_LOG_ERROR() << "GameProfileDatabase: commit failed: " << filePath.toStdString()
                           << " (" << file.errorString().toStdString() << ")";
            return false;
        }
        return true;
    }
};

GameProfileDatabase::GameProfileDatabase() : m_impl(std::make_unique<Impl>()) {
}

GameProfileDatabase::~GameProfileDatabase() {
    close();
}

bool GameProfileDatabase::open(const std::string& dbPath) {
    close();
    m_impl->filePath = QString::fromStdString(dbPath);
    if (!m_impl->load()) {
        return false;
    }
    m_impl->opened = true;
    return true;
}

void GameProfileDatabase::close() {
    m_impl->opened = false;
}

bool GameProfileDatabase::isOpen() const {
    return m_impl->opened;
}

bool GameProfileDatabase::saveProfile(const GameProfile& profile) {
    if (!m_impl->opened) return false;
    m_impl->profiles[profile.gameName] = profile;
    return m_impl->persist();
}

std::optional<GameProfile> GameProfileDatabase::loadProfile(const std::string& gameName) {
    if (!m_impl->opened) return std::nullopt;
    const auto it = m_impl->profiles.find(gameName);
    if (it == m_impl->profiles.end()) return std::nullopt;
    return it->second;
}

bool GameProfileDatabase::deleteProfile(const std::string& gameName) {
    if (!m_impl->opened) return false;
    if (m_impl->profiles.erase(gameName) == 0) return false;
    return m_impl->persist();
}

std::vector<std::string> GameProfileDatabase::listGames() {
    std::vector<std::string> games;
    if (!m_impl->opened) return games;
    games.reserve(m_impl->profiles.size());
    for (const auto& entry : m_impl->profiles) {
        games.push_back(entry.first);
    }
    // Plus récent d'abord — même convention que "ORDER BY last_updated DESC" côté SQL d'origine.
    std::sort(games.begin(), games.end(), [this](const std::string& a, const std::string& b) {
        return m_impl->profiles.at(a).lastUpdated > m_impl->profiles.at(b).lastUpdated;
    });
    return games;
}

bool GameProfileDatabase::recordSession(const LearningSession& session) {
    if (!m_impl->opened) return false;
    m_impl->sessions.push_back(session);

    for (const auto& pattern : session.discoveredPatterns) {
        if (pattern.suggestedValueType.empty()) continue;
        auto& counter = m_impl->successRates[Impl::successKey(session.gameName, pattern.suggestedValueType)];
        counter.totalCount++;
        if (session.wasSuccessful) counter.successCount++;
    }

    return m_impl->persist();
}

std::vector<LearningSession> GameProfileDatabase::getSessions(const std::string& gameName, int limit) {
    std::vector<LearningSession> matching;
    if (!m_impl->opened) return matching;

    for (const auto& s : m_impl->sessions) {
        if (s.gameName == gameName) matching.push_back(s);
    }
    // Plus récent d'abord — même convention que "ORDER BY timestamp DESC LIMIT" côté SQL d'origine.
    std::sort(matching.begin(), matching.end(), [](const LearningSession& a, const LearningSession& b) {
        return a.timestamp > b.timestamp;
    });
    if (limit >= 0 && static_cast<int>(matching.size()) > limit) {
        matching.resize(static_cast<size_t>(limit));
    }
    return matching;
}

double GameProfileDatabase::getSuccessRate(const std::string& gameName, const std::string& valueType) {
    if (!m_impl->opened) return 0.0;
    const auto it = m_impl->successRates.find(Impl::successKey(gameName, valueType));
    if (it == m_impl->successRates.end() || it->second.totalCount == 0) return 0.0;
    return static_cast<double>(it->second.successCount) / it->second.totalCount;
}

int GameProfileDatabase::getSessionCount(const std::string& gameName) {
    if (!m_impl->opened) return 0;
    return static_cast<int>(std::count_if(
        m_impl->sessions.begin(), m_impl->sessions.end(),
        [&](const LearningSession& s) { return s.gameName == gameName; }));
}

bool GameProfileDatabase::vacuum() {
    // Pas de notion de "vacuum" pour un fichier JSON réécrit en entier à
    // chaque persist() — no-op réussi tant que la base est ouverte.
    return m_impl->opened;
}

bool GameProfileDatabase::clearOldSessions(int64_t olderThanTimestamp) {
    if (!m_impl->opened) return false;
    const auto before = m_impl->sessions.size();
    m_impl->sessions.erase(
        std::remove_if(m_impl->sessions.begin(), m_impl->sessions.end(),
            [&](const LearningSession& s) { return s.timestamp < olderThanTimestamp; }),
        m_impl->sessions.end());
    if (m_impl->sessions.size() == before) return true;
    return m_impl->persist();
}

} // namespace killcore
