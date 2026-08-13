#include "instruction_patch_suggester.h"

#include "patch/aob_scanner.h"

#include <algorithm>
#include <cstdint>

#include <QPair>
#include <QStringList>

#if defined(KILLENGINE_HAS_ZYDIS)
#include <Zydis/Zydis.h>
#endif

namespace killcore {

namespace {

uint8_t byteAt(const QByteArray& bytes, int index) {
    return static_cast<uint8_t>(bytes.at(index));
}

bool hasBytes(const QByteArray& bytes, int index, int needed) {
    return index >= 0 && needed >= 0 && bytes.size() - index >= needed;
}

bool isLegacyPrefix(uint8_t b) {
    switch (b) {
        case 0xF0:
        case 0xF2:
        case 0xF3:
        case 0x2E:
        case 0x36:
        case 0x3E:
        case 0x26:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
            return true;
        default:
            return false;
    }
}

bool opcodeHasModRm(uint8_t opcode) {
    if ((opcode >= 0x00 && opcode <= 0x03) || (opcode >= 0x08 && opcode <= 0x0B) ||
        (opcode >= 0x10 && opcode <= 0x13) || (opcode >= 0x18 && opcode <= 0x1B) ||
        (opcode >= 0x20 && opcode <= 0x23) || (opcode >= 0x28 && opcode <= 0x2B) ||
        (opcode >= 0x30 && opcode <= 0x33) || (opcode >= 0x38 && opcode <= 0x3B)) {
        return true;
    }

    switch (opcode) {
        case 0x63:
        case 0x69:
        case 0x6B:
        case 0x80:
        case 0x81:
        case 0x82:
        case 0x83:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x87:
        case 0x88:
        case 0x89:
        case 0x8A:
        case 0x8B:
        case 0x8C:
        case 0x8D:
        case 0x8E:
        case 0x8F:
        case 0xC0:
        case 0xC1:
        case 0xC6:
        case 0xC7:
        case 0xD0:
        case 0xD1:
        case 0xD2:
        case 0xD3:
        case 0xF6:
        case 0xF7:
        case 0xFE:
        case 0xFF:
            return true;
        default:
            return false;
    }
}

bool twoByteOpcodeHasModRm(uint8_t opcode) {
    if (opcode >= 0x80 && opcode <= 0x8F) return false; // Jcc rel32
    if (opcode >= 0x90 && opcode <= 0x9F) return true;  // SETcc r/m8
    if (opcode >= 0xB0 && opcode <= 0xBF) return true;
    switch (opcode) {
        case 0xAF:
        case 0xBE:
        case 0xBF:
        case 0xB6:
        case 0xB7:
            return true;
        default:
            return false;
    }
}

int modRmTailSize(const QByteArray& bytes, int modRmIndex) {
    if (!hasBytes(bytes, modRmIndex, 1)) return -1;

    const uint8_t modrm = byteAt(bytes, modRmIndex);
    const uint8_t mod = (modrm >> 6) & 0x03;
    const uint8_t rm = modrm & 0x07;
    int size = 1;

    bool hasSib = rm == 4 && mod != 3;
    uint8_t sibBase = 0;
    if (hasSib) {
        if (!hasBytes(bytes, modRmIndex + size, 1)) return -1;
        const uint8_t sib = byteAt(bytes, modRmIndex + size);
        sibBase = sib & 0x07;
        ++size;
    }

    if (mod == 0 && rm == 5) {
        size += 4;
    } else if (mod == 0 && hasSib && sibBase == 5) {
        size += 4;
    } else if (mod == 1) {
        size += 1;
    } else if (mod == 2) {
        size += 4;
    }

    return hasBytes(bytes, modRmIndex, size) ? size : -1;
}

QString hintForOpcode(uint8_t opcode, bool twoByte) {
    if (twoByte) {
        if (opcode >= 0x80 && opcode <= 0x8F) return "jcc";
        if (opcode >= 0x90 && opcode <= 0x9F) return "setcc";
        return "0F";
    }
    if (opcode == 0x89 || opcode == 0x88 || opcode == 0xC6 || opcode == 0xC7) return "write-like";
    if (opcode == 0x8B || opcode == 0x8A || opcode == 0x8D) return "read/lea";
    if (opcode == 0xE8) return "call";
    if (opcode == 0xE9 || opcode == 0xEB || (opcode >= 0x70 && opcode <= 0x7F)) return "jump";
    if (opcode == 0xC3 || opcode == 0xC2) return "ret";
    if (opcode == 0x90) return "nop";
    if (opcode == 0xCC) return "int3";
    return "x64";
}

QString repeatedNopBytes(int length) {
    QByteArray bytes;
    bytes.resize(std::max(0, length));
    std::fill(bytes.begin(), bytes.end(), static_cast<char>(0x90));
    return bytesToAobPattern(bytes);
}

QString bytesToWildcardPattern(const QByteArray& bytes, const QList<QPair<int, int>>& wildcardRanges) {
    QStringList tokens;
    tokens.reserve(bytes.size());
    for (int i = 0; i < bytes.size(); ++i) {
        bool wildcard = false;
        for (const auto& range : wildcardRanges) {
            if (i >= range.first && i < range.first + range.second) {
                wildcard = true;
                break;
            }
        }
        if (wildcard) {
            tokens.append("??");
        } else {
            tokens.append(QString("%1").arg(static_cast<uint8_t>(bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper());
        }
    }
    return tokens.join(' ');
}

} // namespace

InstructionInfo decodeX64InstructionLength(const QByteArray& bytes) {
    InstructionInfo info;
    if (bytes.isEmpty()) {
        info.error = "Aucun byte à décoder.";
        return info;
    }

#if defined(KILLENGINE_HAS_ZYDIS)
    ZydisDisassembledInstruction instruction;
    if (ZYAN_SUCCESS(ZydisDisassembleIntel(
            ZYDIS_MACHINE_MODE_LONG_64,
            0,
            reinterpret_cast<const ZyanU8*>(bytes.constData()),
            static_cast<ZyanUSize>(bytes.size()),
            &instruction))) {
        info.success = true;
        info.length = static_cast<int>(instruction.info.length);
        info.mnemonicHint = QString::fromLatin1(ZydisMnemonicGetString(instruction.info.mnemonic));
        info.disassembly = QString::fromLatin1(instruction.text);
        info.decoder = "zydis";
        QList<QPair<int, int>> wildcardRanges;
        if (instruction.info.raw.disp.size > 0) {
            wildcardRanges.append({
                static_cast<int>(instruction.info.raw.disp.offset),
                static_cast<int>(instruction.info.raw.disp.size / 8)
            });
        }
        for (const auto& imm : instruction.info.raw.imm) {
            if (imm.size > 0) {
                wildcardRanges.append({
                    static_cast<int>(imm.offset),
                    static_cast<int>(imm.size / 8)
                });
            }
        }
        info.stableAobPattern = bytesToWildcardPattern(bytes.left(info.length), wildcardRanges);
        return info;
    }
#endif

    int index = 0;
    while (hasBytes(bytes, index, 1) && isLegacyPrefix(byteAt(bytes, index))) {
        ++index;
    }
    while (hasBytes(bytes, index, 1) && byteAt(bytes, index) >= 0x40 && byteAt(bytes, index) <= 0x4F) {
        ++index;
    }
    if (!hasBytes(bytes, index, 1)) {
        info.error = "Instruction tronquée après préfixes.";
        return info;
    }

    const uint8_t opcode = byteAt(bytes, index++);
    bool twoByte = false;
    uint8_t op = opcode;
    if (opcode == 0x0F) {
        if (!hasBytes(bytes, index, 1)) {
            info.error = "Opcode 0F incomplet.";
            return info;
        }
        twoByte = true;
        op = byteAt(bytes, index++);
    }

    int extra = 0;
    const bool hasModRm = twoByte ? twoByteOpcodeHasModRm(op) : opcodeHasModRm(op);
    if (hasModRm) {
        const int modRmSize = modRmTailSize(bytes, index);
        if (modRmSize < 0) {
            info.error = "Instruction ModRM tronquée.";
            return info;
        }
        extra += modRmSize;
    }

    if (!twoByte) {
        if (op == 0xC2) extra += 2;
        else if (op == 0xE8 || op == 0xE9) extra += 4;
        else if (op == 0xEB || (op >= 0x70 && op <= 0x7F)) extra += 1;
        else if (op >= 0xB8 && op <= 0xBF) extra += 4;
        else if (op == 0x68) extra += 4;
        else if (op == 0x6A) extra += 1;
        else if (op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) extra += 1;
        else if (op == 0x81 || op == 0x69 || op == 0xC7) extra += 4;
        else if (op == 0x6B) extra += 1;
    } else if (op >= 0x80 && op <= 0x8F) {
        extra += 4;
    }

    const int length = index + extra;
    if (!hasBytes(bytes, 0, length)) {
        info.error = "Bytes insuffisants pour l'instruction.";
        return info;
    }

    info.success = true;
    info.length = length;
    info.mnemonicHint = hintForOpcode(op, twoByte);
    info.stableAobPattern = bytesToAobPattern(bytes.left(length));
    return info;
}

QList<PatchSuggestion> suggestInstructionPatches(const InstructionInfo& instruction) {
    QList<PatchSuggestion> suggestions;
    if (!instruction.success || instruction.length <= 0) return suggestions;

    PatchSuggestion nop;
    nop.label = QString("NOP x%1").arg(instruction.length);
    nop.bytesText = repeatedNopBytes(instruction.length);
    nop.description = "Neutralise l'instruction en gardant exactement la même longueur.";
    nop.risky = false;
    suggestions.append(nop);

    PatchSuggestion int3;
    int3.label = "INT3 debug";
    int3.bytesText = repeatedNopBytes(instruction.length);
    if (instruction.length > 0) {
        int3.bytesText.replace(0, 2, "CC");
    }
    int3.description = "Breakpoint logiciel pour valider que le code passe ici; expérimental.";
    int3.risky = true;
    suggestions.append(int3);

    if (instruction.length >= 1) {
        PatchSuggestion ret;
        ret.label = "RET + NOP";
        ret.bytesText = repeatedNopBytes(instruction.length);
        ret.bytesText.replace(0, 2, "C3");
        ret.description = "Force un retour immédiat; très risqué hors début de fonction.";
        ret.risky = true;
        suggestions.append(ret);
    }

    return suggestions;
}

} // namespace killcore
