#include <gtest/gtest.h>

#include "candidates/candidate_store.h"

#include <limits>

TEST(CandidateStoreTest, SortsAndPaginates) {
    killcore::ScanResult scan;
    scan.matches.append({0x3000, killcore::ValueType::Int32});
    scan.matches.append({0x1000, killcore::ValueType::Int32});
    scan.matches.append({0x2000, killcore::ValueType::Int64});

    killcore::CandidateStore store;
    store.replaceFromScan(scan, {});

    auto page = store.page(0, 2);
    ASSERT_EQ(page.totalCount, 3u);
    ASSERT_EQ(page.candidates.size(), 2);
    EXPECT_EQ(page.candidates[0].address, 0x1000u);
    EXPECT_EQ(page.candidates[1].address, 0x2000u);

    page = store.page(1, 2);
    ASSERT_EQ(page.candidates.size(), 1);
    EXPECT_EQ(page.candidates[0].address, 0x3000u);
}

TEST(CandidateStoreTest, FiltersByAddressText) {
    killcore::ScanResult scan;
    scan.matches.append({0x1000, killcore::ValueType::Int32});
    scan.matches.append({0x2000, killcore::ValueType::Int32});

    killcore::CandidateStore store;
    store.replaceFromScan(scan, {});

    auto page = store.page(0, 10, "20");
    ASSERT_EQ(page.totalCount, 1u);
    ASSERT_EQ(page.candidates.size(), 1);
    EXPECT_EQ(page.candidates[0].address, 0x2000u);
}

// Phase 5 — validation avec un large set de candidats (100 000+)
TEST(CandidateStoreTest, HandlesLargeCandidateSet) {
    constexpr int kCandidateCount = 100000;

    killcore::ScanResult scan;
    scan.matches.reserve(kCandidateCount);
    for (int i = 0; i < kCandidateCount; ++i) {
        scan.matches.append({static_cast<uint64_t>(0x10000 + i * 4), killcore::ValueType::Int32});
    }

    killcore::CandidateStore store;
    store.replaceFromScan(scan, {});

    // Le store doit contenir exactement kCandidateCount entrées
    EXPECT_EQ(store.size(), static_cast<size_t>(kCandidateCount));

    // La pagination doit fonctionner sur un large set
    auto page = store.page(0, 100);
    EXPECT_EQ(page.totalCount, static_cast<size_t>(kCandidateCount));
    EXPECT_EQ(page.candidates.size(), 100u);

    // La dernière page doit être correcte
    const size_t lastPageIndex = (kCandidateCount / 100) - 1;
    page = store.page(lastPageIndex, 100);
    EXPECT_EQ(page.candidates.size(), 100u);

    // Le filtrage doit fonctionner sur un large set
    page = store.page(0, 50, "1234");
    EXPECT_GT(page.totalCount, 0u);
    EXPECT_LE(page.totalCount, static_cast<size_t>(kCandidateCount));
}

TEST(CandidateStoreTest, SpillsLargeCandidateSetToTemporaryFile) {
    killcore::ScanResult scan;
    for (int i = 0; i < 12; ++i) {
        scan.matches.append({static_cast<uint64_t>(0x2000 + i * 4), killcore::ValueType::Int32});
    }

    killcore::CandidateStore store;
    store.setFileBackedThreshold(5);
    store.replaceFromScan(scan, QByteArray::fromHex("64000000"));

    EXPECT_TRUE(store.isFileBacked());
    EXPECT_FALSE(store.backingFilePath().isEmpty());
    EXPECT_EQ(store.size(), 12u);

    auto page = store.page(1, 4);
    ASSERT_EQ(page.totalCount, 12u);
    ASSERT_EQ(page.candidates.size(), 4);
    EXPECT_EQ(page.candidates[0].address, 0x2010u);
    EXPECT_EQ(page.candidates[0].lastValue, QByteArray::fromHex("64000000"));
}

