#pragma once

#include "scanner/scan_types.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>

namespace killcore {

/**
 * @brief Multi-round consensus accumulator for changed-pages diff sessions.
 *
 * Motivation (SC2 live session, 30/08/2026): on targets where the displayed
 * value only exists as volatile UI copies, a single diff round returns
 * hundreds of hits that become stale before they can be verified. The
 * reliable signal is the intersection across rounds: addresses that follow
 * EVERY displayed transition (140 -> 135 -> 130 ...) while staying readable.
 *
 * This class is pure logic (no process access). The caller feeds it the hit
 * list of each round (same QVariantMap shape as finishChangedPagesDiff hits),
 * then probes the accumulated addresses itself (it owns the memory reader)
 * and reports back the probe status. Scoring and elimination are
 * deterministic and unit-testable:
 *   - confirmed probe     : roundsConfirmed++  (strong signal)
 *   - contradicted probe  : contradictionRounds++, eliminated at 2 misses
 *                           (one miss forgiven: volatile UI caches get
 *                           rewritten transiently)
 *   - stale (unreadable)  : staleRounds++, eliminated at 4 (page too
 *                           volatile to be useful for page guard)
 *
 * Ranking score = roundsConfirmed * 2.0 + bestConfidence - staleRounds * 0.25
 */
class ChangedPagesConsensus {
public:
    /// Accumulated candidate address.
    struct Entry {
        uint64_t address{0};
        ValueType type{ValueType::Int32};
        QString variantLabel;
        int roundsSeen{0};          ///< Rounds where the address appeared as a hit.
        int roundsConfirmed{0};     ///< Probes where the address still matched.
        int staleRounds{0};         ///< Probes where the page was unreadable.
        int contradictionRounds{0}; ///< Probes where the value stopped following.
        double bestConfidence{0.0};
        double lastValueNumber{0.0};
        QString lastValueHex;
        QString origin;
        QString regionBase;
        QString protection;
        QString memoryType;
        bool eliminated{false};
    };

    /// Merges one round of diff hits into the accumulator. Each hit must
    /// contain address / type / variantLabel (as produced by
    /// finishChangedPagesDiff or applyChangedPagesRound); other fields
    /// (confidence, lastValueHex, lastValueNumber, origin, regionBase,
    /// protection, memoryType) are optional metadata.
    void applyRound(const QVariantList& hits);

    /// Returns the top active (non-eliminated) addresses to re-probe this
    /// round, sorted by score descending. Each item is
    /// {address, type, variantLabel} for the caller to read memory.
    QVariantList checkpoints(int maxCount) const;

    /// Applies one probe result. `probe` must contain address / type /
    /// variantLabel and status: "confirmed" | "contradicted" | "stale".
    void applyProbeResult(const QVariantMap& probe);

    /// Deterministic ranking score for an entry (higher is better).
    double score(const Entry& entry) const;

    /// Active (non-eliminated) entries sorted by score descending, capped
    /// to maxResults. Each item carries all Entry fields plus score.
    QVariantList rankedEntries(int maxResults) const;

    int totalEntries() const;
    int eliminatedCount() const;
    int confirmedEntries(int minRounds) const;
    int roundsApplied() const;
    void reset();

    /// Stable key identifying one (address, type, variant) triple.
    static QString entryKey(uint64_t address, ValueType type, const QString& variantLabel);

private:
    QHash<QString, Entry> m_entries;
    int m_rounds{0};
};

} // namespace killcore