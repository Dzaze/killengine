// EXTMOD-2 — tests de la logique pure de diff par pages 4K et de la
// classification de stabilite timeline (aucun processus reel requis).

#include "profiler/page_diff_analyzer.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QHash>

namespace {

QByteArray makeContent(qsizetype size, char fill) {
    return QByteArray(static_cast<int>(size), fill);
}

} // namespace

TEST(PageDiffAnalyzerTest, SplitIntoPagesProducesExpectedCountAndHashesEveryPage) {
    uint64_t budget = 1024ull * 1024ull;
    const QByteArray content = makeContent(4096 * 3 + 100, 'A');
    const auto pages = killcore::splitIntoPages(0x1000, content, budget, 4096);

    ASSERT_EQ(pages.size(), 4);
    EXPECT_EQ(pages[0].address, 0x1000ull);
    EXPECT_EQ(pages[1].address, 0x1000ull + 4096);
    EXPECT_EQ(pages[3].address, 0x1000ull + 4096ull * 3);
    for (const auto& page : pages) {
        EXPECT_TRUE(page.hashed);
    }
}

TEST(PageDiffAnalyzerTest, SplitIntoPagesRespectsByteBudget) {
    uint64_t budget = 4096; // Only enough for the first page.
    const QByteArray content = makeContent(4096 * 2, 'B');
    const auto pages = killcore::splitIntoPages(0, content, budget, 4096);

    ASSERT_EQ(pages.size(), 2);
    EXPECT_FALSE(pages[0].bytes.isEmpty());
    EXPECT_TRUE(pages[1].bytes.isEmpty()); // Budget exhausted.
    EXPECT_EQ(budget, 0u);
}

TEST(PageDiffAnalyzerTest, DiffPageContentsFindsOnlyChangedPages) {
    uint64_t budgetA = 1024ull * 1024ull;
    uint64_t budgetB = 1024ull * 1024ull;
    QByteArray contentA = makeContent(4096 * 3, 'X');
    QByteArray contentB = contentA;
    contentB[4096 + 10] = 'Y'; // Flip one byte inside the second page only.

    const auto before = killcore::splitIntoPages(0x2000, contentA, budgetA, 4096);
    const auto after = killcore::splitIntoPages(0x2000, contentB, budgetB, 4096);

    const auto diffs = killcore::diffPageContents(before, after, false, false, 16);
    ASSERT_EQ(diffs.size(), 1);
    EXPECT_EQ(diffs[0].address, 0x2000ull + 4096ull);
    EXPECT_EQ(diffs[0].byteDiffCount, 1);
    ASSERT_EQ(diffs[0].sampleDeltas.size(), 1);
    const QVariantMap delta = diffs[0].sampleDeltas.first().toMap();
    EXPECT_EQ(delta.value("offset").toLongLong(), 10);
    EXPECT_EQ(delta.value("before").toInt(), static_cast<int>('X'));
    EXPECT_EQ(delta.value("after").toInt(), static_cast<int>('Y'));
    EXPECT_EQ(diffs[0].classification, "mapped_data_changed");
}

TEST(PageDiffAnalyzerTest, DiffPageContentsClassifiesExecutableAndInjectedModule) {
    uint64_t budgetA = 1024ull * 1024ull;
    uint64_t budgetB = 1024ull * 1024ull;
    QByteArray contentA = makeContent(4096, 'C');
    QByteArray contentB = contentA;
    contentB[0] = 'D';

    const auto before = killcore::splitIntoPages(0x3000, contentA, budgetA, 4096);
    const auto after = killcore::splitIntoPages(0x3000, contentB, budgetB, 4096);

    const auto codeDiffs = killcore::diffPageContents(before, after, /*executableRegion=*/true, false, 16);
    ASSERT_EQ(codeDiffs.size(), 1);
    EXPECT_EQ(codeDiffs[0].classification, "code_patch_candidate");

    const auto moduleDiffs = killcore::diffPageContents(before, after, false, /*newlyAddedModuleRegion=*/true, 16);
    ASSERT_EQ(moduleDiffs.size(), 1);
    EXPECT_EQ(moduleDiffs[0].classification, "injected_module_state");
}

TEST(PageDiffAnalyzerTest, DiffPageContentsSkipsUnhashedOrUnchangedPages) {
    killcore::PageContent unchangedA;
    unchangedA.address = 0x100;
    unchangedA.hash = 42;
    unchangedA.hashed = true;
    killcore::PageContent unchangedB = unchangedA;

    killcore::PageContent notHashed;
    notHashed.address = 0x200;
    notHashed.hashed = false;
    notHashed.hash = 1;
    killcore::PageContent notHashedChanged = notHashed;
    notHashedChanged.hash = 2;

    const auto diffs = killcore::diffPageContents(
        {unchangedA, notHashed}, {unchangedB, notHashedChanged}, false, false, 16);
    EXPECT_TRUE(diffs.isEmpty());
}

TEST(PageDiffAnalyzerTest, DiffPageContentsIgnoresPagesOnlyPresentInAfter) {
    killcore::PageContent onlyAfter;
    onlyAfter.address = 0x9000;
    onlyAfter.hash = 7;
    onlyAfter.hashed = true;

    const auto diffs = killcore::diffPageContents({}, {onlyAfter}, false, false, 16);
    EXPECT_TRUE(diffs.isEmpty()); // Region-level "added" territory, not a page-level "changed" one.
}

