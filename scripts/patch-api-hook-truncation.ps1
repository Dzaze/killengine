# Patch ponctuel : borne la copie des noms module/fonction dans l'IPC
# api_hook avant toWCharArray (tronquer la QString AVANT la copie, sinon
# debordement possible du wchar_t[64]/[128] de l'IPC pour de longs noms).
# Patron d'encodage du repo : UTF-8 sans BOM, CRLF preserve.

$ErrorActionPreference = 'Stop'

$f = Join-Path $PSScriptRoot '..\core\inject\api_hook.cpp'
$f = [System.IO.Path]::GetFullPath($f)

$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom) { $text = $text.Substring(1) }

$old = @'
    config.moduleName.toWCharArray(state->moduleName);
    state->moduleName[qMin(config.moduleName.size(), kModuleNameMaxChars - 1)] = L'\0';
    config.functionName.toWCharArray(state->functionName);
    state->functionName[qMin(config.functionName.size(), kFunctionNameMaxChars - 1)] = L'\0';
'@
$new = @'
    // toWCharArray ne borne pas la destination : tronquer la QString AVANT
    // la copie pour ne jamais déborder du tableau wchar_t de l'IPC.
    const QString moduleName = config.moduleName.left(kModuleNameMaxChars - 1);
    const QString functionName = config.functionName.left(kFunctionNameMaxChars - 1);
    moduleName.toWCharArray(state->moduleName);
    state->moduleName[moduleName.size()] = L'\0';
    functionName.toWCharArray(state->functionName);
    state->functionName[functionName.size()] = L'\0';
'@

# Les here-strings sont en LF : normaliser le fichier complet en CRLF a la fin.
$old = $old -replace "`r`n", "`n"
$new = $new -replace "`r`n", "`n"

if (-not $text.Contains($old)) {
    # Re-tenter en CRLF (fichier potentiellement deja en CRLF).
    $oldCrlf = $old -replace "`n", "`r`n"
    if ($text.Contains($oldCrlf)) {
        $text = $text.Replace($oldCrlf, ($new -replace "`n", "`r`n"))
    } else {
        Write-Error 'Bloc original introuvable — fichier deja patche ou contenu divergent.'
        exit 1
    }
} else {
    $text = $text.Replace($old, ($new -replace "`n", "`r`n"))
}

# Normaliser l'ensemble du fichier en CRLF (le repo est 100% CRLF).
$text = $text -replace "`r`n", "`n" -replace "`n", "`r`n"

$enc = New-Object System.Text.UTF8Encoding($hasBom)
[System.IO.File]::WriteAllText($f, $text, $enc)
Write-Output 'PATCHED api_hook.cpp (troncature avant toWCharArray)'