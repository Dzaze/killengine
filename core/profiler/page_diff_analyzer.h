#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>

namespace killcore {

/**
 * @brief Page-level (4K by default) content splitting + diffing for the
 * External Tool Profiler (EXTMOD-1/EXTMOD-2).
 *
 * Motivation (SC2/Wand live session, 02/09/2026, see docs/PHASE_TRACKER.md
 * "EXTMOD-2"): a whole-region hash only says "this 66 MB region changed
 * somewhere" — not useful to locate the actual accroche point. This module
 * hashes (and optionally retains raw bytes for) fixed-size pages inside an
 * already-read region buffer, so the caller (ExternalToolProfiler, which
 * owns the real memory reads) can produce a page-level diff with module
 * offsets and bounded byte deltas.
 *
 * Pure logic, no process/OS access — unit-testable with synthetic buffers.
 */
struct PageContent {
    uint64_t address{0};   ///< Absolute address of the page start.
    uint64_t hash{0};      ///< FNV-1a hash of the page bytes (only meaningful if hashed).
    bool hashed{false};
    QByteArray bytes;      ///< Raw content, empty if outside the byte-retention budget.
};

/// Splits `content` (already read starting at `baseAddress`) into fixed-size
/// pages, hashing every page and retaining raw bytes only while
/// `byteBudgetRemaining` allows it (decremented per page kept; pass a
/// reference so the same budget can be shared across the whole checkpoint
/// capture, not just one region).
QList<PageContent> splitIntoPages(
    uint64_t baseAddress,
    const QByteArray& content,
    uint64_t& byteBudgetRemaining,
    uint64_t pageSize = 4096);

/// Result of comparing one page between two checkpoints.
struct PageDiffResult {
    uint64_t address{0};
    bool hashChanged{false};
    qint64 byteDiffCount{-1};    ///< -1 = raw bytes unavailable on at least one side.
    QVariantList sampleDeltas;   ///< Bounded list of {offset, before, after} (uint8 each).
    QString classification;     ///< "injected_module_state" | "code_patch_candidate" | "mapped_data_changed"
};

/// Pure page-level diff between two page-content lists of the SAME region
/// (pages matched by absolute address). Only pages whose hash changed are
/// returned. `executableRegion`/`newlyAddedModuleRegion` steer the
/// two-checkpoint classification (the richer runtime_noise/toggle_state
/// classification needs a multi-step timeline, see ProfilerTimelineTracker).
QList<PageDiffResult> diffPageContents(
    const QList<PageContent>& before,
    const QList<PageContent>& after,
    bool executableRegion,
    bool newlyAddedModuleRegion,
    int maxSampleDeltas = 16);

QVariantMap pageDiffResultToVariant(const PageDiffResult& result);

/**
 * @brief Timeline stability accumulator (EXTMOD-2).
 *
 * Tracks, for a named ordered sequence of steps (e.g. baseline /
 * tool_attached_off / toggle_on / stimulus_done / toggle_off), which page
 * addresses actually change and how — the signal the tracker asked for:
 * "stabilité ON/OFF/stimulus". A page that changes on every single step is
 * noise (a tick/frame counter); a page that changes exactly once is a
 * one-time state change (e.g. injected module init); a page that changes on
 * more than one step but not on every step is the strongest candidate for
 * holding the actual toggled effect.
 */
class ProfilerTimelineTracker {
public:
    struct PageStability {
        uint64_t address{0};
        int changeCount{0};
        int stepsPresent{0};
        QString classification; ///< "toggle_state_candidate" | "runtime_noise" | "one_time_state_change"
        QStringList changedAtSteps;
    };

    /// Starts a new named step. Must be called before any recordPageHash()
    /// call for that step.
    void beginStep(const QString& stepName);

    /// Records one page's hash for the step currently open (call beginStep
    /// first). Safe to call in any address order within a step.
    void recordPageHash(uint64_t address, uint64_t hash);

    /// Classified pages across the whole session so far, unchanged pages
    /// excluded, sorted with toggle_state_candidate first (strongest
    /// signal), then one_time_state_change, then runtime_noise.
    QList<PageStability> classifyPages() const;
    QVariantList classifyPagesAsVariant() const;

    int stepCount() const;
    QStringList stepNames() const;
    void reset();

private:
    QStringList m_stepNames;
    // address -> hash per step, one entry per step index, -1 (via qint64) = page absent that step.
    QHash<uint64_t, QList<qint64>> m_hashesByAddress;
};

} // namespace killcore
