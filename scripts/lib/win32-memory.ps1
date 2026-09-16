<#
Petit helper P/Invoke pour ecrire directement dans la memoire d'un AUTRE
process (pas via KillEngine) -- sert a AM-4 (docs/PHASE_TRACKER.md, 16/09/2026)
pour simuler un changement de valeur independant dans KillEngineTestTarget.exe
pendant le parcours "scan", sans dependre d'une automatisation UI fragile sur
sa fenetre Qt native (cliquer son propre bouton "Damage (-10)" via UI
Automation aurait ajoute une deuxieme technologie d'automatisation pour un
gain nul : le but du test est de prouver que KillEngine detecte un changement
qu'il n'a pas lui-meme provoque, peu importe le mecanisme qui l'a produit).
#>

if (-not ('KillEngineTest.Win32Memory' -as [type])) {
    Add-Type -Namespace KillEngineTest -Name Win32Memory -MemberDefinition @'
        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
        public static extern System.IntPtr OpenProcess(uint processAccess, bool bInheritHandle, int processId);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool WriteProcessMemory(System.IntPtr hProcess, System.IntPtr lpBaseAddress, byte[] lpBuffer, int dwSize, out System.IntPtr lpNumberOfBytesWritten);

        [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(System.IntPtr hObject);
'@
}

function Write-Int32ToExternalProcess {
    param(
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][string]$AddressHex,
        [Parameter(Mandatory = $true)][int]$Value
    )

    $PROCESS_VM_WRITE = 0x0020
    $PROCESS_VM_OPERATION = 0x0008
    $access = $PROCESS_VM_WRITE -bor $PROCESS_VM_OPERATION

    $cleanHex = $AddressHex -replace '^0x', ''
    $addr = [Convert]::ToInt64($cleanHex, 16)

    $handle = [KillEngineTest.Win32Memory]::OpenProcess($access, $false, $ProcessId)
    if ($handle -eq [IntPtr]::Zero) {
        throw "OpenProcess a echoue pour PID $ProcessId (erreur Win32 $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
    }
    try {
        $bytes = [BitConverter]::GetBytes([int32]$Value)
        $written = [IntPtr]::Zero
        $ok = [KillEngineTest.Win32Memory]::WriteProcessMemory($handle, [IntPtr]$addr, $bytes, $bytes.Length, [ref]$written)
        if (-not $ok -or $written.ToInt64() -ne $bytes.Length) {
            throw "WriteProcessMemory a echoue pour 0x$cleanHex (erreur Win32 $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        }
    } finally {
        [KillEngineTest.Win32Memory]::CloseHandle($handle) | Out-Null
    }
}
