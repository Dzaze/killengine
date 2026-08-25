<#
Relais de patch de code via un process PowerShell distinct de KillEngine.exe.

Contexte : sur certaines machines (EDR d'entreprise, voir docs/POWER_UP_ROADMAP.md section O),
VirtualProtectEx(PAGE_EXECUTE_READWRITE) cross-process est refuse (ERROR_ACCESS_DENIED)
specifiquement pour KillEngine.exe (binaire maison non signe) alors que le meme appel depuis
powershell.exe (signe, systeme) reussit instantanement sur la meme cible/adresse -- verifie
empiriquement le 25-26/08/2026. Ce script delegue uniquement l'operation sensible (changement de
protection + ecriture) a powershell.exe ; KillEngine reste responsable de tout le reste (scan,
verification, decision d'appliquer un patch).

Usage : appele par KillEngine (QProcess) avec -ParamsFile pointant vers un JSON
  { "pid": 1234, "addressHex": "7ff7bf683b8e", "patchBytesHex": "90 90 90" }
Ecrit un JSON de resultat sur stdout :
  { "success": bool, "bytesWritten": int, "originalBytesHex": "...", "error": "..." }
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$ParamsFile
)

$ErrorActionPreference = 'Stop'

function Write-JsonResult {
    param($obj)
    $obj | ConvertTo-Json -Compress
}

try {
    $params = Get-Content -Path $ParamsFile -Raw | ConvertFrom-Json
} catch {
    Write-JsonResult @{ success = $false; bytesWritten = 0; originalBytesHex = ""; error = "Parametres invalides : $_" }
    exit 0
}

$targetPid = [int]$params.pid
$address = [Convert]::ToUInt64($params.addressHex, 16)
$patchBytes = [byte[]]($params.patchBytesHex -split '\s+' | Where-Object { $_ -ne '' } | ForEach-Object { [Convert]::ToByte($_, 16) })

if ($patchBytes.Length -eq 0) {
    Write-JsonResult @{ success = $false; bytesWritten = 0; originalBytesHex = ""; error = "Aucun byte de patch." }
    exit 0
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KePatchRelay {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool VirtualProtectEx(IntPtr hProcess, IntPtr lpAddress, UIntPtr dwSize, uint flNewProtect, out uint lpflOldProtect);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, int dwSize, out int lpNumberOfBytesRead);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, int nSize, out int lpNumberOfBytesWritten);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool FlushInstructionCache(IntPtr hProcess, IntPtr lpBaseAddress, UIntPtr dwSize);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr hObject);
}
"@

$PROCESS_QUERY_INFORMATION = 0x0400
$PROCESS_VM_OPERATION = 0x0008
$PROCESS_VM_READ = 0x0010
$PROCESS_VM_WRITE = 0x0020
$ACCESS = $PROCESS_QUERY_INFORMATION -bor $PROCESS_VM_OPERATION -bor $PROCESS_VM_READ -bor $PROCESS_VM_WRITE
$PAGE_EXECUTE_READWRITE = 0x40

$result = @{ success = $false; bytesWritten = 0; originalBytesHex = ""; error = "" }

$h = [KePatchRelay]::OpenProcess($ACCESS, $false, $targetPid)
if ($h -eq [IntPtr]::Zero) {
    $result.error = "OpenProcess a echoue (error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
    Write-JsonResult $result
    exit 0
}

try {
    $addr = [IntPtr]::new([int64]$address)
    $size = $patchBytes.Length

    $original = New-Object byte[] $size
    $readCount = 0
    if (-not [KePatchRelay]::ReadProcessMemory($h, $addr, $original, $size, [ref]$readCount)) {
        $result.error = "ReadProcessMemory (bytes originaux) a echoue (error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        Write-JsonResult $result
        exit 0
    }
    $result.originalBytesHex = ($original | ForEach-Object { $_.ToString('x2') }) -join ' '

    $oldProtect = 0
    if (-not [KePatchRelay]::VirtualProtectEx($h, $addr, [UIntPtr]::new($size), $PAGE_EXECUTE_READWRITE, [ref]$oldProtect)) {
        $result.error = "VirtualProtectEx(RWX) a echoue (error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
        Write-JsonResult $result
        exit 0
    }

    $writeCount = 0
    $writeOk = [KePatchRelay]::WriteProcessMemory($h, $addr, $patchBytes, $size, [ref]$writeCount)
    $writeErr = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

    $restoreProtect = 0
    [KePatchRelay]::VirtualProtectEx($h, $addr, [UIntPtr]::new($size), $oldProtect, [ref]$restoreProtect) | Out-Null
    [KePatchRelay]::FlushInstructionCache($h, $addr, [UIntPtr]::new($size)) | Out-Null

    if (-not $writeOk -or $writeCount -ne $size) {
        $result.error = "WriteProcessMemory a echoue apres VirtualProtectEx reussi (error=$writeErr)."
        Write-JsonResult $result
        exit 0
    }

    $result.success = $true
    $result.bytesWritten = $writeCount
} finally {
    [KePatchRelay]::CloseHandle($h) | Out-Null
}

Write-JsonResult $result
