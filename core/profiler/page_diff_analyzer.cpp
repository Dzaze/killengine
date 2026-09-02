#include "profiler/page_diff_analyzer.h"

#include <algorithm>

namespace killcore {

namespace {

uint64_t pageHash(const char* data, qsizetype length) {
    uint64_t hash = 1469598103934665603ull;
    for (qsizetype i = 0; i < length; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

QList<PageContent> splitIntoPages(
    uint64_t baseAddress,
    const QByteArray& content,
    uint64_t& byteBudgetRemaining,
    uint64_t pageSize) {
    QList<PageContent> pages;
    if (pageSize == 0 || content.isEmpty()) {
        return pages;
    }

    const qsizetype total = content.size();
    pages.reserve(static_cast<int>((total + static_cast<qsizetype>(pageSize) - 1) / static_cast<qsizetype>(pageSize)));

    for (qsizetype offset = 0; offset < total; offset += static_cast<qsizetype>(pageSize)) {
        const qsizetype length = std::min<qsizetype>(static_cast<qsizetype>(pageSize), total - offset);

        PageContent page;
        page.address = baseAddress + static_cast<uint64_t>(offset);
        page.hash = pageHash(content.constData() + offset, length);
        page.hashed = true;

        if (byteBudgetRemaining >= static_cast<uint64_t>(length)) {
            page.bytes = content.mid(static_cast<int>(offset), static_cast<int>(length));
            byteBudgetRemaining -= static_cast<uint64_t>(length);
        }

        pages.append(page);
    }

    return pages;
}

QList<PageDiffResult> diffPageContents(
    const QList<PageContent>& before,
    const QList<PageContent>& after,
    bool executableRegion,
    bool newlyAddedModuleRegion,
    int maxSampleDeltas) {
    QHash<uint64_t, PageContent> beforeByAddress;
    beforeByAddress.reserve(before.size());
    for (const auto& page : before) {
        beforeByAddress.insert(page.address, page);
    }

    const QString classification = newlyAddedModuleRegion
        ? "injected_module_state"
        : (executableRegion ? "code_patch_candidate" : "mapped_data_changed");

    QList<PageDiffResult> results;
    for (const auto& afterPage : after) {
        const auto it = beforeByAddress.constFind(afterPage.address);
        if (it == beforeByAddress.constEnd()) {
            continue; // Page only exists in `after`: reported at region level (regionsAdded), not here.
        }
        const PageContent& beforePage = it.value();
        if (!beforePage.hashed || !afterPage.hashed || beforePage.hash == afterPage.hash) {
            continue;
        }

        PageDiffResult diff;
        diff.address = afterPage.address;
        diff.hashChanged = true;
        diff.classification = classification;

        if (!beforePage.bytes.isEmpty() && !afterPage.bytes.isEmpty()
            && beforePage.bytes.size() == afterPage.bytes.size()) {
            qint64 differing = 0;
            const qsizetype length = beforePage.bytes.size();
            for (qsizetype i = 0; i < length; ++i) {
                const unsigned char b = static_cast<unsigned char>(beforePage.bytes.at(i));
                const unsigned char a = static_cast<unsigned char>(afterPage.bytes.at(i));
                if (b != a) {
                    ++differing;
                    if (diff.sampleDeltas.size() < maxSampleDeltas) {
                        QVariantMap delta;
                        delta["offset"] = static_cast<qulonglong>(i);
                        delta["before"] = static_cast<int>(b);
                        delta["after"] = static_cast<int>(a);
                        diff.sampleDeltas.append(delta);
                    }
                }
            }
            diff.byteDiffCount = differing;
        }

        results.append(diff);
    }

    return results;
}

QVariantMap pageDiffResultToVariant(const PageDiffResult& result) {
    QVariantMap variant;
    variant["address"] = QString::number(result.address, 16);
    variant["hashChanged"] = result.hashChanged;
    variant["byteDiffCount"] = result.byteDiffCount;
    variant["sampleDeltas"] = result.sampleDeltas;
    variant["classification"] = result.classification;
    return variant;
}

void ProfilerTimelineTracker::beginStep(const QString& stepName) {
    m_stepNames.append(stepName);
}

void ProfilerTimelineTracker::recordPageHash(uint64_t address, uint64_t hash) {
    if (m_stepNames.isEmpty()) {
        return; // beginStep() not called — ignore rather than misattribute to the wrong step.
    }
    const int currentIndex = m_stepNames.size() - 1;
    QList<qint64>& hashes = m_hashesByAddress[address];
    while (hashes.size() < currentIndex) {
        hashes.append(-1);
    }
    hashes.append(static_cast<qint64>(hash));
}

QList<ProfilerTimelineTracker::PageStability> ProfilerTimelineTracker::classifyPages() const {
    QList<PageStability> results;
    const int totalSteps = m_stepNames.size();
    if (totalSteps < 2) {
        return results;
    }
    const int totalTransitions = totalSteps - 1;

    for (auto it = m_hashesByAddress.constBegin(); it != m_hashesByAddress.constEnd(); ++it) {
        QList<qint64> hashes = it.value();
        while (hashes.size() < totalSteps) {
            hashes.append(-1);
        }

        int changeCount = 0;
        int stepsPresent = 0;
        QStringList changedAtSteps;
        for (int i = 0; i < totalSteps; ++i) {
            if (hashes.at(i) != -1) {
                ++stepsPresent;
            }
            if (i > 0 && hashes.at(i) != hashes.at(i - 1)) {
                ++changeCount;
                changedAtSteps.append(m_stepNames.at(i));
            }
        }

        if (changeCount == 0) {
            continue;
        }

        PageStability stability;
        stability.address = it.key();
        stability.changeCount = changeCount;
        stability.stepsPresent = stepsPresent;
        stability.changedAtSteps = changedAtSteps;

        if (changeCount == 1) {
            stability.classification = "one_time_state_change";
        } else if (changeCount == totalTransitions && totalTransitions >= 2) {
            stability.classification = "runtime_noise";
        } else {
            stability.classification = "toggle_state_candidate";
        }

        results.append(stability);
    }

    const auto rank = [](const QString& classification) {
        if (classification == "toggle_state_candidate") return 0;
        if (classification == "one_time_state_change") return 1;
        return 2; // runtime_noise
    };
    std::sort(results.begin(), results.end(), [&](const PageStability& a, const PageStability& b) {
        const int rankA = rank(a.classification);
        const int rankB = rank(b.classification);
        if (rankA != rankB) return rankA < rankB;
        return a.changeCount > b.changeCount;
    });

    return results;
}

QVariantList ProfilerTimelineTracker::classifyPagesAsVariant() const {
    QVariantList list;
    for (const auto& stability : classifyPages()) {
        QVariantMap entry;
        entry["address"] = QString::number(stability.address, 16);
        entry["changeCount"] = stability.changeCount;
        entry["stepsPresent"] = stability.stepsPresent;
        entry["classification"] = stability.classification;
        entry["changedAtSteps"] = QVariant(stability.changedAtSteps);
        list.append(entry);
    }
    return list;
}

int ProfilerTimelineTracker::stepCount() const {
    return m_stepNames.size();
}

QStringList ProfilerTimelineTracker::stepNames() const {
    return m_stepNames;
}

void ProfilerTimelineTracker::reset() {
    m_stepNames.clear();
    m_hashesByAddress.clear();
}

} // namespace killcore
