#include <gtest/gtest.h>

#include "scanner/display_value_tracker.h"

#include <QByteArray>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

void writeInt32(QByteArray* buffer, int offset, int32_t value) {
    ASSERT_NE(buffer, nullptr);
    ASSERT_GE(offset, 0);
    ASSERT_LE(offset + static_cast<int>(sizeof(value)), buffer->size());
    std::memcpy(buffer->data() + offset, &value, sizeof(value));
}

} // namespace

TEST(UiStringTracker, FindsIsolatedAsciiAndUtf16DisplayedNumber) {
    QByteArray memory(128, '\0');
    memory.replace(8, 3, QByteArray("150"));
    memory.replace(32, 2, QByteArray("50"));
    const QByteArray utf16 = killcore::encodeUiStringValue("50", "utf16");
    std::memcpy(memory.data() + 64, utf16.constData(), utf16.size());

    const auto matches = killcore::findUiStringMatchesInBuffer(
        memory,
        "50",
        true,
        true,
        true,
        16);

    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches.at(0).offset, 32U);
    EXPECT_EQ(matches.at(0).encoding, "ascii");
    EXPECT_EQ(matches.at(1).offset, 64U);
    EXPECT_EQ(matches.at(1).encoding, "utf16");
}

TEST(UiStringTracker, FindsNumericSourceNearTrackedString) {
    QByteArray memory(512, '\0');
    writeInt32(&memory, 180, 50);
    memory.replace(224, 2, QByteArray("50"));
    writeInt32(&memory, 300, 50 * 4096);

    const auto stringMatches = killcore::findUiStringMatchesInBuffer(memory, "50", true, false, true, 8);
    ASSERT_EQ(stringMatches.size(), 1);

    const auto sources = killcore::findUiStringSourcesInBuffer(
        memory,
        stringMatches.first().offset,
        stringMatches.first().byteLength,
        "50",
        20,
        1,
        128);

    ASSERT_FALSE(sources.isEmpty());
    const auto int32Source = std::find_if(sources.begin(), sources.end(), [](const auto& hit) {
        return hit.offset == 180U
            && hit.type == killcore::ValueType::Int32
            && hit.variantLabel == "Int32";
    });
    ASSERT_NE(int32Source, sources.end());
    EXPECT_DOUBLE_EQ(int32Source->valueNumber, 50.0);

    const auto scaledSource = std::find_if(sources.begin(), sources.end(), [](const auto& hit) {
        return hit.offset == 300U
            && hit.type == killcore::ValueType::Int32
            && hit.variantLabel == "Int32 x4096";
    });
    ASSERT_NE(scaledSource, sources.end());
}

TEST(UiStringTracker, RebuildsExpectedBytesForTrackedVariant) {
    QString error;
    const QByteArray expected = killcore::targetBytesForTypeAndVariant(
        "85",
        killcore::ValueType::Int32,
        "Int32 x4096",
        &error);

    ASSERT_TRUE(error.isEmpty()) << error.toStdString();
    ASSERT_EQ(expected.size(), static_cast<qsizetype>(sizeof(int32_t)));

    int32_t stored = 0;
    std::memcpy(&stored, expected.constData(), sizeof(stored));
    EXPECT_EQ(stored, 85 * 4096);
}
