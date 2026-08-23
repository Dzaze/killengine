# Patch : integre MinHook (source locale tools/minhook-master) au build racine.
# UTF-8 sans BOM, CRLF preserve.

$ErrorActionPreference = 'Stop'

$f = Join-Path $PSScriptRoot '..\CMakeLists.txt'
$f = [System.IO.Path]::GetFullPath($f)

$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom) { $text = $text.Substring(1) }

if ($text.Contains('minhook-master/minhook-master')) {
    Write-Output 'DEJA PATCHE'
    exit 0
}

$old = @'
# ---------------------------------------------------------------------------
# Définitions globales du projet
# ---------------------------------------------------------------------------
'@
$new = @'
# ---------------------------------------------------------------------------
# MinHook — moteur d'inline hook (trampoline robuste) pour le composant injecté
# d'interception de fonctions (core/inject/api_hook_handler). Source locale
# (tools/minhook-master/minhook-master, v1.3.3) plutôt que FetchContent : déjà
# présente sur disque, pas de dépendance réseau au configure. Cible : `minhook`.
# ---------------------------------------------------------------------------
add_subdirectory(tools/minhook-master/minhook-master EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Définitions globales du projet
# ---------------------------------------------------------------------------
'@

$old = $old -replace "`r`n", "`n"
$new = $new -replace "`r`n", "`n"

if ($text.Contains($old)) {
    $text = $text.Replace($old, $new)
} else {
    $oldCrlf = $old -replace "`n", "`r`n"
    if ($text.Contains($oldCrlf)) {
        $text = $text.Replace($oldCrlf, ($new -replace "`n", "`r`n"))
    } else {
        Write-Error 'Ancre introuvable dans CMakeLists.txt racine.'
        exit 1
    }
}

$text = $text -replace "`r`n", "`n" -replace "`n", "`r`n"
$enc = New-Object System.Text.UTF8Encoding($hasBom)
[System.IO.File]::WriteAllText($f, $text, $enc)
Write-Output 'PATCHED CMakeLists.txt (MinHook add_subdirectory)'