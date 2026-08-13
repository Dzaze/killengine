#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace killcore {

struct InstructionInfo {
    bool success{false};
    int length{0};
    QString mnemonicHint;
    QString disassembly;
    QString decoder{"builtin"};
    QString stableAobPattern;
    QString error;
};

struct PatchSuggestion {
    QString label;
    QString bytesText;
    QString description;
    bool risky{false};
};

InstructionInfo decodeX64InstructionLength(const QByteArray& bytes);
QList<PatchSuggestion> suggestInstructionPatches(const InstructionInfo& instruction);

} // namespace killcore
