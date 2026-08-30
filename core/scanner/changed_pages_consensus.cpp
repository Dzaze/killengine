// PHASE 250 (SC2 solarite investigation, 30/08/2026) : logique pure de
// consensus multi-rounds pour les sessions changed-pages diff. Voir
// changed_pages_consensus.h pour la motivation (copies UI volatiles qui
// pourrissent les hits d'un diff one-shot avant qu'on puisse les verifier).

#include "scanner/changed_pages_consensus.h"

#include <algorithm>

namespace killcore {

namespace {

bool parseHexAddress(const QString& text, uint64_t* address) {
    if (!address) {
        return false;
    }
    QString normalized = text.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok) {
        return false;
    }
    *address = parsed;
    return true;
}

constexpr int kMaxContradictionRounds = 2;
constexpr int kMaxStaleRounds = 4;

} // namespace

QString ChangedPagesConsensus::entryKey(uint64_t address, ValueType type, const QString& variantLabel) {
    return QString("%1|%2|%3")
        .arg(address, 0, 16)
        .arg(valueTypeToString(type))
        .arg(variantLabel);
}

void ChangedPagesConsensus::applyRound(const QVariantList& hits) {
    ++m_rounds;
    for (const auto& hit : hits) {
        const QVariantMap map = hit.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(map.value("address").toString(), &address)) {
            continue;
        }
        killcore::ValueType type = killcore::ValueType::Int32;
        if (!killcore::parseValueType(map.value("type").toString(), &type)) {
            continue;
        }
        const QString variantLabel = map.value("variantLabel").toString();
        const QString key = entryKey(address, type, variantLabel);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            Entry entry;
            entry.address = address;
            entry.type = type;
            entry.variantLabel = variantLabel;
            it = m_entries.insert(key, entry);
        }
        if (it->eliminated) {
            continue;
        }
        ++it->roundsSeen;
        const double confidence = map.value("confidence", 0.0).toDouble();
        if (confidence > it->bestConfidence) {
            it->bestConfidence = confidence;
        }
        if (map.contains("lastValueHex")) {
            it->lastValueHex = map.value("lastValueHex").toString();
        }
        if (map.contains("lastValueNumber")) {
            it->lastValueNumber = map.value("lastValueNumber").toDouble();
        }
        if (map.contains("origin")) {
            it->origin = map.value("origin").toString();
        }
        if (map.contains("regionBase")) {
            it->regionBase = map.value("regionBase").toString();
        }
        if (map.contains("protection")) {
            it->protection = map.value("protection").toString();
        }
        if (map.contains("memoryType")) {
            it->memoryType = map.value("memoryType").toString();
        }
    }
}

QVariantList ChangedPagesConsensus::checkpoints(int maxCount) const {
    QVariantList result;
    if (maxCount <= 0) {
        return result;
    }
    QList<const Entry*> active;
    active.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it->eliminated) {
            active.append(&it.value());
        }
    }
    std::sort(active.begin(), active.end(), [this](const Entry* a, const Entry* b) {
        const double scoreA = score(*a);
        const double scoreB = score(*b);
        if (scoreA != scoreB) {
            return scoreA > scoreB;
        }
        return a->address < b->address;
    });
    for (const Entry* entry : active) {
        if (result.size() >= maxCount) {
            break;
        }
        QVariantMap checkpoint;
        checkpoint["address"] = QString("%1").arg(entry->address, 0, 16).toUpper();
        checkpoint["type"] = valueTypeToString(entry->type);
        checkpoint["variantLabel"] = entry->variantLabel;
        result.append(checkpoint);
    }
    return result;
}

void ChangedPagesConsensus::applyProbeResult(const QVariantMap& probe) {
    uint64_t address = 0;
    if (!parseHexAddress(probe.value("address").toString(), &address)) {
        return;
    }
    killcore::ValueType type = killcore::ValueType::Int32;
    if (!killcore::parseValueType(probe.value("type").toString(), &type)) {
        return;
    }
    const QString variantLabel = probe.value("variantLabel").toString();
    // find() non-const : on doit pouvoir modifier les compteurs de l'entree.
    const auto it = m_entries.find(entryKey(address, type, variantLabel));
    if (it == m_entries.end() || it->eliminated) {
        return;
    }

    const QString status = probe.value("status").toString().toLower();
    if (status == QLatin1String("confirmed")) {
        ++it->roundsConfirmed;
        if (probe.contains("lastValueHex")) {
            it->lastValueHex = probe.value("lastValueHex").toString();
        }
        if (probe.contains("lastValueNumber")) {
            it->lastValueNumber = probe.value("lastValueNumber").toDouble();
        }
        return;
    }
    if (status == QLatin1String("contradicted")) {
        ++it->contradictionRounds;
        if (it->contradictionRounds >= kMaxContradictionRounds) {
            it->eliminated = true;
        }
        return;
    }
    if (status == QLatin1String("stale")) {
        ++it->staleRounds;
        if (it->staleRounds >= kMaxStaleRounds) {
            it->eliminated = true;
        }
    }
}

double ChangedPagesConsensus::score(const Entry& entry) const {
    return entry.roundsConfirmed * 2.0 + entry.bestConfidence - entry.staleRounds * 0.25;
}

QVariantList ChangedPagesConsensus::rankedEntries(int maxResults) const {
    QVariantList result;
    if (maxResults <= 0) {
        return result;
    }
    QList<const Entry*> active;
    active.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it->eliminated) {
            active.append(&it.value());
        }
    }
    std::sort(active.begin(), active.end(), [this](const Entry* a, const Entry* b) {
        const double scoreA = score(*a);
        const double scoreB = score(*b);
        if (scoreA != scoreB) {
            return scoreA > scoreB;
        }
        return a->address < b->address;
    });
    for (const Entry* entry : active) {
        if (result.size() >= maxResults) {
            break;
        }
        QVariantMap item;
        item["address"] = QString("%1").arg(entry->address, 0, 16).toUpper();
        item["type"] = valueTypeToString(entry->type);
        item["variantLabel"] = entry->variantLabel;
        item["roundsSeen"] = entry->roundsSeen;
        item["roundsConfirmed"] = entry->roundsConfirmed;
        item["staleRounds"] = entry->staleRounds;
        item["contradictionRounds"] = entry->contradictionRounds;
        item["bestConfidence"] = entry->bestConfidence;
        item["score"] = score(*entry);
        item["lastValueHex"] = entry->lastValueHex;
        item["lastValueNumber"] = entry->lastValueNumber;
        item["origin"] = entry->origin;
        item["regionBase"] = entry->regionBase;
        item["protection"] = entry->protection;
        item["memoryType"] = entry->memoryType;
        result.append(item);
    }
    return result;
}

int ChangedPagesConsensus::totalEntries() const {
    return m_entries.size();
}

int ChangedPagesConsensus::eliminatedCount() const {
    int count = 0;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (it->eliminated) {
            ++count;
        }
    }
    return count;
}

int ChangedPagesConsensus::confirmedEntries(int minRounds) const {
    int count = 0;
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        if (!it->eliminated && it->roundsConfirmed >= minRounds) {
            ++count;
        }
    }
    return count;
}

int ChangedPagesConsensus::roundsApplied() const {
    return m_rounds;
}

void ChangedPagesConsensus::reset() {
    m_entries.clear();
    m_rounds = 0;
}

} // namespace killcore