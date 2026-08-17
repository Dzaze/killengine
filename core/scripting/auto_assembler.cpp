#include "auto_assembler.h"

#include "logging/logger.h"
#include "patch/instruction_patch_suggester.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QRegularExpression>
#include <QStringList>
#include <QHash>

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

bool parseRawData(const QString& mnemonic, const QString& text, QByteArray* out, QString* error) {
    const QStringList parts = text.split(QRegularExpression(QStringLiteral(R"([\s,]+)")), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        if (error) *error = "Raw byte directive is empty";
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
            if (error) *error = QStringLiteral("Invalid raw value '%1'").arg(part);
            return false;
        }

        if (dwordMode) {
            appendI32(out, value);
        } else {
            if (value < 0 || value > 0xFF) {
                if (error) *error = QStringLiteral("Raw byte out of range '%1'").arg(part);
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
        default:
            if (error) *error = "Instruction requires a full assembler backend";
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

    if (error) *error = QStringLiteral("Unknown branch target '%1'").arg(instr.target);
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
        script.error = QStringLiteral("Unsupported auto-assembler syntax: %1").arg(line);
        script.errorLine = lineNum;
        return script;
    }

    script.success = true;
    return script;
}

AutoAsmCompileResult compileAutoAsmScript(const AutoAsmScript& script, uint64_t baseAddress) {
    AutoAsmCompileResult result;
    if (!script.success) {
        result.error = script.error.isEmpty() ? "Invalid script" : script.error;
        result.errorLine = script.errorLine;
        return result;
    }

    QHash<QString, uint64_t> labelAddresses;
    uint64_t offset = 0;
    for (const auto& instr : script.instructions) {
        if (instr.type == AutoAsmInstructionType::Label && instr.value == 1) {
            labelAddresses.insert(instr.target, baseAddress + offset);
            AutoAsmLabel label;
            label.name = instr.target;
            label.address = baseAddress + offset;
            result.labels.append(label);
            continue;
        }

        QString sizeError;
        const int size = encodedInstructionSize(instr, &sizeError);
        if (size < 0) {
            result.error = sizeError;
            result.errorLine = instr.line;
            return result;
        }
        offset += static_cast<uint64_t>(size);
    }

    offset = 0;
    for (const auto& instr : script.instructions) {
        switch (instr.type) {
            case AutoAsmInstructionType::Alloc:
            case AutoAsmInstructionType::Dealloc:
            case AutoAsmInstructionType::Label:
                break;
            case AutoAsmInstructionType::Nop:
                result.code.append('\x90');
                offset += 1;
                break;
            case AutoAsmInstructionType::Ret:
                result.code.append('\xC3');
                offset += 1;
                break;
            case AutoAsmInstructionType::Int3:
                result.code.append('\xCC');
                offset += 1;
                break;
            case AutoAsmInstructionType::RawBytes: {
                QByteArray raw;
                QString rawError;
                if (!parseRawData(instr.destination.isEmpty() ? "db" : instr.destination, instr.target, &raw, &rawError)) {
                    result.error = rawError;
                    result.errorLine = instr.line;
                    return result;
                }
                result.code.append(raw);
                offset += static_cast<uint64_t>(raw.size());
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
                const int64_t rel = static_cast<int64_t>(target) - static_cast<int64_t>(baseAddress + offset + instrSize);
                if (rel < std::numeric_limits<int32_t>::min() || rel > std::numeric_limits<int32_t>::max()) {
                    result.error = "Branch target is outside rel32 range";
                    result.errorLine = instr.line;
                    return result;
                }

                if (instr.type == AutoAsmInstructionType::Jmp) {
                    result.code.append('\xE9');
                } else if (instr.type == AutoAsmInstructionType::Call) {
                    result.code.append('\xE8');
                } else {
                    result.code.append('\x0F');
                    result.code.append(instr.type == AutoAsmInstructionType::Je ? '\x84' : '\x85');
                }
                appendI32(&result.code, rel);
                offset += static_cast<uint64_t>(instrSize);
                break;
            }
            default:
                result.error = "Instruction requires a full assembler backend";
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
        result.error = "Invalid script (parse failed)";
        return result;
    }

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = "Invalid process handle";
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // 1. Allouer la mémoire demandée
    for (const auto& scriptAlloc : script.allocations) {
        AutoAsmAllocation alloc = scriptAlloc;
        LPVOID pMem = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(alloc.size),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!pMem) {
            result.error = QStringLiteral("VirtualAllocEx failed for '%1'").arg(alloc.name);
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

    // Le parser accepte déjà la syntaxe CE-like, mais l'exécution de mnemonics
    // nécessite un assembleur x64 fiable (Keystone/asmtk). Ne jamais retourner
    // success=true pour un script qui n'a pas réellement été compilé/patché.
    for (const auto& instr : script.instructions) {
        if (instr.type != AutoAsmInstructionType::Alloc &&
            instr.type != AutoAsmInstructionType::Dealloc &&
            instr.type != AutoAsmInstructionType::Label) {
            result.error = "Auto-assembler execution requires a real x64 assembler backend before patching code";
            for (const auto& allocated : result.allocations) {
                if (allocated.remoteAddress) {
                    VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(allocated.remoteAddress), 0, MEM_RELEASE);
                }
            }
            result.allocations.clear();
            return result;
        }
    }

    result.success = true;

    KE_LOG_INFO() << "AutoAsm: script allocated " << result.allocations.size()
                  << " remote blocks; no code patch emitted";
#else
    (void)process;
    result.error = "Auto-assembler is Windows-only";
#endif

    return result;
}

bool restoreAutoAsmScript(const ProcessHandle& process, const AutoAsmResult& result) {
#ifdef Q_OS_WIN
    if (!result.success || result.originalBytes.isEmpty()) {
        return false;
    }

    const HANDLE hProcess = process.rawHandle();

    // Restaurer les bytes originaux
    DWORD oldProtect = 0;
    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(result.patchAddress),
                     static_cast<SIZE_T>(result.originalBytes.size()),
                     PAGE_EXECUTE_READWRITE, &oldProtect);

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(result.patchAddress),
                                       result.originalBytes.constData(),
                                       static_cast<SIZE_T>(result.originalBytes.size()), &bytesWritten);

    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(result.patchAddress),
                     static_cast<SIZE_T>(result.originalBytes.size()), oldProtect, &oldProtect);

    // Libérer les allocations
    for (const auto& alloc : result.allocations) {
        if (alloc.remoteAddress) {
            VirtualFreeEx(hProcess, reinterpret_cast<LPVOID>(alloc.remoteAddress), 0, MEM_RELEASE);
        }
    }

    return ok && bytesWritten == static_cast<SIZE_T>(result.originalBytes.size());
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
