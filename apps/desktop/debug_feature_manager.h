#pragma once

#include "debug/breakpoint_freeze.h"
#include "debug/hardware_breakpoint.h"
#include "debug/inprocess_breakpoint.h"
#include "debug/page_guard.h"
#include "debug/speedhack.h"
#include "freeze/freeze_manager.h"
#include "inject/api_hook.h"
#include "process/process_handle.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <memory>

namespace killengine {

class DebugFeatureManager : public QObject {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using IsAttachedCallback = std::function<bool()>;
    using PidCallback = std::function<int()>;
    using NextRequestIdCallback = std::function<int()>;
    using ResultCallback = std::function<void(const QVariantMap&)>;

    DebugFeatureManager(
        const killcore::ProcessHandle& handle,
        killcore::FreezeManager& freeze,
        TelemetryCallback telemetry,
        IsAttachedCallback isAttached,
        PidCallback pid,
        NextRequestIdCallback nextRequestId,
        ResultCallback findWhatWritesFinished,
        ResultCallback findWhatAccessesFinished,
        ResultCallback pageGuardWatchFinished,
        ResultCallback inProcessBreakpointWatchFinished,
        QObject* parent = nullptr);
    ~DebugFeatureManager() override;

    void stopBreakpointFreeze();
    void stopInjectionSessions();
    void resetHardwareBreakpointStateForPreviousTarget(int previousPid);
    bool deferDetachIfBusy();
    bool restartBreakpointFreezeFromRegistry(killcore::BreakpointFreezeMode mode, QString* error = nullptr);

    QVariantMap freezeWithBreakpoint(
        const QString& addressHex,
        const QString& valueType,
        const QString& value,
        const QVariantMap& options);
    QVariantMap escalatePollingFreezeToBreakpoint(const QString& addressHex);
    QVariantMap stopBreakpointFreezeCommand();
    QVariantMap getBreakpointFreezeStats() const;
    QVariantMap findWhatWrites(const QString& addressHex, const QVariantMap& options);
    QVariantMap findWhatWritesAsync(const QString& addressHex, const QVariantMap& options);
    QVariantMap cancelFindWhatWrites();
    QVariantMap findWhatAccesses(const QString& addressHex, const QVariantMap& options);
    QVariantMap findWhatAccessesAsync(const QString& addressHex, const QVariantMap& options);
    QVariantMap findWhatExecutes(const QString& instructionAddressHex, const QVariantMap& options);
    QVariantMap startPageGuardWatchAsync(const QString& addressHex, const QVariantMap& options);
    QVariantMap cancelPageGuardWatch();
    QVariantMap startInProcessBreakpointWatchAsync(const QString& addressHex, const QVariantMap& options);
    QVariantMap startInProcessExecuteWatchAsync(const QString& instructionAddressHex, const QVariantMap& options);
    QVariantMap startInProcessExecuteWatch(const QString& instructionAddressHex, const QVariantMap& options);
    QVariantMap cancelInProcessBreakpointWatch();
    QVariantMap startInProcessBreakpointFreeze(
        const QString& addressHex,
        const QString& valueType,
        const QString& value,
        const QVariantMap& options);
    QVariantMap stopInProcessBreakpointFreeze();
    QVariantMap getInProcessBreakpointFreezeStats() const;
    QVariantMap startSpeedhack(double factor);
    QVariantMap setSpeedhackFactor(double factor);
    QVariantMap stopSpeedhack();
    QVariantMap startApiHook(const QString& moduleName, const QString& functionName, int mode, qlonglong forcedReturnValue);
    QVariantMap stopApiHook();
    QVariantMap getApiHookStatus() const;
    QVariantMap getSpeedhackStatus() const;
    QVariantMap disassembleBackward(const QString& addressHex, const QVariantMap& options) const;
    QVariantMap validatePageStability(const QString& addressHex, const QVariantMap& options) const;

private:
    QVariantMap activateBreakpointFreezeFor(
        uint64_t address,
        killcore::ValueType type,
        const QByteArray& frozenBytes,
        killcore::BreakpointFreezeMode mode,
        const QString& modeText);

    const killcore::ProcessHandle& m_handle;
    killcore::FreezeManager& m_freeze;
    TelemetryCallback m_appendScanTelemetry;
    IsAttachedCallback m_isAttached;
    PidCallback m_pid;
    NextRequestIdCallback m_nextRequestId;
    ResultCallback m_findWhatWritesFinished;
    ResultCallback m_findWhatAccessesFinished;
    ResultCallback m_pageGuardWatchFinished;
    ResultCallback m_inProcessBreakpointWatchFinished;

    std::unique_ptr<killcore::BreakpointFreezeManager> m_breakpointFreeze;
    std::shared_ptr<killcore::CancellationToken> m_activeDebugCancellation;
    bool m_findWhatWritesInProgress{false};
    bool m_findWhatAccessesInProgress{false};
    bool m_pageGuardWatchInProgress{false};
    std::shared_ptr<killcore::PageGuardSession> m_activePageGuardSession;
    bool m_inProcessBreakpointWatchInProgress{false};
    std::shared_ptr<killcore::InProcessBreakpointSession> m_activeInProcessBreakpointSession;
    std::shared_ptr<killcore::InProcessBreakpointSession> m_inProcessBreakpointFreezeSession;
    std::unique_ptr<killcore::SpeedhackSession> m_speedhackSession;
    std::unique_ptr<killcore::ApiHookSession> m_apiHookSession;
};

} // namespace killengine
