#include "structure_analyzer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace killcore {

namespace {

bool isPrintableAscii(char c) {
    return c >= 0x20 && c <= 0x7E;
}

bool looksLikeAsciiString(const QByteArray& data, int offset, int minLen = 4) {
    int len = 0;
    while (offset + len < data.size() && isPrintableAscii(data[offset + len])) {
        len++;
    }
    if (offset + len < data.size() && data[offset + len] == 0) {
        return len >= minLen;
    }
    return false;
}

bool looksLikeUtf16String(const QByteArray& data, int offset, int minLen = 4) {
    int len = 0;
    while (offset + len * 2 + 1 < data.size()) {
        char lo = data[offset + len * 2];
        char hi = data[offset + len * 2 + 1];
        if (hi != 0) return false;
        if (lo == 0) break;
        if (!isPrintableAscii(lo)) return false;
        len++;
    }
    return len >= minLen;
}

bool looksLikePointer(const QByteArray& data, int offset) {
    if (offset + 8 > data.size()) return false;
    uint64_t val = 0;
    std::memcpy(&val, data.constData() + offset, 8);
    return val >= 0x10000 && val <= 0x7FFFFFFFFFFFULL;
}

QString bytesToHex(const QByteArray& data, int offset, int size) {
    QString hex;
    hex.reserve(size * 3);
    for (int i = 0; i < size; ++i) {
        if (offset + i >= data.size()) break;
        hex += QStringLiteral("%1 ").arg(static_cast<unsigned char>(data[offset + i]), 2, 16, QChar('0'));
    }
    return hex.trimmed().toUpper();
}

StructureField makeField(int offset, FieldType type, const QVariant& value, const QByteArray& data, int size) {
    StructureField field;
    field.offset = offset;
    field.type = type;
    field.interpretedValue = value;
    field.rawHex = bytesToHex(data, offset, size);
    return field;
}

int fieldTypeSize(FieldType type, const QString& rawHex) {
    switch (type) {
        case FieldType::Int8:
        case FieldType::UInt8:
            return 1;
        case FieldType::Int16:
        case FieldType::UInt16:
            return 2;
        case FieldType::Int32:
        case FieldType::UInt32:
        case FieldType::Float32:
            return 4;
        case FieldType::Int64:
        case FieldType::UInt64:
        case FieldType::Float64:
        case FieldType::Pointer64:
            return 8;
        case FieldType::AsciiString:
        case FieldType::Utf16String:
            return rawHex.split(' ', Qt::SkipEmptyParts).size();
        default:
            return 1;
    }
}

void addTypedField(QList<StructureField>& fields, const QByteArray& data, int offset, const StructureAnalysisOptions& options) {
    if (offset + 1 <= data.size()) {
        const auto byte = static_cast<unsigned char>(data[offset]);
        if (byte < 128) {
            fields.append(makeField(offset, FieldType::Int8, static_cast<int>(static_cast<int8_t>(byte)), data, 1));
        }
        fields.append(makeField(offset, FieldType::UInt8, static_cast<int>(byte), data, 1));
    }

    if (offset % 2 == 0 && offset + 2 <= data.size()) {
        int16_t i16;
        uint16_t u16;
        std::memcpy(&i16, data.constData() + offset, 2);
        std::memcpy(&u16, data.constData() + offset, 2);
        if (i16 >= 0 && i16 < 100000) {
            fields.append(makeField(offset, FieldType::Int16, static_cast<int>(i16), data, 2));
        } else if (u16 > 0 && u16 < 0xFFFF) {
            fields.append(makeField(offset, FieldType::UInt16, static_cast<int>(u16), data, 2));
        }
    }

    if (offset % 4 == 0 && offset + 4 <= data.size()) {
        int32_t i32;
        uint32_t u32;
        float f32;
        std::memcpy(&i32, data.constData() + offset, 4);
        std::memcpy(&u32, data.constData() + offset, 4);
        std::memcpy(&f32, data.constData() + offset, 4);

        if (std::isfinite(f32) && std::fabs(f32) > 0.001f && std::fabs(f32) < 1e10f) {
            fields.append(makeField(offset, FieldType::Float32, f32, data, 4));
        }
        if (i32 >= 0 && i32 < 10000000) {
            fields.append(makeField(offset, FieldType::Int32, i32, data, 4));
        } else if (u32 > 0 && u32 < 0xFFFFFFFFU && i32 < 0) {
            fields.append(makeField(offset, FieldType::UInt32, u32, data, 4));
        }
    }

    if (offset % 8 == 0 && offset + 8 <= data.size()) {
        int64_t i64;
        uint64_t u64;
        double f64;
        std::memcpy(&i64, data.constData() + offset, 8);
        std::memcpy(&u64, data.constData() + offset, 8);
        std::memcpy(&f64, data.constData() + offset, 8);
        if (i64 >= 0 && i64 < 1000000000) {
            fields.append(makeField(offset, FieldType::Int64, static_cast<qlonglong>(i64), data, 8));
        } else if (u64 > 0 && u64 < 0xFFFFFFFFFFFFFFFFULL && i64 < 0) {
            fields.append(makeField(offset, FieldType::UInt64, static_cast<qulonglong>(u64), data, 8));
        }

        if (std::isfinite(f64) && std::fabs(f64) > 0.001 && std::fabs(f64) < 1e100) {
            fields.append(makeField(offset, FieldType::Float64, f64, data, 8));
        }

        if (options.detectPointers && looksLikePointer(data, offset)) {
            uint64_t ptr;
            std::memcpy(&ptr, data.constData() + offset, 8);
            fields.append(makeField(offset, FieldType::Pointer64, static_cast<qulonglong>(ptr), data, 8));
        }
    }

    if (options.detectAscii && looksLikeAsciiString(data, offset)) {
        int len = 0;
        while (offset + len < data.size() && isPrintableAscii(data[offset + len])) len++;
        QString str = QString::fromLatin1(data.constData() + offset, len);
        fields.append(makeField(offset, FieldType::AsciiString, str, data, len + 1));
    }

    if (options.detectUtf16 && offset + 8 <= data.size() && looksLikeUtf16String(data, offset)) {
        int len = 0;
        while (offset + len * 2 + 1 < data.size()) {
            char lo = data[offset + len * 2];
            if (lo == 0) break;
            len++;
        }
        QString str = QString::fromUtf16(
            reinterpret_cast<const char16_t*>(data.constData() + offset), len);
        fields.append(makeField(offset, FieldType::Utf16String, str, data, len * 2 + 2));
    }
}

} // namespace

