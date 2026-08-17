# Patch P1-3b : MemoryView.vue - boutons hex/dump avec indentation correcte (12 espaces)
$ErrorActionPreference = 'Stop'
function Fix-Lf([string]$s) { return $s.Replace("`r`n", "`n") }

$f = 'ui\src\views\MemoryView.vue'
$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }

if ($text -notmatch 'startHexEdit\(\)') {
    # Ancre avec indentation reelle : 12 espaces pour </button>, 10 pour </div>
    $a4 = Fix-Lf @'
            </button>
          </div>
'@
    $a4 = $a4.TrimEnd("`n")
    if (-not $text.Contains($a4)) { throw "Ancre </button></div> introuvable - verifier l'indentation" }

    $buttons = Fix-Lf @'
            </button>
              <button
                v-if="store.memoryPreviewAddress"
                class="preview-action-btn"
                type="button"
                @click="startHexEdit()"
              >
                {{ hexEditMode ? 'Fermer hex' : 'Editer hex' }}
              </button>
              <button
                v-if="store.memoryPreviewAddress"
                class="preview-action-btn"
                type="button"
                :disabled="dumpBusy"
                @click="dumpPreviewRegion()"
              >
                {{ dumpBusy ? 'Dump...' : 'Dump' }}
              </button>
          </div>
          <div v-if="hexEditMode" class="hex-edit-row">
            <input
              v-model="hexEditValue"
              class="hex-edit-input"
              type="text"
              placeholder="48 8B 00 90 ..."
              spellcheck="false"
            />
            <button
              class="preview-action-btn"
              type="button"
              :disabled="hexEditBusy || !hexEditValue.trim()"
              @click="applyHexEdit()"
            >
              {{ hexEditBusy ? 'Ecriture...' : 'Ecrire bytes' }}
            </button>
            <button
              v-if="lastWrittenHex"
              class="preview-action-btn"
              type="button"
              :disabled="hexEditBusy"
              @click="restoreLastHexEdit()"
            >
              Restaurer
            </button>
          </div>
          <p v-if="hexEditResult" :class="hexEditResult.success ? 'hex-result-ok' : 'hex-result-err'">
            {{ hexEditResult.success ? 'Ecriture OK (' + (hexEditResult.bytesWritten ?? 0) + ' octets)' : String(hexEditResult.error ?? 'Echec') }}
          </p>
          <div class="dump-row">
            <label class="dump-label">Taille dump</label>
            <select v-model.number="dumpSize" class="dump-select">
              <option :value="256">256 B</option>
              <option :value="1024">1 KB</option>
              <option :value="16384">16 KB</option>
              <option :value="262144">256 KB</option>
              <option :value="1048576">1 MB</option>
            </select>
            <p v-if="dumpResult" :class="dumpResult.success ? 'hex-result-ok' : 'hex-result-err'">
              {{ dumpResult.success ? 'Dump OK : ' + String(dumpResult.filePath ?? '') : String(dumpResult.error ?? 'Echec') }}
            </p>
          </div>
'@
    # Remplacer seulement la PREMIERE occurrence (celle du bouton "Basculer en expert")
    $firstIdx = $text.IndexOf($a4)
    $text = $text.Substring(0, $firstIdx) + $buttons + $text.Substring($firstIdx + $a4.Length)

    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: boutons hex/dump ajoutes a MemoryView.vue"
} else {
    Write-Host "SKIP: boutons deja presents"
}

# Styles toujours si absents
$text2 = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($f))
if ($text2 -notmatch 'hex-edit-row \{') {
    $a5 = '@media (max-width: 900px) {'
    if (-not $text2.Contains($a5)) { throw "Ancre media introuvable" }
    $styles = Fix-Lf @'
.hex-edit-row {
  display: flex;
  gap: 8px;
  align-items: center;
  margin: 8px 0;
}

.hex-edit-input {
  flex: 1;
  min-width: 0;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.hex-result-ok {
  color: #22c55e;
  font-size: 12px;
  word-break: break-all;
}

.hex-result-err {
  color: #ef4444;
  font-size: 12px;
}

.dump-row {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-top: 6px;
}

.dump-label {
  color: var(--text-dim);
  font-size: 12px;
}

.dump-select {
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}

@media (max-width: 900px) {
'@
    $text2 = $text2.Replace($a5, $styles)
    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text2, $encoding)
    Write-Host "OK: styles hex/dump ajoutes"
}