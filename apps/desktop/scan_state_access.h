#pragma once

#include "candidates/candidate_store.h"
#include "snapshot/snapshot_store.h"

#include <cstddef>

namespace killengine {

class ScanStateAccess {
public:
    ScanStateAccess(
        killcore::CandidateStore& candidates,
        killcore::CandidateStore& previousCandidates,
        killcore::SnapshotStore& snapshot);

    killcore::CandidateStore& candidates();
    const killcore::CandidateStore& candidates() const;

    killcore::CandidateStore& previousCandidates();
    const killcore::CandidateStore& previousCandidates() const;

    killcore::SnapshotStore& snapshot();
    const killcore::SnapshotStore& snapshot() const;

    void setFileBackedThreshold(size_t threshold);

    void clearCandidates();
    void clearPreviousCandidates();
    void clearSnapshot();
    void clearAll();

private:
    killcore::CandidateStore& m_candidates;
    killcore::CandidateStore& m_previousCandidates;
    killcore::SnapshotStore& m_snapshot;
};

} // namespace killengine
