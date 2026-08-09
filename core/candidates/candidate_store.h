#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QTemporaryFile>

#include <cstddef>
#include <cstdint>
#include <memory>

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
    CandidateStore() = default;
    ~CandidateStore();

    CandidateStore(const CandidateStore&) = delete;
    CandidateStore& operator=(const CandidateStore&) = delete;
    CandidateStore(CandidateStore&& other) noexcept;
    CandidateStore& operator=(CandidateStore&& other) noexcept;

    void clear();
    void replaceFromScan(const ScanResult& scan, const QByteArray& scannedValue);
    void replaceCandidates(const QList<Candidate>& candidates);

    size_t size() const;
    bool isEmpty() const;
    const QList<Candidate>& candidates() const;
    bool isFileBacked() const;
    QString backingFilePath() const;
    size_t fileBackedThreshold() const;
    void setFileBackedThreshold(size_t threshold);

    void sortByAddress(bool ascending = true);
    CandidatePage page(size_t pageIndex, size_t pageSize, const QString& addressFilter = {}) const;

private:
    struct StoredCandidate {
        uint64_t address{0};
        uint8_t type{0};
        uint8_t valueSize{0};
        char value[8]{};
    };

    void persistIfNeeded();
    bool writeCandidatesToFile(const QList<Candidate>& candidates);
    bool readCandidateAt(size_t index, Candidate* candidate) const;
    Candidate storedToCandidate(const StoredCandidate& stored) const;
    StoredCandidate candidateToStored(const Candidate& candidate) const;
    void loadFileBackedCandidates() const;

    mutable QList<Candidate> m_candidates;
    size_t m_totalCount{0};
    size_t m_fileBackedThreshold{250000};
    std::unique_ptr<QTemporaryFile> m_backingFile;
};

} // namespace killcore
