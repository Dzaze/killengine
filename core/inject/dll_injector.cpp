#include "dll_injector.h"

#include "logging/logger.h"
#include "process/process_enumerator.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <QFileInfo>

namespace killcore {

#ifdef Q_OS_WIN
namespace {
/// Racine-cause identifiee le 20/08/2026 (voir docs/STRATEGY_ROOM.md) via un
/// reproducteur Win32 pur, independant de tout code KillEngine : un cycle
/// DebugActiveProcess/DebugActiveProcessStop sur une cible, immediatement
/// suivi de VirtualAllocEx/WriteProcessMemory/CreateRemoteThread sur cette
/// meme cible, declenche de facon intermittente un refus ACCES REFUSE
/// (error=5) — confirme non lie a nos flags d'acces (meme PROCESS_ALL_ACCESS
/// litteral echoue), non lie a un delai, et TOUJOURS present apres
/// desactivation de la protection temps reel Windows Defender classique.
/// Cause la plus probable restante : Microsoft Defender for Endpoint (le
/// service ATP, distinct de la protection temps reel de base, present sur
/// cette machine) — la sequence "attache debugger -> detache -> alloue de la
/// memoire + cree un thread distant dans la meme cible" est un heuristique
/// classique de detection d'injection de code, meme quand l'usage est
/// legitime (debug/instrumentation). Pas quelque chose que KillEngine peut
/// forcer a marcher — message actionnable a la place d'une erreur Win32 nue.
QString accessDeniedHint(DWORD err) {
    if (err != ERROR_ACCESS_DENIED) return {};
    return QStringLiteral(
        " Un antivirus/EDR (ex. Microsoft Defender for Endpoint) bloque probablement cette action : "
        "poser un breakpoint externe puis injecter dans la meme cible juste apres ressemble a une "
        "technique d'injection de code, meme si l'usage ici est legitime. Reessaie, ou ajoute une "
        "exclusion pour KillEngine.exe dans ton antivirus/EDR si le blocage persiste.");
}
} // namespace
#endif

uint64_t getRemoteProcAddress(const QString& moduleName, const QString& functionName) {
#ifdef Q_OS_WIN
    const HMODULE hLocal = GetModuleHandleW(moduleName.toStdWString().c_str());
    if (!hLocal) {
        return 0;
    }

    const FARPROC func = GetProcAddress(hLocal, functionName.toStdString().c_str());
    if (!func) {
        return 0;
    }

    // Sur Windows, kernel32 (et ntdll) sont chargés à la même adresse dans tous les
    // processus d'une même session. On peut donc utiliser l'adresse locale directement.
    return reinterpret_cast<uint64_t>(func);
#else
    (void)moduleName;
    (void)functionName;
    return 0;
#endif
}

InjectionResult injectDll(const ProcessHandle& process, const QString& dllPath) {
    InjectionResult result;

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = "Invalid process handle";
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // 1. Allouer de la mémoire pour le chemin DLL (unicode)
    const std::wstring widePath = dllPath.toStdWString();
    const SIZE_T pathSize = (widePath.size() + 1) * sizeof(wchar_t);

    LPVOID pRemotePath = VirtualAllocEx(hProcess, nullptr, pathSize,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemotePath) {
        const DWORD err = GetLastError();
        result.error = QStringLiteral("VirtualAllocEx failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
        return result;
    }

    // 2. Écrire le chemin DLL
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, pRemotePath, widePath.c_str(), pathSize, &bytesWritten) ||
        bytesWritten != pathSize) {
        const DWORD err = GetLastError();
        result.error = QStringLiteral("WriteProcessMemory failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return result;
    }

    // 3. Trouver LoadLibraryW
    const uint64_t loadLibraryAddr = getRemoteProcAddress(QStringLiteral("kernel32.dll"), QStringLiteral("LoadLibraryW"));
    if (!loadLibraryAddr) {
        result.error = "Cannot find LoadLibraryW address";
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return result;
    }

    // 4. CreateRemoteThread pour appeler LoadLibraryW(pRemotePath)
    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr),
        pRemotePath,
        0,
        nullptr);

    if (!hThread) {
        const DWORD err = GetLastError();
        result.error = QStringLiteral("CreateRemoteThread failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
        return result;
    }

    // 5. Attendre la fin du chargement
    WaitForSingleObject(hThread, 10000); // 10s timeout

    // 6. Récupérer le module chargé. GetExitCodeThread ne retourne qu'un DWORD,
    // donc il tronque le HMODULE dans un processus 64-bit. On énumère les modules
    // après LoadLibrary pour obtenir une base correcte.
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    const QString dllName = QFileInfo(dllPath).fileName();
    const auto modules = ProcessEnumerator::enumerateModules(process.pid());
    for (const auto& module : modules) {
        if (module.name.compare(dllName, Qt::CaseInsensitive) == 0) {
            result.moduleBase = module.baseAddress;
            break;
        }
    }
    if (result.moduleBase == 0) {
        result.moduleBase = static_cast<uint64_t>(exitCode);
    }

    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);

    result.success = (result.moduleBase != 0);
    if (!result.success) {
        result.error = "LoadLibraryW returned NULL in remote process";
    }

    KE_LOG_INFO() << "DllInjector: injected " << dllPath.toStdString() << " into PID " << process.pid()
                  << " moduleBase=0x" << std::hex << result.moduleBase;
#else
    (void)process;
    (void)dllPath;
    result.error = "DLL injection is Windows-only";
#endif

    return result;
}

InjectionResult injectShellcode(const ProcessHandle& process, const QByteArray& shellcode) {
    InjectionResult result;

#ifdef Q_OS_WIN
    if (!process.isValid() || shellcode.isEmpty()) {
        result.error = "Invalid process handle or empty shellcode";
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // 1. Allouer de la mémoire avec droits d'exécution
    LPVOID pRemoteCode = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(shellcode.size()),
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pRemoteCode) {
        result.error = QStringLiteral("VirtualAllocEx failed (error=%1)").arg(GetLastError());
        return result;
    }

    // 2. Écrire le shellcode
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, pRemoteCode, shellcode.constData(),
                            static_cast<SIZE_T>(shellcode.size()), &bytesWritten) ||
        bytesWritten != static_cast<SIZE_T>(shellcode.size())) {
        result.error = QStringLiteral("WriteProcessMemory failed (error=%1)").arg(GetLastError());
        VirtualFreeEx(hProcess, pRemoteCode, 0, MEM_RELEASE);
        return result;
    }

    // 3. Créer un thread qui exécute le shellcode
    HANDLE hThread = CreateRemoteThread(
        hProcess,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(pRemoteCode),
        nullptr, // pas d'argument
        0,
        nullptr);

    if (!hThread) {
        result.error = QStringLiteral("CreateRemoteThread failed (error=%1)").arg(GetLastError());
        VirtualFreeEx(hProcess, pRemoteCode, 0, MEM_RELEASE);
        return result;
    }

    result.success = true;
    result.remoteThreadHandle = reinterpret_cast<uint64_t>(hThread);

    KE_LOG_INFO() << "DllInjector: shellcode injected (" << shellcode.size()
                  << " bytes) into PID " << process.pid();
#else
    (void)process;
    (void)shellcode;
    result.error = "Shellcode injection is Windows-only";
#endif

    return result;
}

} // namespace killcore
