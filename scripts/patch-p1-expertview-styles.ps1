# Patch P1-5b : styles ExpertView
$ErrorActionPreference = 'Stop'
$f = 'ui\src\views\ExpertView.vue'
$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }

if ($text -notmatch '\.group-scan-row \{') {
    $lastStyle = $text.LastIndexOf('</style>')
    if ($lastStyle -lt 0) { throw "Balise style introuvable" }
    $styles = @'

/* P1 : scan groupe + watch chains */
.group-scan-entries {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin: 6px 0;
}

.group-scan-row {
  display: flex;
  gap: 6px;
  align-items: center;
}

.group-scan-row .input-mini,
.row-actions .input-mini {
  width: 90px;
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}

.group-scan-row .grow {
  flex: 1;
  min-width: 0;
}

.group-scan-results {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: 220px;
  overflow-y: auto;
  margin-top: 6px;
}

.group-scan-result-row {
  display: flex;
  gap: 8px;
  align-items: center;
  font-size: 12px;
}

.row-actions {
  display: flex;
  gap: 6px;
  align-items: center;
  margin-top: 6px;
}

.row-actions .input-mini {
  width: 70px;
}
'@
    $styles = $styles.Replace("`r`n", "`n")
    $text = $text.Substring(0, $lastStyle) + $styles + "`n" + $text.Substring($lastStyle)
    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: styles group scan ajoutes"
} else {
    Write-Host "SKIP: styles deja presents"
}