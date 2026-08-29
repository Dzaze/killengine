#pragma once

#include "freeze/freeze_manager.h"
#include "input/global_hotkey.h"
#include "process/process_handle.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>

class QLabel;
class QWidget;

namespace killcore {
enum class BreakpointFreezeMode;
}

namespace killengine {

class FreezeHotkeyOverlayManager : public QObject {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using PidCallback = std::function<int()>;
    using AddressVerifiedCallback = std::function<bool(uint64_t)>;
    using RestartBreakpointFreezeCallback = std::function<bool(killcore::BreakpointFreezeMode, QString*)>;
    using ResultCallback = std::function<void(const QVariantMap&)>;

    FreezeHotkeyOverlayManager(
        const killcore::ProcessHandle& handle,
        TelemetryCallback telemetry,
        PidCallback pid,
        AddressVerifiedCallback addressVerified,
        RestartBreakpointFreezeCallback restartBreakpointFreeze,
        ResultCallback globalHotkeyTriggered,
        ResultCallback freezeInstabilityDetected,
        QObject* parent = nullptr);

    killcore::FreezeManager& freezeManager();
    const killcore::FreezeManager& freezeManager() const;
    void clearFreezeState();
    void stopFreezeTimer();

    QVariantMap setFreezeValue(const QString& addressHex, const QString& valueType, const QString& value, bool enabled);
    QVariantMap setFreezeInterval(int intervalMs);
    QVariantMap registerGlobalHotkey(const QString& comboText, const QVariantMap& actionMap);
    QVariantMap unregisterGlobalHotkey(int id);
    QVariantMap getGlobalHotkeys() const;
    QVariantMap clearGlobalHotkeys();
    QVariantMap setTrainerOverlayVisible(bool visible, const QVariantMap& options);
    QVariantMap updateTrainerOverlay(const QVariantMap& state);

private:
    void applyFreezeTick();

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    PidCallback m_pid;
    AddressVerifiedCallback m_hasAddressBeenWriteVerified;
    RestartBreakpointFreezeCallback m_restartBreakpointFreeze;
    ResultCallback m_globalHotkeyTriggered;
    ResultCallback m_freezeInstabilityDetected;
    killcore::FreezeManager m_freeze;
    std::unique_ptr<killcore::GlobalHotkeyManager> m_hotkeys;
    QTimer m_freezeTimer;
    QPointer<QWidget> m_trainerOverlay;
    QPointer<QLabel> m_trainerOverlayLabel;
};

} // namespace killengine
