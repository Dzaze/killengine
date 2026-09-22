#pragma once

#include "process/process_handle.h"
#include "scan_state_access.h"
#include "scanner/scan_types.h"

#include <QObject>
#include <QList>
#include <QVariantList>
#include <QVariantMap>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace killcore {
class CancellationToken;
}

namespace killengine {

class ApplicationController;

class ScanningCoreManager : public QObject {
public:
    explicit ScanningCoreManager(ApplicationController& controller, QObject* parent = nullptr);

    bool isScanInProgress() const;
    void requestCancelActiveScan();

    QVariantMap startExactScan(const QString& value, const QString& valueType);
    QVariantMap startExactScanMultiType(const QString& value, const QString& valueType, const QVariantMap& expertOptions);
    QVariantMap startExactScanExpert(const QString& value, const QString& valueType, const QVariantMap& expertOptions);
    QVariantMap scanEncryptedValue(const QString& value, const QString& valueType, const QVariantMap& options);
    QVariantMap startExactScanAsync(const QString& value, const QString& valueType, const QVariantMap& expertOptions);
    QVariantMap nextScanAsync(const QString& mode, const QString& value);
    QVariantMap cancelActiveScan();
    QVariantMap nextScan(const QString& mode, const QString& value);
    QVariantMap undoCandidateScan();
    QVariantMap getCandidates(int pageIndex, int pageSize, const QString& addressFilter) const;
    QVariantMap captureUnknownSnapshot();
    QVariantMap captureUnknownSnapshotWithOptions(const QVariantMap& expertOptions);
    QVariantMap captureUnknownSnapshotAsync();
    QVariantMap captureUnknownSnapshotAsyncWithOptions(const QVariantMap& expertOptions);
    QVariantMap unknownNextScan(const QString& mode, const QString& valueType, const QString& deltaValue = QString());
    QVariantMap unknownNextScanAsync(const QString& mode, const QString& valueType, const QString& deltaValue = QString());
    QVariantMap scanGroupScan(const QVariantList& entries, const QVariantMap& options);

private:
    ScanStateAccess scanState();
    ScanStateAccess scanState() const;
    void clearCandidateUndo();
    void clearCandidateValueHistory();
    bool rememberCandidatesForUndo(QString* error);
    void recordCandidateObservations(const QVariantList& observations);
    void detectStableCandidateGroup(killcore::NextScanMode mode, const QList<killcore::Candidate>& survivors, QVariantMap* result);
    void appendSmartSearchDebug(const QString& event, const QVariantMap& payload) const;
    void appendScanTelemetry(const QString& event, const QVariantMap& payload) const;
    bool hasAddressBeenWriteVerified(uint64_t address) const;
    void scanStarted();
    void scanProgress(int percent);
    void scanStatsUpdated(int candidateCount);
    void scanFinished(const QVariantMap& result);

    ApplicationController& m_controller;
};

} // namespace killengine
