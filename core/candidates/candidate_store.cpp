#include "candidate_store.h"

#include <algorithm>

namespace killcore {

void CandidateStore::clear() {
    m_candidates.clear();
}

void CandidateStore::replaceFromScan(const ScanResult& scan, const QByteArray& scannedValue) {
    m_candidates.clear();
    m_candidates.reserve(scan.matches.size());

    for (const auto& match : scan.matches) {
        m_candidates.append({match.address, match.type, scannedValue});
    }

    sortByAddress();
}

void CandidateStore::replaceCandidates(const QList<Candidate>& candidates) {
    m_candidates = candidates;
    sortByAddress();
}

size_t CandidateStore::size() const {
    return static_cast<size_t>(m_candidates.size());
}

bool CandidateStore::isEmpty() const {
    return m_candidates.isEmpty();
}

const QList<Candidate>& CandidateStore::candidates() const {
    return m_candidates;
}

void CandidateStore::sortByAddress(bool ascending) {
    std::sort(m_candidates.begin(), m_candidates.end(),
              [ascending](const Candidate& a, const Candidate& b) {
                  return ascending ? a.address < b.address : a.address > b.address;
              });
}

CandidatePage CandidateStore::page(size_t pageIndex, size_t pageSize, const QString& addressFilter) const {
    CandidatePage result;
    result.pageIndex = pageIndex;
    result.pageSize = pageSize;

    if (pageSize == 0) {
        return result;
    }

    QList<Candidate> filtered;
    if (addressFilter.trimmed().isEmpty()) {
        filtered = m_candidates;
    } else {
        const QString normalized = addressFilter.trimmed().remove("0x", Qt::CaseInsensitive).toLower();
        for (const auto& candidate : m_candidates) {
            if (QString::number(candidate.address, 16).toLower().contains(normalized)) {
                filtered.append(candidate);
            }
        }
    }

    result.totalCount = static_cast<size_t>(filtered.size());
    const size_t start = pageIndex * pageSize;
    if (start >= result.totalCount) {
        return result;
    }

    const size_t end = std::min(start + pageSize, result.totalCount);
    for (size_t i = start; i < end; ++i) {
        result.candidates.append(filtered.at(static_cast<qsizetype>(i)));
    }

    return result;
}

} // namespace killcore
