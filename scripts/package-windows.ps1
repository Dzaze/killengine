# KillEngine Windows packaging script
# Usage:
#   .\scripts\package-windows.ps1
#   .\scripts\package-windows.ps1 -SkipBuild
#   .\scripts\package-windows.ps1 -ExcludeModel
#   .\scripts\package-windows.ps1 -SkipClrInspector
#   .\scripts\package-windows.ps1 -RequireSigning   (fail the build instead of shipping KillEngine.exe unsigned; see docs/CODE_SIGNING.md)

param(
    [switch]$SkipBuild,
    [switch]$ExcludeModel,
    [switch]$SkipClrInspector,
    [switch]$SkipKernelDriver,
    [switch]$RequireSigning
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildBin = Join-Path $repoRoot "build\bin"
$distRoot = Join-Path $repoRoot "dist"
$packageName = "KillEngine-portable"
$packageRoot = Join-Path $distRoot $packageName
$zipPath = Join-Path $distRoot "$packageName.zip"

function Copy-ItemIfExists {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path $Path) {
        Copy-Item -LiteralPath $Path -Destination $Destination -Recurse -Force
    }
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

# PORT-5 (docs/PORTABILITY_ROADMAP.md) : emet une archive de module
# installable localement (voir scripts/install-module-from-archive.ps1) a
# partir d'une sortie de build/publish deja produite ci-dessous -- jamais une
# seconde compilation. Le MODULE_MANIFEST.json embarque permet une
# verification d'integrite (SHA256) et d'architecture entierement offline,
# sans URL de telechargement inventee.
function New-ModuleArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ModuleId,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$OutputZip
    )

    $sourceFull = (Resolve-Path -LiteralPath $SourceDir).Path
    $files = Get-ChildItem -LiteralPath $sourceFull -Recurse -File

    $manifestFiles = @()
    foreach ($file in $files) {
        $relative = [System.IO.Path]::GetRelativePath($sourceFull, $file.FullName) -replace '\\', '/'
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash.ToLowerInvariant()
        $manifestFiles += [ordered]@{
            path   = $relative
            sha256 = $hash
            size   = $file.Length
        }
    }

    $manifest = [ordered]@{
        moduleId = $ModuleId
        version  = $Version
        arch     = "win-x64"
        files    = $manifestFiles
    }

    $stagingDir = Join-Path $env:TEMP "killengine_module_archive_$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
    try {
        Get-ChildItem -LiteralPath $sourceFull -Force | Copy-Item -Destination $stagingDir -Recurse -Force
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stagingDir "MODULE_MANIFEST.json") -Encoding UTF8

        $outputDir = Split-Path -Parent $OutputZip
        New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
        if (Test-Path -LiteralPath $OutputZip) {
            Remove-Item -LiteralPath $OutputZip -Force
        }
        Compress-Archive -Path (Join-Path $stagingDir "*") -DestinationPath $OutputZip -CompressionLevel Optimal
        Write-Host "Module archive: $OutputZip ($($files.Count) file(s))" -ForegroundColor Cyan
    } finally {
        Remove-Item -LiteralPath $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (-not $SkipBuild) {
    & (Join-Path $repoRoot "scripts\build.ps1")
    if ($LASTEXITCODE -ne 0) {
        throw "scripts\build.ps1 failed with exit code $LASTEXITCODE — refusing to package a stale/missing binary."
    }
}

$exePath = Join-Path $buildBin "KillEngine.exe"
if (-not (Test-Path $exePath)) {
    throw "KillEngine.exe was not found at $exePath. Run .\scripts\build.ps1 first."
}

if (Test-Path $packageRoot) {
    $resolvedPackageRoot = (Resolve-Path $packageRoot).Path
    $resolvedDistRoot = if (Test-Path $distRoot) { (Resolve-Path $distRoot).Path } else { $distRoot }
    if (-not $resolvedPackageRoot.StartsWith($resolvedDistRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean unexpected package path: $resolvedPackageRoot"
    }
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

$excludedNames = @(
    "killengine_unit_tests.exe",
    "killengine_unit_tests.pdb",
    "killengine_integration_tests.exe",
    "killengine_integration_tests.pdb",
    "KillEngineBenchmark.exe",
    "KillEngineBenchmark.pdb",
    "KillEngineTestTarget.exe",
    "KillEngineTestTarget.pdb",
    "lz4.pdb"
)

$excludedExtensions = @(".pdb", ".ilk", ".exp", ".lib")

# PORT-1 (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : le motif "*d.dll" ci-dessous
# est une heuristique de nom (suffixe "d" ajoute par convention aux DLL de debug
# Qt/MSVC, ex. Qt6Cored.dll), pas une vraie detection de build debug -- il attrape
# aussi des noms de projet qui se terminent legitimement par "d", ex. mtmd.dll
# (bibliotheque multimodale de llama.cpp, confirme Release par ses imports PE
# reels llama-cli-impl.dll -> llama-server-impl.dll -> mtmd.dll). Liste explicite
# plutot que d'affiner davantage l'heuristique de nom ; a completer si un futur
# faux positif est trouve.
$debugFilterExceptions = @("mtmd.dll")

# build\bin is a live dev workspace, not just build output: running KillEngine.exe
# from there (as every dev session does) creates real session state next to the
# binaries -- QSettings INI (docs/PORTABILITY_ROADMAP.md P1), WebEngine persistent
# profile/cache/cookies/history (P2), logs (P3), crash dumps (P4), Pattern Learning
# database (P5). None of that belongs in a package meant to start clean on someone
# else's machine (AM-1, docs/PHASE_TRACKER.md, 16/09/2026). "model" is excluded
# here too: it gets its own -ExcludeModel-aware copy from the repo below, copying
# it here first would let a GGUF already staged in build\bin\model bypass that flag.
$excludedDirNames = @(
    "KillEngine",
    "webengine",
    "logs",
    "crashes",
    "data",
    "model"
)

Get-ChildItem -LiteralPath $buildBin -Force | ForEach-Object {
    if ($_.PSIsContainer -and $excludedDirNames -contains $_.Name) {
        return
    }
    if ($excludedNames -contains $_.Name) {
        return
    }
    if (-not $_.PSIsContainer -and $excludedExtensions -contains $_.Extension) {
        return
    }
    if (-not $_.PSIsContainer -and $_.Name -match "d\.dll$" -and $debugFilterExceptions -notcontains $_.Name) {
        return
    }

    Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Recurse -Force
}

$debugPatterns = @(
    "*.pdb",
    "*.ilk",
    "*.exp",
    "*.lib",
    "*.debug.*",
    "*d.dll",
    "*d.exe",
    "msvcp*d*.dll",
    "vcruntime*d*.dll",
    "ucrt*d*.dll",
    "concrt*d*.dll"
)

foreach ($pattern in $debugPatterns) {
    Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
        Where-Object { $debugFilterExceptions -notcontains $_.Name } |
        Remove-Item -Force
}

$signResult = & (Join-Path $repoRoot "scripts\codesign.ps1") -Path (Join-Path $packageRoot "KillEngine.exe") -RequireSigning:$RequireSigning

# PORT-1 (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : KillEngine.exe et les
# runtimes llama.cpp importent MSVCP140.dll/VCRUNTIME140.dll/VCRUNTIME140_1.dll
# (confirme via "dumpbin /dependents" le 17/09) -- absentes de build\bin, donc
# absentes du paquet jusqu'ici. `vc_redist.x64.exe` (deja copie plus bas) exige
# une installation systeme (UAC, modifie l'etat de la machine) : ca contredit
# "extraire et lancer", donc on l'embarque aussi en app-local, la methode de
# redistribution officiellement supportee par Microsoft pour ce cas exact.
#
# Source du CRT embarque -- IMPORTANT, corrige apres un vrai crash live : NE
# PAS se limiter au toolset utilise par scripts\build.ps1 (VS2019 BuildTools,
# 14.29). Verifie le 17/09 par lancement reel : llama-cli.exe + le paquet
# msvcp140/vcruntime140/vcruntime140_1 issus de CE toolset 14.29 plante
# immediatement (0xC0000005) au demarrage du paquet -- les binaires llama.cpp
# vendorises (ggml*.dll, llama-*.dll, mtmd.dll) sont precompiles avec un
# toolset MSVC plus recent que celui utilise pour compiler KillEngine.exe
# lui-meme, et ont besoin du CRT correspondant. Remplacer par la version
# installee sur System32 (643512 octets) a fait disparaitre le crash ; ce
# fichier correspond exactement (meme taille) au CRT du dossier Redist
# 14.51.36231\x64\Microsoft.VC145.CRT d'une installation VS plus recente
# presente sur cette machine (VS "18" BuildTools), distincte de celle que
# build.ps1 utilise pour compiler.
#
# D'ou la strategie : scanner TOUTES les installations Visual Studio
# presentes (tous dossiers VC\Redist\MSVC\<version>\x64\Microsoft.VC*.CRT,
# toutes editions/versions confondues) et prendre la version de CRT la plus
# recente trouvee, plutot que celle du toolset de compilation. Sans risque
# pour KillEngine.exe : la famille runtime "140" (VC140/141/142/143/145) est
# garantie par Microsoft binairement retro-compatible -- un binaire compile
# avec un toolset plus ancien fonctionne avec un CRT plus recent, jamais
# l'inverse. Ne JAMAIS copier directement depuis System32 (verifie ce qui est
# installe sur LA machine de packaging au moment du build, pas une version
# garantie a l'avenir ou sur une autre machine) -- source unique : les
# dossiers Redist versionnes livres avec les installations Visual Studio
# elles-memes.
function Find-VcRedistCrtDir {
    $vsRoots = @(
        Get-ChildItem -Path "C:\Program Files (x86)\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue
        Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio" -Directory -ErrorAction SilentlyContinue
    ) | ForEach-Object {
        Get-ChildItem -Path $_.FullName -Directory -ErrorAction SilentlyContinue
    }

    $candidates = foreach ($vsEdition in $vsRoots) {
        $redistRoot = Join-Path $vsEdition.FullName "VC\Redist\MSVC"
        Get-ChildItem -Path $redistRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $crtGlob = Join-Path $_.FullName "x64\Microsoft.VC*.CRT"
            $resolved = Resolve-Path $crtGlob -ErrorAction SilentlyContinue
            if ($resolved) {
                [pscustomobject]@{ Version = [version]($_.Name -replace "[^0-9.]", ""); Path = $resolved.Path }
            }
        }
    }
    $best = $candidates | Sort-Object -Property Version -Descending | Select-Object -First 1
    if ($best) { return $best.Path }
    return $null
}

# msvcp140_1/_2 : Qt6Core/Qt6Gui/Qt6Network/Qt6Quick/Qt6Widgets les importent
# directement (confirme par scripts\verify-native-dependencies.ps1 le 17/09) --
# compagnons du meme CRT "140" (support C++17 supplementaire), pas optionnels.
$requiredVcRedistDlls = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_1.dll", "msvcp140_2.dll")
$missingVcRedistDlls = @($requiredVcRedistDlls | Where-Object { -not (Test-Path -LiteralPath (Join-Path $packageRoot $_) -PathType Leaf) })
if ($missingVcRedistDlls.Count -gt 0) {
    $crtDir = Find-VcRedistCrtDir
    if (-not $crtDir) {
        throw "Impossible de trouver un dossier Redist Visual C++ (VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT) sur cette machine -- installer le composant 'Redistribuables C++' de Visual Studio, ou copier manuellement $($missingVcRedistDlls -join ', ') dans build\bin avant de packager."
    }
    foreach ($dllName in $missingVcRedistDlls) {
        $source = Join-Path $crtDir $dllName
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "DLL redistribuable introuvable dans $crtDir : $dllName"
        }
        Copy-Item -LiteralPath $source -Destination $packageRoot -Force
    }
    Write-Host "Runtime Visual C++ (app-local) embarque depuis $crtDir : $($missingVcRedistDlls -join ', ')" -ForegroundColor Cyan
}

# The real Vue UI (ui\dist, built by scripts\build.ps1 as part of the build above
# unless -SkipBuild) ships as loose files next to the exe -- same portable-by-design
# convention as model\, tools\clr_inspector\ and runtime\lua\ below, and what
# apps\desktop\main.cpp now looks for at "ui/dist/index.html" relative to
# applicationDirPath() (AM-1, docs/PHASE_TRACKER.md, 16/09/2026). Without this,
# a package extracted outside the repo fell back to the ":/index.html" resource
# placeholder ("Interface non construite") for every real user.
$uiDistSource = Join-Path $repoRoot "ui\dist"
$uiDistIndex = Join-Path $uiDistSource "index.html"
if (-not (Test-Path -LiteralPath $uiDistIndex -PathType Leaf)) {
    throw "ui\dist\index.html not found. Run 'npm run build' in ui\ (or .\scripts\build.ps1 without -SkipUi) before packaging."
}

$uiOut = Join-Path $packageRoot "ui"
New-Item -ItemType Directory -Force -Path $uiOut | Out-Null
Copy-Item -LiteralPath $uiDistSource -Destination $uiOut -Recurse -Force
$uiDistOut = Join-Path $uiOut "dist"

$uiDistAssetCount = @(Get-ChildItem -LiteralPath $uiDistOut -Recurse -File -Include "*.js", "*.css" -ErrorAction SilentlyContinue).Count
if ($uiDistAssetCount -eq 0) {
    throw "Packaged ui\dist has no .js/.css assets -- refusing to ship a package that would fall back to the placeholder UI."
}

Copy-ItemIfExists -Path (Join-Path $repoRoot "README.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "LICENSE") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "KILLENGINE_PROJECT_SPEC.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\PHASE_TRACKER.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\USER_GUIDE.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\USER_GUIDE_EN.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\V1_REGRESSION_CHECKLIST.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\AUTOMATION_API.md") -Destination $packageRoot
Copy-ItemIfExists -Path (Join-Path $repoRoot "docs\PORTABILITY_ROADMAP.md") -Destination $packageRoot

$llamaCli = Find-FirstExistingFile -Paths @(
    (Join-Path $repoRoot "third_party\llama.cpp\llama-cli.exe"),
    (Join-Path $repoRoot "third_party\llama.cpp\build\bin\Release\llama-cli.exe"),
    (Join-Path $repoRoot "third_party\llama.cpp\build\bin\llama-cli.exe"),
    (Join-Path $buildBin "llama-cli.exe")
)

if ($llamaCli) {
    Copy-Item -LiteralPath $llamaCli -Destination (Join-Path $packageRoot "llama-cli.exe") -Force
}

$scriptsOut = Join-Path $packageRoot "scripts"
New-Item -ItemType Directory -Force -Path $scriptsOut | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\killengine.lua") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\automation-pipe-call.ps1") -Destination $scriptsOut -Force

# EDR helper scripts (used by the Modules view for Defender exclusions/registry changes)
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\add_defender_exclusion.bat") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\disable_defender_registry.bat") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\enable_defender_registry.bat") -Destination $scriptsOut -Force

