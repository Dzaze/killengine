#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>
#include <optional>

namespace killcore {

/// Types de tokens dans le script auto-assembler.
enum class ScriptTokenType {
    Identifier,     // alloc, label, mov, jmp, etc.
    Number,         // 123, 0xFF
    Address,        // "game.exe"+0x1234
    String,         // "texte"
    Operator,       // +, -, *, /, :
    Newline,
    Comment,        // //
    EndOfFile,
};

struct ScriptToken {
    ScriptTokenType type;
    QString value;
    int line{0};
};

/// Types d'instructions auto-assembler supportées.
enum class AutoAsmInstructionType {
    Alloc,          // alloc(name, size)
    Dealloc,        // dealloc(name)
    Label,          // label(name)
    Mov,            // mov [addr], value
    Add,            // add [addr], value
    Sub,            // sub [addr], value
    Inc,            // inc [addr]
    Dec,            // dec [addr]
    Cmp,            // cmp [addr], value
    Jmp,            // jmp target
    Je,             // je target
    Jne,            // jne target
    Push,           // push value
    Pop,            // pop [addr]
    Call,           // call target
    Ret,            // ret
    Nop,            // nop
    Int3,           // int3
    RawBytes,       // db/de/dd
};

/// Une instruction parsée du script.
struct AutoAsmInstruction {
    AutoAsmInstructionType type;
    QString target;          // adresse/label/module
    int64_t offset{0};       // offset optionnel
    QString destination;     // pour mov/add/sub: destination
    int64_t value{0};        // valeur immédiate
    QByteArray rawBytes;     // pour db/de
    int line{0};
};

/// Une allocation mémoire demandée par le script.
struct AutoAsmAllocation {
    QString name;
    size_t size{0};
    uint64_t remoteAddress{0}; ///< Rempli après exécution
};

/// Un label défini dans le script.
struct AutoAsmLabel {
    QString name;
    uint64_t address{0};
};

/// Résultat du parsing d'un script.
struct AutoAsmScript {
    bool success{false};
    QString error;
    int errorLine{0};
    QList<AutoAsmInstruction> instructions;
    QList<AutoAsmAllocation> allocations;
    QList<AutoAsmLabel> labels;
};

/// Résultat de l'exécution d'un script.
struct AutoAsmResult {
    bool success{false};
    QString error;
    QList<AutoAsmAllocation> allocations; ///< Avec remoteAddress rempli
    QByteArray originalBytes;             ///< Bytes originaux pour restore
    uint64_t patchAddress{0};             ///< Adresse qui a été patchée
    int patchSize{0};
};

/// Résultat de compilation en bytes x64 pour les instructions supportées.
struct AutoAsmCompileResult {
    bool success{false};
    QString error;
    int errorLine{0};
    QByteArray code;
    QList<AutoAsmLabel> labels;
};

/**
 * @brief Parse un script auto-assembler.
 *
 * Syntaxe supportée :
 *   alloc(newmem, 256)
 *   label(returnhere)
 *
 *   newmem:
 *     mov [rax+08], (int)9999
 *     jmp returnhere
 *
 *   "game.exe"+0x12345:
 *     jmp newmem
 *     nop
 *   returnhere:
 */
AutoAsmScript parseAutoAsmScript(const QString& scriptText);

/**
 * @brief Compile le sous-ensemble runtime supporté en bytes x64.
 *
 * Support v0 volontairement borné : nop, ret, int3, db/de/dd, jmp/call/je/jne
 * vers adresse absolue ou label local. Les instructions mémoire complexes sont
 * refusées proprement tant qu'un backend assembleur complet n'est pas branché.
 */
AutoAsmCompileResult compileAutoAsmScript(const AutoAsmScript& script, uint64_t baseAddress = 0);

/**
 * @brief Exécute un script auto-assembler dans un processus distant.
 *
 * 1. Résout les allocations (VirtualAllocEx)
 * 2. Compile les instructions en bytes
 * 3. Sauvegarde les bytes originaux à l'adresse patchée
 * 4. Écrit le code compilé
 */
AutoAsmResult executeAutoAsmScript(const ProcessHandle& process, const AutoAsmScript& script);

/**
 * @brief Restaure les bytes originaux après exécution d'un script.
 */
bool restoreAutoAsmScript(const ProcessHandle& process, const AutoAsmResult& result);

/**
 * @brief Convertit un nombre en texte (hex ou decimal).
 */
QString formatNumber(int64_t value);

} // namespace killcore
