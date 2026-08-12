#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstddef>

namespace killcore {

struct UiStringMatch {
    size_t offset{0};
    QString encoding;
    int byteLength{0};
};

struct UiStringSourceHit {
    size_t offset{0};
    ValueType type{ValueType::Int32};
    QString variantLabel;
    QByteArray bytes;
    double valueNumber{0.0};
    size_t distanceBytes{0};
    double confidence{0.0};
};

QByteArray encodeUiStringValue(const QString& value, const QString& encoding);

QList<UiStringMatch> findUiStringMatchesInBuffer(
    const QByteArray& buffer,
    const QString& value,
    bool scanAscii,
    bool scanUtf16,
    bool numericBoundary,
    int maxResults);

QList<UiStringSourceHit> findUiStringSourcesInBuffer(
    const QByteArray& buffer,
    size_t stringOffset,
    int stringByteLength,
    const QString& value,
    int maxResults,
    int alignment,
    int radiusBytes);

QByteArray targetBytesForTypeAndVariant(
    const QString& value,
    ValueType type,
    const QString& variantLabel,
    QString* error = nullptr);

} // namespace killcore
