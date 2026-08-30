#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace killcore {

/// Type de champ interprété dans une structure mémoire.
enum class FieldType {
    Unknown,
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
    Pointer64,
    AsciiString,
    Utf16String,
};

/// Un champ interprété dans une structure.
struct StructureField {
    int offset{0};              ///< Offset relatif à la base
    FieldType type{FieldType::Unknown};
    QString label;              ///< Nom optionnel (ex: "HP", "Mana")
    QVariant interpretedValue;  ///< Valeur décodée (int, float, string, ptr)
    QString rawHex;             ///< Représentation hex lisible
    bool changed{false};        ///< True si le champ a changé depuis la dernière capture
};

/// Résultat de l'analyse d'une structure.
struct StructureAnalysisResult {
    bool success{false};
    QString error;
    uint64_t baseAddress{0};
    int size{0};
    QList<StructureField> fields;
    QByteArray rawData;
    QByteArray previousData;    ///< Pour diff
};

/// Options d'analyse de structure.
struct StructureAnalysisOptions {
    int windowSize{256};        ///< Taille de la fenêtre à analyser (octets)
    bool detectAscii{true};     ///< Détecter les strings ASCII
    bool detectUtf16{true};     ///< Détecter les strings UTF-16
    bool detectPointers{true};  ///< Détecter les pointeurs 64-bit
};

/// Décode une fenêtre mémoire en champs typés.
StructureAnalysisResult analyzeStructure(
    const QByteArray& data,
    uint64_t baseAddress,
    const StructureAnalysisOptions& options = {});

/// Compare deux captures et marque les champs modifiés.
StructureAnalysisResult diffStructure(
    const StructureAnalysisResult& previous,
    const QByteArray& currentData);

/// Déduit un template de structure depuis deux instances (ex: joueur 1 et joueur 2).
/// Calcule le delta d'offset et retourne les offsets relatifs.
struct StructureTemplate {
    QString name;
    int64_t instanceDelta{0};   ///< Delta entre deux instances
    QList<StructureField> fields;
};

StructureTemplate deduceTemplate(
    const StructureAnalysisResult& instance1,
    const StructureAnalysisResult& instance2,
    const QString& name = QString());

/// Instance probable de la meme structure, extrapolee depuis l'espacement A/B.
struct StructureInstanceCandidate {
    int relativeIndex{0};       ///< 0 = instance A, 1 = instance B
    uint64_t baseAddress{0};
    uint64_t fieldAddress{0};
    bool inputInstance{false};
};

/// Options de deduction d'espacement entre deux instances.
struct StructureInstanceDeltaOptions {
    int beforeCount{2};         ///< Nombre d'instances probables avant A
    int afterCount{4};          ///< Nombre d'instances probables apres A
};

/// Resultat de deduction d'espacement entre deux instances de meme layout.
struct StructureInstanceDeltaResult {
    bool success{false};
    QString error;
    QString warning;
    bool compatibleLayout{false};
    uint64_t baseAddressA{0};
    uint64_t baseAddressB{0};
    uint64_t fieldAddressA{0};
    uint64_t fieldAddressB{0};
    int64_t fieldOffsetA{0};
    int64_t fieldOffsetB{0};
    int64_t fieldOffsetDelta{0};
    int64_t instanceDelta{0};
    int64_t fieldAddressDelta{0};
    QList<StructureInstanceCandidate> candidates;
};

/// Deduit le stride entre deux instances depuis deux bases et un champ homologue.
StructureInstanceDeltaResult inferStructureInstanceDelta(
    uint64_t baseAddressA,
    uint64_t fieldAddressA,
    uint64_t baseAddressB,
    uint64_t fieldAddressB,
    const StructureInstanceDeltaOptions& options = {});

/// Convertit un FieldType en chaîne lisible.
QString fieldTypeToString(FieldType type);

/// Convertit un champ en valeur lisible.
QString fieldToReadable(const StructureField& field);

} // namespace killcore
