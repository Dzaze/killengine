#include "dll_injector.h"

#include "localization/localization.h"
#include "logging/logger.h"
#include "process/process_enumerator.h"

#ifdef Q_OS_WIN
#include <aclapi.h>
#include <windows.h>
#include <winternl.h>
#endif

#include <QDir>
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
    return KE_TXT(
        " Un antivirus/EDR (ex. Microsoft Defender for Endpoint) bloque probablement cette action : "
        "poser un breakpoint externe puis injecter dans la même cible juste après ressemble à une "
        "technique d'injection de code, même si l'usage ici est légitime. Réessaie, ou ajoute une "
        "exclusion pour KillEngine.exe dans ton antivirus/EDR si le blocage persiste.",
        " An antivirus/EDR (e.g. Microsoft Defender for Endpoint) is likely blocking this action: "
        "setting an external breakpoint and then injecting into the same target right after looks like a "
        "code injection technique, even though the usage here is legitimate. Try again, or add an "
        "exclusion for KillEngine.exe in your antivirus/EDR if the block persists.");
}

bool prepareAppContainerReadableDllCopy(const QString& dllPath, uint32_t targetPid, QString* copiedPath, QString* error) {
    const QFileInfo info(dllPath);
    const QString targetPath = QDir(info.absolutePath()).filePath(
        QStringLiteral("injected_%1_%2_%3").arg(targetPid).arg(GetTickCount64()).arg(info.fileName()));

    const std::wstring source = QDir::toNativeSeparators(dllPath).toStdWString();
    const std::wstring target = QDir::toNativeSeparators(targetPath).toStdWString();
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        if (error) *error = KE_TXT("CopyFileW vers la copie AppContainer a échoué (erreur=%1).", "CopyFileW to the AppContainer copy failed (error=%1).").arg(GetLastError());
        return false;
    }

    uint8_t sidData[SECURITY_MAX_SID_SIZE];
    PSID packageSid = sidData;
    DWORD sidSize = sizeof(sidData);
    if (!CreateWellKnownSid(WELL_KNOWN_SID_TYPE::WinBuiltinAnyPackageSid, nullptr, packageSid, &sidSize)) {
        if (error) *error = KE_TXT("CreateWellKnownSid (ALL APPLICATION PACKAGES) a échoué (erreur=%1).", "CreateWellKnownSid (ALL APPLICATION PACKAGES) failed (error=%1).").arg(GetLastError());
        return false;
    }

    PACL oldAcl = nullptr;
    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(target.c_str(), SE_OBJECT_TYPE::SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, &oldAcl, nullptr, &securityDescriptor);
    if (status != ERROR_SUCCESS) {
        if (error) *error = KE_TXT("GetNamedSecurityInfoW sur la copie AppContainer a échoué (erreur=%1).", "GetNamedSecurityInfoW on the AppContainer copy failed (error=%1).").arg(status);
        return false;
    }

    PACL newAcl = nullptr;
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = ACCESS_MODE::SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_FORM::TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_TYPE::TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWCH>(packageSid);

    status = SetEntriesInAclW(1, &access, oldAcl, &newAcl);
    if (status != ERROR_SUCCESS) {
        LocalFree(securityDescriptor);
        if (error) *error = KE_TXT("SetEntriesInAclW pour ALL APPLICATION PACKAGES a échoué (erreur=%1).", "SetEntriesInAclW for ALL APPLICATION PACKAGES failed (error=%1).").arg(status);
        return false;
    }

    status = SetNamedSecurityInfoW(const_cast<LPWSTR>(target.c_str()), SE_OBJECT_TYPE::SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr, newAcl, nullptr);
    LocalFree(newAcl);
    LocalFree(securityDescriptor);
    if (status != ERROR_SUCCESS) {
        if (error) *error = KE_TXT("SetNamedSecurityInfoW sur la copie AppContainer a échoué (erreur=%1).", "SetNamedSecurityInfoW on the AppContainer copy failed (error=%1).").arg(status);
        return false;
    }

    if (copiedPath) *copiedPath = targetPath;
    return true;
}
/// Typedef pour NtCreateThreadEx (fonction non documentée de ntdll.dll).
/// Utilisée pour créer un thread distant sans passer par CreateRemoteThread,
/// ce qui évite la détection d'injection par les anti-cheat (SC2, etc.).
typedef NTSTATUS(NTAPI* NtCreateThreadEx_t)(
    PHANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    LPVOID ObjectAttributes,
    HANDLE ProcessHandle,
    LPTHREAD_START_ROUTINE StartRoutine,
    LPVOID Argument,
    ULONG CreateFlags,
    SIZE_T ZeroBits,
    SIZE_T StackSize,
    SIZE_T MaximumStackSize,
    LPVOID AttributeList);

