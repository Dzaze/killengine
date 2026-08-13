#include "code_patch.h"

#include "memory/memory_writer.h"

namespace killcore {

namespace {

int hexNibble(QChar ch) {
    const ushort c = ch.toUpper().unicode();
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

CodePatchResult writePatchBytes(const ProcessHandle& process, uint64_t address, const QByteArray& bytes, bool verify) {
    CodePatchResult result;
    result.address = address;

    if (bytes.isEmpty()) {
        result.error = "Aucun byte de patch.";
        return result;
    }

    MemoryWriter writer(process);
    const auto write = writer.write(address, bytes, verify);
    result.success = write.success;
    result.verified = write.verified;
    result.protectionChanged = write.protectionChanged;
    result.bytesWritten = write.bytesWritten;
    result.previousBytes = write.previousValue;
    result.error = write.errorMessage;
    return result;
}

} // namespace

PatchBytes parsePatchBytes(const QString& bytesText) {
    PatchBytes parsed;
    const QStringList tokens = bytesText.simplified().split(' ', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        parsed.error = "Bytes de patch vides.";
        return parsed;
    }

    for (const QString& rawToken : tokens) {
        QString token = rawToken.trimmed();
        if (token.startsWith("0x", Qt::CaseInsensitive)) {
            token = token.mid(2);
        }
        if (token == "?" || token == "??") {
            parsed.error = "Un patch doit contenir des bytes exacts, pas de wildcard.";
            parsed.bytes.clear();
            return parsed;
        }
        if (token.size() != 2) {
            parsed.error = QString("Byte de patch invalide: '%1'. Utilise par exemple '90 90'.").arg(rawToken);
            parsed.bytes.clear();
            return parsed;
        }

        const int hi = hexNibble(token.at(0));
        const int lo = hexNibble(token.at(1));
        if (hi < 0 || lo < 0) {
            parsed.error = QString("Octet hex invalide: '%1'.").arg(rawToken);
            parsed.bytes.clear();
            return parsed;
        }

        parsed.bytes.append(static_cast<char>((hi << 4) | lo));
    }

    return parsed;
}

CodePatchResult applyCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& patchBytes, bool verify) {
    return writePatchBytes(process, address, patchBytes, verify);
}

CodePatchResult restoreCodePatch(const ProcessHandle& process, uint64_t address, const QByteArray& originalBytes, bool verify) {
    return writePatchBytes(process, address, originalBytes, verify);
}

} // namespace killcore
