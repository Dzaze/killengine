<#
Installe un module complementaire (lua_runtime, clr_inspector) depuis une
archive locale precompilee, sans compilateur/SDK sur la machine utilisateur.
Voir docs/PORTABILITY_ROADMAP.md#port-5.

L'archive doit contenir un MODULE_MANIFEST.json a sa racine :
  {
    "moduleId": "lua_runtime",
    "version": "5.5.1",
    "arch": "win-x64",
    "files": [ { "path": "lua.exe", "sha256": "<hex>", "size": 123456 } ]
  }

Usage (appele par ApplicationController::installModule, pas destine a un
usage manuel courant) :
  .\scripts\install-module-from-archive.ps1 -ModuleId lua_runtime -ArchivePath C:\...\killengine-module-lua_runtime-win-x64.zip -TargetDir C:\...\runtime\lua
#>
param(
    [Parameter(Mandatory)]
    [ValidateSet('lua_runtime', 'clr_inspector')]
    [string]$ModuleId,

    [Parameter(Mandatory)]
    [string]$ArchivePath,

    [Parameter(Mandatory)]
    [string]$TargetDir
)

$ErrorActionPreference = "Stop"

# Get-FileHash/Compress-Archive/Expand-Archive viennent de modules integres
# (Microsoft.PowerShell.Utility / .Archive) normalement charges automatiquement
# via $env:PSModulePath -- observe en pratique un PSModulePath herite d'un
# process parent d'une AUTRE version de PowerShell (ex. pwsh 7 lancant ce
# powershell.exe 5.1 via QProcess) qui ne pointe pas vers les modules integres
# du moteur reellement en cours d'execution, faisant echouer l'autoload en
# silence ("terme non reconnu"). Resolution explicite via $PSHOME (toujours
# correct pour LE moteur qui execute ce script), independante de PSModulePath.
foreach ($builtinModule in @('Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Archive', 'Microsoft.PowerShell.Management')) {
    if (Get-Module -Name $builtinModule) { continue }
    $builtinModulePath = Join-Path $PSHOME "Modules\$builtinModule\$builtinModule.psd1"
    if (Test-Path -LiteralPath $builtinModulePath) {
        Import-Module -Name $builtinModulePath -ErrorAction Stop
    }
}

$ExpectedArch = "win-x64"
$MaxArchiveBytes = 500MB

function Write-Step {
    param([string]$Message)
    Write-Host $Message
}