/// Crée un thread distant via NtCreateThreadEx au lieu de CreateRemoteThread.
/// Retourne le handle du thread ou NULL en cas d'échec.
HANDLE createRemoteThreadViaNt(HANDLE hProcess, LPTHREAD_START_ROUTINE startRoutine, LPVOID argument) {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) {
        return nullptr;
    }

    auto pNtCreateThreadEx = reinterpret_cast<NtCreateThreadEx_t>(
        GetProcAddress(hNtdll, "NtCreateThreadEx"));
    if (!pNtCreateThreadEx) {
        return nullptr;
    }

    HANDLE hThread = nullptr;
    NTSTATUS status = pNtCreateThreadEx(
        &hThread,
        THREAD_ALL_ACCESS,
        nullptr,
        hProcess,
        startRoutine,
        argument,
        0,      // CreateFlags (0 = pas de flags spéciaux)
        0,      // ZeroBits
        0,      // StackSize (0 = taille par défaut)
        0,      // MaximumStackSize
        nullptr // AttributeList
    );

    if (status != 0 || !hThread) {
        KE_LOG_DEBUG() << "NtCreateThreadEx failed (status=0x" << std::hex << status << ")";
        return nullptr;
    }

    return hThread;
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
    return injectDll(process, dllPath, {});
}

InjectionResult injectDll(const ProcessHandle& process, const QString& dllPath, const InjectDllOptions& options) {
    InjectionResult result;

#ifdef Q_OS_WIN
    if (!process.isValid()) {
        result.error = KE_TXT("Handle de processus invalide.", "Invalid process handle.");
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // Indicateur stable pour la decision de retry ci-dessous -- independant du
    // texte affiche (traduit via KE_TXT) pour eviter le meme piege que celui
    // corrige dans profile_manager.cpp/auto_resolver.cpp
    // (docs/BACKEND_UI_LOCALIZATION_ROADMAP.md, B3) : comparer sur le texte
    // localise casserait silencieusement ce fallback pour un utilisateur EN.
    bool loadLibraryReturnedNull = false;

    auto loadDllPath = [&](const QString& path) {
        InjectionResult attempt;
        loadLibraryReturnedNull = false;

        // 1. Allouer de la mémoire pour le chemin DLL (unicode)
        const std::wstring widePath = QDir::toNativeSeparators(path).toStdWString();
        const SIZE_T pathSize = (widePath.size() + 1) * sizeof(wchar_t);

        LPVOID pRemotePath = VirtualAllocEx(hProcess, nullptr, pathSize,
                                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!pRemotePath) {
            const DWORD err = GetLastError();
            attempt.error = KE_TXT("Échec de VirtualAllocEx (erreur=%1).%2", "VirtualAllocEx failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
            return attempt;
        }

        // 2. Écrire le chemin DLL
        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(hProcess, pRemotePath, widePath.c_str(), pathSize, &bytesWritten) ||
            bytesWritten != pathSize) {
            const DWORD err = GetLastError();
            attempt.error = KE_TXT("Échec de WriteProcessMemory (erreur=%1).%2", "WriteProcessMemory failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
            VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
            return attempt;
        }

        // 3. Trouver LoadLibraryW
        const uint64_t loadLibraryAddr = getRemoteProcAddress(QStringLiteral("kernel32.dll"), QStringLiteral("LoadLibraryW"));
        if (!loadLibraryAddr) {
            attempt.error = KE_TXT("Adresse de LoadLibraryW introuvable.", "Cannot find LoadLibraryW address.");
            VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
            return attempt;
        }

        // 4. Créer un thread distant pour appeler LoadLibraryW(pRemotePath)
        //    Si useNtCreateThreadEx est activé, utiliser NtCreateThreadEx au lieu de
        //    CreateRemoteThread pour éviter la détection d'injection par les anti-cheat.
        HANDLE hThread = nullptr;
        if (options.useNtCreateThreadEx) {
            hThread = createRemoteThreadViaNt(
                hProcess,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr),
                pRemotePath);
            if (!hThread) {
                // Fallback sur CreateRemoteThread si NtCreateThreadEx échoue
                KE_LOG_WARN() << "DllInjector: NtCreateThreadEx failed, falling back to CreateRemoteThread";
                hThread = CreateRemoteThread(
                    hProcess,
                    nullptr,
                    0,
                    reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr),
                    pRemotePath,
                    0,
                    nullptr);
            }
        } else {
            hThread = CreateRemoteThread(
                hProcess,
                nullptr,
                0,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibraryAddr),
                pRemotePath,
                0,
                nullptr);
        }

        if (!hThread) {
            const DWORD err = GetLastError();
            attempt.error = KE_TXT("Échec de CreateRemoteThread (erreur=%1).%2", "CreateRemoteThread failed (error=%1).%2").arg(err).arg(accessDeniedHint(err));
            VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);
            return attempt;
        }

        // 5. Attendre la fin du chargement
        WaitForSingleObject(hThread, 10000); // 10s timeout

        // 6. Récupérer le module chargé. GetExitCodeThread ne retourne qu'un DWORD,
        // donc il tronque le HMODULE dans un processus 64-bit. On énumère les modules
        // après LoadLibrary pour obtenir une base correcte.
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        const QString dllName = QFileInfo(path).fileName();
        const auto modules = ProcessEnumerator::enumerateModules(process.pid());
        for (const auto& module : modules) {
            if (module.name.compare(dllName, Qt::CaseInsensitive) == 0) {
                attempt.moduleBase = module.baseAddress;
                break;
            }
        }
        if (attempt.moduleBase == 0) {
            attempt.moduleBase = static_cast<uint64_t>(exitCode);
        }

        CloseHandle(hThread);
        VirtualFreeEx(hProcess, pRemotePath, 0, MEM_RELEASE);

        attempt.success = (attempt.moduleBase != 0);
        if (!attempt.success) {
            attempt.error = KE_TXT("LoadLibraryW a retourné NULL dans le processus distant.", "LoadLibraryW returned NULL in the remote process.");
            loadLibraryReturnedNull = true;
        }
        return attempt;
    };

    QString loadedPath = dllPath;
    if (options.forceUniqueLoad) {
        QString copiedPath;
        QString copyError;
        if (!prepareAppContainerReadableDllCopy(dllPath, process.pid(), &copiedPath, &copyError)) {
            result.error = KE_TXT("Copie DLL unique impossible : %1", "Unable to make a unique DLL copy: %1").arg(copyError);
            KE_LOG_WARN() << "DllInjector: unique copy failed for " << dllPath.toStdString()
                          << " into PID " << process.pid() << ": " << result.error.toStdString();
            return result;
        }
        loadedPath = copiedPath;
        KE_LOG_INFO() << "DllInjector: forcing unique DLL load via " << copiedPath.toStdString()
                      << " into PID " << process.pid();
    }

    result = loadDllPath(loadedPath);
    if (!options.forceUniqueLoad
        && !result.success
        && loadLibraryReturnedNull) {
        const QString firstError = result.error;
        QString copiedPath;
        QString copyError;
        if (prepareAppContainerReadableDllCopy(dllPath, process.pid(), &copiedPath, &copyError)) {
            KE_LOG_WARN() << "DllInjector: LoadLibraryW returned NULL for " << loadedPath.toStdString()
                          << ", retrying with AppContainer-readable copy " << copiedPath.toStdString();
            loadedPath = copiedPath;
            result = loadDllPath(copiedPath);
            if (!result.success) {
                result.error = KE_TXT("%1 ; le fallback AppContainer via %2 a échoué : %3",
                                   "%1; the AppContainer fallback via %2 failed: %3")
                                   .arg(firstError, copiedPath, result.error);
            }
        } else {
            result.error = KE_TXT("%1 ; fallback AppContainer impossible : %2", "%1; AppContainer fallback not possible: %2")
                               .arg(firstError, copyError);
        }
    }


    if (result.success) {
        KE_LOG_INFO() << "DllInjector: injected " << loadedPath.toStdString() << " into PID " << process.pid()
                      << " moduleBase=0x" << std::hex << result.moduleBase;
    } else {
        KE_LOG_WARN() << "DllInjector: injection failed for " << loadedPath.toStdString() << " into PID "
                      << process.pid() << ": " << result.error.toStdString();
    }