StructureAnalysisResult analyzeStructure(
    const QByteArray& data,
    uint64_t baseAddress,
    const StructureAnalysisOptions& options) {

    StructureAnalysisResult result;
    if (data.isEmpty()) {
        result.error = "Empty data";
        return result;
    }

    result.success = true;
    result.baseAddress = baseAddress;
    result.size = data.size();
    result.rawData = data;

    for (int offset = 0; offset < data.size(); ++offset) {
        addTypedField(result.fields, data, offset, options);
    }

    return result;
}

StructureAnalysisResult diffStructure(
    const StructureAnalysisResult& previous,
    const QByteArray& currentData) {

    StructureAnalysisResult result;
    if (!previous.success || previous.rawData.isEmpty()) {
        result.error = "No previous data to diff";
        return result;
    }

    result.success = true;
    result.baseAddress = previous.baseAddress;
    result.size = currentData.size();
    result.rawData = currentData;
    result.previousData = previous.rawData;

    StructureAnalysisOptions options;
    for (int offset = 0; offset < currentData.size(); ++offset) {
        addTypedField(result.fields, currentData, offset, options);
    }

    const int compareLen = std::min(previous.rawData.size(), currentData.size());
    for (auto& field : result.fields) {
        if (field.offset < compareLen) {
            const int fieldSize = fieldTypeSize(field.type, field.rawHex);
            const int checkLen = std::min(fieldSize, compareLen - field.offset);
            if (checkLen > 0) {
                field.changed = (std::memcmp(
                    previous.rawData.constData() + field.offset,
                    currentData.constData() + field.offset,
                    checkLen) != 0);
            }
        }
    }

    return result;
}

StructureTemplate deduceTemplate(
    const StructureAnalysisResult& instance1,
    const StructureAnalysisResult& instance2,
    const QString& name) {

    StructureTemplate tmpl;
    tmpl.name = name;
    tmpl.instanceDelta = static_cast<int64_t>(instance2.baseAddress) - static_cast<int64_t>(instance1.baseAddress);
    tmpl.fields = instance1.fields;

    return tmpl;
}

QString fieldTypeToString(FieldType type) {
    switch (type) {
        case FieldType::Int8:        return "Int8";
        case FieldType::UInt8:       return "UInt8";
        case FieldType::Int16:       return "Int16";
        case FieldType::UInt16:      return "UInt16";
        case FieldType::Int32:       return "Int32";
        case FieldType::UInt32:      return "UInt32";
        case FieldType::Int64:       return "Int64";
        case FieldType::UInt64:      return "UInt64";
        case FieldType::Float32:     return "Float32";
        case FieldType::Float64:     return "Float64";
        case FieldType::Pointer64:   return "Ptr64";
        case FieldType::AsciiString: return "ASCII";
        case FieldType::Utf16String: return "UTF16";
        default:                     return "Unknown";
    }
}

QString fieldToReadable(const StructureField& field) {
    const QString typeStr = fieldTypeToString(field.type);
    QString valueStr;

    switch (field.type) {
        case FieldType::Pointer64:
            valueStr = QStringLiteral("0x%1").arg(field.interpretedValue.toULongLong(), 0, 16);
            break;
        case FieldType::AsciiString:
        case FieldType::Utf16String:
            valueStr = QStringLiteral("\"%1\"").arg(field.interpretedValue.toString());
            break;
        case FieldType::Float32:
            valueStr = QString::number(field.interpretedValue.toFloat(), 'f', 4);
            break;
        case FieldType::Float64:
            valueStr = QString::number(field.interpretedValue.toDouble(), 'f', 6);
            break;
        default:
            valueStr = field.interpretedValue.toString();
            break;
    }

    return QStringLiteral("[%1] %2 = %3").arg(field.offset, 4, 16, QChar('0')).arg(typeStr, valueStr);
}

} // namespace killcore
