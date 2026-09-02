# test-sc2-stealth.ps1
# Script de test pour le mode stealth SC2 avec KillEngine
# Usage: .\scripts\test-sc2-stealth.ps1 [-Pid <pid>] [-InitialMinerals <n>] [-DesiredMinerals <n>]

param(
    [int]$TargetPid = 0,
    [int]$InitialMinerals = 50,
    [int]$DesiredMinerals = 9999,
    [switch]$NoBuild,
    [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Write-Host "=== Test Mode Stealth SC2 ===" -ForegroundColor Cyan
Write-Host ""

# Vérifier que KillEngine.exe existe
$killenginePath = Join-Path $root "build\bin\KillEngine.exe"
if (-not (Test-Path $killenginePath)) {
    Write-Error "KillEngine.exe non trouvé: $killenginePath"
    Write-Host "Lancez d'abord: .\scripts\build.ps1" -ForegroundColor Yellow
    exit 1
}

# Vérifier que lua.exe est disponible
$luaPath = "lua.exe"
$luaFullPath = Get-Command lua.exe -ErrorAction SilentlyContinue
if (-not $luaFullPath) {
    # Essayer lua54.exe ou luajit.exe
    $luaPath = "lua54.exe"
    $luaFullPath = Get-Command lua54.exe -ErrorAction SilentlyContinue
    if (-not $luaFullPath) {
        $luaPath = "luajit.exe"
        $luaFullPath = Get-Command luajit.exe -ErrorAction SilentlyContinue
    }
}

if (-not $luaFullPath) {
    Write-Error "Lua non trouvé. Installez Lua ou ajoutez-le au PATH"
    exit 1
}

Write-Host "Lua trouvé: $($luaFullPath.Source)" -ForegroundColor Green

# Si pas de PID fourni, chercher SC2
if ($TargetPid -eq 0) {
    Write-Host "Recherche de StarCraft 2..." -ForegroundColor Yellow
    $sc2Process = Get-Process "SC2" -ErrorAction SilentlyContinue
    if (-not $sc2Process) {
        $sc2Process = Get-Process | Where-Object { $_.ProcessName -like "*SC2*" -or $_.ProcessName -like "*StarCraft*" } | Select-Object -First 1
    }
    
    if ($sc2Process) {
        $TargetPid = $sc2Process.Id
        Write-Host "SC2 trouvé: PID $TargetPid" -ForegroundColor Green
    } else {
        Write-Host "SC2 non trouvé. Options:" -ForegroundColor Yellow
        Write-Host "  1. Lancez SC2 d'abord"
        Write-Host "  2. Utilisez -Pid <pid> pour spécifier un autre processus"
        Write-Host "  3. Utilisez KillEngineTestTarget.exe pour tester"
        
        # Proposer de lancer le test target
        $testTarget = Join-Path $root "build\bin\KillEngineTestTarget.exe"
        if (Test-Path $testTarget) {
            Write-Host ""
            Write-Host "Lancement de KillEngineTestTarget.exe pour test..." -ForegroundColor Cyan
            Start-Process $testTarget -WindowStyle Normal
            Start-Sleep -Seconds 2
            $testProcess = Get-Process "KillEngineTestTarget" -ErrorAction SilentlyContinue
            if ($testProcess) {
                $TargetPid = $testProcess.Id
                Write-Host "Test target lancé: PID $TargetPid" -ForegroundColor Green
            }
        }
    }
}

if ($TargetPid -eq 0) {
    Write-Error "Aucun processus cible trouvé. Spécifiez -TargetPid <pid>"
    exit 1
}

Write-Host ""
Write-Host "Configuration:" -ForegroundColor Cyan
Write-Host "  PID cible: $TargetPid"
Write-Host "  Minéraux initiaux: $InitialMinerals"
Write-Host "  Minéraux désirés: $DesiredMinerals"
Write-Host ""

# Lancer KillEngine avec le pipe d'automatisation activé
Write-Host "Lancement de KillEngine avec pipe d'automatisation..." -ForegroundColor Cyan

$env:KILLENGINE_AUTOMATION_PIPE = "1"
$env:KILLENGINE_ROOT = $root

$killengineProcess = Start-Process -FilePath $killenginePath -PassThru -WindowStyle Normal

Write-Host "KillEngine lancé (PID: $($killengineProcess.Id))" -ForegroundColor Green
Write-Host "Attente du démarrage du pipe..." -ForegroundColor Yellow

# Attendre que le pipe soit prêt (max 30s)
$pipeReady = $false
$attempts = 0
$maxAttempts = 30

while (-not $pipeReady -and $attempts -lt $maxAttempts) {
    Start-Sleep -Seconds 1
    $attempts++
    
    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "KillEngineAutomationPipe", [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(1000)
        $pipe.Close()
        $pipeReady = $true
        Write-Host "Pipe prêt!" -ForegroundColor Green
    } catch {
        Write-Host "  Attente... ($attempts/$maxAttempts)" -ForegroundColor Gray
    }
}

if (-not $pipeReady) {
    Write-Error "Le pipe n'est pas devenu prêt après $maxAttempts secondes"
    Stop-Process -Id $killengineProcess.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

# Lancer le script Lua
Write-Host ""
Write-Host "Exécution du script Lua..." -ForegroundColor Cyan

$luaScript = Join-Path $root "scripts\lua_examples\sc2_minerals_stealth.lua"
$luaArgs = @($TargetPid, $InitialMinerals, $DesiredMinerals)

Push-Location $root
try {
    & $luaPath $luaScript @luaArgs
} finally {
    Pop-Location
}

$luaExitCode = $LASTEXITCODE

Write-Host ""
if ($luaExitCode -eq 0) {
    Write-Host "Script terminé avec succès!" -ForegroundColor Green
} else {
    Write-Host "Script terminé avec code d'erreur: $luaExitCode" -ForegroundColor Red
}

# Nettoyage
if (-not $KeepOpen) {
    Write-Host ""
    Write-Host "Fermeture de KillEngine..." -ForegroundColor Yellow
    Stop-Process -Id $killengineProcess.Id -Force -ErrorAction SilentlyContinue
} else {
    Write-Host ""
    Write-Host "KillEngine laissé ouvert (PID: $($killengineProcess.Id))" -ForegroundColor Cyan
    Write-Host "Fermez-le manuellement quand vous avez terminé"
}

exit $luaExitCode