TEST(PageDiffAnalyzerTest, DiffPageContentsCapsSampleDeltasButKeepsFullByteCount) {
    uint64_t budgetA = 1024ull * 1024ull;
    uint64_t budgetB = 1024ull * 1024ull;
    QByteArray contentA = makeContent(4096, '\0');
    QByteArray contentB = makeContent(4096, '\1'); // Every byte differs.

    const auto before = killcore::splitIntoPages(0, contentA, budgetA, 4096);
    const auto after = killcore::splitIntoPages(0, contentB, budgetB, 4096);

    const auto diffs = killcore::diffPageContents(before, after, false, false, 5);
    ASSERT_EQ(diffs.size(), 1);
    EXPECT_EQ(diffs[0].byteDiffCount, 4096);
    EXPECT_EQ(diffs[0].sampleDeltas.size(), 5);
}

TEST(PageDiffAnalyzerTest, TimelineTrackerClassifiesRuntimeNoiseTogglingCandidateAndOneTime) {
    killcore::ProfilerTimelineTracker tracker;

    // 5 steps: baseline, off, on, stimulus, off-again.
    tracker.beginStep("baseline");
    tracker.recordPageHash(0xA000, 1); // noise: changes every step.
    tracker.recordPageHash(0xB000, 1); // toggle-ish: flips only when toggled.
    tracker.recordPageHash(0xC000, 1); // one-time: changes once (module init) then stable.
    tracker.recordPageHash(0xD000, 1); // stable: never changes.

    tracker.beginStep("tool_attached_off");
    tracker.recordPageHash(0xA000, 2);
    tracker.recordPageHash(0xB000, 1);
    tracker.recordPageHash(0xC000, 99);
    tracker.recordPageHash(0xD000, 1);

    tracker.beginStep("toggle_on");
    tracker.recordPageHash(0xA000, 3);
    tracker.recordPageHash(0xB000, 2);
    tracker.recordPageHash(0xC000, 99);
    tracker.recordPageHash(0xD000, 1);

    tracker.beginStep("stimulus_done");
    tracker.recordPageHash(0xA000, 4);
    tracker.recordPageHash(0xB000, 2);
    tracker.recordPageHash(0xC000, 99);
    tracker.recordPageHash(0xD000, 1);

    tracker.beginStep("toggle_off");
    tracker.recordPageHash(0xA000, 5);
    tracker.recordPageHash(0xB000, 1);
    tracker.recordPageHash(0xC000, 99);
    tracker.recordPageHash(0xD000, 1);

    ASSERT_EQ(tracker.stepCount(), 5);

    const auto results = tracker.classifyPages();
    QHash<uint64_t, killcore::ProfilerTimelineTracker::PageStability> byAddress;
    for (const auto& r : results) {
        byAddress.insert(r.address, r);
    }

    ASSERT_TRUE(byAddress.contains(0xA000));
    EXPECT_EQ(byAddress[0xA000].classification, "runtime_noise");
    EXPECT_EQ(byAddress[0xA000].changeCount, 4);

    ASSERT_TRUE(byAddress.contains(0xB000));
    EXPECT_EQ(byAddress[0xB000].classification, "toggle_state_candidate");
    EXPECT_EQ(byAddress[0xB000].changeCount, 2);

    ASSERT_TRUE(byAddress.contains(0xC000));
    EXPECT_EQ(byAddress[0xC000].classification, "one_time_state_change");
    EXPECT_EQ(byAddress[0xC000].changeCount, 1);

    EXPECT_FALSE(byAddress.contains(0xD000)); // Never changes: excluded entirely.

    // toggle_state_candidate must rank ahead of runtime_noise and one_time_state_change.
    ASSERT_FALSE(results.isEmpty());
    EXPECT_EQ(results.first().classification, "toggle_state_candidate");
}

TEST(PageDiffAnalyzerTest, TimelineTrackerHandlesPageAbsentInSomeSteps) {
    killcore::ProfilerTimelineTracker tracker;

    tracker.beginStep("baseline"); // Page not present yet (e.g. module not loaded).
    tracker.beginStep("module_loaded");
    tracker.recordPageHash(0x5000, 10);
    tracker.beginStep("module_still_loaded");
    tracker.recordPageHash(0x5000, 10);

    const auto results = tracker.classifyPages();
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results.first().address, 0x5000ull);
    EXPECT_EQ(results.first().stepsPresent, 2);
    EXPECT_EQ(results.first().classification, "one_time_state_change"); // absent -> present counts as one change.
    ASSERT_EQ(results.first().changedAtSteps.size(), 1);
    EXPECT_EQ(results.first().changedAtSteps.first(), "module_loaded");
}

TEST(PageDiffAnalyzerTest, TimelineTrackerRequiresAtLeastTwoSteps) {
    killcore::ProfilerTimelineTracker tracker;
    tracker.beginStep("only_step");
    tracker.recordPageHash(0x1, 1);
    EXPECT_TRUE(tracker.classifyPages().isEmpty());
}

TEST(PageDiffAnalyzerTest, TimelineTrackerResetClearsSteps) {
    killcore::ProfilerTimelineTracker tracker;
    tracker.beginStep("a");
    tracker.recordPageHash(0x1, 1);
    tracker.beginStep("b");
    tracker.recordPageHash(0x1, 2);
    ASSERT_EQ(tracker.stepCount(), 2);

    tracker.reset();
    EXPECT_EQ(tracker.stepCount(), 0);
    EXPECT_TRUE(tracker.stepNames().isEmpty());
    EXPECT_TRUE(tracker.classifyPages().isEmpty());
}
