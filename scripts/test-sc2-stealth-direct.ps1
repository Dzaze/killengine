# test-sc2-stealth-direct.ps1
# Script de test SC2 sans Lua - utilise directement le pipe d'automatisation
# Usage: .\scripts\test-sc2-stealth-direct.ps1 [-TargetPid <pid>] [-InitialMinerals <n>] [-DesiredMinerals <n>]

param(
    [int]$TargetPid = 0,
    [int]$InitialMinerals = 50,
    [int]$DesiredMinerals = 9999,
    [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Write-Host "=== Test Mode Stealth SC2 (Direct Pipe) ===" -ForegroundColor Cyan
Write-Host ""

# Vérifier que KillEngine.exe existe
$killenginePath = Join-Path $root "build\bin\KillEngine.exe"
if (-not (Test-Path $killenginePath)) {
    Write-Error "KillEngine.exe non trouvé: $killenginePath"
    Write-Host "Lancez d'abord: .\scripts\build.ps1" -ForegroundColor Yellow
    exit 1
}

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
        Write-Error "SC2 non trouvé. Lancez SC2 d'abord ou utilisez -TargetPid <pid>"
        exit 1
    }
}

Write-Host ""
Write-Host "Configuration:" -ForegroundColor Cyan
Write-Host "  PID cible: $TargetPid"
Write-Host "  Minéraux initiaux: $InitialMinerals"
Write-Host "  Minéraux désirés: $DesiredMinerals"
Write-Host ""

# Fonction pour appeler le pipe d'automatisation
function Invoke-AutomationPipe {
    param(
        [string]$Method,
        [array]$Params = @()
    )
    
    $request = @{
        id = 1
        method = $Method
        params = $Params
    } | ConvertTo-Json -Compress
    
    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", "KillEngineAutomationPipe", [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(5000)
        
        $writer = New-Object System.IO.StreamWriter($pipe)
        $reader = New-Object System.IO.StreamReader($pipe)
        
        $writer.WriteLine($request)
        $writer.Flush()
        
        $response = $reader.ReadLine()
        
        $writer.Close()
        $reader.Close()
        $pipe.Close()
        
        return $response | ConvertFrom-Json
    } catch {
        Write-Error "Erreur pipe: $_"
        return $null
    }
}

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

# === TEST DU PIPE ===
Write-Host ""
Write-Host "=== Test du pipe d'automatisation ===" -ForegroundColor Cyan

# 1. Ping
Write-Host "Test ping..." -NoNewline
$ping = Invoke-AutomationPipe -Method "ping" -Params @("test")
if ($ping -and $ping.result -eq "pong") {
    Write-Host " OK" -ForegroundColor Green
} else {
    Write-Host " ECHEC" -ForegroundColor Red
}

# 2. Version
Write-Host "Récupération version..." -NoNewline
$version = Invoke-AutomationPipe -Method "getVersion"
if ($version -and $version.result) {
    Write-Host " $($version.result)" -ForegroundColor Green
} else {
    Write-Host " ECHEC" -ForegroundColor Red
}

# 3. Attacher au processus
Write-Host ""
Write-Host "Attachement au processus SC2 (PID: $TargetPid)..." -ForegroundColor Cyan
$attach = Invoke-AutomationPipe -Method "attachProcess" -Params @($TargetPid)
if ($attach -and $attach.result -eq $true) {
    Write-Host "Attaché avec succès!" -ForegroundColor Green
} else {
    Write-Host "Échec de l'attachement" -ForegroundColor Red
    if ($attach.error) {
        Write-Host "Erreur: $($attach.error.message)" -ForegroundColor Red
    }
}

# 4. Activer le mode stealth
Write-Host ""
Write-Host "Activation du mode stealth SC2..." -ForegroundColor Cyan
$stealth = Invoke-AutomationPipe -Method "applyStealthMode" -Params @("sc2")
if ($stealth -and $stealth.result -eq $true) {
    Write-Host "Mode stealth activé!" -ForegroundColor Green
} else {
    Write-Host "Échec ou mode non supporté" -ForegroundColor Yellow
}

# 5. Scanner les minéraux (multi-type)
Write-Host ""
Write-Host "Scan des minéraux (valeur: $InitialMinerals)..." -ForegroundColor Cyan
Write-Host "  - Int32..." -NoNewline
$scan1 = Invoke-AutomationPipe -Method "startExactScan" -Params @("Int32", $InitialMinerals)
if ($scan1 -and $scan1.result -eq $true) {
    Write-Host " OK" -ForegroundColor Green
} else {
    Write-Host " ECHEC" -ForegroundColor Red
}

# Attendre un peu le scan
Start-Sleep -Seconds 2

# Récupérer les candidats
Write-Host "Récupération des candidats..." -NoNewline
$candidates = Invoke-AutomationPipe -Method "getCandidates"
if ($candidates -and $candidates.result) {
    $count = $candidates.result.Count
    Write-Host " $count candidats trouvés" -ForegroundColor Green
    
    if ($count -gt 0 -and $count -le 10) {
        Write-Host ""
        Write-Host "Candidats trouvés:" -ForegroundColor Cyan
        foreach ($cand in $candidates.result | Select-Object -First 5) {
            Write-Host "  - $($cand.address): $($cand.currentValue)" -ForegroundColor Gray
        }
        
        # Écrire la nouvelle valeur sur le premier candidat
        $firstCandidate = $candidates.result[0]
        Write-Host ""
        Write-Host "Écriture de $DesiredMinerals sur $($firstCandidate.address)..." -ForegroundColor Cyan
        
        $write = Invoke-AutomationPipe -Method "writeMemoryValue" -Params @($firstCandidate.address, "Int32", $DesiredMinerals)
        if ($write -and $write.result -eq $true) {
            Write-Host "Écriture réussie!" -ForegroundColor Green
        } else {
            Write-Host "Échec de l'écriture" -ForegroundColor Red
            if ($write.error) {
                Write-Host "Erreur: $($write.error.message)" -ForegroundColor Red
            }
        }
    } elseif ($count -eq 0) {
        Write-Host ""
        Write-Host "Aucun candidat trouvé avec Int32. Essai avec Float32..." -ForegroundColor Yellow
        
        $scan2 = Invoke-AutomationPipe -Method "startExactScan" -Params @("Float32", [float]$InitialMinerals)
        Start-Sleep -Seconds 2
        $candidates2 = Invoke-AutomationPipe -Method "getCandidates"
        
        if ($candidates2 -and $candidates2.result -and $candidates2.result.Count -gt 0) {
            Write-Host "Trouvé $($candidates2.result.Count) candidats en Float32!" -ForegroundColor Green
        }
    } else {
        Write-Host "Trop de candidats ($count). Affinez la recherche dans l'UI." -ForegroundColor Yellow
    }
} else {
    Write-Host " ECHEC" -ForegroundColor Red
}

# 6. Détacher
Write-Host ""
Write-Host "Détachement..." -ForegroundColor Cyan
$detach = Invoke-AutomationPipe -Method "detachProcess"
if ($detach -and $detach.result -eq $true) {
    Write-Host "Détaché avec succès!" -ForegroundColor Green
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

Write-Host ""
Write-Host "=== Test terminé ===" -ForegroundColor Cyan
