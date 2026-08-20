<#
Build KillEngineClrInspector (tools/clr_inspector/KillEngineClrInspector), le
helper .NET ClrMD du candidat 8 de docs/POWER_UP_ROADMAP.md. Meme esprit que
scripts/build-clr-test-target.ps1 : projet .NET separe, pas construit par
CMake/scripts/build.ps1, script de build dedie.

Usage:
  .\scripts\build-clr-inspector.ps1
  .\scripts\build-clr-inspector.ps1 -Configuration Debug
  .\scripts\build-clr-inspector.ps1 -Test
  .\scripts\build-clr-inspector.ps1 -Run
#>
param(
    [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'

$dotnetCmd = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetCmd) {
    Write-Error "SDK .NET introuvable sur le PATH (commande 'dotnet' absente). Installer le SDK .NET 8+ pour construire KillEngineClrInspector."
    exit 1
}

$projectPath = Join-Path $PSScriptRoot '..\tools\clr_inspector\KillEngineClrInspector\KillEngineClrInspector.csproj'
if (-not (Test-Path $projectPath)) {
    Write-Error "Projet introuvable : $projectPath"
    exit 1
}

Write-Output "Build KillEngineClrInspector ($Configuration)..."
& dotnet build $projectPath -c $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build KillEngineClrInspector echoue (code $LASTEXITCODE)."
    exit $LASTEXITCODE
}

$outputDll = Join-Path $PSScriptRoot "..\tools\clr_inspector\KillEngineClrInspector\bin\$Configuration\net8.0\KillEngineClrInspector.dll"
Write-Output "Build OK -> $outputDll"

if ($Test) {
    # Construit aussi KillEngineClrTestTarget : les auto-tests bout-en-bout
    # lancent les deux processus reels, voir docs/KILLENGINE_CLR_INSPECTOR_SPEC.md.
    & "$PSScriptRoot\build-clr-test-target.ps1" -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $testProjectPath = Join-Path $PSScriptRoot '..\tools\clr_inspector\KillEngineClrInspector.Tests\KillEngineClrInspector.Tests.csproj'
    Write-Output "Run auto-tests bout-en-bout ($Configuration)..."
    & dotnet test $testProjectPath -c $Configuration
    exit $LASTEXITCODE
}

if ($Run) {
    Write-Output "Lancement de KillEngineClrInspector (Ctrl+C pour arreter, ou methode pipe 'shutdown')..."
    & dotnet $outputDll
}
