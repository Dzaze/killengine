#pragma once

#include "process/process_handle.h"
#include "profiler/page_diff_analyzer.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>

namespace killengine {

/// EXTMOD-1 : outil generique de profilage d'un modificateur externe.
///
/// Capture des checkpoints nommes (ex: "baseline", "toggle_on", "stimulus")
/// de l'etat structurel du process attache (modules charges + carte memoire
/// avec protections/types) et produit un diff entre deux checkpoints :
/// modules ajoutes/retires, pages ajoutees/retirees/changees, hash de
/// contenu par page pour detecter une modification sans devoir tout dumper.
/// Reste volontairement generique (pas de vocabulaire lie a un jeu ou un
/// outil tiers precis) : usage prevu = comprendre passivement ce qu'un
/// outil externe autorise modifie sur une cible locale, sur n'importe quel
/// process attache.
class ExternalToolProfiler {
public:
    using TelemetryCallback = std::function<void(const QString&, const QVariantMap&)>;
    using PidCallback = std::function<int()>;

    ExternalToolProfiler(
        const killcore::ProcessHandle& handle,
        TelemetryCallback telemetry,
        PidCallback pid);

    /// A appeler a chaque nouvel attachProcess : les checkpoints d'une
    /// session precedente n'ont plus de sens sur un autre PID.
    void clearSessionState();

    /// Capture l'etat courant (modules + carte memoire, hash optionnel du
    /// contenu) sous un label. Ecrase un checkpoint existant du meme label.
    QVariantMap captureProfilerCheckpoint(const QString& label, const QVariantMap& options);

    /// Compare deux checkpoints deja captures et classe les differences.
    QVariantMap getProfilerDiff(const QString& labelA, const QString& labelB, const QVariantMap& options) const;

    /// Liste les labels actuellement en session (ordre de capture).
    QVariantMap listProfilerCheckpoints() const;

    QVariantMap clearProfilerSession();

    /// EXTMOD-2 : demarre/poursuit une session timeline nommee. A appeler
    /// avant chaque etape (ex: "baseline", "tool_attached_off", "toggle_on",
    /// "stimulus_done", "toggle_off") : capture un checkpoint sous le meme
    /// label que l'etape puis alimente le tracker de stabilite par page.
    /// Options identiques a captureProfilerCheckpoint (moduleName,
    /// maxHashBytesMb, maxPageBytesMb).
    QVariantMap recordProfilerTimelineStep(const QString& stepName, const QVariantMap& options);

    /// EXTMOD-2 : resume de la session timeline courante — classification de
    /// stabilite par page (toggle_state_candidate / runtime_noise /
    /// one_time_state_change) sur l'ensemble des etapes enregistrees.
    QVariantMap getProfilerTimelineSummary() const;

    /// Vide la session timeline (checkpoints eux-memes conserves).
    QVariantMap clearProfilerTimeline();

private:
    struct ProfilerModuleState {
        QString name;
        QString path;
        uint64_t baseAddress{0};
        uint64_t size{0};
    };

    struct ProfilerRegionState {
        uint64_t baseAddress{0};
        uint64_t size{0};
        QString protection;
        QString memoryType;
        bool executable{false};
        bool writable{false};
        bool hashed{false};
        uint64_t contentHash{0};
        /// EXTMOD-2 : pages 4K de la region (hash toujours present si
        /// `hashed`, bytes bruts seulement dans la limite du budget
        /// maxPageBytesMb — voir splitIntoPages).
        QList<killcore::PageContent> pages;
    };

    struct ProfilerCheckpoint {
        QString label;
        qint64 capturedAtMs{0};
        int pid{0};
        QList<ProfilerModuleState> modules;
        QList<ProfilerRegionState> regions;
    };

    QVariantMap moduleStateToVariant(const ProfilerModuleState& module) const;
    QVariantMap regionStateToVariant(const ProfilerRegionState& region) const;
    QString classifyRegion(
        const ProfilerRegionState& region,
        const QList<ProfilerModuleState>& newlyAddedModules) const;
    static bool isNewlyAddedModuleRegion(
        const ProfilerRegionState& region,
        const QList<ProfilerModuleState>& newlyAddedModules);
    /// Resout module+offset pour une adresse absolue. Chaine vide si
    /// l'adresse ne tombe dans aucun module connu (page privee/mappee).
    static QString resolveModuleOffsetLabel(uint64_t address, const QList<ProfilerModuleState>& modules);
    /// Capture interne partagee par captureProfilerCheckpoint() et
    /// recordProfilerTimelineStep() : construit un ProfilerCheckpoint sous
    /// `label` a partir de l'etat courant du process attache.
    QVariantMap captureCheckpointInternal(const QString& label, const QVariantMap& options);

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    PidCallback m_pid;
    QHash<QString, ProfilerCheckpoint> m_checkpoints;
    QList<QString> m_checkpointOrder;
    killcore::ProfilerTimelineTracker m_timeline;
};

} // namespace killengine
