#include "smart_watchdog_manager.h"

#include <algorithm>
#include <utility>

#include <QDateTime>

namespace killengine {

SmartWatchdogManager::SmartWatchdogManager(QObject* parent)
    : QObject(parent)
    , m_watchdog(std::make_unique<killcore::SmartWatchdog>())
    , m_pollTimer(new QTimer(this))
{
    connect(m_pollTimer, &QTimer::timeout, this, &SmartWatchdogManager::onPollTick);
}

SmartWatchdogManager::~SmartWatchdogManager() = default;

void SmartWatchdogManager::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;

    if (m_enabled) {
        startPolling();
    } else {
        stopPolling();
    }
}

void SmartWatchdogManager::setConfig(const killcore::WatchdogConfig& config) {
    m_watchdog->setConfig(config);
}

killcore::WatchdogConfig SmartWatchdogManager::config() const {
    return m_watchdog->config();
}

void SmartWatchdogManager::watchWrite(uint64_t address, uint64_t writtenValue, uint64_t originalValue, uint32_t valueSize) {
    killcore::WatchdogEntry entry;
    entry.address = address;
    entry.writtenValue = writtenValue;
    entry.originalValue = originalValue;
    entry.valueSize = valueSize;
    entry.startTimeMs = QDateTime::currentMSecsSinceEpoch();
    entry.active = true;

    m_watchdog->addEntry(entry);

    if (m_enabled && !m_pollTimer->isActive()) {
        startPolling();
    }
}

void SmartWatchdogManager::unwatchAddress(uint64_t address) {
    m_watchdog->removeEntry(address);
}

void SmartWatchdogManager::clearAll() {
    m_watchdog->clear();
}

size_t SmartWatchdogManager::activeWatchCount() const {
    size_t count = 0;
    for (const auto& entry : m_watchdog->entries()) {
        if (entry.active) {
            ++count;
        }
    }
    return count;
}

void SmartWatchdogManager::setReadValueCallback(ReadValueCallback callback) {
    m_readValueCallback = std::move(callback);
}

void SmartWatchdogManager::startPolling() {
    auto config = m_watchdog->config();
    m_pollTimer->start(static_cast<int>(config.pollIntervalMs));
}

void SmartWatchdogManager::stopPolling() {
    m_pollTimer->stop();
}

void SmartWatchdogManager::onPollTick() {
    if (!m_enabled || m_watchdog->entries().empty()) {
        return;
    }

    auto currentTime = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());

    if (!m_readValueCallback) {
        return;
    }

    auto results = m_watchdog->checkAll(currentTime, m_readValueCallback);

    for (const auto& [address, result] : results) {
        if (result.resyncConfirmed) {
            auto enrichedResult = result;
            const auto& entries = m_watchdog->entries();
            auto it = std::find_if(entries.begin(), entries.end(), [address](const killcore::WatchdogEntry& entry) {
                return entry.address == address;
            });
            if (it != entries.end()) {
                enrichedResult.twinAddress = m_watchdog->detectTwinPattern(
                    address,
                    it->writtenValue,
                    it->valueSize,
                    [this, size = it->valueSize](uint64_t candidateAddress) -> std::optional<uint64_t> {
                        return m_readValueCallback ? m_readValueCallback(candidateAddress, size) : std::nullopt;
                    });
            }

            QString suggestion = m_watchdog->generateSuggestion(enrichedResult, address);
            emit resyncDetected(address, suggestion);

            // Detection de pattern jumeau
            if (enrichedResult.twinAddress.has_value()) {
                emit twinPatternDetected(address, enrichedResult.twinAddress.value());
            }
        } else if (result.state == killcore::WatchdogState::Expired) {
            emit writeConfirmedStable(address);
        }
    }
}

} // namespace killengine
