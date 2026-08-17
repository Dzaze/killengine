# Patch P1-4 : app.ts store - group scan state + doGroupScan + watch pointer chains
$ErrorActionPreference = 'Stop'

# app.ts est en quel format ? Detecter et adapter
function Read-Text([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    if ($hasBom -and $text.Length -gt 0 -and $text[0] -eq [char]0xFEFF) { $text = $text.Substring(1) }
    $nl = if ($text.Contains("`r`n") -and -not $text.Contains("a`r`nb")) { "`r`n" } else { "`n" }
    # detection simple : majorite CRLF ?
    $crlfCount = ([regex]::Matches($text, "`r`n")).Count
    $lfCount = ([regex]::Matches($text, "(?<!`r)`n")).Count
    $nl = if ($crlfCount -gt $lfCount) { "`r`n" } else { "`n" }
    return @{ Text = $text; HasBom = $hasBom; NL = $nl }
}

$f = 'ui\src\stores\app.ts'
$t = Read-Text $f
$text = $t.Text
$nl = $t.NL

function NL-Adapt([string]$s) {
    # Les here-strings du script sont CRLF ; convertir vers NL cible
    return $s.Replace("`r`n", "`n").Replace("`n", $script:nl)
}

if ($text -notmatch 'doGroupScan') {

    # ---- 1. State : apres encryptedScanResult (recherche par index) ----
    $anchorState = "const encryptedScanResult = ref<EncryptedScanResult | null>(null)"
    if (-not $text.Contains($anchorState)) { throw "Ancre encryptedScanResult introuvable dans app.ts" }

    $stateBlock = NL-Adapt @'

// ---- Scan groupe (P1) : N valeurs avec offsets fixes connus ----
export interface GroupScanEntryInput {
  offset: number
  type: string
  value: string
}
const groupScanEntries = ref<Array<{ offset: string, type: string, value: string }>>([
  { offset: '0', type: 'Int32', value: '' },
  { offset: '4', type: 'Int32', value: '' },
])
const groupScanResult = ref<EncryptedScanResult | null>(null)
const groupScanBusy = ref(false)
const groupScanMaxDistance = ref(64)

// ---- Watch pointer chain (P1) : suit une chaine de pointeurs en live ----
export interface WatchedPointerChain {
  id: number
  label: string
  chain: { module: string, baseOffset: string, offsets: string[] }
  type: string
  finalAddress: string
  value: string
  previousValue: string
  changed: boolean
  error: string
  updatedAt: string
}
const watchedPointerChains = ref<WatchedPointerChain[]>([])
let nextWatchedChainId = 1
'@
    $text = $text.Replace($anchorState, $anchorState + $stateBlock)

    # ---- 2. Fonction doGroupScan : avant doEncryptedScan ----
    $anchorFn = 'async function doEncryptedScan() {'
    if (-not $text.Contains($anchorFn)) { throw "Ancre doEncryptedScan introuvable" }

    $fnBlock = NL-Adapt @'
async function doGroupScan() {
    if (scanBusy.value) return
    const controller = backend.getController()
    if (!controller.scanGroupScan) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0,
        error: 'Scan groupe non exposé par ce backend.', matches: [],
      }
      addActionLog('scan', 'Scan groupe indisponible', groupScanResult.value.error, 'warning')
      return
    }
    const entries = groupScanEntries.value
      .filter((entry) => entry.value.trim() !== '' && entry.offset.trim() !== '')
      .map((entry) => ({ offset: Number(entry.offset), type: entry.type, value: entry.value.trim() }))
    if (entries.length < 2) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0,
        error: 'Renseigne au moins 2 valeurs avec leurs offsets.', matches: [],
      }
      return
    }

    try {
      scanBusy.value = true
      groupScanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Scan groupe en cours...'
      addActionLog('scan', `Scan groupe (${entries.length} valeurs)`, entries.map((e) => `+${e.offset}:${e.value}`).join(' '), 'info')
      groupScanResult.value = await controller.scanGroupScan(entries, {
        maxDistance: groupScanMaxDistance.value,
        maxResults: 1000,
        startAddress: expertStartAddress.value.trim() || undefined,
        stopAddress: expertStopAddress.value.trim() || undefined,
        alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
        writableOnly: true,
      })
      setScanProgress(1)
      if (groupScanResult.value.success) {
        addActionLog('scan', 'Scan groupe terminé', `${groupScanResult.value.matchesFound} structure(s) trouvée(s).`, 'success')
      } else {
        addActionLog('scan', 'Scan groupe échoué', groupScanResult.value.error, 'error')
      }
    } catch (e) {
      groupScanResult.value = {
        success: false, partial: false, regionsScanned: 0, bytesScanned: 0,
        matchesFound: 0, matchesReturned: 0, error: String(e), matches: [],
      }
      addActionLog('scan', 'Scan groupe échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
      groupScanBusy.value = false
      scanStatusText.value = ''
    }
  }

  function addGroupScanEntry() {
    const last = groupScanEntries.value[groupScanEntries.value.length - 1]
    const lastOffset = last ? Number(last.offset || '0') : 0
    groupScanEntries.value.push({ offset: String(lastOffset + 4), type: last?.type ?? 'Int32', value: '' })
  }

  function removeGroupScanEntry(index: number) {
    if (groupScanEntries.value.length <= 2) return
    groupScanEntries.value.splice(index, 1)
  }

  function clearGroupScanEntries() {
    groupScanEntries.value = [{ offset: '0', type: 'Int32', value: '' }, { offset: '4', type: 'Int32', value: '' }]
    groupScanResult.value = null
  }

  // ---- Watch pointer chain ----
  async function addWatchedPointerChain(chain: { module: string, baseOffset: string, offsets: string[] }, type = 'Int32', label = '') {
    const controller = backend.getController()
    if (!controller.resolvePointerChain) return null
    try {
      const resolve = await controller.resolvePointerChain(chain)
      if (!resolve.success || !resolve.finalAddress) {
        addActionLog('watch', 'Chaîne non résolue', resolve.error ?? 'Résolution impossible.', 'warning')
        return null
      }
      const normalized = resolve.finalAddress.replace(/^0x/i, '')
      const entry: WatchedPointerChain = {
        id: nextWatchedChainId++,
        label: label || `Chaîne #${nextWatchedChainId - 1}`,
        chain: { ...chain },
        type,
        finalAddress: normalized,
        value: '',
        previousValue: '',
        changed: false,
        error: '',
        updatedAt: new Date().toLocaleTimeString(),
      }
      watchedPointerChains.value.push(entry)
      await refreshWatchedPointerChain(entry.id)
      return entry
    } catch (e) {
      addActionLog('watch', 'Erreur ajout chaîne', String(e), 'error')
      return null
    }
  }

  async function refreshWatchedPointerChain(id: number): Promise<WatchedPointerChain | null> {
    const entry = watchedPointerChains.value.find((item) => item.id === id)
    if (!entry) return null
    const controller = backend.getController()
    let updated = entry
    try {
      if (controller.resolvePointerChain) {
        const resolve = await controller.resolvePointerChain(entry.chain)
        if (resolve.success && resolve.finalAddress) {
          entry.finalAddress = resolve.finalAddress.replace(/^0x/i, '')
        } else if (!resolve.success) {
          entry.error = resolve.error ?? 'Résolution impossible.'
        }
      }
      const preview = await controller.readMemoryPreview(entry.finalAddress, valueTypeReadSize(entry.type))
      const value = decodeTypedPreviewValue(preview, entry.type)
      updated = {
        ...entry,
        previousValue: entry.value,
        value,
        changed: entry.value !== '' && value !== entry.value,
        error: preview.success ? '' : preview.error,
        updatedAt: new Date().toLocaleTimeString(),
      }
    } catch (e) {
      updated = { ...entry, error: String(e), updatedAt: new Date().toLocaleTimeString() }
    }
    watchedPointerChains.value = watchedPointerChains.value.map((item) => (item.id === id ? updated : item))
    return updated
  }

  async function refreshWatchedPointerChains() {
    for (const entry of watchedPointerChains.value.slice(0, 20)) {
      await refreshWatchedPointerChain(entry.id)
    }
  }

  function removeWatchedPointerChain(id: number) {
    watchedPointerChains.value = watchedPointerChains.value.filter((item) => item.id !== id)
  }

  function clearWatchedPointerChains() {
    watchedPointerChains.value = []
  }

