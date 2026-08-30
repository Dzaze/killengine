#pragma once

#include "process/process_handle.h"
#include "scanner/changed_pages_consensus.h"
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

    /// PHASE 250 (SC2) : session changed-pages multi-rounds. Contrairement au
    /// diff one-shot ci-dessus, les blocs capturés roulent d'un round à
    /// l'autre (roll-forward de la baseline) et un consensus déterministe
    /// accumule les adresses qui suivent CHAQUE transition affichée
    /// (140 -> 135 -> 130 ...), élimine celles qui contredisent ou deviennent
    /// illisibles, et classe les survivantes. C'est l'intersection multi-rounds
    /// qui isole la source gameplay des centaines de copies UI volatiles.
    QVariantMap startChangedPagesSession(const QVariantMap& options);
    QVariantMap applyChangedPagesRound(
        const QString& previousValue,
        const QString& currentValue,
        const QVariantMap& options);
    QVariantMap getChangedPagesConsensus(const QVariantMap& options) const;
    QVariantMap stopChangedPagesSession();

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

    struct ChangedPagesDiffStats {
        int blocksChecked{0};
        int blocksChanged{0};
        int unreadable{0};
        int blocksDropped{0};
        uint64_t bytesChecked{0};
        uint64_t changedBytes{0};
    };

    /// Diff les blocs contre l'état courant de la mémoire et retourne les hits
    /// (même forme que finishChangedPagesDiff). Si rollForward, la baseline de
    /// chaque bloc relu avec succès devient son contenu courant (prêt pour le
    /// round suivant) et les blocs devenus illisibles sont retirés — c'est ce
    /// qui rend la session réutilisable round après round sans re-capture.
    QVariantList diffChangedPagesBlocks(
        QList<ChangedPagesDiffBlockState>& blocks,
        const QString& previousValue,
        const QString& currentValue,
        int maxHits,
        int maxDistanceToChange,
        bool rollForward,
        ChangedPagesDiffStats* stats) const;

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    ScanOptionsCallback m_scanOptionsFromSettingsAndExpertOptions;
    ScanStartedCallback m_scanStarted;
    ScanProgressCallback m_scanProgress;
    QList<UiInvestigationWindowState> m_windows;
    QList<UiInvestigationProbeBlockState> m_probeBlocks;
    QList<ChangedPagesDiffBlockState> m_changedPagesDiffBlocks;
    killcore::ChangedPagesConsensus m_changedPagesConsensus;
    bool m_changedPagesSessionActive{false};
    qint64 m_changedPagesSessionStartedMs{0};
    qint64 m_changedPagesSessionLastRoundMs{0};
    qint64 m_startedMs{0};
};

} // namespace killengine
