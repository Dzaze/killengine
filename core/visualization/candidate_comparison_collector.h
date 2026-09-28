#pragma once

#include "scanner/scan_types.h"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace killcore {

/**
 * @brief UX-PRODUIT-16 (16B) -- une série suivie par le comparateur.
 *
 * `id` (pas seulement `address`) est la clé stable côté appelant : deux
 * séries peuvent partager la même adresse avec des types/facteurs
 * différents (ex. comparer une lecture Int32 et Float32 au même endroit).
 */
struct ComparisonSeriesConfig {
    QString id;
    uint64_t address{0};
    ValueType type{ValueType::Int32};
    double factor{1.0};
    QString label;
};

/// Un point lu pour UNE série, à l'intérieur d'un tour identifié par
/// `batchId`. `timestampMs` est l'instant de CETTE lecture précise (les
/// lectures d'un même tour sont séquentielles, pas simultanées) -- ne jamais
/// le confondre avec le début du tour, exposé séparément par
/// CandidateComparisonCollector::tourTimings().
struct ComparisonPoint {
    uint32_t batchId{0};
    uint64_t timestampMs{0};
    bool isValid{false};
    std::vector<uint8_t> rawBytes; // vide si !isValid -- jamais d'octets partiels trompeurs
};

struct ComparisonTourTiming {
    uint32_t batchId{0};
    uint64_t tourStartMs{0};
    uint64_t tourEndMs{0};
};

enum class ComparisonStopReason {
    UserStop,
    DurationReached,
    TargetLost,
};

struct ComparisonPairwiseCorrelation {
    QString seriesIdA;
    QString seriesIdB;
    bool computable{false};
    double coefficient{0.0};
    int pairCount{0};
    QString reason; ///< Vide si computable==true.
};

/// UX-PRODUIT-16 (16C) -- repère horodaté saisi par l'utilisateur pendant une
/// capture active. `timestampMs` utilise la même horloge relative que
/// ComparisonPoint::timestampMs/ComparisonTourTiming (steady_clock ancré au
/// début de la capture) -- jamais une horloge murale, pour rester sur le même
/// axe temporel que les points/tours.
struct ComparisonMarker {
    uint32_t id{0};
    uint64_t timestampMs{0};
    QString text;
};

/**
 * @brief Collecteur de comparaison -- même idiome que MemoryTimelineCollector
 * (pimpl, std::thread + condition_variable d'arrêt interruptible, join avant
 * toute réassignation de thread, jamais de reassign d'un thread encore
 * joinable -- piège déjà documenté et corrigé dans
 * memory_timeline_collector.cpp), mais PAS la même classe : concept absent
 * de Timeline ici, le TOUR de lecture (toutes les séries configurées, lues
 * à la suite, taguées du même `batchId`) -- nécessaire pour une corrélation
 * qui apparie par tour réel plutôt que par indice de tableau (motif
 * confirmé trompeur dans MemoryTimelineAnalyzer::findCorrelations) ou par
 * simple lag approximatif.
 *
 * Un seul et unique run à la fois (comme MemoryTimelineCollector) : appeler
 * configure()/startCollection() alors qu'une capture est active échoue
 * proprement (false), aucune configuration partielle n'est appliquée.
 */
class CandidateComparisonCollector {
public:
    CandidateComparisonCollector();
    ~CandidateComparisonCollector();
    CandidateComparisonCollector(const CandidateComparisonCollector&) = delete;
    CandidateComparisonCollector& operator=(const CandidateComparisonCollector&) = delete;

    /// Valide et mémorise 2 à 6 séries (ids non vides et uniques), clampe
    /// `intervalMs` à un minimum de 50 et `maxDurationMs` entre l'intervalle
    /// effectif et un maximum de 120000. Retourne false (rien n'est changé)
    /// si la configuration est invalide ou si une capture est déjà active.
    bool configure(const std::vector<ComparisonSeriesConfig>& series, uint32_t intervalMs, uint32_t maxDurationMs);

