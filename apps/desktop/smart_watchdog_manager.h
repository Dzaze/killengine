#pragma once

#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include "smart_watchdog/smart_watchdog.h"

namespace killengine {

/// Manager Qt pour le Smart Watchdog
class SmartWatchdogManager : public QObject {
    Q_OBJECT

public:
    using ReadValueCallback = std::function<std::optional<uint64_t>(uint64_t address, uint32_t size)>;

    explicit SmartWatchdogManager(QObject* parent = nullptr);
    ~SmartWatchdogManager();

    /// Active/desactive la surveillance automatique
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// Configure le watchdog
    void setConfig(const killcore::WatchdogConfig& config);
    killcore::WatchdogConfig config() const;

    /// Ajoute une entree de surveillance apres une ecriture
    void watchWrite(uint64_t address, uint64_t writtenValue, uint64_t originalValue, uint32_t valueSize);

    /// Arrete la surveillance d'une adresse
    void unwatchAddress(uint64_t address);

    /// Arrete toutes les surveillances
    void clearAll();

    /// Nombre d'entrees actives
    size_t activeWatchCount() const;

    /// Injecte la lecture memoire reelle. Laisse le manager testable sans process.
    void setReadValueCallback(ReadValueCallback callback);

signals:
    void resyncDetected(uint64_t address, QString suggestion);
    void writeConfirmedStable(uint64_t address);
    void twinPatternDetected(uint64_t displayAddress, uint64_t sourceAddress);

private slots:
    void onPollTick();

private:
    bool m_enabled{false};
    std::unique_ptr<killcore::SmartWatchdog> m_watchdog;
    QTimer* m_pollTimer{nullptr};
    ReadValueCallback m_readValueCallback;

    void startPolling();
    void stopPolling();
};

} // namespace killengine
