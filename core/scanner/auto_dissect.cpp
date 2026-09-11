#include "auto_dissect.h"

#include "localization/localization.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace killcore {

/// Lit une valeur typée depuis un buffer à un offset.
static QVariant readFieldValue(const QByteArray& buf, int offset, FieldType type) {
    if (offset < 0 || offset + 8 > buf.size()) return QVariant();
    const char* p = buf.constData() + offset;
    switch (type) {
        case FieldType::Int8:   return QVariant::fromValue(static_cast<int>(static_cast<int8_t>(*p)));
        case FieldType::UInt8:  return QVariant::fromValue(static_cast<int>(static_cast<uint8_t>(*p)));
        case FieldType::Int16:  return QVariant::fromValue(static_cast<int>(*reinterpret_cast<const int16_t*>(p)));
        case FieldType::UInt16: return QVariant::fromValue(static_cast<int>(*reinterpret_cast<const uint16_t*>(p)));
        case FieldType::Int32:  return QVariant::fromValue(*reinterpret_cast<const int32_t*>(p));
        case FieldType::UInt32: return QVariant::fromValue(*reinterpret_cast<const uint32_t*>(p));
        case FieldType::Int64:  return QVariant::fromValue(*reinterpret_cast<const int64_t*>(p));
        case FieldType::UInt64: return QVariant::fromValue(*reinterpret_cast<const uint64_t*>(p));
        case FieldType::Float32: return QVariant::fromValue(*reinterpret_cast<const float*>(p));
        case FieldType::Float64: return QVariant::fromValue(*reinterpret_cast<const double*>(p));
        case FieldType::Pointer64: return QVariant::fromValue(*reinterpret_cast<const uint64_t*>(p));
        default: return QVariant();
    }
}

// Voir la déclaration dans auto_dissect.h pour le contrat complet (regions
// triées, non chevauchantes) — définition non-static ici pour rester
// testable en isolation (tests/unit/test_auto_dissect.cpp).
bool isPointerValid(const QList<MemoryRegion>& sortedRegions, uint64_t addr) {
    if (addr == 0) return false;
    auto it = std::upper_bound(
        sortedRegions.begin(), sortedRegions.end(), addr,
        [](uint64_t value, const MemoryRegion& region) { return value < region.baseAddress; });
    if (it == sortedRegions.begin()) return false;
    --it;
    if (addr >= it->baseAddress && addr < it->baseAddress + it->size) {
        return it->state == MemoryState::Committed && it->readable;
    }
    return false;
}

/// Calcule un score de confiance pour une instance candidate.
static double computeConfidence(const StructureTemplate& tmpl, const QByteArray& buf,
                                 const QList<MemoryRegion>& regions, bool requirePointerValidity) {
    if (tmpl.fields.isEmpty()) return 0.0;
    int score = 0;
    int total = 0;
    for (const auto& f : tmpl.fields) {
        ++total;
        const auto val = readFieldValue(buf, f.offset, f.type);
        if (!val.isValid()) continue;

        // Les pointeurs doivent pointer vers des pages valides
        if (f.type == FieldType::Pointer64) {
            const uint64_t ptrAddr = val.toULongLong();
            if (ptrAddr == 0) { ++score; continue; } // null pointer = acceptable
            if (!requirePointerValidity || isPointerValid(regions, ptrAddr)) {
                ++score;
            }
            continue;
        }

        // Les strings doivent contenir des caractères imprimables
        if (f.type == FieldType::AsciiString || f.type == FieldType::Utf16String) {
            ++score; // on suppose que la détection de string est fiable
            continue;
        }

        // Les valeurs numériques non-zéro sont plus probables
        const double d = val.toDouble();
        if (d != 0.0) ++score;
        else ++score; // zéro = acceptable aussi
    }
    return total > 0 ? static_cast<double>(score) / total : 0.0;
}

