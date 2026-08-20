#pragma once

#include "process/process_handle.h"

#include <QByteArray>
#include <QHash>
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
    ModuleLabel,    // "module.exe"+0x1234:  (ouvre une region sur un site EXISTANT du processus cible)
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
    QString target;          // adresse/label/module ; pour ModuleLabel: nom du module ("game.exe")
    int64_t offset{0};       // offset optionnel ; pour ModuleLabel: offset dans le module
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

/// Une région de mémoire réellement écrite par executeAutoAsmScript, avec de
/// quoi la restaurer. `wasAllocated` distingue un bloc fraîchement alloué
/// (VirtualAllocEx, libéré à la restauration) d'un site EXISTANT du
/// processus cible (ex: "game.exe"+0x1234:, bytes originaux réécrits en
/// place à la restauration, jamais libéré).
struct AutoAsmPatchedRegion {
    uint64_t address{0};
    QByteArray originalBytes;
    int size{0};
    bool wasAllocated{false};
};

/// Résultat de l'exécution d'un script.
struct AutoAsmResult {
    bool success{false};
    QString error;
    int errorLine{0};                           ///< Ligne du script en cause si error vient de la compilation
    QList<AutoAsmAllocation> allocations;       ///< Avec remoteAddress rempli
    QList<AutoAsmPatchedRegion> patchedRegions; ///< Une entrée par région réellement écrite
};

/// Une région de code compilée, à écrire à `baseAddress` (allouée ou site existant).
struct AutoAsmCompiledRegion {
    uint64_t baseAddress{0};
    QByteArray code;
};

/// Contexte de résolution d'adresses pour compileAutoAsmScript : nécessaire
/// dès qu'un script référence un label lié à un alloc() (ex: "newmem:") ou
/// un site existant du processus cible (ex: "game.exe"+0x1234:).
struct AutoAsmCompileContext {
    QHash<QString, uint64_t> allocationAddresses; ///< nom alloc() -> adresse distante déjà allouée
    QHash<QString, uint64_t> moduleBaseAddresses; ///< nom de module -> adresse de base dans le processus cible
};

/// Résultat de compilation en bytes x64 pour les instructions supportées.
/// Un script "simple" (aucun label lié à un alloc()/module) produit UNE
/// région à `baseAddress`, comme avant. Un script CE-style à deux régions
/// (site existant + trampoline alloué) produit une région par changement de
/// curseur ("nom:" lié à un alloc(), ou "module"+offset:).
struct AutoAsmCompileResult {
    bool success{false};
    QString error;
    int errorLine{0};
    QList<AutoAsmCompiledRegion> regions;
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
 * @brief Compile le sous-ensemble runtime supporté en bytes x64, une région
 * par changement de curseur d'adresse.
 *
 * Support volontairement borné : nop, ret, int3, db/de/dd, jmp/call/je/jne
 * vers adresse absolue ou label local, mov [registre64+/-déplacement], imm32.
 * Les instructions mémoire plus complexes (index, RIP-relatif) sont refusées
 * proprement tant qu'un backend assembleur complet n'est pas branché.
 *
 * `baseAddress` sert de curseur initial pour un script sans aucun label lié
 * a un alloc()/module (rétro-compatible avec les scripts à une seule région).
 * `context` résout les changements de curseur explicites ("nom:" lié à un
 * alloc() connu, ou "module"+offset:) — nécessaire dès qu'un script en utilise.
 */
AutoAsmCompileResult compileAutoAsmScript(
    const AutoAsmScript& script,
    uint64_t baseAddress = 0,
    const AutoAsmCompileContext& context = {});

/**
 * @brief Exécute un script auto-assembler dans un processus distant.
 *
 * 1. Résout les allocations (VirtualAllocEx)
 * 2. Résout les modules référencés par les blocs "module"+offset: dans le processus cible
 * 3. Compile les instructions en bytes, une région par changement de curseur
 * 4. Sauvegarde les bytes originaux de chaque région puis écrit le code compilé
 *
 * Best-effort atomique : si une région échoue à s'écrire après que d'autres
 * ont réussi, les régions déjà écrites sont restaurées avant de retourner
 * l'erreur — le processus cible n'est jamais laissé à moitié patché.
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
