# KillEngine — Script de build
# Usage: .\scripts\build.ps1

$ErrorActionPreference = "Stop"

# Find Visual Studio vcvars64.bat
$vcvars = $null
@(
    "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) | ForEach-Object {
    $resolved = Resolve-Path $_ -ErrorAction SilentlyContinue
    if ($resolved -and -not $vcvars) { $vcvars = $resolved.Path }
}

if (-not $vcvars) {
    Write-Host "ERROR: vcvars64.bat not found!" -ForegroundColor Red
    exit 1
}

# Get ninja from pip install
$ninjaPath = (python -c "import ninja; print(ninja.BIN_DIR)" 2>$null)

function Copy-IfNewer {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        return $false
    }

    $destinationDir = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null

    $copy = $true
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $src = Get-Item -LiteralPath $Source
        $dst = Get-Item -LiteralPath $Destination
        $copy = $src.Length -ne $dst.Length -or $src.LastWriteTimeUtc -gt $dst.LastWriteTimeUtc
    }

    if ($copy) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
    return $copy
}

function Find-FirstExistingFile {
    param([Parameter(Mandatory = $true)][string[]]$Paths)

    foreach ($path in $Paths) {
        if (-not [string]::IsNullOrWhiteSpace($path) -and (Test-Path -LiteralPath $path -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $path).Path
        }
    }
    return $null
}

function Copy-DllsAlongside {
    # llama-cli.exe / llama-server.exe sont des lanceurs minces : le vrai code
    # (ggml*.dll, llama.dll, llama-common.dll, llama-cli-impl.dll,
    # llama-server-impl.dll, mtmd.dll, libomp140.x86_64.dll...) vit dans des
    # DLL a cote de l'exe source. Sans les copier, l'exe copie seul dans
    # build\bin echoue au lancement avec "impossible d'executer le code, car
    # <xxx>-impl.dll est introuvable" (Windows ne cherche pas dans
    # third_party\llama.cpp, seulement a cote de l'exe et dans le PATH).
    param(
        [Parameter(Mandatory = $true)][string]$SourceExe,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    $sourceDir = Split-Path -Parent $SourceExe
    $dlls = Get-ChildItem -LiteralPath $sourceDir -File -Filter "*.dll" -ErrorAction SilentlyContinue
    foreach ($dll in $dlls) {
        [void](Copy-IfNewer -Source $dll.FullName -Destination (Join-Path $DestinationDir $dll.Name))
    }
    return @($dlls).Count
}

function Sync-AiRuntimeLayout {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$BuildBin
    )

    $llamaCli = Find-FirstExistingFile -Paths @(
        (Join-Path $RepoRoot "third_party\llama.cpp\llama-cli.exe"),
        (Join-Path $RepoRoot "third_party\llama.cpp\build\bin\Release\llama-cli.exe"),
        (Join-Path $RepoRoot "third_party\llama.cpp\build\bin\llama-cli.exe"),
        (Join-Path $BuildBin "llama-cli.exe")
    )

    if ($llamaCli) {
        [void](Copy-IfNewer -Source $llamaCli -Destination (Join-Path $BuildBin "llama-cli.exe"))
        [void](Copy-DllsAlongside -SourceExe $llamaCli -DestinationDir $BuildBin)
    } else {
        Write-Host "WARNING: llama-cli.exe not found; build\bin will report IA embarquée indisponible." -ForegroundColor Yellow
    }

    # llama-server.exe: backend persistant (modele charge une seule fois, prompts caches).
    $llamaServer = Find-FirstExistingFile -Paths @(
        (Join-Path $RepoRoot "third_party\llama.cpp\llama-server.exe"),
        (Join-Path $RepoRoot "third_party\llama.cpp\build\bin\Release\llama-server.exe"),
        (Join-Path $RepoRoot "third_party\llama.cpp\build\bin\llama-server.exe"),
        (Join-Path $BuildBin "llama-server.exe")
    )

    if ($llamaServer) {
        [void](Copy-IfNewer -Source $llamaServer -Destination (Join-Path $BuildBin "llama-server.exe"))
        $dllCount = Copy-DllsAlongside -SourceExe $llamaServer -DestinationDir $BuildBin
        Write-Host "AI runtime DLLs staged in build\bin: $dllCount file(s)." -ForegroundColor Green
    } else {
        Write-Host "WARNING: llama-server.exe not found; l'IA retombera sur llama-cli (plus lent)." -ForegroundColor Yellow
    }

    $modelRoot = Join-Path $RepoRoot "model"
    $modelOut = Join-Path $BuildBin "model"
    New-Item -ItemType Directory -Force -Path $modelOut | Out-Null
    [void](Copy-IfNewer -Source (Join-Path $modelRoot "README.md") -Destination (Join-Path $modelOut "README.md"))

    $copiedModels = 0
    Get-ChildItem -LiteralPath $modelRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object {
            # [System.IO.Path]::GetRelativePath n'existe pas sous Windows PowerShell 5 (.NET Framework)
            $relativeDir = $_.FullName.Substring($modelRoot.Length).TrimStart('\', '/')
            $destinationDir = Join-Path $modelOut $relativeDir
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
            [void](Copy-IfNewer -Source (Join-Path $_.FullName "README.md") -Destination (Join-Path $destinationDir "README.md"))
            [void](Copy-IfNewer -Source (Join-Path $_.FullName "MODEL_MANIFEST.md") -Destination (Join-Path $destinationDir "MODEL_MANIFEST.md"))
            [void](Copy-IfNewer -Source (Join-Path $_.FullName "MODEL_MANIFEST.json") -Destination (Join-Path $destinationDir "MODEL_MANIFEST.json"))
            Get-ChildItem -LiteralPath $_.FullName -File -Filter "*.gguf" -ErrorAction SilentlyContinue |
                ForEach-Object {
                    [void](Copy-IfNewer -Source $_.FullName -Destination (Join-Path $destinationDir $_.Name))
                    $copiedModels += 1
                }
        }

    Get-ChildItem -LiteralPath $modelOut -Recurse -File -Filter "*.partial" -ErrorAction SilentlyContinue |
        Remove-Item -Force

    if ($copiedModels -eq 0) {
        Write-Host "WARNING: no .gguf model staged under build\bin\model." -ForegroundColor Yellow
    } else {
        Write-Host "AI runtime layout staged in build\bin: $copiedModels model file(s)." -ForegroundColor Green
    }
}

Write-Host "Building KillEngine..." -ForegroundColor Cyan

$batchContent = @"
@echo off
call "$vcvars" >nul 2>&1
set "VSLANG=1033"
set "PATH=$ninjaPath;%PATH%"
cmake --build build --config Release
"@

$tempBat = Join-Path $env:TEMP ("killengine_build_{0}_{1}.bat" -f $PID, [guid]::NewGuid().ToString("N"))
Set-Content -LiteralPath $tempBat -Value $batchContent -Encoding ASCII
try {
    & cmd /c "`"$tempBat`""
    $buildExitCode = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $tempBat -ErrorAction SilentlyContinue
}

if ($buildExitCode -eq 0) {
    $repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
    Sync-AiRuntimeLayout -RepoRoot $repoRoot.Path -BuildBin (Join-Path $repoRoot.Path "build\bin")
    Write-Host "`nBuild successful!" -ForegroundColor Green
    Write-Host "Executable: build\bin\KillEngine.exe" -ForegroundColor Cyan
} else {
    Write-Host "`nBuild FAILED!" -ForegroundColor Red
    exit $buildExitCode
}
