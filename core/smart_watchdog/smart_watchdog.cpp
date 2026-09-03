#include "smart_watchdog.h"

#include <algorithm>

namespace killcore {

void SmartWatchdog::addEntry(const WatchdogEntry& entry) {
    // Supprime l'entree existante si presente
    removeEntry(entry.address);
    m_entries.push_back(entry);
}

void SmartWatchdog::removeEntry(uint64_t address) {
    m_entries.erase(
        std::remove_if(m_entries.begin(), m_entries.end(),
            [address](const WatchdogEntry& e) { return e.address == address; }),
        m_entries.end()
    );
}

std::vector<std::pair<uint64_t, WatchdogCheckResult>> SmartWatchdog::checkAll(
    uint64_t currentTimeMs,
    std::function<std::optional<uint64_t>(uint64_t address, uint32_t size)> getCurrentValue
) {
    std::vector<std::pair<uint64_t, WatchdogCheckResult>> results;

    for (auto& entry : m_entries) {
        if (!entry.active) continue;

        auto currentValueOpt = getCurrentValue(entry.address, entry.valueSize);
        if (!currentValueOpt.has_value()) continue;

        auto result = checkEntry(entry, currentTimeMs, currentValueOpt.value());
        entry.totalChecks += 1;
        entry.consecutiveResyncTicks = result.resyncTickCount;

        if (result.state == WatchdogState::ResyncDetected || result.state == WatchdogState::Expired) {
            entry.active = false;
        }

        results.emplace_back(entry.address, result);
    }

    return results;
}

WatchdogCheckResult SmartWatchdog::checkEntry(
    const WatchdogEntry& entry,
    uint64_t currentTimeMs,
    uint64_t currentValue
) const {
    WatchdogCheckResult result;
    result.state = WatchdogState::Watching;

    uint64_t maskedCurrent = maskValue(currentValue, entry.valueSize);
    uint64_t maskedWritten = maskValue(entry.writtenValue, entry.valueSize);
    uint64_t maskedOriginal = maskValue(entry.originalValue, entry.valueSize);

    // Verifie si la valeur est revenue a l'originale (resync)
    if (valuesEqual(maskedCurrent, maskedOriginal, entry.valueSize)) {
        result.resyncTickCount = entry.consecutiveResyncTicks + 1;

        if (result.resyncTickCount >= m_config.resyncThreshold) {
            result.resyncConfirmed = true;
            result.state = WatchdogState::ResyncDetected;
        }
    } else if (valuesEqual(maskedCurrent, maskedWritten, entry.valueSize)) {
        // Valeur toujours a ce qu'on a ecrit
        result.resyncTickCount = 0;

        // Verifie expiration
        if (currentTimeMs >= entry.startTimeMs + m_config.watchDurationMs) {
            result.state = WatchdogState::Expired;
        } else {
            result.state = WatchdogState::Stable;
        }
    } else {
        // Valeur differente (ni originale ni ecrite) - changement naturel
        result.resyncTickCount = 0;
    }

    return result;
}

std::optional<uint64_t> SmartWatchdog::detectTwinPattern(
    uint64_t baseAddress,
    uint64_t expectedValue,
    uint32_t valueSize,
    std::function<std::optional<uint64_t>(uint64_t address)> getValueAt
) const {
    if (!m_config.detectTwinPattern) return std::nullopt;

    // Pattern typique : copie a +8, +16, +24 octets (structures avec affichage + source)
    const std::vector<uint32_t> typicalOffsets = {8, 16, 24, 32, 4, 12};

    for (uint32_t offset : typicalOffsets) {
        if (offset > m_config.twinSearchRange) continue;

        uint64_t candidateAddress = baseAddress + offset;
        auto valueOpt = getValueAt(candidateAddress);

        if (valueOpt.has_value() && valuesEqual(valueOpt.value(), expectedValue, valueSize)) {
            return candidateAddress;
        }
    }

    // Recherche etendue : scan lineaire dans la range
    for (uint32_t offset = 4; offset <= m_config.twinSearchRange; offset += 4) {
        if (std::find(typicalOffsets.begin(), typicalOffsets.end(), offset) != typicalOffsets.end()) {
            continue; // Deja teste
        }

        uint64_t candidateAddress = baseAddress + offset;
        auto valueOpt = getValueAt(candidateAddress);

        if (valueOpt.has_value() && valuesEqual(valueOpt.value(), expectedValue, valueSize)) {
            return candidateAddress;
        }
    }

    return std::nullopt;
}

QString SmartWatchdog::generateSuggestion(const WatchdogCheckResult& result, uint64_t address) const {
    if (result.resyncConfirmed) {
        QString msg = QString("Resynchronisation detectee a 0x%1 - ")
            .arg(address, 0, 16);

        if (result.twinAddress.has_value()) {
            msg += QString("Pattern jumeau detecte. Essayer l'adresse 0x%1 (source probable)")
                .arg(result.twinAddress.value(), 0, 16);
        } else {
            msg += "La valeur est recalculee. Essayer 'Find What Writes' ou 'Trace UI String' pour trouver la source.";
        }

        return msg;
    }

    if (result.state == WatchdogState::Stable) {
        return QString("Ecriture stable a 0x%1 - aucune resynchronisation detectee")
            .arg(address, 0, 16);
    }

    if (result.state == WatchdogState::Expired) {
        return QString("Surveillance terminee a 0x%1 - valeur maintenue")
            .arg(address, 0, 16);
    }

    return QString("Surveillance en cours a 0x%1...").arg(address, 0, 16);
}

bool SmartWatchdog::valuesEqual(uint64_t a, uint64_t b, uint32_t size) const {
    return maskValue(a, size) == maskValue(b, size);
}

uint64_t SmartWatchdog::maskValue(uint64_t value, uint32_t size) const {
    switch (size) {
        case 1: return value & 0xFF;
        case 2: return value & 0xFFFF;
        case 4: return value & 0xFFFFFFFF;
        case 8: return value;
        default: return value & 0xFFFFFFFF;
    }
}

} // namespace killcore
