using System.Runtime.InteropServices;

namespace KillEngine.ClrInspector.Tests;

/// <summary>
/// Reimplemente cote test, en P/Invoke Win32 direct, EXACTEMENT le meme
/// shellcode et la meme sequence d'injection que
/// apps/desktop/application_controller.cpp::callClrInstanceMethod (chantier
/// natif) : sub rsp,0x28 / mov rcx,this / mov rdx,valeur / mov rax,nativeCode
/// / call rax / add rsp,0x28 / xor eax,eax / ret, VirtualAllocEx +
/// WriteProcessMemory + CreateRemoteThread + WaitForSingleObject(timeout).
///
/// Pourquoi dupliquer ici plutot que d'appeler ApplicationController
/// directement : ApplicationController est un objet Qt/C++ heberge par
/// KillEngine.exe (application GUI), inaccessible depuis une suite xUnit
/// .NET sans un harness d'automation Qt qui n'existe pas dans ce depot. Cette
/// classe sert donc de preuve independante que le MECANISME (adresse native
/// resolue par ClrSession.ResolveInstanceMethodAddress + shellcode fixe +
/// injectShellcode-equivalent) appelle reellement le setter C# avec sa
/// logique metier -- pas un raccourci qui contournerait la question. Le test
/// qui l'utilise (EndToEndTests.WritableSetter_*) verifie l'effet de bord
/// via le pipe de controle de la cible (oracle independant de ClrMD), meme
/// discipline que les autres tests d'ecriture de ce fichier.
/// </summary>
internal static class NativeSetterInvoker
{
    private const uint ProcessAllAccess = 0x001F0FFF;
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint MemRelease = 0x8000;
    private const uint PageExecuteReadWrite = 0x40;

    /// <summary>
    /// Construit le shellcode x64 exact specifie pour l'appel d'un setter
    /// d'instance : "this" en RCX, valeur immediate optionnelle en RDX,
    /// call sur l'adresse native resolue. <paramref name="paramImmediate"/>
    /// est ignore si <paramref name="hasParam"/> est faux (setter 0-arg).
    /// </summary>
    public static byte[] BuildCallInstanceMethodShellcode(ulong objectAddress, bool hasParam, ulong paramImmediate, ulong nativeCodeAddress)
    {
        using var ms = new MemoryStream(40);
        void Bytes(params byte[] b) => ms.Write(b, 0, b.Length);
        void Imm64(ulong v) => ms.Write(BitConverter.GetBytes(v), 0, 8);

        Bytes(0x48, 0x83, 0xEC, 0x28);       // sub rsp, 0x28
        Bytes(0x48, 0xB9); Imm64(objectAddress); // mov rcx, <objectAddress>
        if (hasParam)
        {
            Bytes(0x48, 0xBA); Imm64(paramImmediate); // mov rdx, <valeur>
        }
        Bytes(0x48, 0xB8); Imm64(nativeCodeAddress);  // mov rax, <nativeCodeAddress>
        Bytes(0xFF, 0xD0);                    // call rax
        Bytes(0x48, 0x83, 0xC4, 0x28);        // add rsp, 0x28
        Bytes(0x33, 0xC0);                    // xor eax, eax
        Bytes(0xC3);                          // ret
        return ms.ToArray();
    }

    /// <summary>
    /// Injecte et execute le shellcode dans le process cible, attend sa fin
    /// (timeout borne), puis nettoie (VirtualFreeEx, handles fermes). Ne
    /// leve pas si le thread distant timeout -- retourne false dans ce cas,
    /// au lieu d'attendre indefiniment (meme discipline que
    /// ApplicationController::callClrInstanceMethod).
    /// </summary>
    public static bool InvokeInstanceMethod(int pid, ulong objectAddress, bool hasParam, ulong paramImmediate, ulong nativeCodeAddress, int timeoutMs = 3000)
    {
        byte[] shellcode = BuildCallInstanceMethodShellcode(objectAddress, hasParam, paramImmediate, nativeCodeAddress);

        IntPtr hProcess = OpenProcess(ProcessAllAccess, false, pid);
        if (hProcess == IntPtr.Zero)
        {
            throw new InvalidOperationException($"OpenProcess(pid={pid}) a echoue (error={Marshal.GetLastWin32Error()}).");
        }

        IntPtr remoteMem = IntPtr.Zero;
        IntPtr hThread = IntPtr.Zero;
        try
        {
            remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)shellcode.Length, MemCommit | MemReserve, PageExecuteReadWrite);
            if (remoteMem == IntPtr.Zero)
            {
                throw new InvalidOperationException($"VirtualAllocEx a echoue (error={Marshal.GetLastWin32Error()}).");
            }

            if (!WriteProcessMemory(hProcess, remoteMem, shellcode, shellcode.Length, out nint written) || written != shellcode.Length)
            {
                throw new InvalidOperationException($"WriteProcessMemory a echoue (error={Marshal.GetLastWin32Error()}).");
            }

            hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, remoteMem, IntPtr.Zero, 0, out _);
            if (hThread == IntPtr.Zero)
            {
                throw new InvalidOperationException($"CreateRemoteThread a echoue (error={Marshal.GetLastWin32Error()}).");
            }

            uint waitResult = WaitForSingleObject(hThread, (uint)timeoutMs);
            return waitResult == 0; // WAIT_OBJECT_0
        }
        finally
        {
            if (hThread != IntPtr.Zero) CloseHandle(hThread);
            if (remoteMem != IntPtr.Zero) VirtualFreeEx(hProcess, remoteMem, 0, MemRelease);
            CloseHandle(hProcess);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr address, uint size, uint allocationType, uint protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr address, byte[] buffer, int size, out nint bytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr threadAttributes, uint stackSize, IntPtr startAddress, IntPtr parameter, uint creationFlags, out nint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint millisecondsTimeout);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr address, uint size, uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
