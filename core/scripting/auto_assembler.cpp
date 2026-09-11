#include "auto_assembler.h"

#include "localization/localization.h"
#include "logging/logger.h"
#include "memory/memory_writer.h"
#include "patch/instruction_patch_suggester.h"
#include "process/process_enumerator.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QRegularExpression>
#include <QStringList>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>

namespace killcore {

namespace {

ScriptToken makeToken(ScriptTokenType type, const QString& value, int line) {
    return ScriptToken{type, value, line};
}

QStringList tokenize(const QString& text) {
    // Pour la v1, on traite ligne par ligne (simplification)
    return text.split('\n', Qt::SkipEmptyParts);
}

AutoAsmInstructionType parseInstructionType(const QString& mnemonic) {
    const QString lower = mnemonic.toLower();
    if (lower == "alloc")   return AutoAsmInstructionType::Alloc;
    if (lower == "dealloc") return AutoAsmInstructionType::Dealloc;
    if (lower == "label")   return AutoAsmInstructionType::Label;
    if (lower == "mov")     return AutoAsmInstructionType::Mov;
    if (lower == "add")     return AutoAsmInstructionType::Add;
    if (lower == "sub")     return AutoAsmInstructionType::Sub;
    if (lower == "inc")     return AutoAsmInstructionType::Inc;
    if (lower == "dec")     return AutoAsmInstructionType::Dec;
    if (lower == "cmp")     return AutoAsmInstructionType::Cmp;
    if (lower == "jmp")     return AutoAsmInstructionType::Jmp;
    if (lower == "je")      return AutoAsmInstructionType::Je;
    if (lower == "jne")     return AutoAsmInstructionType::Jne;
    if (lower == "push")    return AutoAsmInstructionType::Push;
    if (lower == "pop")     return AutoAsmInstructionType::Pop;
    if (lower == "call")    return AutoAsmInstructionType::Call;
    if (lower == "ret")     return AutoAsmInstructionType::Ret;
    if (lower == "nop")     return AutoAsmInstructionType::Nop;
    if (lower == "int3")    return AutoAsmInstructionType::Int3;
    if (lower == "db" || lower == "de" || lower == "dd") return AutoAsmInstructionType::RawBytes;
    return AutoAsmInstructionType::Nop; // Default
}

bool parseNumber(const QString& text, int64_t* out) {
    bool ok = false;
    if (text.startsWith("0x", Qt::CaseInsensitive)) {
        *out = static_cast<int64_t>(text.toULongLong(&ok, 16));
    } else {
        *out = text.toLongLong(&ok);
    }
    return ok;
}

// Patterns regex pour le parsing
const QRegularExpression reAlloc(QStringLiteral(R"(alloc\(\s*(\w+)\s*,\s*(\d+)\s*\))"));
const QRegularExpression reDealloc(QStringLiteral(R"(dealloc\(\s*(\w+)\s*\))"));
const QRegularExpression reLabel(QStringLiteral(R"(label\(\s*(\w+)\s*\))"));
const QRegularExpression reLabelDef(QStringLiteral(R"(^(\w+)\s*:\s*$)"));
const QRegularExpression reModuleLabelDef(QStringLiteral(R"RX(^"([^"]+)"\s*\+\s*(0x[0-9a-fA-F]+|\d+)\s*:\s*$)RX"));
const QRegularExpression reMov(QStringLiteral(R"(mov\s+\[([^\]]+)\]\s*,\s*(.+))"));
const QRegularExpression reJmp(QStringLiteral(R"((jmp|je|jne|call)\s+(\S+))"));
const QRegularExpression reAddr(QStringLiteral(R"((?:")([^"]+)(?:"\s*\+\s*)(0x[0-9a-fA-F]+|\d+))"));

void appendI32(QByteArray* out, int64_t value) {
    const int32_t narrowed = static_cast<int32_t>(value);
    out->append(static_cast<char>(narrowed & 0xFF));
    out->append(static_cast<char>((narrowed >> 8) & 0xFF));
    out->append(static_cast<char>((narrowed >> 16) & 0xFF));
    out->append(static_cast<char>((narrowed >> 24) & 0xFF));
}

bool parseAutoAsmNumber(QString text, int64_t* out) {
    text = text.trimmed();
    if (text.endsWith('h', Qt::CaseInsensitive)) {
        text = QStringLiteral("0x") + text.left(text.size() - 1);
    }
    return parseNumber(text, out);
}

// "mov [reg+disp], imm" : registre de base 64 bits + deplacement optionnel,
// PAS d'index/echelle/RIP-relatif (v1 volontairement borne au cas qui
// correspond a une adresse cible capturee par Ecrit par : "cible 0x..."
// derriere une instruction "mov [base+disp], ..."). Table des noms de
// registres 64 bits vers leur code 0-15 (registres etendus r8-r15 -> REX.B).
int registerCode(const QString& name, bool* extended) {
    static const QHash<QString, int> table = {
        {"rax", 0}, {"rcx", 1}, {"rdx", 2}, {"rbx", 3},
        {"rsp", 4}, {"rbp", 5}, {"rsi", 6}, {"rdi", 7},
        {"r8", 8}, {"r9", 9}, {"r10", 10}, {"r11", 11},
        {"r12", 12}, {"r13", 13}, {"r14", 14}, {"r15", 15},
    };
    const auto it = table.constFind(name.toLower());
    if (it == table.constEnd()) return -1;
    *extended = it.value() >= 8;
    return it.value();
}

const QRegularExpression reMemOperand(QStringLiteral(R"(^\s*(r[a-z0-9]+)\s*(?:([+-])\s*(0x[0-9a-fA-F]+|\d+))?\s*$)"));

// Encode "mov dword ptr [baseReg+disp], imm32" (opcode C7 /0). Ecrit un
// operande 32 bits (dword) uniquement : couvre le cas le plus courant
// (Int32/UInt32/score/XP/vie) sans etendre la portee a byte/word/qword.
bool encodeMemImmMov(const QString& destination, int64_t immediate, QByteArray* out, QString* error) {
    const auto match = reMemOperand.match(destination.trimmed());
    if (!match.hasMatch()) {
        if (error) *error = KE_TXT(
            "Destination mémoire non supportée : '%1'. Seule la forme [registre64+/-déplacement] est gérée "
            "(ex: [rdi+8], [rax-4], [rbx]) — pas d'index, d'échelle, ni d'adressage RIP-relatif.",
            "Unsupported memory destination: '%1'. Only the [register64+/-displacement] form is handled "
            "(e.g. [rdi+8], [rax-4], [rbx]) — no index, scale, or RIP-relative addressing.").arg(destination);
        return false;
    }

    bool extended = false;
    const int regCode = registerCode(match.captured(1), &extended);
    if (regCode < 0) {
        if (error) *error = KE_TXT("Registre inconnu : '%1'.", "Unknown register: '%1'.").arg(match.captured(1));
        return false;
    }

    int64_t disp = 0;
    if (!match.captured(3).isEmpty()) {
        int64_t magnitude = 0;
        if (!parseAutoAsmNumber(match.captured(3), &magnitude)) {
            if (error) *error = KE_TXT("Déplacement invalide : '%1'.", "Invalid displacement: '%1'.").arg(match.captured(3));
            return false;
        }
        disp = match.captured(2) == "-" ? -magnitude : magnitude;
    }
    if (disp < std::numeric_limits<int32_t>::min() || disp > std::numeric_limits<int32_t>::max()) {
        if (error) *error = KE_TXT("Déplacement hors plage (max disp32).", "Displacement out of range (max disp32).");
        return false;
    }

    const int rmField = regCode & 0x7;
    const bool needsSib = rmField == 4;  // rsp/r12 comme base impose un octet SIB.
    // rbp/r13 avec mod=00 est reserve (RIP-relatif) : forcer mod=01 disp8=0
    // plutot que de produire un encodage ambigu/invalide.
    const bool forceDisp8Zero = rmField == 5 && disp == 0;

    uint8_t mod;
    QByteArray dispBytes;
    if (disp == 0 && !forceDisp8Zero) {
        mod = 0b00;
    } else if (disp >= -128 && disp <= 127) {
        mod = 0b01;
        dispBytes.append(static_cast<char>(static_cast<int8_t>(disp)));
    } else {
        mod = 0b10;
        appendI32(&dispBytes, disp);
    }

    QByteArray code;
    if (extended) {
        code.append(static_cast<char>(0x41));  // REX.B (etend ModRM.rm / SIB.base)
    }
    code.append(static_cast<char>(0xC7));
    code.append(static_cast<char>((mod << 6) | (0 << 3) | (needsSib ? 0b100 : static_cast<uint8_t>(rmField))));
    if (needsSib) {
        code.append(static_cast<char>((0 << 6) | (0b100 << 3) | static_cast<uint8_t>(rmField)));  // pas d'index
    }
    code.append(dispBytes);
    appendI32(&code, immediate);

    *out = code;
    return true;
}

bool parseRawData(const QString& mnemonic, const QString& text, QByteArray* out, QString* error) {
    const QStringList parts = text.split(QRegularExpression(QStringLiteral(R"([\s,]+)")), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        if (error) *error = KE_TXT("Directive raw byte vide.", "Raw byte directive is empty.");
        return false;
    }

    const bool dwordMode = mnemonic.compare("dd", Qt::CaseInsensitive) == 0;
    for (const auto& part : parts) {
        QString token = part.trimmed();
        bool ok = false;
        int64_t value = 0;
        if (token.endsWith('h', Qt::CaseInsensitive)) {
            token = token.left(token.size() - 1);
            value = static_cast<int64_t>(token.toULongLong(&ok, 16));
        } else if (token.startsWith("0x", Qt::CaseInsensitive)) {
            value = static_cast<int64_t>(token.mid(2).toULongLong(&ok, 16));
        } else {
            value = static_cast<int64_t>(token.toULongLong(&ok, 16));
        }

        if (!ok) {
            if (error) *error = KE_TXT("Valeur raw invalide '%1'", "Invalid raw value '%1'").arg(part);
            return false;
        }

        if (dwordMode) {
            appendI32(out, value);
        } else {
            if (value < 0 || value > 0xFF) {
                if (error) *error = KE_TXT("Byte raw hors plage '%1'", "Raw byte out of range '%1'").arg(part);
                return false;
            }
            out->append(static_cast<char>(value & 0xFF));
        }
    }
    return true;
}

int encodedInstructionSize(const AutoAsmInstruction& instr, QString* error) {
    switch (instr.type) {
        case AutoAsmInstructionType::Alloc:
        case AutoAsmInstructionType::Dealloc:
        case AutoAsmInstructionType::Label:
        case AutoAsmInstructionType::ModuleLabel:
            return 0;
        case AutoAsmInstructionType::Nop:
        case AutoAsmInstructionType::Ret:
        case AutoAsmInstructionType::Int3:
            return 1;
        case AutoAsmInstructionType::Jmp:
        case AutoAsmInstructionType::Call:
            return 5;
        case AutoAsmInstructionType::Je:
        case AutoAsmInstructionType::Jne:
            return 6;
        case AutoAsmInstructionType::RawBytes: {
            QByteArray raw;
            return parseRawData(instr.destination.isEmpty() ? "db" : instr.destination, instr.target, &raw, error)
                ? raw.size()
                : -1;
        }
        case AutoAsmInstructionType::Mov: {
            QByteArray encoded;
            return encodeMemImmMov(instr.destination, instr.value, &encoded, error) ? encoded.size() : -1;
        }
        default:
            if (error) *error = KE_TXT("L'instruction nécessite un assembleur complet.", "Instruction requires a full assembler backend.");
            return -1;
    }
}

bool resolveBranchTarget(const AutoAsmInstruction& instr,
                         const QHash<QString, uint64_t>& labels,
                         uint64_t* target,
                         QString* error) {
    int64_t numeric = 0;
    if (parseAutoAsmNumber(instr.target, &numeric)) {
        *target = static_cast<uint64_t>(numeric);
        return true;
    }

    const auto it = labels.constFind(instr.target);
    if (it != labels.constEnd()) {
        *target = it.value();
        return true;
    }

    if (error) *error = KE_TXT("Cible de branchement inconnue '%1'", "Unknown branch target '%1'").arg(instr.target);
    return false;
}

} // namespace

AutoAsmScript parseAutoAsmScript(const QString& scriptText) {
    AutoAsmScript script;
    const QStringList lines = tokenize(scriptText);

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        const int lineNum = i + 1;

        // Ignorer commentaires et lignes vides
        if (line.isEmpty() || line.startsWith("//") || line.startsWith("{")) {
            continue;
        }

        // alloc(name, size)
        auto m = reAlloc.match(line);
        if (m.hasMatch()) {
            AutoAsmAllocation alloc;
            alloc.name = m.captured(1);
            alloc.size = m.captured(2).toULongLong();
            script.allocations.append(alloc);

            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Alloc;
            instr.target = alloc.name;
            instr.value = static_cast<int64_t>(alloc.size);
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // dealloc(name)
        m = reDealloc.match(line);
        if (m.hasMatch()) {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Dealloc;
            instr.target = m.captured(1);
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // label(name)
        m = reLabel.match(line);
        if (m.hasMatch()) {
            AutoAsmLabel label;
            label.name = m.captured(1);
            script.labels.append(label);

            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Label;
            instr.target = m.captured(1);
            instr.value = 0;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // "module.exe"+0x1234: — ouvre une region sur un site EXISTANT du
        // processus cible (pattern CE classique : jmp vers un trampoline
        // alloue ailleurs). Teste AVANT reLabelDef : les deux regex ne se
        // recoupent pas (celle-ci exige des guillemets), mais autant garder
        // le cas le plus specifique en premier.
        m = reModuleLabelDef.match(line);
        if (m.hasMatch()) {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::ModuleLabel;
            instr.target = m.captured(1);
            int64_t moduleOffset = 0;
            parseAutoAsmNumber(m.captured(2), &moduleOffset);
            instr.offset = moduleOffset;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // Label definition: name:
        m = reLabelDef.match(line);
        if (m.hasMatch()) {
            AutoAsmLabel label;
            label.name = m.captured(1);
            script.labels.append(label);

            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Label;
            instr.target = m.captured(1);
            instr.value = 1;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // mov [addr], value
        m = reMov.match(line);
        if (m.hasMatch()) {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Mov;
            instr.destination = m.captured(1);
            int64_t val = 0;
            parseNumber(m.captured(2).trimmed(), &val);
            instr.value = val;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // jmp/je/jne/call target
        m = reJmp.match(line);
        if (m.hasMatch()) {
            AutoAsmInstruction instr;
            instr.type = parseInstructionType(m.captured(1));
            instr.target = m.captured(2);
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // nop / ret
        if (line.toLower() == "nop") {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Nop;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }
        if (line.toLower() == "ret") {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Ret;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }
        if (line.toLower() == "int3") {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::Int3;
            instr.line = lineNum;
            script.instructions.append(instr);
            continue;
        }

        // db/de/dd raw bytes
        if (line.toLower().startsWith("db ") || line.toLower().startsWith("de ") || line.toLower().startsWith("dd ")) {
            AutoAsmInstruction instr;
            instr.type = AutoAsmInstructionType::RawBytes;
            instr.line = lineNum;
            instr.destination = line.left(2).toLower();
            instr.target = line.mid(3).trimmed();
            script.instructions.append(instr);
            continue;
        }

        script.success = false;
        script.error = KE_TXT("Syntaxe auto-assembleur non prise en charge : %1", "Unsupported auto-assembler syntax: %1").arg(line);
        script.errorLine = lineNum;
        return script;
    }

    script.success = true;
    return script;
}

namespace {

// Resout l'adresse absolue d'un bloc "module"+offset:. Erreur si le module
// n'est pas dans le contexte (processus non attache, ou nom de module
// inconnu du processus cible au moment de la compilation).
bool resolveModuleBlockBase(const QString& moduleName, int64_t moduleOffset,
                             const AutoAsmCompileContext& context, uint64_t* out, QString* error) {
    const auto it = context.moduleBaseAddresses.constFind(moduleName);
    if (it == context.moduleBaseAddresses.constEnd()) {
        if (error) *error = KE_TXT(
            "Module inconnu : '%1'. Vérifie que le processus est attaché et que ce module y est bien chargé.",
            "Unknown module: '%1'. Check that the process is attached and that this module is actually loaded in it."
        ).arg(moduleName);
        return false;
    }
    *out = it.value() + static_cast<uint64_t>(moduleOffset);
    return true;
}

} // namespace

AutoAsmCompileResult compileAutoAsmScript(const AutoAsmScript& script, uint64_t baseAddress, const AutoAsmCompileContext& context) {
    AutoAsmCompileResult result;
    if (!script.success) {
        result.error = script.error.isEmpty() ? KE_TXT("Script invalide.", "Invalid script.") : script.error;
        result.errorLine = script.errorLine;
        return result;
    }

    // "nom:" ne demarre une NOUVELLE region que si nom correspond a un
    // alloc() connu (ex: "newmem:") ; sinon c'est un simple label local dans
    // la region courante (ex: "returnhere:"), comme avant.
    auto resolveAllocLabelBase = [&](const QString& name) -> std::optional<uint64_t> {
        const auto it = context.allocationAddresses.constFind(name);
        return it != context.allocationAddresses.constEnd() ? std::optional<uint64_t>(it.value()) : std::nullopt;
    };

    // ---- Passe 1 : resout labelAddresses en suivant les resets de curseur.
    // `baseAddress` sert de curseur initial (retro-compat scripts a une
    // seule region, sans aucun "nom:"/module lie).
    QHash<QString, uint64_t> labelAddresses;
    {
        uint64_t cursor = baseAddress;
        for (const auto& instr : script.instructions) {
            if (instr.type == AutoAsmInstructionType::Label && instr.value == 1) {
                if (const auto reset = resolveAllocLabelBase(instr.target)) {
                    cursor = *reset;
                }
                labelAddresses.insert(instr.target, cursor);
                continue;
            }
            if (instr.type == AutoAsmInstructionType::ModuleLabel) {
                QString moduleError;
                if (!resolveModuleBlockBase(instr.target, instr.offset, context, &cursor, &moduleError)) {
                    result.error = moduleError;
                    result.errorLine = instr.line;
                    return result;
                }
                continue;
            }
            if (instr.type == AutoAsmInstructionType::Alloc || instr.type == AutoAsmInstructionType::Dealloc) {
                continue;
            }

            QString sizeError;
            const int size = encodedInstructionSize(instr, &sizeError);
            if (size < 0) {
                result.error = sizeError;
                result.errorLine = instr.line;
                return result;
            }
            cursor += static_cast<uint64_t>(size);
        }
    }
    for (auto it = labelAddresses.constBegin(); it != labelAddresses.constEnd(); ++it) {
        result.labels.append(AutoAsmLabel{it.key(), it.value()});
    }

    // ---- Passe 2 : emet les bytes, une AutoAsmCompiledRegion par reset de
    // curseur (donc une seule region pour un script simple, deux pour le
    // pattern CE classique site-existant + trampoline alloue).
    AutoAsmCompiledRegion* currentRegion = nullptr;
    auto startRegion = [&](uint64_t address) -> AutoAsmCompiledRegion& {
        result.regions.append(AutoAsmCompiledRegion{address, {}});
        currentRegion = &result.regions.last();
        return *currentRegion;
    };
    uint64_t cursor = baseAddress;
    for (const auto& instr : script.instructions) {
        if (instr.type == AutoAsmInstructionType::Label) {
            // value==0 : label(name) — simple declaration avancee, resolue en
            // passe 1, rien a emettre. value==1 : "name:" — reset de curseur
            // uniquement si nom lie a un alloc() connu (sinon label local).
            if (instr.value == 1) {
                if (const auto reset = resolveAllocLabelBase(instr.target)) {
                    startRegion(*reset);
                    cursor = *reset;
                }
            }
            continue;
        }
        if (instr.type == AutoAsmInstructionType::ModuleLabel) {
            uint64_t resolved = 0;
            QString moduleError;
            // Deja valide en passe 1 : ne peut plus echouer ici sauf incoherence interne.
            resolveModuleBlockBase(instr.target, instr.offset, context, &resolved, &moduleError);
            startRegion(resolved);
            cursor = resolved;
            continue;
        }
        if (instr.type == AutoAsmInstructionType::Alloc || instr.type == AutoAsmInstructionType::Dealloc) {
            continue;
        }
        if (!currentRegion) {
            // Script sans aucun "nom:"/module lie avant sa premiere
            // instruction reelle : demarre quand meme une region a
            // baseAddress, comme le faisait l'ancien modele mono-region.
            startRegion(baseAddress);
        }

        switch (instr.type) {
            case AutoAsmInstructionType::Nop:
                currentRegion->code.append('\x90');
                cursor += 1;
                break;
            case AutoAsmInstructionType::Ret:
                currentRegion->code.append('\xC3');
                cursor += 1;
                break;
            case AutoAsmInstructionType::Int3:
                currentRegion->code.append('\xCC');
                cursor += 1;
                break;
            case AutoAsmInstructionType::RawBytes: {
                QByteArray raw;
                QString rawError;
                if (!parseRawData(instr.destination.isEmpty() ? "db" : instr.destination, instr.target, &raw, &rawError)) {
                    result.error = rawError;
                    result.errorLine = instr.line;
                    return result;
                }
                currentRegion->code.append(raw);
                cursor += static_cast<uint64_t>(raw.size());
                break;
            }
            case AutoAsmInstructionType::Mov: {
                QByteArray encoded;
                QString movError;
                if (!encodeMemImmMov(instr.destination, instr.value, &encoded, &movError)) {
                    result.error = movError;
                    result.errorLine = instr.line;
                    return result;
                }
                currentRegion->code.append(encoded);
                cursor += static_cast<uint64_t>(encoded.size());
                break;
            }
            case AutoAsmInstructionType::Jmp:
            case AutoAsmInstructionType::Call:
            case AutoAsmInstructionType::Je:
            case AutoAsmInstructionType::Jne: {
                uint64_t target = 0;
                QString targetError;
                if (!resolveBranchTarget(instr, labelAddresses, &target, &targetError)) {
                    result.error = targetError;
                    result.errorLine = instr.line;
                    return result;
                }

                const int instrSize = (instr.type == AutoAsmInstructionType::Je || instr.type == AutoAsmInstructionType::Jne) ? 6 : 5;
                const int64_t rel = static_cast<int64_t>(target) - static_cast<int64_t>(cursor + instrSize);
                if (rel < std::numeric_limits<int32_t>::min() || rel > std::numeric_limits<int32_t>::max()) {
                    result.error = KE_TXT("Cible de branchement hors de la plage rel32.", "Branch target is outside rel32 range.");
                    result.errorLine = instr.line;
                    return result;
                }

                if (instr.type == AutoAsmInstructionType::Jmp) {
                    currentRegion->code.append('\xE9');
                } else if (instr.type == AutoAsmInstructionType::Call) {
                    currentRegion->code.append('\xE8');
                } else {
                    currentRegion->code.append('\x0F');
                    currentRegion->code.append(instr.type == AutoAsmInstructionType::Je ? '\x84' : '\x85');
                }
                appendI32(&currentRegion->code, rel);
                cursor += static_cast<uint64_t>(instrSize);
                break;
            }
            default:
                result.error = KE_TXT("L'instruction nécessite un assembleur complet.", "Instruction requires a full assembler backend.");
                result.errorLine = instr.line;
                return result;
        }
    }

    result.success = true;
    return result;
}

AutoAsmResult executeAutoAsmScript(const ProcessHandle& process, const AutoAsmScript& script) {
    AutoAsmResult result;

    if (!script.success) {
        result.error = KE_TXT("Script invalide (échec du parsing).", "Invalid script (parse failed).");
        return result;
    }

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = KE_TXT("Handle de processus invalide.", "Invalid process handle.");
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // 1. Allouer la mémoire demandée
    for (const auto& scriptAlloc : script.allocations) {
        AutoAsmAllocation alloc = scriptAlloc;
        LPVOID pMem = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(alloc.size),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!pMem) {
            result.error = KE_TXT("Échec de VirtualAllocEx pour '%1'", "VirtualAllocEx failed for '%1'").arg(alloc.name);
            for (const auto& allocated : result.allocations) {
                if (allocated.remoteAddress) {
                    VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(allocated.remoteAddress), 0, MEM_RELEASE);
                }
            }
            result.allocations.clear();
            return result;
        }
        alloc.remoteAddress = reinterpret_cast<uint64_t>(pMem);
        result.allocations.append(alloc);
        KE_LOG_INFO() << "AutoAsm: allocated " << alloc.name.toStdString() << " at 0x" << std::hex << alloc.remoteAddress;
    }

    auto freeAllocations = [&]() {
        for (const auto& allocated : result.allocations) {
            if (allocated.remoteAddress) {
                VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(allocated.remoteAddress), 0, MEM_RELEASE);
            }
        }
        result.allocations.clear();
    };

    const bool hasRealInstructions = std::any_of(script.instructions.begin(), script.instructions.end(), [](const auto& instr) {
        return instr.type != AutoAsmInstructionType::Alloc
            && instr.type != AutoAsmInstructionType::Dealloc
            && instr.type != AutoAsmInstructionType::Label;
    });
    if (!hasRealInstructions) {
        result.success = true;
        KE_LOG_INFO() << "AutoAsm: script allocated " << result.allocations.size() << " remote block(s); no code to write";
        return result;
    }

    // 2. Resoudre les modules references par les blocs "module"+offset: dans
    // le processus cible (necessaire pour rediriger un site EXISTANT, ex.
    // repris du RIP capture par "Ecrit par" — au-dela des blocs alloues
    // qu'executeAutoAsmScript gerait seul jusqu'ici).
    AutoAsmCompileContext context;
    for (const auto& alloc : result.allocations) {
        context.allocationAddresses.insert(alloc.name, alloc.remoteAddress);
    }
    const bool referencesModules = std::any_of(script.instructions.begin(), script.instructions.end(), [](const auto& instr) {
        return instr.type == AutoAsmInstructionType::ModuleLabel;
    });
    if (referencesModules) {
        const auto modules = ProcessEnumerator::enumerateModules(process.pid());
        for (const auto& instr : script.instructions) {
            if (instr.type != AutoAsmInstructionType::ModuleLabel) continue;
            if (context.moduleBaseAddresses.contains(instr.target)) continue;
            for (const auto& module : modules) {
                if (module.name.compare(instr.target, Qt::CaseInsensitive) == 0) {
                    context.moduleBaseAddresses.insert(instr.target, module.baseAddress);
                    break;
                }
            }
        }
    }

    // 3. Compiler (une region par changement de curseur : bloc alloue ou site existant).
    const uint64_t baseAddress = result.allocations.isEmpty() ? 0 : result.allocations.first().remoteAddress;
    const auto compiled = compileAutoAsmScript(script, baseAddress, context);
    if (!compiled.success) {
        result.error = compiled.error;
        result.errorLine = compiled.errorLine;
        freeAllocations();
        return result;
    }
    if (compiled.regions.isEmpty()) {
        result.success = true;
        return result;
    }

    // 4. Ecrire chaque region. Best-effort atomique : si une region echoue
    // apres que d'autres ont deja ete ecrites, on restaure celles-la avant
    // de rendre la main — un patch partiel (ex: le jmp du site pose mais pas
    // le trampoline derriere) planterait le processus cible au prochain
    // passage sur ce code.
    QSet<uint64_t> allocatedAddresses;
    for (const auto& alloc : result.allocations) {
        allocatedAddresses.insert(alloc.remoteAddress);
    }

    MemoryWriter writer(process);
    for (const auto& region : compiled.regions) {
        if (region.code.isEmpty()) continue;
        const auto written = writer.write(region.baseAddress, region.code, /*verify=*/true);
        if (!written.success || !written.verified) {
            result.error = written.errorMessage.isEmpty()
                ? KE_TXT("Échec de l'écriture à 0x%1.", "Write failed at 0x%1.").arg(region.baseAddress, 0, 16)
                : written.errorMessage;
            for (const auto& patched : result.patchedRegions) {
                writer.write(patched.address, patched.originalBytes, /*verify=*/false);
            }
            result.patchedRegions.clear();
            freeAllocations();
            return result;
        }
        AutoAsmPatchedRegion patched;
        patched.address = region.baseAddress;
        patched.originalBytes = written.previousValue;
        patched.size = static_cast<int>(region.code.size());
        patched.wasAllocated = allocatedAddresses.contains(region.baseAddress);
        result.patchedRegions.append(patched);

        KE_LOG_WARN() << "AutoAsm: wrote " << region.code.size() << " byte(s) at 0x" << std::hex << region.baseAddress
                      << (patched.wasAllocated ? " (allocated block)" : " (existing site)");
    }

    result.success = true;
#else
    (void)process;
    result.error = KE_TXT("L'auto-assembleur est réservé à Windows.", "Auto-assembler is Windows-only.");
#endif

    return result;
}

bool restoreAutoAsmScript(const ProcessHandle& process, const AutoAsmResult& result) {
#ifdef Q_OS_WIN
    if (!result.success) {
        return false;
    }

    const HANDLE hProcess = process.rawHandle();

    // Une region "site existant" (wasAllocated=false, ex: "game.exe"+0x1234:)
    // DOIT etre restauree : sans ca le jmp injecte reste actif et le
    // processus cible plante ou se comporte n'importe comment des que ce
    // code repasse la. Une region "bloc alloue" n'a rien d'obligatoire a
    // restaurer avant liberation (memoire fraiche, pas de code preexistant a
    // reparer), mais on le fait quand meme par uniformite/prudence. On tente
    // TOUJOURS toutes les regions (pas de retour anticipe sur un echec) pour
    // ne jamais laisser un site existant patche parce qu'une autre region a
    // echoue a se restaurer avant lui.
    MemoryWriter writer(process);
    bool restoreOk = true;
    for (const auto& patched : result.patchedRegions) {
        if (patched.originalBytes.isEmpty()) continue;
        const auto written = writer.write(patched.address, patched.originalBytes, /*verify=*/true);
        if (!written.success || !written.verified) {
            restoreOk = false;
            KE_LOG_WARN() << "AutoAsm: failed to restore " << patched.originalBytes.size()
                          << " byte(s) at 0x" << std::hex << patched.address
                          << (patched.wasAllocated ? " (allocated block)" : " (existing site — code may be left patched!)");
        }
    }

    // Libérer les allocations
    for (const auto& alloc : result.allocations) {
        if (alloc.remoteAddress) {
            VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(alloc.remoteAddress), 0, MEM_RELEASE);
        }
    }

    return restoreOk;
#else
    (void)process;
    (void)result;
    return false;
#endif
}

QString formatNumber(int64_t value) {
    if (value > 0xFF) {
        return QStringLiteral("0x%1 (%2)").arg(value, 0, 16).arg(value);
    }
    return QString::number(value);
}

} // namespace killcore
