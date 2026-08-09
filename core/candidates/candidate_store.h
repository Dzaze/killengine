#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QTemporaryFile>

#include <cstddef>
#include <cstdint>
#include <functional>
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

struct CandidateStreamSnapshot {
    bool fileBacked{false};
    QString filePath;
    size_t totalCount{0};
    QList<Candidate> memoryCandidates;
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
    size_t storageBytes() const;
    size_t estimatedMemoryBytes() const;
    void setFileBackedThreshold(size_t threshold);
    bool firstCandidate(Candidate* candidate) const;
    CandidateStreamSnapshot streamSnapshot() const;
    static bool forEachCandidate(
        const CandidateStreamSnapshot& snapshot,
        const std::function<bool(const Candidate&)>& visitor,
        QString* error = nullptr);

    bool beginFileBackedReplacement(QString* error = nullptr);
    bool appendFileBackedCandidate(const Candidate& candidate, QString* error = nullptr);
    bool finishFileBackedReplacement(QString* error = nullptr);

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
    static Candidate storedToCandidate(const StoredCandidate& stored);
    static StoredCandidate candidateToStored(const Candidate& candidate);
    void loadFileBackedCandidates() const;

    mutable QList<Candidate> m_candidates;
    size_t m_totalCount{0};
    size_t m_fileBackedThreshold{250000};
    std::unique_ptr<QTemporaryFile> m_backingFile;
};

} // namespace killcore