// Le format binaire fichier (StoredCandidate) a longtemps perdu confidence/
// variantLabel/secondaryVariant au round-trip (retour silencieux à
// confidence=1.0) dès qu'un scan dépassait fileBackedThreshold — ce qui
// arrive facilement sur un jeu de plusieurs Go de RAM. Ce test verrouille
// que ces champs survivent bien au bascule fichier.
TEST(CandidateStoreTest, PreservesConfidenceAndVariantAcrossFileBackedRoundTrip) {
    killcore::ScanResult scan;
    scan.matches.append({0x1000, killcore::ValueType::Int32, 0.72, "Float32 x100", true});
    scan.matches.append({0x2000, killcore::ValueType::Int32, 0.96, "", false});
    for (int i = 0; i < 10; ++i) {
        scan.matches.append({static_cast<uint64_t>(0x3000 + i * 4), killcore::ValueType::Int32});
    }

    killcore::CandidateStore store;
    store.setFileBackedThreshold(5);
    store.replaceFromScan(scan, QByteArray::fromHex("64000000"));
    ASSERT_TRUE(store.isFileBacked());

    auto page = store.page(0, 2);
    ASSERT_EQ(page.candidates.size(), 2);
    EXPECT_EQ(page.candidates[0].address, 0x1000u);
    EXPECT_DOUBLE_EQ(page.candidates[0].confidence, 0.72);
    EXPECT_EQ(page.candidates[0].variantLabel, "Float32 x100");
    EXPECT_TRUE(page.candidates[0].secondaryVariant);

    EXPECT_EQ(page.candidates[1].address, 0x2000u);
    EXPECT_DOUBLE_EQ(page.candidates[1].confidence, 0.96);
    EXPECT_TRUE(page.candidates[1].variantLabel.isEmpty());
    EXPECT_FALSE(page.candidates[1].secondaryVariant);
}

TEST(CandidateStoreTest, FiltersFileBackedCandidatesWithoutHydratingWholeStore) {
    killcore::ScanResult scan;
    scan.matches.append({0x1000, killcore::ValueType::Int32});
    scan.matches.append({0x1234, killcore::ValueType::Int32});
    scan.matches.append({0x2234, killcore::ValueType::Int32});
    scan.matches.append({0x3000, killcore::ValueType::Int32});

    killcore::CandidateStore store;
    store.setFileBackedThreshold(2);
    store.replaceFromScan(scan, {});

    ASSERT_TRUE(store.isFileBacked());
    auto page = store.page(0, 10, "234");
    ASSERT_EQ(page.totalCount, 2u);
    ASSERT_EQ(page.candidates.size(), 2);
    EXPECT_EQ(page.candidates[0].address, 0x1234u);
    EXPECT_EQ(page.candidates[1].address, 0x2234u);

    const auto& hydrated = store.candidates();
    ASSERT_EQ(hydrated.size(), 4);
    EXPECT_EQ(hydrated[0].address, 0x1000u);
}

TEST(CandidateStoreTest, IteratesFileBackedSnapshotAsStream) {
    killcore::ScanResult scan;
    for (int i = 0; i < 6; ++i) {
        scan.matches.append({static_cast<uint64_t>(0x4000 + i * 8), killcore::ValueType::Int64});
    }

    killcore::CandidateStore store;
    store.setFileBackedThreshold(3);
    store.replaceFromScan(scan, QByteArray::fromHex("0102030405060708"));

    const auto snapshot = store.streamSnapshot();
    ASSERT_TRUE(snapshot.fileBacked);
    ASSERT_EQ(snapshot.totalCount, 6u);

    size_t seen = 0;
    uint64_t lastAddress = 0;
    QString error;
    const bool ok = killcore::CandidateStore::forEachCandidate(snapshot, [&](const killcore::Candidate& candidate) {
        ++seen;
        lastAddress = candidate.address;
        EXPECT_EQ(candidate.type, killcore::ValueType::Int64);
        EXPECT_EQ(candidate.lastValue, QByteArray::fromHex("0102030405060708"));
        return true;
    }, &error);

    EXPECT_TRUE(ok) << error.toStdString();
    EXPECT_EQ(seen, 6u);
    EXPECT_EQ(lastAddress, 0x4028u);
}

