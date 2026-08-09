#include <gtest/gtest.h>

#include "snapshot/snapshot_codec.h"

TEST(SnapshotCodecTest, CompressesAndDecompressesLz4Block) {
    QByteArray input;
    input.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        input.append(static_cast<char>(i % 17));
    }

    const auto compressed = killcore::SnapshotCodec::compressLz4(input);
    ASSERT_TRUE(compressed.success) << compressed.errorMessage.toStdString();
    EXPECT_TRUE(compressed.block.compressed);
    EXPECT_EQ(compressed.block.originalSize, input.size());
    EXPECT_GT(compressed.block.data.size(), 0);

    const auto decompressed = killcore::SnapshotCodec::decompressLz4(
        compressed.block.data.constData(),
        compressed.block.data.size(),
        compressed.block.originalSize);
    ASSERT_TRUE(decompressed.success) << decompressed.errorMessage.toStdString();
    EXPECT_EQ(decompressed.block.data, input);
}

TEST(SnapshotCodecTest, HandlesEmptyBlock) {
    const QByteArray input;

    const auto compressed = killcore::SnapshotCodec::compressLz4(input);
    ASSERT_TRUE(compressed.success) << compressed.errorMessage.toStdString();
    EXPECT_TRUE(compressed.block.data.isEmpty());
    EXPECT_EQ(compressed.block.originalSize, 0);

    const auto decompressed = killcore::SnapshotCodec::decompressLz4(
        compressed.block.data.constData(),
        compressed.block.data.size(),
        compressed.block.originalSize);
    ASSERT_TRUE(decompressed.success) << decompressed.errorMessage.toStdString();
    EXPECT_TRUE(decompressed.block.data.isEmpty());
}
