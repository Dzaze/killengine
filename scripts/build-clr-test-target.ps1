<#
Build KillEngineClrTestTarget (tests/clr_targets/KillEngineClrTestTarget), la
cible de test CLR dediee au futur candidat 8 de docs/POWER_UP_ROADMAP.md
(ClrMD/SOS). Projet .NET separe, pas construit par CMake/scripts/build.ps1 :
ce script est le point d'entree dedie, dans le meme esprit que
scripts/package-windows.ps1 ou scripts/benchmark-performance.ps1 pour les
autres sous-chantiers qui ne font pas partie du build principal.

Prerequis : SDK .NET (verifie sur cette machine : 9.0.202, cible net8.0).
Si `dotnet` est absent du PATH, ce script s'arrete avec un message clair
plutot que d'echouer silencieusement -- le SDK .NET n'est pas suppose etre
une dependance obligatoire pour construire KillEngine.exe lui-meme.

Usage:
  .\scripts\build-clr-test-target.ps1
  .\scripts\build-clr-test-target.ps1 -Configuration Debug
  .\scripts\build-clr-test-target.ps1 -Run
#>
param(
    [string]$Configuration = 'Release',
    [switch]$Run
)

$ErrorActionPreference = 'Stop'

$dotnetCmd = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetCmd) {
    Write-Error "SDK .NET introuvable sur le PATH (commande 'dotnet' absente). Installer le SDK .NET 8+ pour construire KillEngineClrTestTarget."
    exit 1
}

$projectPath = Join-Path $PSScriptRoot '..\tests\clr_targets\KillEngineClrTestTarget\KillEngineClrTestTarget.csproj'
if (-not (Test-Path $projectPath)) {
    Write-Error "Projet introuvable : $projectPath"
    exit 1
}

Write-Output "Build KillEngineClrTestTarget ($Configuration)..."
& dotnet build $projectPath -c $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build KillEngineClrTestTarget echoue (code $LASTEXITCODE)."
    exit $LASTEXITCODE
}

$outputDll = Join-Path $PSScriptRoot "..\tests\clr_targets\KillEngineClrTestTarget\bin\$Configuration\net8.0\KillEngineClrTestTarget.dll"
Write-Output "Build OK -> $outputDll"

if ($Run) {
    Write-Output "Lancement de KillEngineClrTestTarget (Ctrl+C pour arreter, ou methode pipe 'shutdown')..."
    & dotnet $outputDll
}
