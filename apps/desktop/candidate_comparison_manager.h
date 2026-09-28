#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace killengine {

/**
 * @brief Manager Qt pour le comparateur visuel de candidats (UX-PRODUIT-16).
 *
 * Même motif que MemoryTimelineManager : pont entre
 * killcore::CandidateComparisonCollector (core, pur) et l'UI Vue, avec une
 * version async de l'arrêt (stopCollectionAsync) pour ne jamais bloquer le
 * thread Qt sur le join() du thread de capture -- utilisée par la garde
 * attach/detach au même titre que MemoryTimelineManager::stopCollectionAsync.
 */
class CandidateComparisonManager : public QObject {
    Q_OBJECT

public:
    explicit CandidateComparisonManager(QObject* parent = nullptr);
    ~CandidateComparisonManager();

    /// `series` : liste de QVariantMap {id, address (hex string), type
    /// (nom ValueType, ex. "Int32"), factor, label}. `options` :
    /// intervalMs, maxDurationMs (bornés par le collecteur). Retourne
    /// {success, error?}.
    Q_INVOKABLE QVariantMap startComparison(const QVariantList& series, const QVariantMap& options);
    Q_INVOKABLE void stopCollection();
    Q_INVOKABLE void stopCollectionAsync();
    Q_INVOKABLE bool isCollecting() const;
    Q_INVOKABLE QVariantMap getStatus() const;
    Q_INVOKABLE QVariantMap getSamples(const QString& seriesId, int offset, int limit) const;
    Q_INVOKABLE QVariantMap getCorrelations() const;
    /// Repère horodaté (16C) -- refusé hors capture active, texte vide/>500
    /// caractères, ou 100 repères déjà atteints. Retourne {success, id?,
    /// timestampMs?, error?}.
    Q_INVOKABLE QVariantMap addMarker(const QString& text);
    /// Export JSON versionné borné de la capture courante (16C) -- même
    /// idiome que MemoryTimelineManager::exportToJson (le fichier est écrit
    /// par le collecteur core, pas ici).
    Q_INVOKABLE bool exportToJson(const QString& filepath) const;

    void setProcessHandle(void* handle);
    void* processHandle() const;

signals:
    /// reason ∈ {"user_stop", "duration_reached", "target_lost"}. Émis
    /// EXACTEMENT une fois par capture, quelle que soit la voie de fin
    /// (bouton Stop, stopCollectionAsync, ou sortie naturelle -- durée max/
    /// cible perdue) : voir CandidateComparisonCollector::setFinishedCallback
    /// (AUDIT-PIPE-A2), source unique câblée dans le constructeur.
    void comparisonFinished(const QString& reason);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace killengine