# Test Signing helper scripts (prerequisite to load the unsigned kernel driver,
# see docs/KILLENGINE_KERNEL_DRIVER_ARCHITECTURE.md) — used by Settings'
# getTestSigningStatus/setTestSigningEnabledAsync.
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\enable_test_signing.bat") -Destination $scriptsOut -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\disable_test_signing.bat") -Destination $scriptsOut -Force

# Kernel driver install/uninstall script (used by the Modules view's
# installModule("kernel_driver")) — the compiled .sys itself is copied below.
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\install-kernel-driver.ps1") -Destination $scriptsOut -Force

# PORT-5 : script d'installation de module depuis archive locale, utilisé par
# installModule("lua_runtime" | "clr_inspector") — doit être présent même
# dans un paquet allégé sans runtime\lua ni tools\clr_inspector.
Copy-Item -LiteralPath (Join-Path $repoRoot "scripts\install-module-from-archive.ps1") -Destination $scriptsOut -Force

$luaExamples = Join-Path $repoRoot "scripts\lua_examples"
if (Test-Path $luaExamples) {
    Copy-Item -LiteralPath $luaExamples -Destination $scriptsOut -Recurse -Force
}

$luaRuntimeExe = Find-FirstExistingFile -Paths @(
    (Join-Path $repoRoot "runtime\lua\lua.exe"),
    (Join-Path $repoRoot "runtime\lua\lua54.exe"),
    (Join-Path $repoRoot "runtime\lua\lua5.4.exe"),
    (Join-Path $repoRoot "runtime\lua\luajit.exe"),
    (Join-Path $repoRoot "third_party\lua\lua.exe"),
    (Join-Path $repoRoot "third_party\lua\lua54.exe"),
    (Join-Path $repoRoot "third_party\lua\lua5.4.exe"),
    (Join-Path $repoRoot "third_party\lua\luajit.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua54.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\lua5.4.exe"),
    (Join-Path $repoRoot "third_party\lua\bin\luajit.exe"),
    (Join-Path $repoRoot "tools\lua\lua.exe"),
    (Join-Path $repoRoot "tools\lua\lua54.exe"),
    (Join-Path $repoRoot "tools\lua\lua5.4.exe"),
    (Join-Path $repoRoot "tools\lua\luajit.exe"),
    (Join-Path $buildBin "runtime\lua\lua.exe"),
    (Join-Path $buildBin "runtime\lua\lua54.exe"),
    (Join-Path $buildBin "runtime\lua\lua5.4.exe"),
    (Join-Path $buildBin "runtime\lua\luajit.exe")
)

