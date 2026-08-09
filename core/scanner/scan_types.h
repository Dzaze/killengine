#pragma once

#include <QList>
#include <QByteArray>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>

namespace killcore {

enum class ValueType {
    Int32,
    Int64,
    Float32,
    Float64,
};

struct ScanValue {
    ValueType type{ValueType::Int32};
    QVariant  value;
};

enum class NextScanMode {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased,
    Delta,
};

struct ScanMatch {
    uint64_t address{0};
    ValueType type{ValueType::Int32};
};

struct ScanOptions {
    size_t chunkSize{1024 * 1024};
    size_t maxResults{10000};
};

struct ScanResult {
    bool    success{false};
    bool    partial{false};
    bool    cancelled{false};
    size_t  regionsScanned{0};
    size_t  bytesScanned{0};
    size_t  matchesFound{0};
    QString errorMessage;
    QList<ScanMatch> matches;
};

QString valueTypeToString(ValueType type);
bool parseValueType(const QString& text, ValueType* out);
bool parseScanValue(const QString& text, ValueType type, ScanValue* out, QString* error = nullptr);
bool parseNextScanMode(const QString& text, NextScanMode* out);
size_t valueTypeSize(ValueType type);
QByteArray scanValueToBytes(const ScanValue& value);

} // namespace killcore
