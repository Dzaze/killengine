#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>

#include <cstdint>
#include <functional>

namespace killcore {

/// Mode de transformation pour le scan de valeurs chiffrées.
enum class EncryptedScanMode {
    XorKey,   ///< value ^ key
    AddKey,   ///< value + key
    SubKey,   ///< value - key
    NotBits,  ///< ~value (bitwise NOT)
    GroupScan,///< Recherche de N valeurs avec offsets fixes connus
};

/// Options du scan chiffré.
struct EncryptedScanOptions {
    EncryptedScanMode mode{EncryptedScanMode::XorKey};
    uint64_t key{0};            ///< Clé pour XOR/Add/Sub
    int keySearchBits{0};       ///< 0 = clé fournie ; 8/16/32 = brute-force sur ces bits
    ValueType valueType{ValueType::Int32};
};

/// Un élément d'un group scan (valeur + offset relatif).
struct GroupScanEntry {
    int64_t offset{0};          ///< Offset relatif à l'adresse de base
    ValueType type{ValueType::Int32};
    QVariant value;             ///< Valeur attendue
};

/// Options du group scan.
struct GroupScanOptions {
    QList<GroupScanEntry> entries;
    int maxDistance{256};       ///< Distance max entre premier et dernier offset
    size_t maxResults{1000};
};

/// Résultat d'un scan chiffré.
struct EncryptedScanResult {
    bool success{false};
    bool partial{false};
    QString error;
    uint64_t keyFound{0};       ///< Clé trouvée si brute-force
    QList<ScanMatch> matches;
};

/// Applique une transformation à une valeur brute.
uint64_t applyEncryption(uint64_t rawValue, EncryptedScanMode mode, uint64_t key);

/// Inverse une transformation pour retrouver la valeur brute depuis la valeur affichée.
uint64_t reverseEncryption(uint64_t displayValue, EncryptedScanMode mode, uint64_t key);

/// Scanne un buffer à la recherche d'une valeur chiffrée.
/// Pour chaque offset potentiel, teste toutes les clés (si keySearchBits > 0).
EncryptedScanResult scanEncryptedInBuffer(
    const QByteArray& buffer,
    uint64_t baseAddress,
    uint64_t displayValue,
    const EncryptedScanOptions& options);

/// Scanne un buffer à la recherche d'un groupe de valeurs proches.
EncryptedScanResult scanGroupInBuffer(
    const QByteArray& buffer,
    uint64_t baseAddress,
    const GroupScanOptions& options);

} // namespace killcore