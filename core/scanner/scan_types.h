#pragma once

#include "scanner/performance_profile.h"

#include <QList>
#include <QByteArray>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace killcore {

enum class ValueType {
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
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
    Between, ///< Candidats dont la valeur courante est dans [min, max] (borné, valeur = "min,max").
};

struct ScanMatch {
    uint64_t address{0};
    ValueType type{ValueType::Int32};
    double confidence{1.0}; ///< Phase 13 : score de confiance [0.0, 1.0].
    QString  variantLabel;  ///< Phase 13 : libellé de variante (ex. "Float32 x100").
};

struct ScanProgress {
    size_t regionsTotal{0};
    size_t regionsScanned{0};
    size_t bytesTotal{0};
    size_t bytesScanned{0};
    size_t matchesFound{0};
};

struct ScanOptions {
    size_t chunkSize{0};              ///< 0 = choisi par PerformanceProfile.
    size_t maxResults{10000};
    PerformanceMode performanceMode{PerformanceMode::Auto};
    size_t maxWorkerThreads{0};       ///< 0 = choisi par PerformanceProfile.
    uint64_t maxInFlightBytes{0};     ///< 0 = choisi par PerformanceProfile.

    // Filtres Mode Expert (Phase 12)
    uint64_t startAddress{0};          ///< Adresse de début (0 = début de chaque région)
    uint64_t stopAddress{0};           ///< Adresse de fin (0 = fin de chaque région)
    size_t   alignment{1};             ///< Alignement des adresses candidates (1 = pas d'alignement)
    bool     writableOnly{false};      ///< Ne scanner que les régions writables
    bool     executableOnly{false};    ///< Ne scanner que les régions executables
    bool     copyOnWriteOnly{false};   ///< Ne scanner que les régions copy-on-write (mapped privé writable)
    bool     fastScan{true};           ///< Active l'alignement automatique par taille de type
    std::function<void(const ScanProgress&)> progressCallback;

    // SC2-UNKNOWN-1 — Unknown scan en mode Delta (SnapshotStore::compare) :
    // delta brut attendu ((valeur courante - valeur snapshot) == targetDelta,
    // tolérance selon le type), et libellé de variante à apposer sur chaque
    // ScanMatch retourné (ex. "Int32 x4096" — voir scanner/value_variants.h
    // generateDeltaVariants, l'appelant boucle une fois par variante d'échelle).
    double  targetDelta{0.0};
    QString matchVariantLabel;
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
