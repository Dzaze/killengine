#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <optional>

namespace killcore {

/// Famille d'opération suivie par le centre d'activité (UX-PRODUIT-12).
enum class ActivityKind {
    ScanExact,
    ScanAuto,
    ScanNext,
    ScanCaptureUnknown,
    ScanUnknownNext,
    TimelineCollection,
    LuaScript,
    ModuleInstall,
    SaveFileWatch,
    CandidateComparison, ///< UX-PRODUIT-16 — capture de comparaison multi-candidats.
};

/// États autorisés d'une activité. Une fois dans un état terminal
/// (Completed/Cancelled/Failed/Interrupted), l'entrée ne redevient jamais
/// Running — un nouveau départ crée une nouvelle entrée.
enum class ActivityState {
    Running,
    CancelRequested,
    Completed,
    Cancelled,
    Failed,
    Interrupted,
};

QString activityKindToString(ActivityKind kind);
QString activityStateToString(ActivityState state);

/// Cible mémoire éventuellement associée à une activité (scan, Timeline...).
struct ActivityTarget {
    QString pid;                   // chaîne : précision JS 53 bits non garantie côté frontend
    QString processName;
    QString attachmentGeneration;  // voir ApplicationController::m_attachmentGeneration
};

struct ActivityEntry {
    QString operationId;           // opaque, généré par le registre
    ActivityKind kind{ActivityKind::ScanExact};
    ActivityState state{ActivityState::Running};
    qint64 revision{0};            // incrémenté à CHAQUE mutation de cette entrée
    qint64 startedAtMs{0};         // horloge monotone (QElapsedTimer côté appelant)
    qint64 finishedAtMs{0};        // 0 tant que running
    QVariant progress;             // invalide/null = indéterminé, sinon 0..100
    QString summary;
    QString errorMessage;
    bool canCancel{false};
    bool acknowledged{false};      // consultée par l'utilisateur (tiroir ouvert dessus)
    QString resultRef;             // référence opaque pour "Voir" / détection "résultat remplacé"
    std::optional<ActivityTarget> target;
    QString requestId;             // corrèle au requestId legacy du domaine (scan/debug/install)
};

/// Nombre max d'entrées TERMINALES conservées (les entrées Running/CancelRequested
/// ne sont JAMAIS évincées, quel que soit leur nombre — fiche UX-PRODUIT-12).
constexpr int kActivityTerminalCap = 200;

/// Registre central des opérations en cours/terminées (scans, Timeline, Lua,
/// installations, surveillance de fichier). Pure logique, sans QObject : la
/// notification Qt (signal activityUpdated) est portée par
/// apps/desktop/activity_manager.h, qui possède une instance de cette classe.
///
/// IMPORTANT — thread-safety : AUCUN mutex ici. Toutes les méthodes doivent
/// être appelées depuis le thread Qt/propriétaire du contrôleur, soit
/// directement (appel Q_INVOKABLE), soit depuis une continuation déjà
/// marshalée via QMetaObject::invokeMethod(..., Qt::QueuedConnection) — le
/// même motif que ScanningCoreManager utilise pour scanFinished. Ne JAMAIS
/// appeler beginActivity/updateProgress/finish depuis un callback qui tourne
/// encore sur un worker thread (ex. options.progressCallback avant son propre
/// marshal via emitQueuedScanProgress).
class ActivityRegistry {
public:
    /// Démarre une activité et retourne son ID opaque. Ne doit être appelé
    /// qu'après toute validation synchrone réussie (un refus ne doit jamais
    /// produire d'entrée running fantôme).
    QString beginActivity(ActivityKind kind,
                           const QString& summary,
                           bool canCancel,
                           std::optional<ActivityTarget> target = std::nullopt,
                           const QString& requestId = QString());

    bool updateProgress(const QString& operationId, QVariant progress, const QString& summary = QString());
    bool markCancelRequested(const QString& operationId);
    bool finish(const QString& operationId,
                ActivityState terminalState,
                const QString& summary,
                const QString& errorMessage,
                const QString& resultRef = QString());
    bool acknowledge(const QString& operationId);

    std::optional<ActivityEntry> find(const QString& operationId) const;
    /// Running d'abord (ordre de démarrage), puis terminales (plus récente d'abord).
    QList<ActivityEntry> snapshot() const;
    qint64 globalRevision() const;
    int runningCount() const;
    int unacknowledgedTerminalCount() const;

private:
    void evictTerminalIfNeeded();

    QHash<QString, ActivityEntry> m_entries;
    QList<QString> m_order;   // ordre d'insertion de TOUTES les entrées (plus ancien en tête)
    qint64 m_globalRevision{0};
    qint64 m_nextOperationSeq{1};
};

} // namespace killcore
