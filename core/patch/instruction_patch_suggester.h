#pragma once

#include "scanner/scan_types.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

namespace killcore {

struct InstructionInfo {
    bool success{false};
    int length{0};
    QString mnemonicHint;
    QString disassembly;
    QString decoder{"builtin"};
    QString category{"unknown"};
    QString rawBytesText;
    QString stableAobPattern;
    QString error;
    // Position (offset/taille en octets dans l'instruction) du SEUL operande
    // immediat, si l'instruction en a exactement un (ex: "mov dword [rdi+8],
    // 0x64" a un immediat 32 bits). -1/0 si l'instruction n'a pas
    // d'immediat, en a plusieurs (rare, pas gere pour rester simple/surs),
    // ou si le decodeur utilise n'est pas Zydis (KILLENGINE_HAS_ZYDIS absent
    // : le decodeur builtin ne calcule pas cette info).
    int immediateOffset{-1};
    int immediateSize{0};
    // Registre de base (ex: "rdi") + deplacement de l'operande MEMOIRE
    // destination, si celle-ci a la forme simple [base+disp] exploitable par
    // encodeMemImmMov (core/scripting/auto_assembler.cpp) : pas d'index, pas
    // d'echelle, pas RIP-relatif. memBaseRegister vide si l'instruction n'a
    // pas d'operande memoire ou si sa forme est trop complexe. Rempli
    // uniquement par le decodeur Zydis, quelle que soit la source (immediat
    // OU registre) — sert a construire un trampoline "force cette valeur ici"
    // meme quand suggestInstructionPatches() ne propose pas de patch d'octets
    // direct (source registre, pas d'immediat a substituer).
    QString memBaseRegister;
    int64_t memDisplacement{0};
};

struct PatchSuggestion {
    QString label;
    QString bytesText;
    QString description;
    QString category;
    QString riskLevel{"medium"};
    bool risky{false};
    // Quand vrai, bytesText n'est qu'un point de depart (bytes originaux) :
    // le frontend doit demander une valeur a l'utilisateur puis reconstruire
    // les bytes en substituant valueSize octets (little-endian) a partir de
    // valueOffset, plutot que d'appliquer bytesText tel quel.
    bool needsValueInput{false};
    int valueOffset{-1};
    int valueSize{0};
};

InstructionInfo decodeX64InstructionLength(const QByteArray& bytes);
QList<PatchSuggestion> suggestInstructionPatches(const InstructionInfo& instruction);

struct BackwardDisassemblyResult {
    bool success{false};
    QString error;
    // Dans l'ordre d'execution ; la derniere entree est l'instruction qui
    // commence exactement a targetOffsetInWindow (le RIP cible connu).
    QList<InstructionInfo> instructions;
    // Offset, dans windowBytes, ou commence la premiere instruction de
    // instructions[] — permet à l'appelant de reconstruire une adresse
    // absolue par instruction (windowBaseAddress + startOffsetInWindow + ...).
    int startOffsetInWindow{-1};
};

// x86-64 n'a pas de decodage "en arriere" natif : on essaie de decoder en
// avant depuis chaque offset candidat avant targetOffsetInWindow, et on ne
// garde que le chemin qui retombe exactement sur targetOffsetInWindow (pas
// de depassement/sous-depassement). Sert a reconstruire les instructions qui
// precedent un RIP capture par findWhatWrites (ex: retrouver les champs
// "actuel"/"cible" d'un compteur anime avant l'ecriture visible).
BackwardDisassemblyResult disassembleBackwardWindow(const QByteArray& windowBytes, int targetOffsetInWindow);

// Un champ candidat de disassembleBackwardWindow() resolu en adresse memoire
// absolue (voir resolveCandidateFieldAddresses ci-dessous).
struct ResolvedCandidateField {
    uint64_t address{0};
    QString memBaseRegister;
    int64_t memDisplacement{0};
    ValueType inferredType{ValueType::Int32};
};

// Resout les operandes memoire [registre+deplacement] des instructions
// candidates (celles avec un memBaseRegister non vide) en adresses absolues,
// a partir d'une seule adresse connue : celle reellement ecrite par la
// DERNIERE instruction de la liste (invariant de disassembleBackwardWindow,
// voir BackwardDisassemblyResult ci-dessus). Cette derniere instruction doit
// donc partager le meme registre de base que les candidats a resoudre ;
// hors de la, aucune valeur de registre live n'etant disponible, un candidat
// dont le memBaseRegister differe reste non resolvable et est ignore.
// L'instruction d'ecriture elle-meme est exclue du resultat : son adresse
// (knownWriteTargetAddress) est deja connue de l'appelant comme "ne tient
// pas" (c'est justement pourquoi il cherche une autre source).
QList<ResolvedCandidateField> resolveCandidateFieldAddresses(
    const QList<InstructionInfo>& instructions,
    uint64_t knownWriteTargetAddress);

// Heuristique texte sur mnemonicHint/disassembly pour deviner le type d'une
// valeur lue/ecrite par une instruction candidate, avant d'y ecrire une
// valeur test : motifs SSE scalaires (movss/addss/subss/mulss/divss/
// cvtsi2ss/cvttss2si -> Float32, variantes "sd" -> Float64), sinon Int32 par
// defaut (le cas reel confirme - docs/STRATEGY_ROOM.md, Solitaire XP - est
// un "mov" 32 bits entier).
ValueType inferProbeValueType(const InstructionInfo& instruction);

} // namespace killcore