TEST(CandidateStoreTest, WritesStreamingReplacementToTemporaryFile) {
    killcore::CandidateStore store;
    QString error;
    ASSERT_TRUE(store.beginFileBackedReplacement(&error)) << error.toStdString();

    for (int i = 0; i < 4; ++i) {
        killcore::Candidate candidate;
        candidate.address = static_cast<uint64_t>(0x5000 + i * 4);
        candidate.type = killcore::ValueType::Int32;
        candidate.lastValue = QByteArray::fromHex("2a000000");
        ASSERT_TRUE(store.appendFileBackedCandidate(candidate, &error)) << error.toStdString();
    }
    ASSERT_TRUE(store.finishFileBackedReplacement(&error)) << error.toStdString();

    EXPECT_TRUE(store.isFileBacked());
    EXPECT_EQ(store.size(), 4u);

    auto page = store.page(0, 10);
    ASSERT_EQ(page.totalCount, 4u);
    ASSERT_EQ(page.candidates.size(), 4);
    EXPECT_EQ(page.candidates[3].address, 0x500cu);
    EXPECT_EQ(page.candidates[3].lastValue, QByteArray::fromHex("2a000000"));
}

TEST(CandidateStoreTest, ClonesMemoryBackedCandidates) {
    killcore::ScanResult scan;
    scan.matches.append({0x6000, killcore::ValueType::Int32});
    scan.matches.append({0x6004, killcore::ValueType::Int32});

    killcore::CandidateStore store;
    store.replaceFromScan(scan, QByteArray::fromHex("07000000"));

    QString error;
    auto copy = store.clone(&error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_FALSE(copy.isFileBacked());
    EXPECT_EQ(copy.size(), 2u);

    store.clear();
    auto page = copy.page(0, 10);
    ASSERT_EQ(page.candidates.size(), 2);
    EXPECT_EQ(page.candidates[0].address, 0x6000u);
    EXPECT_EQ(page.candidates[0].lastValue, QByteArray::fromHex("07000000"));
}

TEST(CandidateStoreTest, ClonesFileBackedCandidatesWithoutHydratingSource) {
    killcore::ScanResult scan;
    for (int i = 0; i < 8; ++i) {
        scan.matches.append({static_cast<uint64_t>(0x7000 + i * 4), killcore::ValueType::Int32});
    }

    killcore::CandidateStore store;
    store.setFileBackedThreshold(3);
    store.replaceFromScan(scan, QByteArray::fromHex("09000000"));
    ASSERT_TRUE(store.isFileBacked());

    QString error;
    auto copy = store.clone(&error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    EXPECT_TRUE(copy.isFileBacked());
    EXPECT_EQ(copy.size(), 8u);

    store.clear();
    auto page = copy.page(1, 3);
    ASSERT_EQ(page.totalCount, 8u);
    ASSERT_EQ(page.candidates.size(), 3);
    EXPECT_EQ(page.candidates[0].address, 0x700cu);
    EXPECT_EQ(page.candidates[0].lastValue, QByteArray::fromHex("09000000"));
}

TEST(CandidateStoreTest, PaginatesFileAndMemoryBackedCandidatesIdenticallyAtBoundaries) {
    QList<killcore::Candidate> input;
    for (uint64_t address : {0x1000ull, 0x1200ull, 0x2200ull, 0x3000ull, 0x3200ull}) {
        killcore::Candidate candidate;
        candidate.address = address;
        candidate.type = killcore::ValueType::Int32;
        input.append(candidate);
    }

    for (size_t threshold : {size_t{0}, size_t{1}}) {
        killcore::CandidateStore store;
        store.setFileBackedThreshold(threshold);
        store.replaceCandidates(input);
        ASSERT_EQ(store.isFileBacked(), threshold == 1);

        auto page = store.page(1, 2);
        ASSERT_EQ(page.totalCount, 5u);
        ASSERT_EQ(page.candidates.size(), 2);
        EXPECT_EQ(page.candidates[0].address, 0x2200u);
        EXPECT_EQ(page.candidates[1].address, 0x3000u);

        page = store.page(2, 2);
        ASSERT_EQ(page.totalCount, 5u);
        ASSERT_EQ(page.candidates.size(), 1);
        EXPECT_EQ(page.candidates[0].address, 0x3200u);

        page = store.page(1, 2, "0x200");
        ASSERT_EQ(page.totalCount, 3u);
        ASSERT_EQ(page.candidates.size(), 1);
        EXPECT_EQ(page.candidates[0].address, 0x3200u);

        page = store.page(std::numeric_limits<size_t>::max(), 2);
        EXPECT_EQ(page.totalCount, 5u);
        EXPECT_TRUE(page.candidates.isEmpty());
    }
}