'@
    $text = $text.Replace($anchorFn, $fnBlock + $anchorFn)

    # ---- 3. Exports : trouver le return du store et ajouter les nouvelles fonctions ----
    # Cherchons un bloc return existant avec doEncryptedScan exporte
    $anchorExport = '    doEncryptedScan,'
    if (-not $text.Contains($anchorExport)) {
        # fallback : chercher doEncryptedScan dans un return
        $idx = $text.IndexOf('doEncryptedScan,')
        if ($idx -lt 0) { throw "Ancre export doEncryptedScan introuvable" }
        $anchorExport = 'doEncryptedScan,'
    }
    $exports = NL-Adapt @'
    doGroupScan,
    addGroupScanEntry,
    removeGroupScanEntry,
    clearGroupScanEntries,
    groupScanEntries,
    groupScanResult,
    groupScanBusy,
    groupScanMaxDistance,
    addWatchedPointerChain,
    refreshWatchedPointerChain,
    refreshWatchedPointerChains,
    removeWatchedPointerChain,
    clearWatchedPointerChains,
    watchedPointerChains,
'@
    $text = $text.Replace($anchorExport, $anchorExport + $nl + $exports.TrimEnd())

    $encoding = New-Object System.Text.UTF8Encoding($t.HasBom)
    [System.IO.File]::WriteAllText($f, $text, $encoding)
    Write-Host "OK: app.ts patche (group scan + watched pointer chains)"
} else {
    Write-Host "SKIP: doGroupScan deja present dans app.ts"
}