    bool startCollection(void* processHandle);
    /// Bloquant (join du thread de capture) -- la version non bloquante pour
    /// le thread Qt vit dans candidate_comparison_manager.*, pas ici : ce
    /// collecteur reste un module core pur, sans dépendance à une boucle
    /// d'événements.
    void stopCollection();
    bool isCollecting() const;
    ComparisonStopReason lastStopReason() const;
    uint32_t skippedTickCount() const;

    using FinishedCallback = std::function<void(ComparisonStopReason)>;
    /// AUDIT-PIPE-A2 : source unique de vérité pour "cette capture est
    /// terminée", appelée EXACTEMENT une fois par run réussi de
    /// startCollection(), depuis le thread de capture lui-même, juste après
    /// que collectionLoop() sorte -- quelle que soit la voie de sortie
    /// (durée max, cible perdue, ou m_shouldStop posé par stopCollection()/
    /// stopCollectionAsync()). Avant ce correctif, seuls les appelants
    /// manuels de stopCollection() émettaient une notification côté manager
    /// -- une sortie naturelle (durée max/cible perdue) sans stop explicite
    /// ne notifiait jamais personne, laissant l'activité "running" pour
    /// toujours côté UI/registre. startCollection() joint toujours le thread
    /// précédent avant d'en lancer un nouveau (voir plus bas) : ce join
    /// garantit que le callback d'un run précédent s'est déjà exécuté avant
    /// qu'un nouveau run ne puisse démarrer, donc jamais de notification
    /// tardive d'un ancien run après le début d'un nouveau. L'appelant doit
    /// rester léger et thread-safe (ne pas toucher l'UI directement -- le
    /// marshaling vers le thread Qt est la responsabilité de l'appelant, ex.
    /// candidate_comparison_manager.cpp).
    void setFinishedCallback(FinishedCallback callback);

    std::vector<ComparisonSeriesConfig> seriesConfigs() const;
    std::vector<ComparisonPoint> pointsForSeries(const QString& seriesId, size_t offset, size_t limit) const;
    size_t pointCountForSeries(const QString& seriesId) const;
    std::vector<ComparisonTourTiming> tourTimings() const;

    /// Corrélation de Pearson par paire de séries, appariée par `batchId`
    /// commun (les deux valides au même tour) -- jamais par indice brut.
    /// `computable=false` si moins de 10 paires valides communes ou variance
    /// nulle sur l'une des deux séries, avec `reason` explicite.
    std::vector<ComparisonPairwiseCorrelation> correlations() const;

    /// Ajoute un repère horodaté (16C). Refuse (false, `out` inchangé) si
    /// aucune capture n'est active, si `text` est vide/dépasse 500
    /// caractères, ou si 100 repères sont déjà enregistrés pour cette
    /// capture. `out` reçoit l'id/timestamp attribués si `out != nullptr`.
    bool addMarker(const QString& text, ComparisonMarker* out);
    std::vector<ComparisonMarker> markers() const;

    /// Export JSON versionné et borné (16C) de la capture courante --
    /// config, séries décodées (mêmes valeurs typées que pointsForSeries via
    /// candidate_comparison_decoder), minutages de tour, corrélations et
    /// repères. Écrit directement sur disque, même idiome que
    /// MemoryTimelineCollector::exportToJson -- aucune capture active n'est
    /// requise (une capture arrêtée reste exportable). N'écrit jamais dans
    /// le format/les fichiers Memory Timeline existants.
    bool exportToJson(const std::string& filepath) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;

    /// Corps de correlations() sans acquérir m_impl->m_mutex -- appelé par
    /// exportToJson() qui tient déjà le verrou (std::mutex non réentrant,
    /// appeler correlations() depuis là ferait un deadlock).
    std::vector<ComparisonPairwiseCorrelation> correlationsLocked() const;
};

} // namespace killcore
