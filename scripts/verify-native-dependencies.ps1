<#
PORT-1 (docs/PORTABILITY_ROADMAP.md, 17/09/2026) : rapport des dependances
natives transitives des EXE/DLL distribues dans un layout KillEngine (dev
build\bin ou paquet portable), imports differes compris. Classe chaque DLL
importee en "provided" (presente dans le layout), "system" (fournie par
Windows sur toute machine cible -- liste blanche ci-dessous) ou "missing"
(ni l'un ni l'autre : signal reel a corriger avant de distribuer). Sert au
build/QA (necessite dumpbin.exe, donc Visual Studio Build Tools installe sur
la machine de packaging) -- jamais execute au demarrage utilisateur.

Limite connue et volontaire : une DLL chargee dynamiquement (LoadLibrary a
l'execution, pas un import PE statique) n'apparait pas ici -- ce rapport
couvre les imports statiques, pas le chargement dynamique. Completer par un
lancement fonctionnel reel (voir scripts\verify-ai-layout.ps1 et le smoke
test de release-check.ps1) pour les DLL optionnelles chargees ainsi.

Usage:
  .\scripts\verify-native-dependencies.ps1
  .\scripts\verify-native-dependencies.ps1 -LayoutRoot .\dist\KillEngine-portable
#>
param(
    [string]$LayoutRoot,
    [switch]$NoRecurse
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $LayoutRoot) {
    $LayoutRoot = Join-Path $repoRoot "build\bin"
}
if (-not (Test-Path -LiteralPath $LayoutRoot -PathType Container)) {
    throw "Layout root introuvable: $LayoutRoot"
}
$resolvedRoot = (Resolve-Path -LiteralPath $LayoutRoot).Path

function Find-Dumpbin {
    $patterns = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe",
        "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe"
    )
    $found = foreach ($pattern in $patterns) {
        Get-Item -Path $pattern -ErrorAction SilentlyContinue
    }
    $found = @($found | Sort-Object -Property FullName -Descending)
    if ($found.Count -gt 0) { return $found[0].FullName }
    return $null
}

$dumpbin = Find-Dumpbin
if (-not $dumpbin) {
    throw "dumpbin.exe introuvable (composant 'Outils de developpement C++' de Visual Studio requis pour ce controle QA -- jamais necessaire au demarrage utilisateur)."
}
Write-Host "dumpbin: $dumpbin" -ForegroundColor DarkGray

# Liste blanche des DLL systeme presentes sur toute machine Windows 10/11
# cible -- ne pas les copier dans le paquet, ne pas les signaler "missing".
# Prefixes api-ms-win-*/ext-ms-* : contrats API-set Windows, resolus par le
# systeme (ucrtbase.dll les fournit), jamais des fichiers reels a distribuer.
$systemDllPrefixes = @("api-ms-win-", "ext-ms-")
$systemDllNames = @(
    "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "gdi32full.dll",
    "advapi32.dll", "shell32.dll", "shlwapi.dll", "ole32.dll", "oleaut32.dll",
    "comctl32.dll", "comdlg32.dll", "ws2_32.dll", "winmm.dll", "imm32.dll",
    "msvcrt.dll", "ntdll.dll", "version.dll", "setupapi.dll", "crypt32.dll",
    "wintrust.dll", "secur32.dll", "sspicli.dll", "netapi32.dll", "iphlpapi.dll",
    "dwmapi.dll", "uxtheme.dll", "mpr.dll", "winspool.drv", "oleacc.dll",
    "propsys.dll", "d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "dxguid.dll",
    "opengl32.dll", "glu32.dll", "powrprof.dll", "userenv.dll", "wtsapi32.dll",
    "bcrypt.dll", "ncrypt.dll", "rpcrt4.dll", "sechost.dll", "win32u.dll",
    "cfgmgr32.dll", "dbghelp.dll", "psapi.dll", "normaliz.dll", "wldap32.dll",
    "credui.dll", "dnsapi.dll", "nsi.dll", "mswsock.dll", "activeds.dll",
    "adsldpc.dll", "avrt.dll", "dhcpcsvc.dll", "dhcpcsvc6.dll", "dwrite.dll",
    "d2d1.dll", "windowscodecs.dll", "mf.dll", "mfplat.dll", "mfreadwrite.dll",
    "evr.dll", "wmvcore.dll", "hid.dll", "cryptbase.dll", "profapi.dll",
    "msasn1.dll", "fwpuclnt.dll", "magnification.dll", "ntmarta.dll",
    "clbcatq.dll", "combase.dll", "kernel.appcore.dll", "wininet.dll",
    "urlmon.dll", "shcore.dll", "gdiplus.dll", "msimg32.dll", "usp10.dll",
    # Trouves live le 17/09 sur le paquet reel (Qt6*/qwindows.dll/opengl32sw.dll) :
    # tous des composants Windows livres avec l'OS, jamais des fichiers a copier.
    "imagehlp.dll", "authz.dll", "winhttp.dll", "pdh.dll", "winusb.dll",
    "uiautomationcore.dll",
    # mscoree.dll : amorce CLR historique presente dans la table d'import de
    # TOUT assembly .NET (y compris .NET 8, meme si l'execution reelle passe
    # par apphost/hostfxr et non par mscoree.dll) -- artefact de format PE,
    # pas une dependance chargee au runtime ; livree avec Windows depuis Vista.
    "mscoree.dll"
)

