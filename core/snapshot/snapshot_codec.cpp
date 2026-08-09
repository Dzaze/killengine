#include "snapshot_codec.h"

#include <lz4.h>

#include <limits>

namespace killcore {

SnapshotCodecResult SnapshotCodec::compressLz4(const QByteArray& input) {
    SnapshotCodecResult result;
    result.block.originalSize = input.size();

    if (input.isEmpty()) {
        result.success = true;
        result.block.compressed = true;
        return result;
    }

    if (input.size() > std::numeric_limits<int>::max()) {
        result.errorMessage = "Snapshot block is too large for LZ4.";
        return result;
    }

    const int inputSize = static_cast<int>(input.size());
    const int maxCompressedSize = LZ4_compressBound(inputSize);
    if (maxCompressedSize <= 0) {
        result.errorMessage = "LZ4 compression bound failed.";
        return result;
    }

    QByteArray compressed;
    compressed.resize(maxCompressedSize);

    const int compressedSize = LZ4_compress_default(
        input.constData(),
        compressed.data(),
        inputSize,
        maxCompressedSize);

    if (compressedSize <= 0) {
        result.errorMessage = "LZ4 compression failed.";
        return result;
    }

    compressed.resize(compressedSize);
    result.block.data = std::move(compressed);
    result.block.compressed = true;
    result.success = true;
    return result;
}

SnapshotCodecResult SnapshotCodec::decompressLz4(const char* data, qsizetype size, qsizetype originalSize) {
    SnapshotCodecResult result;
    result.block.originalSize = originalSize;
    result.block.compressed = false;

    if (originalSize < 0 || size < 0) {
        result.errorMessage = "Invalid snapshot block size.";
        return result;
    }

    if (originalSize > std::numeric_limits<int>::max() || size > std::numeric_limits<int>::max()) {
        result.errorMessage = "Snapshot block is too large for LZ4.";
        return result;
    }

    QByteArray output;
    output.resize(originalSize);

    if (originalSize == 0) {
        result.block.data = std::move(output);
        result.success = true;
        return result;
    }

    const int decompressedSize = LZ4_decompress_safe(
        data,
        output.data(),
        static_cast<int>(size),
        static_cast<int>(originalSize));

    if (decompressedSize != originalSize) {
        result.errorMessage = "LZ4 decompression failed.";
        return result;
    }

    result.block.data = std::move(output);
    result.success = true;
    return result;
}

} // namespace killcore
