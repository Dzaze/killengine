#pragma once

#include "process/process_handle.h"

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

    const killcore::ProcessHandle& m_handle;
    TelemetryCallback m_appendScanTelemetry;
    PidCallback m_pid;
    QHash<QString, ProfilerCheckpoint> m_checkpoints;
    QList<QString> m_checkpointOrder;
};

} // namespace killengine