if ($luaRuntimeExe) {
    $luaRuntimeSource = Split-Path -Parent $luaRuntimeExe
    $luaRuntimeOut = Join-Path $packageRoot "runtime\lua"
    New-Item -ItemType Directory -Force -Path $luaRuntimeOut | Out-Null
    Get-ChildItem -LiteralPath $luaRuntimeSource -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @(".exe", ".dll", ".txt", ".md") -or $_.Name -match "license|copying" } |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination $luaRuntimeOut -Force
        }

    # PORT-5 : archive installable localement (voir install-module-from-archive.ps1)
    # a partir de ce meme runtime deja bundle -- pas de seconde compilation.
    $luaExeName = Split-Path -Leaf $luaRuntimeExe
    # "" | pour fermer immediatement stdin (chunk vide) : lua.exe -v sans
    # script ni -e lit stdin en mode batch (pas de TTY) et se terminerait
    # sinon en attendant une entree interactive qui ne viendra jamais.
    $luaVersionOutput = "" | & (Join-Path $luaRuntimeOut $luaExeName) -v 2>&1 | Select-Object -First 1
    $luaVersionMatch = [regex]::Match([string]$luaVersionOutput, '(\d+\.\d+(\.\d+)?)')
    $luaVersion = if ($luaVersionMatch.Success) { $luaVersionMatch.Value } else { "unknown" }
    New-ModuleArchive -ModuleId "lua_runtime" -Version $luaVersion -SourceDir $luaRuntimeOut `
        -OutputZip (Join-Path $distRoot "module-archives\killengine-module-lua_runtime-win-x64.zip")
} else {
    Write-Warning "Lua runtime not found. Put Lua in runtime\lua, third_party\lua, third_party\lua\bin or tools\lua to bundle scripting support."
}

if (-not $SkipClrInspector) {
    $dotnetCmd = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnetCmd) {
        throw "SDK .NET introuvable sur le PATH. Installe le SDK .NET 8+ ou relance avec -SkipClrInspector pour un package sans inspecteur CLR."
    }

    $clrProject = Join-Path $repoRoot "tools\clr_inspector\KillEngineClrInspector\KillEngineClrInspector.csproj"
    if (-not (Test-Path -LiteralPath $clrProject -PathType Leaf)) {
        throw "Projet KillEngineClrInspector introuvable: $clrProject"
    }

    $clrOut = Join-Path $packageRoot "tools\clr_inspector"
    New-Item -ItemType Directory -Force -Path $clrOut | Out-Null
    Write-Host "Publishing KillEngineClrInspector self-contained..." -ForegroundColor Cyan
    & dotnet publish $clrProject -c Release -r win-x64 --self-contained true -o $clrOut `
        -p:DebugType=None -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish KillEngineClrInspector a échoué (code $LASTEXITCODE)."
    }

    if (-not (Test-Path -LiteralPath (Join-Path $clrOut "KillEngineClrInspector.exe") -PathType Leaf)) {
        throw "Publication ClrMD invalide: KillEngineClrInspector.exe absent de $clrOut"
    }

    Get-ChildItem -LiteralPath $clrOut -Recurse -File -Include "*.pdb", "*.xml" -ErrorAction SilentlyContinue |
        Remove-Item -Force

    # PORT-5 : archive installable localement a partir de cette meme
    # publication self-contained -- pas de second `dotnet publish`.
    $clrVersionInfo = (Get-Item -LiteralPath (Join-Path $clrOut "KillEngineClrInspector.exe")).VersionInfo.FileVersion
    $clrVersion = if ($clrVersionInfo) { $clrVersionInfo } else { "unknown" }
    New-ModuleArchive -ModuleId "clr_inspector" -Version $clrVersion -SourceDir $clrOut `
        -OutputZip (Join-Path $distRoot "module-archives\killengine-module-clr_inspector-win-x64.zip")
} else {
    Write-Warning "KillEngineClrInspector skipped. The CLR view will require a dev-built helper or will report it as unavailable."
}

# Kernel driver (optional module, not required for the app to run): unlike
# the CLR inspector above, building KillEngineKernel.sys needs the WDK/MSBuild
# toolchain, which may not be present on every machine that packages a
# release -- best-effort include, warn instead of failing the whole package.
# Without this, the Modules "Installer" button for kernel_driver has nothing
# to install once the ZIP leaves this machine (install-kernel-driver.ps1 looks
# for the .sys under tools\kernel_driver relative to the exe).
if (-not $SkipKernelDriver) {
    $kernelDriverSys = Get-ChildItem (Join-Path $repoRoot "tools\kernel_driver") -Recurse -Filter "KillEngineKernel.sys" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\Release\*" } |
        Select-Object -First 1 -ExpandProperty FullName

    if ($kernelDriverSys) {
        # install-kernel-driver.ps1 requires the found path to contain
        # "\Release\" (its -Configuration filter) -- keep that segment.
        $kernelDriverOut = Join-Path $packageRoot "tools\kernel_driver\Release"
        New-Item -ItemType Directory -Force -Path $kernelDriverOut | Out-Null
        Copy-Item -LiteralPath $kernelDriverSys -Destination $kernelDriverOut -Force
        Write-Host "Kernel driver bundled: $kernelDriverSys" -ForegroundColor Cyan
    } else {
        Write-Warning "KillEngineKernel.sys (Release) not found under tools\kernel_driver. Build it first with .\scripts\build-kernel-driver.ps1, or pass -SkipKernelDriver to suppress this warning. The Modules 'Install' button for the kernel driver will report the driver as missing in this package."
    }
} else {
    Write-Warning "Kernel driver skipped (-SkipKernelDriver). The Modules 'Install' button for the kernel driver will report the driver as missing in this package."
}

$modelRoot = Join-Path $repoRoot "model"
$modelOut = Join-Path $packageRoot "model"
New-Item -ItemType Directory -Force -Path $modelOut | Out-Null
Copy-ItemIfExists -Path (Join-Path $modelRoot "README.md") -Destination $modelOut

Get-ChildItem -LiteralPath $modelRoot -Directory -ErrorAction SilentlyContinue |
    ForEach-Object {
        $destination = Join-Path $modelOut $_.Name
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-ItemIfExists -Path (Join-Path $_.FullName "README.md") -Destination $destination
        Copy-ItemIfExists -Path (Join-Path $_.FullName "MODEL_MANIFEST.md") -Destination $destination
        Copy-ItemIfExists -Path (Join-Path $_.FullName "MODEL_MANIFEST.json") -Destination $destination
    }

if (-not $ExcludeModel) {
    Get-ChildItem -LiteralPath $modelRoot -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue |
        ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($modelRoot, $_.DirectoryName)
            $destination = Join-Path $modelOut $relative
            New-Item -ItemType Directory -Force -Path $destination | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
        }

    $packagedModels = @(Get-ChildItem -LiteralPath $modelOut -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue)
    if ($packagedModels.Count -eq 0) {
        throw "No embedded .gguf model was found in $modelOut. Product packages must include AI models. Use -ExcludeModel only for lightweight development packages."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "llama-cli.exe") -PathType Leaf)) {
        throw "llama-cli.exe was not found. Product packages must include the embedded AI runtime. Put it in third_party\llama.cpp or build\bin, or use -ExcludeModel only for lightweight development packages."
    }
}

Get-ChildItem -LiteralPath $modelOut -Recurse -File -Filter "*.partial" -ErrorAction SilentlyContinue |
    Remove-Item -Force

$packageReadme = @"
KillEngine portable package
===========================

Run:
  KillEngine.exe

Notes:
  - This package is intended for local/offline testing.
  - llama-cli.exe is copied automatically when present in third_party/llama.cpp.
  - Lua scripting uses runtime\lua\lua.exe when bundled, then falls back to PATH.
  - Lua helper scripts live in scripts\killengine.lua, scripts\automation-pipe-call.ps1 and scripts\lua_examples.
  - CLR inspection uses tools\clr_inspector\KillEngineClrInspector.exe when bundled.
  - GGUF models are included by default.
  - Use -ExcludeModel only for lightweight development packages.
  - Use -SkipClrInspector only for lightweight development packages without the CLR helper.
  - A lightweight package built with -SkipClrInspector or without runtime\lua can still install
    those modules later, without any compiler/SDK, from the module archives this same build
    produces at dist\module-archives\killengine-module-<id>-win-x64.zip (Modules view, "Install"
    button; see docs/PORTABILITY_ROADMAP.md#port-5).
  - The normal product layout is model\<ai-name>\ next to KillEngine.exe.
  - Agent folders use MODEL_MANIFEST.json and may point to shared GGUF weights.
  - A custom model path is only an advanced override.
  - Logs, crash dumps, settings, workspace data and Pattern Learning profiles are all stored next to KillEngine.exe (portable by design) -- see PORTABILITY_ROADMAP.md.
  - The Vue UI bundle is included at ui\dist\ next to KillEngine.exe; do not delete it.
  - Read USER_GUIDE.md for the V1 user workflow.
  - Read AUTOMATION_API.md to script KillEngine via the local JSON-RPC pipe (Lua scripting, or your own agent/tool).
  - If the app does not start from a development checkout, run scripts\diagnose-launch.ps1.
"@

Set-Content -Path (Join-Path $packageRoot "PACKAGE_README.txt") -Value $packageReadme -Encoding ASCII

$requiredRuntimeItems = @(
    "KillEngine.exe",
    "ui\dist\index.html",
    "model\README.md",
    "model\assistant\README.md",
    "model\assistant\MODEL_MANIFEST.json",
    "model\auto_resolver\README.md",
    "model\auto_resolver\MODEL_MANIFEST.json",
    "model\qwen\README.md",
    "model\qwen\MODEL_MANIFEST.md",
    "QtWebEngineProcess.exe",
    "Qt6Core.dll",
    "Qt6Gui.dll",
    "Qt6Widgets.dll",
    "Qt6WebChannel.dll",
    "Qt6WebEngineCore.dll",
    "Qt6WebEngineWidgets.dll",
    "platforms\qwindows.dll",
    "resources\icudtl.dat",
    "resources\qtwebengine_resources.pak",
    "resources\qtwebengine_resources_100p.pak",
    "resources\qtwebengine_resources_200p.pak",
    "resources\v8_context_snapshot.bin",
    "translations\qtwebengine_locales\en-US.pak",
    "PACKAGE_README.txt",
    "scripts\killengine.lua",
    "scripts\automation-pipe-call.ps1",
    "scripts\add_defender_exclusion.bat",
    "scripts\disable_defender_registry.bat",
    "scripts\enable_defender_registry.bat",
    "scripts\enable_test_signing.bat",
    "scripts\disable_test_signing.bat",
    "scripts\install-kernel-driver.ps1",
    "scripts\install-module-from-archive.ps1",
    "scripts\lua_examples\README.md",
    "USER_GUIDE.md",
    "V1_REGRESSION_CHECKLIST.md",
    "AUTOMATION_API.md"
)

if (-not $SkipClrInspector) {
    $requiredRuntimeItems += @(
        "tools\clr_inspector\KillEngineClrInspector.exe"
    )
}

$missingRuntimeItems = @(
    foreach ($item in $requiredRuntimeItems) {
        $path = Join-Path $packageRoot $item
        if (-not (Test-Path $path)) {
            $item
        }
    }
)

if ($missingRuntimeItems.Count -gt 0) {
    throw "Portable package is missing required runtime item(s): $($missingRuntimeItems -join ', ')"
}

$forbiddenRuntimeItems = @(
    "killengine_unit_tests.exe",
    "killengine_integration_tests.exe",
    "KillEngineBenchmark.exe",
    "KillEngineTestTarget.exe",
    "KillEngine",
    "webengine",
    "logs",
    "crashes",
    "data"
)

$forbiddenPresent = @(
    foreach ($item in $forbiddenRuntimeItems) {
        $path = Join-Path $packageRoot $item
        if (Test-Path $path) {
            $item
        }
    }
)

if ($forbiddenPresent.Count -gt 0) {
    throw "Portable package contains development executable(s): $($forbiddenPresent -join ', ')"
}

if (-not $ExcludeModel) {
    $productModels = @(Get-ChildItem -LiteralPath (Join-Path $packageRoot "model") -Recurse -File -Filter "*.gguf" -ErrorAction SilentlyContinue)
    if ($productModels.Count -eq 0) {
        throw "Product package validation failed: no embedded AI model found under model\<ai-name>\*.gguf."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "llama-cli.exe") -PathType Leaf)) {
        throw "Product package validation failed: llama-cli.exe is missing."
    }
}

if (Test-Path $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

Compress-Archive -Path (Join-Path $packageRoot "*") -DestinationPath $zipPath -Force

Write-Host "Portable package ready:" -ForegroundColor Green
Write-Host "  Folder: $packageRoot" -ForegroundColor Cyan
Write-Host "  Zip:    $zipPath" -ForegroundColor Cyan
Write-Host "  Signed: $($signResult.Signed)" -ForegroundColor $(if ($signResult.Signed) { "Cyan" } else { "Yellow" })
