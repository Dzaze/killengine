#include "scan_state_access.h"

namespace killengine {

ScanStateAccess::ScanStateAccess(
    killcore::CandidateStore& candidates,
    killcore::CandidateStore& previousCandidates,
    killcore::SnapshotStore& snapshot)
    : m_candidates(candidates)
    , m_previousCandidates(previousCandidates)
    , m_snapshot(snapshot) {}

killcore::CandidateStore& ScanStateAccess::candidates() {
    return m_candidates;
}

const killcore::CandidateStore& ScanStateAccess::candidates() const {
    return m_candidates;
}

killcore::CandidateStore& ScanStateAccess::previousCandidates() {
    return m_previousCandidates;
}

const killcore::CandidateStore& ScanStateAccess::previousCandidates() const {
    return m_previousCandidates;
}

killcore::SnapshotStore& ScanStateAccess::snapshot() {
    return m_snapshot;
}

const killcore::SnapshotStore& ScanStateAccess::snapshot() const {
    return m_snapshot;
}

void ScanStateAccess::setFileBackedThreshold(size_t threshold) {
    m_candidates.setFileBackedThreshold(threshold);
    m_previousCandidates.setFileBackedThreshold(threshold);
}

void ScanStateAccess::clearCandidates() {
    m_candidates.clear();
}

void ScanStateAccess::clearPreviousCandidates() {
    m_previousCandidates.clear();
}

void ScanStateAccess::clearSnapshot() {
    m_snapshot.clear();
}

void ScanStateAccess::clearAll() {
    clearCandidates();
    clearPreviousCandidates();
    clearSnapshot();
}

} // namespace killengine
