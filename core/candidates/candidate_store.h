#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstddef>
#include <cstdint>

namespace killcore {

struct Candidate {
    uint64_t  address{0};
    ValueType type{ValueType::Int32};
    QByteArray lastValue;
};

struct CandidatePage {
    size_t totalCount{0};
    size_t pageIndex{0};
    size_t pageSize{0};
    QList<Candidate> candidates;
};

class CandidateStore {
public:
    void clear();
    void replaceFromScan(const ScanResult& scan, const QByteArray& scannedValue);
    void replaceCandidates(const QList<Candidate>& candidates);

    size_t size() const;
    bool isEmpty() const;
    const QList<Candidate>& candidates() const;

    void sortByAddress(bool ascending = true);
    CandidatePage page(size_t pageIndex, size_t pageSize, const QString& addressFilter = {}) const;

private:
    QList<Candidate> m_candidates;
};

} // namespace killcore