#else
    (void)process;
    (void)dllPath;
    result.error = KE_TXT("L'injection de DLL est réservée à Windows.", "DLL injection is Windows-only.");
#endif

    return result;
}

InjectionResult injectShellcode(const ProcessHandle& process, const QByteArray& shellcode) {
    InjectionResult result;

#ifdef Q_OS_WIN
    if (!process.isValid() || shellcode.isEmpty()) {
        result.error = KE_TXT("Handle de processus invalide ou shellcode vide.", "Invalid process handle or empty shellcode.");
        return result;
    }

    const HANDLE hProcess = process.rawHandle();

    // 1. Allouer de la mémoire avec droits d'exécution
    LPVOID pRemoteCode = VirtualAllocEx(hProcess, nullptr, static_cast<SIZE_T>(shellcode.size()),
                                         MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pRemoteCode) {
        result.error = KE_TXT("Échec de VirtualAllocEx (erreur=%1)", "VirtualAllocEx failed (error=%1)").arg(GetLastError());
        return result;
    }

    // 2. Écrire le shellcode
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, pRemoteCode, shellcode.constData(),
                            static_cast<SIZE_T>(shellcode.size()), &bytesWritten) ||
        bytesWritten != static_cast<SIZE_T>(shellcode.size())) {
        result.error = KE_TXT("Échec de WriteProcessMemory (erreur=%1)", "WriteProcessMemory failed (error=%1)").arg(GetLastError());
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
        result.error = KE_TXT("Échec de CreateRemoteThread (erreur=%1)", "CreateRemoteThread failed (error=%1)").arg(GetLastError());
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
    result.error = KE_TXT("L'injection de shellcode est réservée à Windows.", "Shellcode injection is Windows-only.");
#endif

    return result;
}

} // namespace killcore
