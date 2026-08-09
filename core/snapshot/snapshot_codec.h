#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>

namespace killcore {

struct SnapshotBlock {
    QByteArray data;
    qsizetype originalSize{0};
    bool compressed{false};
};

struct SnapshotCodecResult {
    bool success{false};
    QString errorMessage;
    SnapshotBlock block;
};

class SnapshotCodec {
public:
    static SnapshotCodecResult compressLz4(const QByteArray& input);
    static SnapshotCodecResult decompressLz4(const char* data, qsizetype size, qsizetype originalSize);
};

} // namespace killcore
