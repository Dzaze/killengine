#include <gtest/gtest.h>

#include "candidates/candidate_store.h"

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