function Test-IsSystemDll {
    param([string]$Name)
    $lower = $Name.ToLowerInvariant()
    if ($systemDllNames -contains $lower) { return $true }
    foreach ($prefix in $systemDllPrefixes) {
        if ($lower.StartsWith($prefix)) { return $true }
    }
    return $false
}

Write-Host "Inventaire du layout: $resolvedRoot" -ForegroundColor Cyan
$layoutFiles = if ($NoRecurse) {
    Get-ChildItem -LiteralPath $resolvedRoot -File -Include "*.exe", "*.dll"
} else {
    Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Include "*.exe", "*.dll"
}
$providedNames = @{}
foreach ($file in $layoutFiles) {
    $providedNames[$file.Name.ToLowerInvariant()] = $true
}
Write-Host "  $($layoutFiles.Count) fichier(s) EXE/DLL trouve(s)." -ForegroundColor DarkGray

function Get-PeDependents {
    param([string]$Path)
    $output = & $dumpbin /dependents $Path 2>&1
    $names = New-Object System.Collections.Generic.List[string]
    $inBlock = $false
    foreach ($line in $output) {
        if ($line -match "following dependencies") { $inBlock = $true; continue }
        if ($inBlock) {
            $trimmed = $line.Trim()
            if ($trimmed -match "^[A-Za-z0-9_.\-]+\.dll$") {
                $names.Add($trimmed)
                continue
            }
            # dumpbin emits one blank line right after the header before the
            # first DLL name -- only treat a blank line as end-of-list once at
            # least one name has already been collected, otherwise this loop
            # exits before ever reading the list (found live via a deliberate
            # missing-dependency fixture test, PORT-1, 17/09/2026).
            if ($trimmed -eq "" -and $names.Count -gt 0) { break }
        }
    }
    return $names
}

$missingReport = New-Object System.Collections.Generic.List[string]
$checkedCount = 0
foreach ($file in $layoutFiles) {
    $checkedCount++
    $dependents = Get-PeDependents -Path $file.FullName
    foreach ($dependent in $dependents) {
        $lower = $dependent.ToLowerInvariant()
        if ($providedNames.ContainsKey($lower)) { continue }
        if (Test-IsSystemDll -Name $dependent) { continue }
        $missingReport.Add("$($file.Name) -> $dependent")
    }
}
Write-Host "  $checkedCount fichier(s) analyse(s) via dumpbin /dependents." -ForegroundColor DarkGray

if ($missingReport.Count -gt 0) {
    Write-Host ""
    Write-Host "Dependances natives non fournies et non reconnues comme systeme :" -ForegroundColor Red
    $missingReport | Sort-Object -Unique | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "(Rappel : ce rapport ne voit pas les DLL chargees dynamiquement via LoadLibrary -- completer par un vrai lancement si une dependance optionnelle n'est pas importee statiquement.)" -ForegroundColor Yellow
    throw "$($missingReport.Count) dependance(s) native(s) potentiellement manquante(s) -- voir la liste ci-dessus."
}

Write-Host ""
Write-Host "Dependances natives OK : toutes les DLL importees sont fournies dans le layout ou reconnues comme systeme Windows." -ForegroundColor Green
