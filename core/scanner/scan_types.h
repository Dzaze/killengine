#pragma once

#include <QList>
#include <QByteArray>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>
#include <functional>

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

struct ScanProgress {
    size_t regionsTotal{0};
    size_t regionsScanned{0};
    size_t bytesTotal{0};
    size_t bytesScanned{0};
    size_t matchesFound{0};
};

struct ScanOptions {
    size_t chunkSize{1024 * 1024};
    size_t maxResults{10000};

    // Filtres Mode Expert (Phase 12)
    uint64_t startAddress{0};          ///< Adresse de début (0 = début de chaque région)
    uint64_t stopAddress{0};           ///< Adresse de fin (0 = fin de chaque région)
    size_t   alignment{1};             ///< Alignement des adresses candidates (1 = pas d'alignement)
    bool     writableOnly{false};      ///< Ne scanner que les régions writables
    bool     executableOnly{false};    ///< Ne scanner que les régions executables
    bool     copyOnWriteOnly{false};   ///< Ne scanner que les régions copy-on-write (mapped privé writable)
    bool     fastScan{true};           ///< Active l'alignement automatique par taille de type
    std::function<void(const ScanProgress&)> progressCallback;
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