AutoDissectResult findStructureInstances(
    const ProcessHandle& process,
    const StructureTemplate& tmpl,
    const AutoDissectOptions& options) {

    AutoDissectResult result;

    if (tmpl.fields.isEmpty()) {
        result.error = KE_TXT("Template vide — aucun champ à matcher.", "Empty template — no field to match.");
        return result;
    }
    if (!process.isValid()) {
        result.error = KE_TXT("Processus invalide.", "Invalid process.");
        return result;
    }

    // Calculer la taille minimale de la structure
    int structSize = 0;
    for (const auto& f : tmpl.fields) {
        int fieldSize = 0;
        switch (f.type) {
            case FieldType::Int8: case FieldType::UInt8: fieldSize = 1; break;
            case FieldType::Int16: case FieldType::UInt16: fieldSize = 2; break;
            case FieldType::Int32: case FieldType::UInt32: case FieldType::Float32: fieldSize = 4; break;
            case FieldType::Int64: case FieldType::UInt64: case FieldType::Float64:
            case FieldType::Pointer64: fieldSize = 8; break;
            case FieldType::AsciiString: case FieldType::Utf16String: fieldSize = 16; break; // min
            default: fieldSize = 4; break;
        }
        structSize = std::max(structSize, f.offset + fieldSize);
    }
    if (structSize < 4) structSize = 4;

    // Récupérer la carte mémoire. MemoryMap::snapshot() les retourne déjà en
    // ordre d'adresses croissantes (VirtualQueryEx avancé séquentiellement),
    // mais on trie explicitement ici pour ne pas faire dépendre la
    // correction de isPointerValid() d'un invariant implicite d'un autre
    // fichier — coût négligeable (une fois, pas par candidat testé).
    auto regions = MemoryMap::snapshot(process);
    std::sort(regions.begin(), regions.end(), [](const MemoryRegion& a, const MemoryRegion& b) {
        return a.baseAddress < b.baseAddress;
    });
    MemoryReader reader(process);

    int instancesFound = 0;
    int regionsScanned = 0;
    int totalCandidates = 0;

    // Scanner chaque région writable
    for (const auto& region : regions) {
        if (instancesFound >= options.maxResults) break;
        if (!region.writable || region.state != MemoryState::Committed) continue;
        if (region.size < static_cast<uint64_t>(structSize)) continue;

        ++regionsScanned;

        // Lire la région par chunks
        const size_t chunkSize = std::min(static_cast<size_t>(region.size), static_cast<size_t>(1024 * 1024));
        for (size_t offset = 0; offset < region.size && instancesFound < options.maxResults; offset += chunkSize) {
            const size_t readSize = std::min(chunkSize + structSize - 1, static_cast<size_t>(region.size - offset));
            const auto readResult = reader.read(region.baseAddress + offset, readSize);
            if (!readResult.success || readResult.bytesRead < static_cast<size_t>(structSize)) continue;

            const QByteArray& data = readResult.data;

            // Tester chaque alignement possible dans le chunk
            for (int i = 0; i + structSize <= data.size() && instancesFound < options.maxResults; i += 4) {
                ++totalCandidates;

                // Vérifier tous les champs du template
                bool allMatch = true;
                QList<QVariant> values;
                values.reserve(tmpl.fields.size());

                for (const auto& f : tmpl.fields) {
                    if (i + f.offset + 8 > data.size()) {
                        allMatch = false;
                        break;
                    }
                    const auto val = readFieldValue(data, i + f.offset, f.type);
                    if (!val.isValid()) {
                        allMatch = false;
                        break;
                    }
                    values.append(val);
                }

                if (!allMatch) continue;

                // Calculer la confiance
                const double confidence = computeConfidence(tmpl, data.mid(i, structSize), regions, options.requirePointerValidity);
                if (confidence < options.minConfidence) continue;

                DiscoveredInstance inst;
                inst.baseAddress = region.baseAddress + offset + i;
                inst.fieldValues = values;
                inst.confidence = confidence;
                result.instances.append(inst);
                ++instancesFound;
            }
        }
    }

    result.scannedRegions = regionsScanned;
    result.totalCandidates = totalCandidates;
    result.success = !result.instances.isEmpty();
    if (result.instances.isEmpty()) {
        result.error = KE_TXT("Aucune instance trouvée correspondant au template.", "No instance found matching the template.");
    }

    return result;
}

} // namespace killcore
