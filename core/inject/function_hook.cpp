#include "function_hook.h"

#include "logging/logger.h"
#include "patch/instruction_patch_suggester.h"

#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace killcore {

QByteArray generateJumpShellcode(uint64_t fromAddress, uint64_t toAddress) {
    QByteArray shellcode;

    // Vérifier si on peut utiliser un JMP rel32 (courte distance, < 2 GB)
    const int64_t delta = static_cast<int64_t>(toAddress) - static_cast<int64_t>(fromAddress) - 5;

    if (delta >= INT32_MIN && delta <= INT32_MAX) {
        // JMP rel32 : E9 xx xx xx xx (5 bytes)
        shellcode.resize(5);
        shellcode[0] = static_cast<char>(0xE9);
        const int32_t rel32 = static_cast<int32_t>(delta);
        std::memcpy(shellcode.data() + 1, &rel32, 4);
    } else {
        // JMP long : MOV RAX, addr64 + JMP RAX (12 bytes)
        // 48 B8 <8 bytes addr> = MOV RAX, imm64
        // FF E0 = JMP RAX
        shellcode.resize(12);
        shellcode[0] = static_cast<char>(0x48);
        shellcode[1] = static_cast<char>(0xB8);
        std::memcpy(shellcode.data() + 2, &toAddress, 8);
        shellcode[10] = static_cast<char>(0xFF);
        shellcode[11] = static_cast<char>(0xE0);
    }

    return shellcode;
}

int calculateTrampolineSize(const ProcessHandle& process, uint64_t address, int minBytes) {
#ifdef Q_OS_WIN
    // Lire suffisamment de bytes pour décoder les instructions
    QByteArray buffer(32, '\0');
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process.rawHandle(), reinterpret_cast<LPCVOID>(address),
                           buffer.data(), 32, &bytesRead)) {
        return minBytes; // Fallback
    }
    buffer.resize(static_cast<int>(bytesRead));

    // Si on a Zydis, décoder les instructions
    // Pour la v1, on utilise le décodeur builtin
    int totalSize = 0;
    int offset = 0;

    while (totalSize < minBytes && offset < buffer.size()) {
        QByteArray instrBytes(buffer.constData() + offset, std::min<int>(15, buffer.size() - offset));
        const auto info = decodeX64InstructionLength(instrBytes);

        if (!info.success || info.length <= 0) {
            // Impossible de décoder, on suppose 1 byte
            totalSize += 1;
            offset += 1;
        } else {
            totalSize += info.length;
            offset += info.length;
        }
    }

    return totalSize;
#else
    (void)process;
    (void)address;
    return minBytes;
#endif
}

