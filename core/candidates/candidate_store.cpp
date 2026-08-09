#include "candidate_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <utility>

namespace killcore {

CandidateStore::~CandidateStore() {
    clear();
}

CandidateStore::CandidateStore(CandidateStore&& other) noexcept
    : m_candidates(std::move(other.m_candidates)),
      m_totalCount(other.m_totalCount),
      m_fileBackedThreshold(other.m_fileBackedThreshold),
      m_backingFile(std::move(other.m_backingFile)) {
    other.m_totalCount = 0;
}

CandidateStore& CandidateStore::operator=(CandidateStore&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    clear();
    m_candidates = std::move(other.m_candidates);
    m_totalCount = other.m_totalCount;
    m_fileBackedThreshold = other.m_fileBackedThreshold;
    m_backingFile = std::move(other.m_backingFile);

    other.m_totalCount = 0;
    return *this;
}

void CandidateStore::clear() {
    m_candidates.clear();
    m_totalCount = 0;
    m_backingFile.reset();
}

void CandidateStore::replaceFromScan(const ScanResult& scan, const QByteArray& scannedValue) {
    m_candidates.clear();
    m_backingFile.reset();
    m_candidates.reserve(scan.matches.size());

    for (const auto& match : scan.matches) {
        m_candidates.append({match.address, match.type, scannedValue});
    }

    sortByAddress();
    m_totalCount = static_cast<size_t>(m_candidates.size());
    persistIfNeeded();
}

void CandidateStore::replaceCandidates(const QList<Candidate>& candidates) {
    m_backingFile.reset();
    m_candidates = candidates;
    sortByAddress();
    m_totalCount = static_cast<size_t>(m_candidates.size());
    persistIfNeeded();
}

size_t CandidateStore::size() const {
    return m_totalCount;
}

bool CandidateStore::isEmpty() const {
    return m_totalCount == 0;
}

const QList<Candidate>& CandidateStore::candidates() const {
    loadFileBackedCandidates();
    return m_candidates;
}

bool CandidateStore::isFileBacked() const {
    return m_backingFile != nullptr;
}

QString CandidateStore::backingFilePath() const {
    return m_backingFile ? m_backingFile->fileName() : QString();
}

size_t CandidateStore::fileBackedThreshold() const {
    return m_fileBackedThreshold;
}

size_t CandidateStore::storageBytes() const {
    if (m_backingFile) {
        return static_cast<size_t>(std::max<qint64>(0, QFileInfo(*m_backingFile).size()));
    }
    return estimatedMemoryBytes();
}

size_t CandidateStore::estimatedMemoryBytes() const {
    size_t total = sizeof(CandidateStore);
    if (m_backingFile && m_candidates.isEmpty()) {
        return total;
    }
    total += static_cast<size_t>(m_candidates.capacity()) * sizeof(Candidate);
    for (const auto& candidate : m_candidates) {
        total += static_cast<size_t>(candidate.lastValue.capacity());
    }
    return total;
}

void CandidateStore::setFileBackedThreshold(size_t threshold) {
    m_fileBackedThreshold = threshold;
    persistIfNeeded();
}

bool CandidateStore::firstCandidate(Candidate* candidate) const {
    if (!candidate || isEmpty()) {
        return false;
    }
    if (isFileBacked() && m_candidates.isEmpty()) {
        return readCandidateAt(0, candidate);
    }
    *candidate = m_candidates.first();
    return true;
}

CandidateStreamSnapshot CandidateStore::streamSnapshot() const {
    CandidateStreamSnapshot snapshot;
    snapshot.fileBacked = isFileBacked() && m_candidates.isEmpty();
    snapshot.filePath = backingFilePath();
    snapshot.totalCount = m_totalCount;
    if (!snapshot.fileBacked) {
        snapshot.memoryCandidates = candidates();
        snapshot.totalCount = static_cast<size_t>(snapshot.memoryCandidates.size());
    }
    return snapshot;
}

bool CandidateStore::forEachCandidate(
    const CandidateStreamSnapshot& snapshot,
    const std::function<bool(const Candidate&)>& visitor,
    QString* error) {
    if (!visitor) {
        if (error) *error = "Candidate visitor is empty.";
        return false;
    }

    if (!snapshot.fileBacked) {
        for (const auto& candidate : snapshot.memoryCandidates) {
            if (!visitor(candidate)) {
                return false;
            }
        }
        return true;
    }

    QFile file(snapshot.filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = "Unable to open candidate stream file.";
        return false;
    }

    for (size_t i = 0; i < snapshot.totalCount; ++i) {
        StoredCandidate stored;
        const qint64 read = file.read(
            reinterpret_cast<char*>(&stored),
            static_cast<qint64>(sizeof(StoredCandidate)));
        if (read != static_cast<qint64>(sizeof(StoredCandidate))) {
            if (error) *error = "Unable to read candidate stream record.";
            return false;
        }
        if (!visitor(storedToCandidate(stored))) {
            return false;
        }
    }
    return true;
}

bool CandidateStore::beginFileBackedReplacement(QString* error) {
    clear();
    auto file = std::make_unique<QTemporaryFile>();
    file->setFileTemplate(QDir::tempPath() + "/killengine_candidates_XXXXXX.kecand");
    file->setAutoRemove(true);
    if (!file->open()) {
        if (error) *error = "Unable to create candidate stream file.";
        return false;
    }
    m_backingFile = std::move(file);
    return true;
}

bool CandidateStore::appendFileBackedCandidate(const Candidate& candidate, QString* error) {
    if (!m_backingFile) {
        if (error) *error = "Candidate stream file is not open.";
        return false;
    }
    const StoredCandidate stored = candidateToStored(candidate);
    const qint64 written = m_backingFile->write(
        reinterpret_cast<const char*>(&stored),
        static_cast<qint64>(sizeof(StoredCandidate)));
    if (written != static_cast<qint64>(sizeof(StoredCandidate))) {
        if (error) *error = "Unable to write candidate stream record.";
        return false;
    }
    ++m_totalCount;
    return true;
}

bool CandidateStore::finishFileBackedReplacement(QString* error) {
    if (!m_backingFile) {
        if (error) *error = "Candidate stream file is not open.";
        return false;
    }
    if (!m_backingFile->flush()) {
        if (error) *error = "Unable to flush candidate stream file.";
        return false;
    }
    m_candidates.clear();
    return true;
}

void CandidateStore::sortByAddress(bool ascending) {
    loadFileBackedCandidates();
    std::sort(m_candidates.begin(), m_candidates.end(),
              [ascending](const Candidate& a, const Candidate& b) {
                  return ascending ? a.address < b.address : a.address > b.address;
              });
    m_totalCount = static_cast<size_t>(m_candidates.size());
}

CandidatePage CandidateStore::page(size_t pageIndex, size_t pageSize, const QString& addressFilter) const {
    CandidatePage result;
    result.pageIndex = pageIndex;
    result.pageSize = pageSize;

    if (pageSize == 0) {
        return result;
    }

    const QString normalized = addressFilter.trimmed().remove("0x", Qt::CaseInsensitive).toLower();
    const bool hasFilter = !normalized.isEmpty();
    const size_t start = pageIndex * pageSize;

    if (isFileBacked() && m_candidates.isEmpty()) {
        size_t matched = 0;
        for (size_t i = 0; i < m_totalCount; ++i) {
            Candidate candidate;
            if (!readCandidateAt(i, &candidate)) {
                break;
            }
            if (hasFilter && !QString::number(candidate.address, 16).toLower().contains(normalized)) {
                continue;
            }
            if (matched >= start && result.candidates.size() < static_cast<qsizetype>(pageSize)) {
                result.candidates.append(candidate);
            }
            ++matched;
        }
        result.totalCount = matched;
        return result;
    }

    QList<Candidate> filtered;
    if (!hasFilter) {
        filtered = m_candidates;
    } else {
        for (const auto& candidate : m_candidates) {
            if (QString::number(candidate.address, 16).toLower().contains(normalized)) {
                filtered.append(candidate);
            }
        }
    }

    result.totalCount = static_cast<size_t>(filtered.size());
    if (start >= result.totalCount) {
        return result;
    }

    const size_t end = std::min(start + pageSize, result.totalCount);
    for (size_t i = start; i < end; ++i) {
        result.candidates.append(filtered.at(static_cast<qsizetype>(i)));
    }

    return result;
}

void CandidateStore::persistIfNeeded() {
    if (m_fileBackedThreshold == 0
        || static_cast<size_t>(m_candidates.size()) <= m_fileBackedThreshold) {
        return;
    }

    writeCandidatesToFile(m_candidates);
}

bool CandidateStore::writeCandidatesToFile(const QList<Candidate>& candidates) {
    auto file = std::make_unique<QTemporaryFile>();
    file->setFileTemplate(QDir::tempPath() + "/killengine_candidates_XXXXXX.kecand");
    file->setAutoRemove(true);
    if (!file->open()) {
        return false;
    }

    for (const auto& candidate : candidates) {
        const StoredCandidate stored = candidateToStored(candidate);
        const qint64 written = file->write(
            reinterpret_cast<const char*>(&stored),
            static_cast<qint64>(sizeof(StoredCandidate)));
        if (written != static_cast<qint64>(sizeof(StoredCandidate))) {
            return false;
        }
    }

    if (!file->flush()) {
        return false;
    }

    m_backingFile = std::move(file);
    m_totalCount = static_cast<size_t>(candidates.size());
    m_candidates.clear();
    return true;
}

bool CandidateStore::readCandidateAt(size_t index, Candidate* candidate) const {
    if (!candidate || !m_backingFile) {
        return false;
    }

    const qint64 offset = static_cast<qint64>(index * sizeof(StoredCandidate));
    if (!m_backingFile->seek(offset)) {
        return false;
    }

    StoredCandidate stored;
    const qint64 read = m_backingFile->read(
        reinterpret_cast<char*>(&stored),
        static_cast<qint64>(sizeof(StoredCandidate)));
    if (read != static_cast<qint64>(sizeof(StoredCandidate))) {
        return false;
    }

    *candidate = storedToCandidate(stored);
    return true;
}

CandidateStore::StoredCandidate CandidateStore::candidateToStored(const Candidate& candidate) {
    StoredCandidate stored;
    stored.address = candidate.address;
    stored.type = static_cast<uint8_t>(candidate.type);
    const qsizetype valueSize = std::min<qsizetype>(candidate.lastValue.size(), sizeof(stored.value));
    stored.valueSize = static_cast<uint8_t>(std::max<qsizetype>(valueSize, 0));
    if (valueSize > 0) {
        std::memcpy(stored.value, candidate.lastValue.constData(), static_cast<size_t>(valueSize));
    }
    return stored;
}

Candidate CandidateStore::storedToCandidate(const StoredCandidate& stored) {
    Candidate candidate;
    candidate.address = stored.address;
    candidate.type = static_cast<ValueType>(stored.type);
    const qsizetype valueSize = std::min<qsizetype>(stored.valueSize, sizeof(stored.value));
    if (valueSize > 0) {
        candidate.lastValue = QByteArray(stored.value, valueSize);
    }
    return candidate;
}

void CandidateStore::loadFileBackedCandidates() const {
    if (!m_backingFile || !m_candidates.isEmpty()) {
        return;
    }

    m_candidates.reserve(static_cast<qsizetype>(m_totalCount));
    for (size_t i = 0; i < m_totalCount; ++i) {
        Candidate candidate;
        if (!readCandidateAt(i, &candidate)) {
            break;
        }
        m_candidates.append(candidate);
    }
}

} // namespace killcore
