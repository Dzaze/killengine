# Patch P1-6 : corrections type-check
$ErrorActionPreference = 'Stop'

function Read-Utf8([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    # detecter NL dominant
    $crlfCount = ([regex]::Matches($text, "`r`n")).Count
    $lfOnly = ([regex]::Matches($text, "(?<!`r)`n")).Count
    $nl = if ($crlfCount -gt $lfOnly) { "`r`n" } else { "`n" }
    return @{ Text = $text; HasBom = $hasBom; NL = $nl }
}

function Write-Utf8([string]$Path, [string]$Text, [bool]$HasBom) {
    $encoding = New-Object System.Text.UTF8Encoding($HasBom)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

# ============ 1. backend.ts : re-ajouter les declarations d'interface perdues ============
$f1 = 'ui\src\services\backend.ts'
$t1 = Read-Utf8 $f1
$txt = $t1.Text
$nl = $t1.NL

if ($txt -notmatch 'findWhatAccessesAsync\?') {
    $anchor = 'findWhatWritesFinished?: QWebChannelSignal<Record<string, unknown>>'
    if (-not $txt.Contains($anchor)) { throw "Ancre backend.ts introuvable" }
    $decl = @'
findWhatAccessesAsync?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  findWhatAccessesFinished?: QWebChannelSignal<Record<string, unknown>>
  scanGroupScan?(entries: Array<{ offset: number, type: string, value: string }>, options: Record<string, unknown>): Promise<EncryptedScanResult>
  writeMemoryHex?(addressHex: string, hexString: string): Promise<Record<string, unknown>>
  dumpMemoryRegion?(addressHex: string, size: number, fileName: string): Promise<Record<string, unknown>>
'@
    $decl = $decl.Replace("`r`n", "`n").Replace("`n", $nl).TrimEnd()
    $txt = $txt.Replace($anchor, $anchor + $nl + $decl)
    Write-Utf8 $f1 $txt $t1.HasBom
    Write-Host "OK: backend.ts declarations re-ajoutees"
} else {
    Write-Host "SKIP: declarations backend.ts deja presentes"
}

# ============ 2. MemoryView.vue : ajouter import + state + fonctions manquants ============
$f2 = 'ui\src\views\MemoryView.vue'
$t2 = Read-Utf8 $f2
$txt = $t2.Text
$nl = $t2.NL
function Adapt([string]$s) { return $s.Replace("`r`n", "`n").Replace("`n", $script:nl) }

if ($txt -notmatch 'const hexEditMode') {
    # Import backend
    $aImp = "import { useAppStore } from '@/stores/app'"
    if (-not $txt.Contains($aImp)) { throw "Ancre import MemoryView introuvable" }
    $txt = $txt.Replace($aImp, $aImp + $nl + "import { backend } from '@/services/backend'")

    # State apres filter
    $aState = "const filter = ref<'all' | 'committed' | 'readable' | 'writable' | 'executable'>('all')"
    if (-not $txt.Contains($aState)) { throw "Ancre filter MemoryView introuvable" }
    $state = Adapt @'

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
    $txt = $txt.Replace($aState, $aState + $state)

    # Fonctions apres scanAroundPreview
    $aFn = Adapt @'
function scanAroundPreview(type = store.exactScanType) {
  void store.scanAroundPreview(store.exactScanValue, type)
}
'@
    $aFn = $aFn.TrimEnd()
    if (-not $txt.Contains($aFn)) { throw "Ancre scanAroundPreview MemoryView introuvable" }
    $fns = Adapt @'

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
    $txt = $txt.Replace($aFn, $aFn + $fns)
    Write-Utf8 $f2 $txt $t2.HasBom
    Write-Host "OK: MemoryView script (import + state + fonctions) ajoute"
} else {
    Write-Host "SKIP: MemoryView script deja present"
}

# ============ 3. ExpertView.vue : corriger addAddressToWatch(match.address, match.type) ============
$f3 = 'ui\src\views\ExpertView.vue'
$t3 = Read-Utf8 $f3
$txt = $t3.Text
$bad = 'store.addAddressToWatch({ address: match.address, type: match.type })'
$good = 'store.addAddressToWatch(match.address, match.type)'
if ($txt.Contains($bad)) {
    $txt = $txt.Replace($bad, $good)
    Write-Utf8 $f3 $txt $t3.HasBom
    Write-Host "OK: ExpertView addAddressToWatch corrige"
} else {
    Write-Host "SKIP/VERIFY: addAddressToWatch - verifier la signature"
    # verifier la signature reelle dans app.ts
    $sig = Select-String -Path 'ui\src\stores\app.ts' -Pattern 'function addAddressToWatch' | Select-Object -First 1
    if ($sig) { Write-Host ("Signature: " + $sig.Line.Trim()) }
}