HookResult installInlineHook(const ProcessHandle& process,
                              uint64_t targetFunction,
                              uint64_t hookFunction) {
    HookResult result;

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = "Invalid process handle";
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // Déterminer le type de JMP nécessaire
    const int64_t delta = static_cast<int64_t>(hookFunction) - static_cast<int64_t>(targetFunction) - 5;
    const bool useShortJump = (delta >= INT32_MIN && delta <= INT32_MAX);
    const int jumpSize = useShortJump ? 5 : 12;

    // Calculer la taille du trampoline (instructions complètes >= jumpSize)
    const int trampolineCodeSize = calculateTrampolineSize(process, targetFunction, jumpSize);

    // Sauvegarder les bytes originaux
    result.originalBytes.resize(trampolineCodeSize);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(targetFunction),
                           result.originalBytes.data(), trampolineCodeSize, &bytesRead)) {
        result.error = QStringLiteral("ReadProcessMemory failed (error=%1)").arg(GetLastError());
        return result;
    }

    // Allouer le trampoline dans le processus cible
    // Trampoline = bytes originaux + JMP retour vers (targetFunction + trampolineCodeSize)
    const QByteArray returnJump = generateJumpShellcode(0, targetFunction + trampolineCodeSize);
    const SIZE_T trampolineTotalSize = static_cast<SIZE_T>(trampolineCodeSize + returnJump.size());

    LPVOID pTrampoline = VirtualAllocEx(hProcess, nullptr, trampolineTotalSize,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pTrampoline) {
        result.error = QStringLiteral("VirtualAllocEx for trampoline failed (error=%1)").arg(GetLastError());
        return result;
    }

    result.trampolineAddress = reinterpret_cast<uint64_t>(pTrampoline);

    // Écrire les bytes originaux dans le trampoline
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, pTrampoline, result.originalBytes.constData(),
                            static_cast<SIZE_T>(trampolineCodeSize), &bytesWritten)) {
        result.error = "Failed to write original bytes to trampoline";
        VirtualFreeEx(hProcess, pTrampoline, 0, MEM_RELEASE);
        return result;
    }

    // Recalculer le JMP retour avec la vraie adresse du trampoline
    const QByteArray realReturnJump = generateJumpShellcode(
        result.trampolineAddress + trampolineCodeSize,
        targetFunction + trampolineCodeSize);

    if (!WriteProcessMemory(hProcess,
                            reinterpret_cast<LPVOID>(result.trampolineAddress + trampolineCodeSize),
                            realReturnJump.constData(),
                            static_cast<SIZE_T>(realReturnJump.size()), &bytesWritten)) {
        result.error = "Failed to write return jump to trampoline";
        VirtualFreeEx(hProcess, pTrampoline, 0, MEM_RELEASE);
        return result;
    }

    // Maintenant patcher la fonction cible avec le JMP vers le hook
    DWORD oldProtect = 0;
    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(targetFunction), jumpSize,
                     PAGE_EXECUTE_READWRITE, &oldProtect);

    const QByteArray hookJump = generateJumpShellcode(targetFunction, hookFunction);
    if (!WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(targetFunction),
                            hookJump.constData(),
                            static_cast<SIZE_T>(hookJump.size()), &bytesWritten)) {
        result.error = "Failed to write hook jump";
        VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(targetFunction), jumpSize,
                         oldProtect, &oldProtect);
        VirtualFreeEx(hProcess, pTrampoline, 0, MEM_RELEASE);
        return result;
    }

    // Restaurer la protection
    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(targetFunction), jumpSize,
                     oldProtect, &oldProtect);

    // Flush instruction cache
    FlushInstructionCache(hProcess, reinterpret_cast<LPCVOID>(targetFunction), jumpSize);

    result.success = true;
    result.hookFunctionAddress = hookFunction;

    KE_LOG_INFO() << "FunctionHook: installed hook at 0x" << std::hex << targetFunction
                  << " -> 0x" << hookFunction
                  << " trampoline=0x" << result.trampolineAddress
                  << " (jump=" << jumpSize << " bytes, trampoline code=" << trampolineCodeSize << ")";
#else
    (void)process;
    (void)targetFunction;
    (void)hookFunction;
    result.error = "Inline hooks are Windows-only";
#endif

    return result;
}

HookResult removeInlineHook(const ProcessHandle& process,
                             uint64_t targetFunction,
                             const QByteArray& originalBytes) {
    HookResult result;

#ifdef Q_OS_WIN
    if (!process.isValid() || originalBytes.isEmpty()) {
        result.error = "Invalid process or empty original bytes";
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // Restaurer les bytes originaux
    DWORD oldProtect = 0;
    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(targetFunction),
                     static_cast<SIZE_T>(originalBytes.size()),
                     PAGE_EXECUTE_READWRITE, &oldProtect);

    SIZE_T bytesWritten = 0;
    const BOOL ok = WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(targetFunction),
                                       originalBytes.constData(),
                                       static_cast<SIZE_T>(originalBytes.size()), &bytesWritten);

    VirtualProtectEx(hProcess, reinterpret_cast<LPVOID>(targetFunction),
                     static_cast<SIZE_T>(originalBytes.size()), oldProtect, &oldProtect);
    FlushInstructionCache(hProcess, reinterpret_cast<LPCVOID>(targetFunction),
                          static_cast<SIZE_T>(originalBytes.size()));

    result.success = (ok && bytesWritten == static_cast<SIZE_T>(originalBytes.size()));
    if (!result.success) {
        result.error = "Failed to restore original bytes";
    }

    KE_LOG_INFO() << "FunctionHook: removed hook at 0x" << std::hex << targetFunction;
#else
    (void)process;
    (void)targetFunction;
    (void)originalBytes;
    result.error = "Inline hooks are Windows-only";
#endif

    return result;
}

} // namespace killcore