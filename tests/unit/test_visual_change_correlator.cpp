#include "scanner/visual_change_correlator.h"
#include <gtest/gtest.h>

namespace {
QVariantMap observation() {
    return {{"description", "I gained one point: 100 -> 101"},
        {"previousValue", "100"}, {"currentValue", "101"}};
}
QVariantMap candidate(QString address = "1000", QString bytes = "64000000") {
    return {{"address", address}, {"type", "Int32"}, {"variantLabel", ""},
        {"bytesHex", bytes}, {"readable", true}, {"source", "changed_pages"}};
}
QVariantMap snapshot(bool after, QVariantList candidates) {
    return {{"success", true}, {"processInstance", "42:1:1234"},
        {"startedMs", after ? 2000 : 1000}, {"finishedMs", after ? 2010 : 1010},
        {"candidates", candidates}};
}
QVariantMap correlate(QVariantList before, QVariantList after, QVariantMap input = observation()) {
    return killcore::correlateVisualChange(input, snapshot(false, before), snapshot(true, after));
}
QString firstStatus(const QVariantMap& result) {
    return result.value("candidates").toList().value(0).toMap().value("status").toString();
}
}

TEST(VisualChangeCorrelator, TwoMatchingSamplesAreCorrelationNeverCausality) {
    const auto result = correlate({candidate()}, {candidate("1000", "65000000")});
    EXPECT_TRUE(result.value("success").toBool());
    EXPECT_EQ(firstStatus(result), "strong");
    EXPECT_FALSE(result.value("causalityProven").toBool());
    EXPECT_TRUE(result.value("observationUncertain").toBool());
}
TEST(VisualChangeCorrelator, MultipleMatchingAddressesStayAmbiguous) {
    const auto result = correlate({candidate(), candidate("2000")},
        {candidate("1000", "65000000"), candidate("2000", "65000000")});
    EXPECT_TRUE(result.value("ambiguous").toBool());
    EXPECT_EQ(result.value("status").toString(), "ambiguous");
}
TEST(VisualChangeCorrelator, SameAddressFromTwoProvidersIsNotIndependentEvidence) {
    auto duplicate = candidate("0x1000", "65000000");
    duplicate["source"] = "ui_string";
    const auto result = correlate({candidate()}, {candidate("1000", "65000000"), duplicate});
    EXPECT_EQ(result.value("candidates").toList().size(), 1);
    EXPECT_FALSE(result.value("ambiguous").toBool());
}
TEST(VisualChangeCorrelator, WrongRecognitionContradictsInsteadOfConfirming) {
    auto input = observation();
    input["currentValue"] = "107";
    EXPECT_EQ(firstStatus(correlate({candidate()}, {candidate("1000", "65000000")}, input)), "contradicted");
}
TEST(VisualChangeCorrelator, UnchangedDecoyIsContradictedByDeclaredChange) {
    EXPECT_EQ(firstStatus(correlate({candidate()}, {candidate()})), "contradicted");
}
TEST(VisualChangeCorrelator, TextOnlyDoesNotInventNumericRecognition) {
    auto input = observation();
    input.remove("previousValue"); input.remove("currentValue");
    EXPECT_EQ(firstStatus(correlate({candidate()}, {candidate("1000", "65000000")}, input)), "weak");
}
TEST(VisualChangeCorrelator, ScaledRepresentationComparesEncodedValues) {
    auto before = candidate("1000", "10270000"); // 100 * 100
    auto after = candidate("1000", "74270000"); // 101 * 100
    before["variantLabel"] = "Int32 x100"; after["variantLabel"] = "Int32 x100";
    EXPECT_EQ(firstStatus(correlate({before}, {after})), "strong");
}
TEST(VisualChangeCorrelator, UnknownVariantCannotFallBackToUnscaledMatch) {
    auto before = candidate(), after = candidate("1000", "65000000");
    before["variantLabel"] = "unknown"; after["variantLabel"] = "unknown";
    EXPECT_EQ(firstStatus(correlate({before}, {after})), "unsupported");
}
TEST(VisualChangeCorrelator, MissingBaselineDoesNotPromoteFreshHit) {
    EXPECT_EQ(firstStatus(correlate({}, {candidate("1000", "65000000")})), "unavailable");
}
TEST(VisualChangeCorrelator, UnreadableCandidateIsNotAContradiction) {
    auto after = candidate(); after["readable"] = false;
    EXPECT_EQ(firstStatus(correlate({candidate()}, {after})), "unavailable");
}
TEST(VisualChangeCorrelator, ReusedPidWithNewCreationTimeInvalidatesWindow) {
    auto after = snapshot(true, {candidate()}); after["processInstance"] = "42:1:5678";
    const auto result = killcore::correlateVisualChange(observation(), snapshot(false, {candidate()}), after);
    EXPECT_FALSE(result.value("success").toBool());
    EXPECT_EQ(result.value("status").toString(), "process_changed");
}
TEST(VisualChangeCorrelator, ExpiredAndReversedSamplesAreRejected) {
    auto before = snapshot(false, {candidate()}), after = snapshot(true, {candidate()});
    after["finishedMs"] = 601001;
    EXPECT_EQ(killcore::correlateVisualChange(observation(), before, after).value("status").toString(), "expired_window");
    after = snapshot(true, {candidate()}); after["startedMs"] = 900;
    EXPECT_FALSE(killcore::correlateVisualChange(observation(), before, after).value("success").toBool());
}
TEST(VisualChangeCorrelator, EmptySourcesRemainInconclusive) {
    const auto result = correlate({}, {});
    EXPECT_EQ(result.value("status").toString(), "inconclusive");
    EXPECT_FALSE(result.value("ambiguous").toBool());
}
TEST(VisualChangeCorrelator, PartialSamplingIsReported) {
    auto before = snapshot(false, {candidate()}); before["partial"] = true;
    const auto result = killcore::correlateVisualChange(observation(), before, snapshot(true, {candidate("1000", "65000000")}));
    EXPECT_TRUE(result.value("partial").toBool());
}
TEST(VisualChangeCorrelator, ExperimentReflectsDeclaredHypotheses) {
    auto input = observation(); input["hypothesis"] = "maximum";
    EXPECT_EQ(correlate({}, {}, input).value("experiment").toString(), "change_maximum_keep_current");
    input["hypothesis"] = "animation";
    EXPECT_EQ(correlate({}, {}, input).value("experiment").toString(), "wait_for_animation_then_repeat");
}
TEST(VisualChangeCorrelator, AnimatedIntermediateValueIsNotStrongCorrelation) {
    EXPECT_EQ(firstStatus(correlate({candidate()}, {candidate("1000", "66000000")})), "contradicted");
}
TEST(VisualChangeCorrelator, EmptyDescriptionRequiresObservation) {
    auto input = observation(); input["description"] = " ";
    EXPECT_FALSE(correlate({}, {}, input).value("success").toBool());
}
