# Patch P1-3 : MemoryView.vue - edition hex inline + dump region (PowerShell, normalisation LF)
$ErrorActionPreference = 'Stop'

# MemoryView.vue est en LF : normaliser toutes les chaines du script en LF
function Fix-Lf([string]$s) { return $s.Replace("`r`n", "`n") }

$f = 'ui\src\views\MemoryView.vue'
$bytes = [System.IO.File]::ReadAllBytes($f)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$text = [System.Text.Encoding]::UTF8.GetString($bytes)
if ($hasBom -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }

if ($text -notmatch 'hexEditMode') {

    # --- 1. Import backend ---
    $a2 = "import { useAppStore } from '@/stores/app'"
    if (-not $text.Contains($a2)) { throw "Ancre import introuvable" }
    $text = $text.Replace($a2, $a2 + "`nimport { backend } from '@/services/backend'")

    # --- 2. State (apres filter ref) ---
    $a1 = "const filter = ref<'all' | 'committed' | 'readable' | 'writable' | 'executable'>('all')"
    if (-not $text.Contains($a1)) { throw "Ancre filter introuvable" }
    $stateBlock = Fix-Lf @'

// Edition hex inline : ecriture de bytes bruts depuis l'inspecteur memoire
const hexEditMode = ref(false)
const hexEditValue = ref('')
const hexEditBusy = ref(false)
const hexEditResult = ref<Record<string, unknown> | null>(null)
const lastWrittenHex = ref('')

// Dump region : export binaire de la zone previewee
const dumpSize = ref(256)
const dumpBusy = ref(false)
const dumpResult = ref<Record<string, unknown> | null>(null)
'@
    $text = $text.Replace($a1, $a1 + $stateBlock)

    # --- 3. Fonctions : insertion apres scanAroundPreview (ancre multiligne normalisee LF) ---
    $a3 = Fix-Lf @'
function scanAroundPreview(type = store.exactScanType) {
  void store.scanAroundPreview(store.exactScanValue, type)
}
'@
    # le here-string se termine par un LF final qu'on retire
    $a3 = $a3.TrimEnd("`n")
    if (-not $text.Contains($a3)) { throw "Ancre scanAroundPreview introuvable" }
    $funcs = Fix-Lf @'

function startHexEdit() {
  if (!store.memoryPreviewAddress) return
  hexEditMode.value = !hexEditMode.value
  if (hexEditMode.value && !hexEditValue.value) {
    hexEditValue.value = store.memoryPreview?.hex ?? ''
  }
  hexEditResult.value = null
}

async function applyHexEdit() {
  if (!store.memoryPreviewAddress || !hexEditValue.value.trim()) return
  if (!await store.confirmRiskAction('write', 'Edition hex', 'Ecriture de bytes bruts a 0x' + store.memoryPreviewAddress + '.')) {
    return
  }
  hexEditBusy.value = true
  hexEditResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.writeMemoryHex) {
      hexEditResult.value = { success: false, error: 'writeMemoryHex non disponible dans ce backend.' }
      return
    }
    const result = await controller.writeMemoryHex(store.memoryPreviewAddress, hexEditValue.value)
    hexEditResult.value = result
    if (result.success) {
      lastWrittenHex.value = String(result.previousHex ?? '')
      await store.readMemoryPreview(store.memoryPreviewAddress, store.memoryPreview?.requestedBytes ?? 64)
    }
  } catch (e) {
    hexEditResult.value = { success: false, error: String(e) }
  } finally {
    hexEditBusy.value = false
  }
}

async function restoreLastHexEdit() {
  if (!store.memoryPreviewAddress || !lastWrittenHex.value) return
  hexEditValue.value = lastWrittenHex.value
  await applyHexEdit()
}

async function dumpPreviewRegion() {
  if (!store.memoryPreviewAddress) return
  dumpBusy.value = true
  dumpResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.dumpMemoryRegion) {
      dumpResult.value = { success: false, error: 'dumpMemoryRegion non disponible dans ce backend.' }
      return
    }
    dumpResult.value = await controller.dumpMemoryRegion(
      store.memoryPreviewAddress,
      dumpSize.value,
      'dump_' + store.memoryPreviewAddress,
    )
  } catch (e) {
    dumpResult.value = { success: false, error: String(e) }
  } finally {
    dumpBusy.value = false
  }
}
'@
    $text = $text.Replace($a3, $a3 + $funcs)

    # --- 4. Boutons preview (apres "Basculer en expert") ---
    $a4 = Fix-Lf @'
              >
                Basculer en expert
              </button>
            </div>
'@
    $a4 = $a4.TrimEnd("`n")
    if (-not $text.Contains($a4)) { throw "Ancre boutons expert introuvable" }
    $buttons = Fix-Lf @'
              >
                Basculer en expert
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
    $text = $text.Replace($a4, $buttons)

    # --- 5. Styles (avant @media) ---
    $a5 = '@media (max-width: 900px) {'
    if (-not $text.Contains($a5)) { throw "Ancre media introuvable" }
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
    $text = $text.Replace($a5, $styles)

    $encoding = New-Object System.Text.UTF8Encoding($hasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: MemoryView.vue patche (hex edit + dump)"
} else {
    Write-Host "SKIP: hexEditMode deja present"
}