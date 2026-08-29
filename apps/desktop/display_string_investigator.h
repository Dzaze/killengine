#pragma once

#include "process/process_handle.h"
#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

namespace killengine {

class UiStringInvestigator {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using ScanOptionsCallback = std::function<killcore::ScanOptions(const QVariantMap&)>;
    using ScanStartedCallback = std::function<void()>;
    using ScanProgressCallback = std::function<void(int)>;

    UiStringInvestigator(
        const killcore::ProcessHandle& handle,
        TelemetryCallback telemetry,
        ScanOptionsCallback scanOptions,
        ScanStartedCallback scanStarted,
        ScanProgressCallback scanProgress);

    QVariantMap scanUiStrings(const QString& value, const QVariantMap& options) const;
    QVariantMap trackUiStringCandidates(const QVariantList& candidates, const QString& value) const;
    QVariantMap analyzeUiStringSources(
        const QVariantMap& stringCandidate,
        const QString& value,
        const QVariantMap& options) const;
    QVariantMap scanMemoryWindow(
        const QString& addressHex,
        const QString& value,
        const QVariantMap& options) const;
    QVariantMap trackUiStringSources(const QVariantList& sourceCandidates, const QString& value) const;
    QVariantMap inspectUiStringOrigins(
        const QVariantList& stringCandidates,
        const QVariantMap& options) const;
    QVariantMap startUiStringInvestigation(
        const QVariantList& stringCandidates,
        const QVariantList& sourceCandidates,
        const QVariantMap& options);
    QVariantMap finishUiStringInvestigation(const QVariantMap& options);
    QVariantMap startChangedPagesDiff(const QVariantMap& options);
    QVariantMap finishChangedPagesDiff(
        const QString& previousValue,
        const QString& currentValue,
        const QVariantMap& options);

private:
    struct UiInvestigationWindowState {
        uint64_t base{0};
        QByteArray before;
        QString label;
        QString reason;
    };

    struct UiInvestigationProbeBlockState {
        uint64_t base{0};
        int size{0};
        uint64_t hash{0};
        QString protection;
        QString memoryType;
    };

    struct ChangedPagesDiffBlockState {
        uint64_t base{0};
        QByteArray before;
        uint64_t hash{0};
        QString protection;
        QString memoryType;
    };

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    ScanOptionsCallback m_scanOptionsFromSettingsAndExpertOptions;
    ScanStartedCallback m_scanStarted;
    ScanProgressCallback m_scanProgress;
    QList<UiInvestigationWindowState> m_windows;
    QList<UiInvestigationProbeBlockState> m_probeBlocks;
    QList<ChangedPagesDiffBlockState> m_changedPagesDiffBlocks;
    qint64 m_startedMs{0};
};

} // namespace killengine
