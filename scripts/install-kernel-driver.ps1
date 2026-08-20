param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    throw "Ce script doit être lancé dans un terminal administrateur. Il installe ou supprime un service driver kernel visible."
}

$serviceName = 'KillEngineKernel'
$repoRoot = Split-Path -Parent $PSScriptRoot
$driver = Get-ChildItem (Join-Path $repoRoot 'tools\kernel_driver') -Recurse -Filter KillEngineKernel.sys -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*\$Configuration\*" } |
    Select-Object -First 1 -ExpandProperty FullName

if ($Uninstall) {
    sc.exe stop $serviceName | Out-Host
    sc.exe delete $serviceName | Out-Host
    Write-Host "KillEngineKernel désinstallé si le service existait."
    exit 0
}

if (-not $driver) {
    throw "KillEngineKernel.sys introuvable. Lance d'abord .\scripts\build-kernel-driver.ps1 -Configuration $Configuration."
}

sc.exe query $serviceName *> $null
if ($LASTEXITCODE -eq 0) {
    sc.exe stop $serviceName | Out-Host
    sc.exe delete $serviceName | Out-Host
}

sc.exe create $serviceName type= kernel start= demand binPath= $driver DisplayName= "KillEngine Kernel Health Probe" | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Création du service driver échouée."
}

sc.exe start $serviceName | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "Démarrage du driver échoué. Vérifie la signature du driver et l'état test-signing Windows."
}

Write-Host "KillEngineKernel démarré. KillEngine doit maintenant voir \\.\KillEngineKernel via KernelDriverBridge::probe()."