# --- 0) Auto-guerison : un swap precedent interrompu au pire moment peut
#        avoir renomme TargetDir en TargetDir.old_<guid> sans jamais le
#        remettre en place. Le restaurer avant toute nouvelle tentative plutot
#        que de laisser le module "manquant" alors qu'une copie valide existe
#        juste a cote.
function Repair-InterruptedSwap {
    param([string]$Target)

    $parent = Split-Path -Parent $Target
    $leaf = Split-Path -Leaf $Target
    if (-not (Test-Path -LiteralPath $parent)) {
        return
    }

    $hasTarget = (Test-Path -LiteralPath $Target) -and
        ((Get-ChildItem -LiteralPath $Target -Force -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0)
    if ($hasTarget) {
        return
    }

    $leftoverOld = Get-ChildItem -LiteralPath $parent -Directory -Filter "$leaf.old_*" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($leftoverOld) {
        Write-Step "Reparation : swap precedent interrompu detecte, restauration de $($leftoverOld.FullName) -> $Target"
        if (Test-Path -LiteralPath $Target) {
            Remove-Item -LiteralPath $Target -Recurse -Force -ErrorAction SilentlyContinue
        }
        Rename-Item -LiteralPath $leftoverOld.FullName -NewName $leaf
    }

    # Nettoie les tentatives .new_* abandonnees (jamais promues -> jamais le
    # module actif, sans danger de les supprimer).
    Get-ChildItem -LiteralPath $parent -Directory -Filter "$leaf.new_*" -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

Repair-InterruptedSwap -Target $TargetDir

# --- 1) Validation de l'archive elle-meme ---
if (-not (Test-Path -LiteralPath $ArchivePath -PathType Leaf)) {
    throw "Archive introuvable : $ArchivePath"
}
if ([System.IO.Path]::GetExtension($ArchivePath).ToLowerInvariant() -ne ".zip") {
    throw "Archive invalide (extension attendue .zip) : $ArchivePath"
}
$archiveInfo = Get-Item -LiteralPath $ArchivePath
if ($archiveInfo.Length -eq 0) {
    throw "Archive vide : $ArchivePath"
}
if ($archiveInfo.Length -gt $MaxArchiveBytes) {
    throw "Archive anormalement volumineuse ($($archiveInfo.Length) octets > $MaxArchiveBytes) : $ArchivePath"
}

# --- 2) Extraction bornee dans un dossier de staging temporaire ---
$stagingDir = Join-Path $env:TEMP "killengine_module_install_$([guid]::NewGuid().ToString('N'))"
$newTargetDir = $null

try {
    New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
    Write-Step "Extraction de l'archive vers $stagingDir ..."
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $stagingDir -Force

    # --- 3) Lecture + validation du manifeste ---
    $manifestPath = Join-Path $stagingDir "MODULE_MANIFEST.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Archive invalide : MODULE_MANIFEST.json absent."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

    if ($manifest.moduleId -ne $ModuleId) {
        throw "Archive invalide : moduleId '$($manifest.moduleId)' ne correspond pas au module attendu '$ModuleId'."
    }
    if ($manifest.arch -ne $ExpectedArch) {
        throw "Archive invalide : architecture '$($manifest.arch)' non supportee (attendu '$ExpectedArch')."
    }
    if (-not $manifest.files -or $manifest.files.Count -eq 0) {
        throw "Archive invalide : aucune entree 'files' dans le manifeste."
    }

    $stagingFull = (Resolve-Path -LiteralPath $stagingDir).Path

    # --- 4) Verification integrale par fichier avant tout contact avec TargetDir ---
    foreach ($entry in $manifest.files) {
        $relPath = [string]$entry.path
        if ([string]::IsNullOrWhiteSpace($relPath)) {
            throw "Archive invalide : entree de manifeste sans 'path'."
        }
        if ($relPath -match '\.\.' -or [System.IO.Path]::IsPathRooted($relPath)) {
            throw "Archive invalide : chemin de fichier suspect refuse ('$relPath')."
        }

        $stagedFile = Join-Path $stagingDir $relPath
        $stagedFileFull = [System.IO.Path]::GetFullPath($stagedFile)
        if (-not $stagedFileFull.StartsWith($stagingFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Archive invalide : chemin de fichier sortant du dossier d'extraction ('$relPath')."
        }
        if (-not (Test-Path -LiteralPath $stagedFileFull -PathType Leaf)) {
            throw "Archive incomplete : fichier annonce absent ('$relPath')."
        }

        if ($entry.size) {
            $actualSize = (Get-Item -LiteralPath $stagedFileFull).Length
            if ($actualSize -ne [int64]$entry.size) {
                throw "Archive incomplete : taille inattendue pour '$relPath' (attendu $($entry.size), obtenu $actualSize)."
            }
        }

        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedFileFull).Hash.ToLowerInvariant()
        $expectedHash = ([string]$entry.sha256).ToLowerInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Empreinte SHA256 invalide pour '$relPath' (attendu $expectedHash, obtenu $actualHash) -- archive corrompue ou alteree."
        }
    }
    Write-Step "Manifeste valide : $($manifest.files.Count) fichier(s) verifie(s) (SHA256)."

    # --- 5) Construction du nouveau module dans un dossier candidat isole ---
    $newTargetDir = "$TargetDir.new_$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force -Path $newTargetDir | Out-Null
    foreach ($entry in $manifest.files) {
        $relPath = [string]$entry.path
        $source = Join-Path $stagingDir $relPath
        $destination = Join-Path $newTargetDir $relPath
        $destinationDir = Split-Path -Parent $destination
        if ($destinationDir -and -not (Test-Path -LiteralPath $destinationDir)) {
            New-Item -ItemType Directory -Force -Path $destinationDir | Out-Null
        }
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    Write-Step "Nouveau module pret dans $newTargetDir."

    # --- 6) Remplacement controle (swap quasi-atomique) ---
    $parentDir = Split-Path -Parent $TargetDir
    if ($parentDir -and -not (Test-Path -LiteralPath $parentDir)) {
        New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
    }

    $backupDir = $null
    if (Test-Path -LiteralPath $TargetDir) {
        $backupDir = "$TargetDir.old_$([guid]::NewGuid().ToString('N'))"
        Rename-Item -LiteralPath $TargetDir -NewName (Split-Path -Leaf $backupDir)
    }

    try {
        Rename-Item -LiteralPath $newTargetDir -NewName (Split-Path -Leaf $TargetDir)
        $newTargetDir = $null
    } catch {
        # Echec du swap final : restaure l'ancien module tel quel, ne jamais
        # laisser TargetDir absent.
        if ($backupDir -and (Test-Path -LiteralPath $backupDir)) {
            Rename-Item -LiteralPath $backupDir -NewName (Split-Path -Leaf $TargetDir)
            $backupDir = $null
        }
        throw
    }

    if ($backupDir -and (Test-Path -LiteralPath $backupDir)) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    Write-Step "Module $ModuleId installe : $TargetDir"
} finally {
    if (Test-Path -LiteralPath $stagingDir) {
        Remove-Item -LiteralPath $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($newTargetDir -and (Test-Path -LiteralPath $newTargetDir)) {
        Remove-Item -LiteralPath $newTargetDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}
