<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import {
  backend,
  type MemoryWriteTarget,
  type PointerChainInfo,
  type PointerChainResolveResult,
  type PointerScanResult,
  type UiStringCandidate,
  type UiStringInvestigationFinishResult,
  type UiStringInvestigationStartResult,
  type UiStringOriginResult,
  type UiStringScanResult,
  type UiStringSourceCandidate,
  type UiStringSourceResult,
  type UiStringSourceTrackResult,
  type UiStringTrackResult,
} from '@/services/backend'

const store = useAppStore()
const selectedCandidateAddresses = ref<string[]>([])
const selectedWriteTargetOverrides = ref<Record<string, MemoryWriteTarget>>({})

// Phase 14 — Pointer Chains
const pointerScanAddress = ref('')
const pointerScanValueType = ref('Int32')
const pointerScanMaxDepth = ref(3)
const pointerScanMaxOffset = ref(0x1000)
const pointerScanResult = ref<PointerScanResult | null>(null)
const pointerScanBusy = ref(false)
const pointerResolveResult = ref<PointerChainResolveResult | null>(null)
const selectedPointerChainIndex = ref<number>(-1)

// Trace UI string — piste pour les valeurs affichees mais pas trouvees en numerique.
const uiStringValue = ref('')
const uiStringNextValue = ref('')
const uiStringAscii = ref(true)
const uiStringUtf16 = ref(true)
const uiStringWritableOnly = ref(true)
const uiStringBoundary = ref(true)
const uiStringSourceRadiusBytes = ref(1024 * 1024)
const uiStringBusy = ref(false)
const uiStringResult = ref<UiStringScanResult | null>(null)
const uiStringTrackResult = ref<UiStringTrackResult | null>(null)
const uiStringSourceResult = ref<UiStringSourceResult | null>(null)
const uiStringSourceTrackResult = ref<UiStringSourceTrackResult | null>(null)
const uiStringOriginResult = ref<UiStringOriginResult | null>(null)
const uiStringCandidates = ref<UiStringCandidate[]>([])
const uiStringSourceCandidates = ref<UiStringSourceCandidate[]>([])
const selectedUiStringAddresses = ref<string[]>([])
const selectedUiSourceAddresses = ref<string[]>([])
const uiStringLiveInvestigation = ref(false)
const uiStringLiveStartedAt = ref<number | null>(null)
const uiStringInvestigationElapsed = ref(0)
const uiStringInvestigationStartResult = ref<UiStringInvestigationStartResult | null>(null)
const uiStringInvestigationFinishResult = ref<UiStringInvestigationFinishResult | null>(null)
let uiStringInvestigationTimer: ReturnType<typeof setInterval> | null = null
const uiStringSourceRadiusOptions = [
  { value: 64 * 1024, label: '64 Ko' },
  { value: 256 * 1024, label: '256 Ko' },
  { value: 1024 * 1024, label: '1 Mo' },
  { value: 4 * 1024 * 1024, label: '4 Mo' },
  { value: 16 * 1024 * 1024, label: '16 Mo' },
]
const uiStringSourceSafeSelectionLimit = 25
const uiStringSourceTypeFilter = ref('all')
const uiStringSourceVariantFilter = ref('all')
const uiStringSourceBatchSize = ref(10)
const uiStringSourceBatchIndex = ref(0)
const uiStringSourceBatchSizeOptions = [5, 10, 25, 50]
const freezeIntervalPresets = [16, 33, 50, 100, 250, 500]

function setUiStringInvestigationActive(active: boolean) {
  uiStringLiveInvestigation.value = active
  uiStringLiveStartedAt.value = active ? Date.now() : null
  uiStringInvestigationElapsed.value = 0

  if (uiStringInvestigationTimer) {
    clearInterval(uiStringInvestigationTimer)
    uiStringInvestigationTimer = null
  }

  if (active) {
    uiStringInvestigationTimer = setInterval(() => {
      if (!uiStringLiveStartedAt.value) return
      uiStringInvestigationElapsed.value = Math.max(0, Math.floor((Date.now() - uiStringLiveStartedAt.value) / 1000))
    }, 1000)
  }
}

onBeforeUnmount(() => {
  if (uiStringInvestigationTimer) {
    clearInterval(uiStringInvestigationTimer)
    uiStringInvestigationTimer = null
  }
})

async function toggleUiStringLiveInvestigation() {
  const controller = backend.getController()
  if (!uiStringLiveInvestigation.value) {
    const strings = selectedUiStringCandidates()
    const sources = selectedUiSourceCandidates()
    if (strings.length === 0 && sources.length === 0) return
    uiStringBusy.value = true
    uiStringInvestigationFinishResult.value = null
    try {
      if (!controller.startUiStringInvestigation) {
        uiStringInvestigationStartResult.value = { success: false, windows: 0, error: 'Methode backend indisponible (mock mode).' }
        return
      }
      const result = await controller.startUiStringInvestigation(strings, sources, {
        radiusBytes: 4096,
        maxWindows: 96,
        maxBytesMb: 24,
        globalProbe: true,
        maxProbeMb: 1024,
        probeBlockSize: 64 * 1024,
      })
      const capturedWindows = Number(result.windows ?? 0)
      uiStringInvestigationStartResult.value = capturedWindows > 0 ? { ...result, success: true } : result
      if (result.success || capturedWindows > 0) {
        setUiStringInvestigationActive(true)
      }
    } catch (e) {
      uiStringInvestigationStartResult.value = { success: false, windows: 0, error: String(e) }
    } finally {
      uiStringBusy.value = false
    }
    return
  }

  uiStringBusy.value = true
  try {
    if (!controller.finishUiStringInvestigation) {
      uiStringInvestigationFinishResult.value = {
        success: false,
        windowsChecked: 0,
        unreadable: 0,
        changedBytes: 0,
        changesFound: 0,
        changes: [],
        error: 'Methode backend indisponible (mock mode).',
      }
      return
    }
    const result = await controller.finishUiStringInvestigation({
      maxChanges: 500,
      maxGlobalValueHits: 500,
      value: (uiStringNextValue.value || uiStringValue.value || store.exactScanValue).trim(),
    })
    uiStringInvestigationFinishResult.value = result
    const globalHits = result.globalValueHits ?? []
    if (globalHits.length > 0) {
      const merged = new Map<string, UiStringSourceCandidate>()
      for (const source of uiStringSourceCandidates.value) {
        merged.set(sourceKey(source), source)
      }
      for (const source of globalHits) {
        merged.set(sourceKey(source), source)
      }
      uiStringSourceCandidates.value = Array.from(merged.values())
        .sort((a, b) => Number(b.confidence ?? 0) - Number(a.confidence ?? 0))
        .slice(0, 500)
      selectedUiSourceAddresses.value = chooseNonOverlappingSources(globalHits).slice(0, uiStringSourceSafeSelectionLimit).map(sourceKey)
    }
  } catch (e) {
    uiStringInvestigationFinishResult.value = {
      success: false,
      windowsChecked: 0,
      unreadable: 0,
      changedBytes: 0,
      changesFound: 0,
      changes: [],
      error: String(e),
    }
  } finally {
    setUiStringInvestigationActive(false)
    uiStringBusy.value = false
  }
}

async function runPointerScan() {
  if (!pointerScanAddress.value.trim()) return
  pointerScanBusy.value = true
  pointerScanResult.value = null
  pointerResolveResult.value = null
  try {
    const controller = backend.getController()
    if (controller.scanPointerChains) {
      const result = await controller.scanPointerChains(pointerScanAddress.value, {
        maxDepth: pointerScanMaxDepth.value,
        maxOffset: pointerScanMaxOffset.value,
        maxResults: 100,
        onlyModuleBase: true,
      })
      pointerScanResult.value = result
    } else {
      pointerScanResult.value = { success: false, error: 'Methode backend indisponible (mock mode).' }
    }
  } catch (e) {
    pointerScanResult.value = { success: false, error: String(e) }
  } finally {
    pointerScanBusy.value = false
  }
}

async function testPointerChain(chain: PointerChainInfo) {
  try {
    const controller = backend.getController()
    if (controller.resolvePointerChain) {
      pointerResolveResult.value = await controller.resolvePointerChain(chain)
    }
  } catch (e) {
    pointerResolveResult.value = { success: false, error: String(e) }
  }
}

function usePointerChainAsCandidate(chain: PointerChainInfo) {
  // Place la chaîne comme adresse candidate pour écriture (test immédiat).
  void testPointerChain(chain).then(() => {
    if (pointerResolveResult.value?.success && pointerResolveResult.value.finalAddress) {
      store.selectedCandidateAddress = pointerResolveResult.value.finalAddress
      store.exactScanType = pointerScanValueType.value
    }
  })
}

async function savePointerChain(chain: PointerChainInfo) {
  const profileName = window.prompt('Nom du profil :', 'StarCraft2')
  if (!profileName) return
  const targetName = window.prompt('Nom de la cible :', 'Minerals')
  if (!targetName) return
  try {
    const controller = backend.getController()
    if (controller.savePointerChainProfileTarget) {
      const result = await controller.savePointerChainProfileTarget(
        profileName,
        targetName,
        chain,
        pointerScanValueType.value,
        'Chaine de pointeurs auto-detectee',
      )
      if (!result.success) {
        window.alert('Erreur sauvegarde profil : ' + (result.error ?? 'inconnue'))
      }
    }
  } catch (e) {
    window.alert('Erreur : ' + String(e))
  }
}

function uiStringKey(candidate: UiStringCandidate) {
  return `${candidate.encoding}:${candidate.address}`
}

function isUiStringSelected(candidate: UiStringCandidate) {
  return selectedUiStringAddresses.value.includes(uiStringKey(candidate))
}

function toggleUiStringSelection(candidate: UiStringCandidate) {
  const key = uiStringKey(candidate)
  if (selectedUiStringAddresses.value.includes(key)) {
    selectedUiStringAddresses.value = selectedUiStringAddresses.value.filter((item) => item !== key)
    return
  }
  selectedUiStringAddresses.value = [...selectedUiStringAddresses.value, key]
}

function toggleAllUiStringSelection() {
  const keys = uiStringCandidates.value.map(uiStringKey)
  const allSelected = keys.length > 0 && keys.every((key) => selectedUiStringAddresses.value.includes(key))
  selectedUiStringAddresses.value = allSelected ? [] : keys
}

function selectedUiStringCandidates() {
  if (selectedUiStringAddresses.value.length === 0) return uiStringCandidates.value
  const selected = new Set(selectedUiStringAddresses.value)
  return uiStringCandidates.value.filter((candidate) => selected.has(uiStringKey(candidate)))
}

async function scanUiStrings() {
  const value = (uiStringValue.value || store.exactScanValue).trim()
  if (!value) return
  uiStringValue.value = value
  uiStringBusy.value = true
  uiStringResult.value = null
  uiStringTrackResult.value = null
  uiStringSourceResult.value = null
  uiStringSourceTrackResult.value = null
  uiStringOriginResult.value = null
  uiStringSourceCandidates.value = []
  selectedUiStringAddresses.value = []
  selectedUiSourceAddresses.value = []
  try {
    const controller = backend.getController()
    if (!controller.scanUiStrings) {
      uiStringResult.value = {
        success: false,
        matchesFound: 0,
        matchesReturned: 0,
        regionsScanned: 0,
        bytesScanned: 0,
        matches: [],
        error: 'Methode backend indisponible (mock mode).',
      }
      uiStringCandidates.value = []
      return
    }
    const result = await controller.scanUiStrings(value, {
      startAddress: store.expertStartAddress || undefined,
      stopAddress: store.expertStopAddress || undefined,
      writableOnly: uiStringWritableOnly.value,
      copyOnWriteOnly: store.expertCopyOnWriteOnly,
      ascii: uiStringAscii.value,
      utf16: uiStringUtf16.value,
      numericBoundary: uiStringBoundary.value,
      maxResults: 5000,
    })
    uiStringResult.value = result
    uiStringCandidates.value = result.matches ?? []
  } catch (e) {
    uiStringResult.value = {
      success: false,
      matchesFound: 0,
      matchesReturned: 0,
      regionsScanned: 0,
      bytesScanned: 0,
      matches: [],
      error: String(e),
    }
    uiStringCandidates.value = []
  } finally {
    uiStringBusy.value = false
  }
}

async function trackUiStrings() {
  const value = uiStringNextValue.value.trim()
  if (!value || uiStringCandidates.value.length === 0) return
  uiStringBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.trackUiStringCandidates) {
      uiStringTrackResult.value = { success: false, checked: 0, unreadable: 0, remaining: 0, survivors: [], error: 'Methode backend indisponible (mock mode).' }
      return
    }
    const result = await controller.trackUiStringCandidates(selectedUiStringCandidates(), value)
    uiStringTrackResult.value = result
    uiStringCandidates.value = result.survivors ?? []
    selectedUiStringAddresses.value = uiStringCandidates.value.map(uiStringKey)
    uiStringValue.value = value
    uiStringSourceResult.value = null
    uiStringSourceTrackResult.value = null
    uiStringOriginResult.value = null
    uiStringSourceCandidates.value = []
    selectedUiSourceAddresses.value = []
  } catch (e) {
    uiStringTrackResult.value = { success: false, checked: 0, unreadable: 0, remaining: 0, survivors: [], error: String(e) }
  } finally {
    uiStringBusy.value = false
  }
}

function watchUiStringCandidate(candidate: UiStringCandidate) {
  store.addAddressToWatch(candidate.address, candidate.encoding === 'utf16' ? 'UInt16' : 'UInt8')
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function useUiStringCandidate(candidate: UiStringCandidate) {
  store.searchQuery = `je trace le texte affiche a 0x${candidate.address} (${candidate.encoding})`
  void store.doSearch()
}

function sourceKey(candidate: UiStringSourceCandidate) {
  return `${candidate.type}:${candidate.variantLabel ?? ''}:${candidate.address}`
}

function uiSourceTypeSize(type: string) {
  if (type.endsWith('8')) return 1
  if (type.endsWith('16')) return 2
  if (type.endsWith('32') || type === 'Float32') return 4
  if (type.endsWith('64') || type === 'Float64') return 8
  return 4
}

function addressNumber(address: string) {
  return Number.parseInt(address.replace(/^0x/i, ''), 16)
}

function chooseNonOverlappingSources(sources: UiStringSourceCandidate[]) {
  const sorted = [...sources].sort((a, b) => {
    const confidenceDelta = Number(b.confidence ?? 0) - Number(a.confidence ?? 0)
    if (Math.abs(confidenceDelta) > 0.000001) return confidenceDelta
    const distanceDelta = Number(a.distanceBytes ?? Number.MAX_SAFE_INTEGER) - Number(b.distanceBytes ?? Number.MAX_SAFE_INTEGER)
    if (distanceDelta !== 0) return distanceDelta
    return uiSourceTypeSize(a.type) - uiSourceTypeSize(b.type)
  })
  const chosen: UiStringSourceCandidate[] = []
  const ranges: Array<{ start: number, end: number }> = []
  const seenKeys = new Set<string>()
  for (const source of sorted) {
    const key = sourceKey(source)
    if (seenKeys.has(key)) continue
    seenKeys.add(key)
    const start = addressNumber(source.address)
    const size = uiSourceTypeSize(source.type)
    const end = start + size
    if (!Number.isFinite(start) || ranges.some((range) => start < range.end && end > range.start)) {
      continue
    }
    ranges.push({ start, end })
    chosen.push(source)
  }
  return chosen
}

const filteredUiStringSourceCandidates = computed(() => uiStringSourceCandidates.value.filter((candidate) => {
  const variant = candidate.variantLabel || '-'
  return (uiStringSourceTypeFilter.value === 'all' || candidate.type === uiStringSourceTypeFilter.value)
    && (uiStringSourceVariantFilter.value === 'all' || variant === uiStringSourceVariantFilter.value)
}))

const uiStringSourceTypeOptions = computed(() => Array.from(new Set(
  uiStringSourceCandidates.value.map((candidate) => candidate.type),
)).sort())

const uiStringSourceVariantOptions = computed(() => Array.from(new Set(
  uiStringSourceCandidates.value.map((candidate) => candidate.variantLabel || '-'),
)).sort())

const safeFilteredUiStringSources = computed(() => chooseNonOverlappingSources(filteredUiStringSourceCandidates.value))

const uiStringSourceBatchCount = computed(() => Math.max(1, Math.ceil(
  safeFilteredUiStringSources.value.length / uiStringSourceBatchSize.value,
)))

const boundedUiStringSourceBatchIndex = computed(() => Math.min(
  uiStringSourceBatchIndex.value,
  uiStringSourceBatchCount.value - 1,
))

const currentUiStringSourceBatch = computed(() => {
  const start = boundedUiStringSourceBatchIndex.value * uiStringSourceBatchSize.value
  return safeFilteredUiStringSources.value.slice(start, start + uiStringSourceBatchSize.value)
})

function isUiSourceSelected(candidate: UiStringSourceCandidate) {
  return selectedUiSourceAddresses.value.includes(sourceKey(candidate))
}

function toggleUiSourceSelection(candidate: UiStringSourceCandidate) {
  const key = sourceKey(candidate)
  if (selectedUiSourceAddresses.value.includes(key)) {
    selectedUiSourceAddresses.value = selectedUiSourceAddresses.value.filter((item) => item !== key)
    return
  }
  selectedUiSourceAddresses.value = [...selectedUiSourceAddresses.value, key]
}

function selectAllUiSources() {
  selectedUiSourceAddresses.value = safeFilteredUiStringSources.value.map(sourceKey)
}

function clearUiSourceSelection() {
  selectedUiSourceAddresses.value = []
}

function selectTopUiSources(limit = uiStringSourceSafeSelectionLimit) {
  selectedUiSourceAddresses.value = safeFilteredUiStringSources.value.slice(0, limit).map(sourceKey)
}

function selectUiSourceBatch() {
  selectedUiSourceAddresses.value = currentUiStringSourceBatch.value.map(sourceKey)
}

function previousUiSourceBatch() {
  uiStringSourceBatchIndex.value = Math.max(0, boundedUiStringSourceBatchIndex.value - 1)
}

function nextUiSourceBatch() {
  uiStringSourceBatchIndex.value = Math.min(uiStringSourceBatchCount.value - 1, boundedUiStringSourceBatchIndex.value + 1)
}

async function analyzeUiStringSources(candidate?: UiStringCandidate, radiusOverrideBytes = uiStringSourceRadiusBytes.value) {
  const targets = candidate ? [candidate] : selectedUiStringCandidates()
  const value = (uiStringValue.value || store.exactScanValue).trim()
  if (targets.length === 0 || !value) return
  uiStringBusy.value = true
  uiStringSourceResult.value = null
  uiStringSourceTrackResult.value = null
  uiStringOriginResult.value = null
  uiStringSourceCandidates.value = []
  selectedUiSourceAddresses.value = []
  try {
    const controller = backend.getController()
    if (!controller.analyzeUiStringSources) {
      uiStringSourceResult.value = {
        success: false,
        matchesFound: 0,
        matchesReturned: 0,
        bytesScanned: 0,
        candidates: [],
        error: 'Methode backend indisponible (mock mode).',
      }
      return
    }
    const merged = new Map<string, UiStringSourceCandidate>()
    let matchesFound = 0
    let bytesScanned = 0
    let partial = false
    let firstError = ''
    for (const target of targets) {
      const result = await controller.analyzeUiStringSources(target, value, {
        radiusBytes: radiusOverrideBytes,
        maxResults: 300,
        alignment: 1,
      })
      if (!result.success && !firstError) firstError = result.error
      matchesFound += Number(result.matchesFound ?? 0)
      bytesScanned += Number(result.bytesScanned ?? 0)
      partial = partial || Boolean(result.partial)
      for (const source of result.candidates ?? []) {
        const existing = merged.get(sourceKey(source))
        if (!existing || Number(source.confidence ?? 0) > Number(existing.confidence ?? 0)) {
          merged.set(sourceKey(source), source)
        }
      }
    }
    const candidates = Array.from(merged.values())
      .sort((a, b) => Number(b.confidence ?? 0) - Number(a.confidence ?? 0))
      .slice(0, 500)
    uiStringSourceResult.value = {
      success: firstError === '',
      partial: partial || merged.size > candidates.length,
      matchesFound,
      matchesReturned: candidates.length,
      bytesScanned,
      radiusBytes: radiusOverrideBytes,
      candidates,
      error: firstError,
    }
    uiStringSourceCandidates.value = candidates
    selectedUiSourceAddresses.value = chooseNonOverlappingSources(candidates).slice(0, uiStringSourceSafeSelectionLimit).map(sourceKey)
  } catch (e) {
    uiStringSourceResult.value = {
      success: false,
      matchesFound: 0,
      matchesReturned: 0,
      bytesScanned: 0,
      candidates: [],
      error: String(e),
    }
  } finally {
    uiStringBusy.value = false
  }
}

async function inspectUiStringOrigins(candidate?: UiStringCandidate) {
  const targets = candidate ? [candidate] : selectedUiStringCandidates()
  if (targets.length === 0) return
  uiStringBusy.value = true
  uiStringOriginResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.inspectUiStringOrigins) {
      uiStringOriginResult.value = {
        success: false,
        targetCount: 0,
        pointerRefsFound: 0,
        bytesScanned: 0,
        regionsScanned: 0,
        targets: [],
        pointerRefs: [],
        error: 'Methode backend indisponible (mock mode).',
      }
      return
    }
    uiStringOriginResult.value = await controller.inspectUiStringOrigins(targets, {
      maxRefs: 500,
      maxScanMb: 512,
      writableOnly: true,
    })
  } catch (e) {
    uiStringOriginResult.value = {
      success: false,
      targetCount: 0,
      pointerRefsFound: 0,
      bytesScanned: 0,
      regionsScanned: 0,
      targets: [],
      pointerRefs: [],
      error: String(e),
    }
  } finally {
    uiStringBusy.value = false
  }
}

async function autoInspectUiStrings() {
  const targets = selectedUiStringCandidates()
  if (targets.length === 0) return
  const radii = Array.from(new Set([
    uiStringSourceRadiusBytes.value,
    1024 * 1024,
    4 * 1024 * 1024,
    16 * 1024 * 1024,
  ])).sort((a, b) => a - b)
  for (const radius of radii) {
    uiStringSourceRadiusBytes.value = radius
    await analyzeUiStringSources(undefined, radius)
    if (uiStringSourceCandidates.value.length > 0) {
      useSelectedUiSourcesForWrite()
      break
    }
  }
  await inspectUiStringOrigins()
}

function selectedUiSourceCandidates() {
  if (selectedUiSourceAddresses.value.length === 0) return filteredUiStringSourceCandidates.value
  const selected = new Set(selectedUiSourceAddresses.value)
  return uiStringSourceCandidates.value.filter((candidate) => selected.has(sourceKey(candidate)))
}

async function trackUiStringSources() {
  const value = uiStringNextValue.value.trim()
  if (!value || uiStringSourceCandidates.value.length === 0) return
  uiStringBusy.value = true
  try {
    const controller = backend.getController()
    if (!controller.trackUiStringSources) {
      uiStringSourceTrackResult.value = {
        success: false,
        checked: 0,
        unreadable: 0,
        remaining: 0,
        survivors: [],
        error: 'Methode backend indisponible (mock mode).',
      }
      return
    }
    const result = await controller.trackUiStringSources(selectedUiSourceCandidates(), value)
    uiStringSourceTrackResult.value = result
    uiStringSourceCandidates.value = result.survivors ?? []
    selectedUiSourceAddresses.value = chooseNonOverlappingSources(uiStringSourceCandidates.value).slice(0, uiStringSourceSafeSelectionLimit).map(sourceKey)
    uiStringValue.value = value
  } catch (e) {
    uiStringSourceTrackResult.value = {
      success: false,
      checked: 0,
      unreadable: 0,
      remaining: 0,
      survivors: [],
      error: String(e),
    }
  } finally {
    uiStringBusy.value = false
  }
}

function useUiSourceCandidate(candidate: UiStringSourceCandidate) {
  store.selectCandidate(candidate.address, candidate.type)
  store.writeValue = uiStringValue.value || store.exactScanValue
  selectedCandidateAddresses.value = [candidate.address]
  selectedWriteTargetOverrides.value = {
    [candidate.address]: {
      address: candidate.address,
      type: candidate.type,
      variantLabel: candidate.variantLabel,
    },
  }
  syncSelectedWriteType()
}

function watchUiSourceCandidate(candidate: UiStringSourceCandidate) {
  store.addAddressToWatch(candidate.address, candidate.type)
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function useSelectedUiSourcesForWrite() {
  const selected = new Set(selectedUiSourceAddresses.value)
  const chosen = chooseNonOverlappingSources(uiStringSourceCandidates.value.filter((candidate) => selected.has(sourceKey(candidate))))
  if (chosen.length === 0) return
  selectedCandidateAddresses.value = chosen.map((candidate) => candidate.address)
  selectedWriteTargetOverrides.value = Object.fromEntries(chosen.map((candidate) => [
    candidate.address,
    {
      address: candidate.address,
      type: candidate.type,
      variantLabel: candidate.variantLabel,
    },
  ]))
  const types = Array.from(new Set(chosen.map((candidate) => candidate.type)))
  if (types.length === 1) {
    store.exactScanType = types[0]
  }
  store.selectedCandidateAddress = chosen[0].address
  store.writeValue = uiStringValue.value || store.exactScanValue
}

const candidatePageTotal = computed(() => {
  if (!store.candidatePage) return 1
  if (store.candidatePage.displaySuppressed) return 1
  return Math.max(1, Math.ceil(store.candidatePage.totalCount / store.candidatePage.pageSize))
})
const currentPageCandidates = computed(() => store.candidatePage?.candidates ?? [])
const displayedCandidates = computed(() => currentPageCandidates.value.filter(
  (candidate) => !store.ignoredCandidateAddresses.includes(candidate.address),
))
const selectedCandidateRecords = computed(() => selectedCandidateAddresses.value
  .map((address) => currentPageCandidates.value.find((candidate) => candidate.address === address))
  .filter((candidate): candidate is NonNullable<typeof candidate> => Boolean(candidate)))
const selectedWriteTargets = computed<MemoryWriteTarget[]>(() => selectedCandidateAddresses.value.map((address) => {
  const override = selectedWriteTargetOverrides.value[address]
  if (override) return override
  const record = currentPageCandidates.value.find((candidate) => candidate.address === address)
  return {
    address,
    type: String(record?.type ?? store.exactScanType),
    variantLabel: record?.variantLabel,
  }
}))
const selectedWriteHasVariants = computed(() => selectedWriteTargets.value.some((target) => Boolean(target.variantLabel)))
const writePlan = computed(() => selectedWriteTargets.value.map((target) => {
  const displayValue = store.writeValue.trim()
  const encodedValue = encodedDisplayWriteValue(displayValue, target.variantLabel)
  return {
    ...target,
    displayValue,
    encodedValue,
    mode: target.variantLabel || target.type,
  }
}))
const writeFailures = computed(() => (store.writeResult?.results ?? []).filter((result) => !result.success))
const selectedCandidateTypes = computed(() => Array.from(new Set(
  selectedWriteTargets.value.map((target) => String(target.variantLabel || target.type)),
)))
const selectedWriteType = computed(() => selectedCandidateTypes.value.length === 1
  ? selectedWriteTargets.value[0]?.type ?? store.exactScanType
  : store.exactScanType)
const hasSelectedWriteTargets = computed(() => selectedCandidateAddresses.value.length > 0)
const writeTargetLabel = computed(() => {
  if (!hasSelectedWriteTargets.value) return ''
  const knownCount = selectedCandidateRecords.value.length
  const typeNote = selectedCandidateTypes.value.length === 1
    ? selectedCandidateTypes.value[0]
    : (selectedWriteHasVariants.value ? 'auto source' : 'type choisi')
  return `${selectedCandidateAddresses.value.length} adresse(s) sélectionnée(s) · ${typeNote}${knownCount < selectedCandidateAddresses.value.length ? ' · certaines hors page' : ''}`
})
const canWriteFromPanel = computed(() => hasSelectedWriteTargets.value
  ? Boolean(store.writeValue.trim())
  : store.canWriteSelectedValue)
const writeButtonLabel = computed(() => hasSelectedWriteTargets.value
  ? `Écrire ${selectedCandidateAddresses.value.length}`
  : 'Écrire')
const expertDense = computed(() => store.uiMode === 'expert')
const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
const exactScanButtonLabel = computed(() => hasCandidateContext.value ? 'Nouveau scan' : 'Premier scan')
const unknownGuideReady = computed(() => Boolean(store.unknownSnapshotResult?.success) || hasCandidateContext.value)
const valueTypeOptions = ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64']

const unknownGuideActions = [
  { mode: 'increased', label: 'ça augmente' },
  { mode: 'decreased', label: 'ça diminue' },
  { mode: 'unchanged', label: 'stable' },
  { mode: 'changed', label: 'ça change' },
] as const
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096] // -1 = Auto
const unknownDepthLabel = (mb: number) => (mb === -1 ? 'Auto' : `${mb} Mo`)

function formatNumber(value: number | undefined) {
  return new Intl.NumberFormat('fr-FR').format(value ?? 0)
}

function formatRate(value: number | undefined) {
  return `${formatNumber(Math.round(value ?? 0))}/s`
}

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} Go`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} Mo`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} Ko`
  return `${formatNumber(bytes)} o`
}

function encodedDisplayWriteValue(value: string, variantLabel?: string): string {
  const cleanValue = value.trim().replace(',', '.')
  if (!cleanValue) return '-'
  const multiplier = variantLabel?.match(/\bx\s*(\d+(?:\.\d+)?)\b/i)
  if (!multiplier) return cleanValue
  const numeric = Number(cleanValue)
  const scale = Number(multiplier[1])
  if (!Number.isFinite(numeric) || !Number.isFinite(scale)) return cleanValue
  const encoded = numeric * scale
  return Number.isInteger(encoded) ? String(encoded) : String(encoded)
}

function confidencePercent(confidence: number | undefined): number {
  if (confidence === undefined || confidence === null) return 100
  return Math.round(confidence * 100)
}

function confidenceClass(confidence: number | undefined): string {
  const pct = confidencePercent(confidence)
  if (pct >= 80) return 'conf-high'
  if (pct >= 50) return 'conf-medium'
  return 'conf-low'
}

function useCandidateInAssistant(address: string, type: string) {
  store.selectCandidate(address, type)
  store.searchQuery = `j'utilise la mémoire 0x${address}`
  void store.doSearch()
}

function isCandidateSelected(address: string) {
  return selectedCandidateAddresses.value.includes(address)
}

function toggleCandidateSelection(address: string) {
  if (isCandidateSelected(address)) {
    selectedCandidateAddresses.value = selectedCandidateAddresses.value.filter((item) => item !== address)
    const { [address]: _removed, ...rest } = selectedWriteTargetOverrides.value
    selectedWriteTargetOverrides.value = rest
    syncSelectedWriteType()
    return
  }
  selectedCandidateAddresses.value = [...selectedCandidateAddresses.value, address]
  const { [address]: _removed, ...rest } = selectedWriteTargetOverrides.value
  selectedWriteTargetOverrides.value = rest
  syncSelectedWriteType()
}

function toggleCurrentPageSelection() {
  const pageAddresses = currentPageCandidates.value.map((candidate) => candidate.address)
  const allPageSelected = pageAddresses.length > 0
    && pageAddresses.every((address) => selectedCandidateAddresses.value.includes(address))
  if (allPageSelected) {
    selectedCandidateAddresses.value = selectedCandidateAddresses.value.filter((address) => !pageAddresses.includes(address))
    selectedWriteTargetOverrides.value = Object.fromEntries(
      Object.entries(selectedWriteTargetOverrides.value).filter(([address]) => !pageAddresses.includes(address)),
    )
    syncSelectedWriteType()
    return
  }
  selectedCandidateAddresses.value = Array.from(new Set([...selectedCandidateAddresses.value, ...pageAddresses]))
  selectedWriteTargetOverrides.value = Object.fromEntries(
    Object.entries(selectedWriteTargetOverrides.value).filter(([address]) => !pageAddresses.includes(address)),
  )
  syncSelectedWriteType()
}

function clearCandidateSelection() {
  selectedCandidateAddresses.value = []
  selectedWriteTargetOverrides.value = {}
}

function syncSelectedWriteType() {
  const selected = currentPageCandidates.value.filter((candidate) => selectedCandidateAddresses.value.includes(candidate.address))
  const types = Array.from(new Set(selected.map((candidate) => String(candidate.type))))
  if (types.length === 1) {
    store.exactScanType = types[0]
  }
  if (selected.length > 0) {
    store.selectedCandidateAddress = selected[0].address
  }
}

function useSelectedCandidatesInAssistant() {
  if (selectedCandidateAddresses.value.length === 0) return
  const addresses = selectedCandidateAddresses.value.map((address) => `0x${address}`).join(' ')
  store.searchQuery = `j'utilise ces mémoires ${addresses}`
  void store.doSearch()
}

function writeSelectedCandidates() {
  if (selectedCandidateAddresses.value.length === 0 || !store.writeValue.trim()) return
  if (selectedWriteHasVariants.value) {
    void store.writeSelectedTargets(selectedWriteTargets.value, store.writeValue)
    return
  }
  void store.writeSelectedAddresses(selectedCandidateAddresses.value, selectedWriteType.value, store.writeValue)
}

function writeFromPanel() {
  if (hasSelectedWriteTargets.value) {
    writeSelectedCandidates()
    return
  }
  void store.writeSelectedValue()
}

function watchCandidate(address: string, type: string) {
  store.addAddressToWatch(address, type)
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function watchedCandidate(address: string) {
  return store.watchedAddresses.find((item) => item.address === address)
}

function candidateCurrentValue(address: string): string {
  return watchedCandidate(address)?.value || '-'
}

function candidateReadError(address: string): string {
  return watchedCandidate(address)?.error || ''
}

function readCandidateValue(address: string, type: string) {
  store.addAddressToWatch(address, type)
  void store.refreshWatchedAddress(address)
}

function freezeCandidateCurrent(address: string, type: string) {
  void store.freezeCandidateCurrent(address, type)
}

onMounted(() => {
  if (store.candidatePage) return
  void store.refreshCandidates()
})
</script>

<template>
  <div class="expert-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.expert') }}</h1>
        <p>{{ store.isAttached ? store.processName : store.statusText }}</p>
      </div>
      <div class="header-actions">
        <button class="btn btn-secondary" :disabled="store.scanBusy" @click="store.resetWorkflow()">
          Nouveau scan
        </button>
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
      </div>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      Attache un processus pour utiliser les outils expert.
    </div>

    <template v-else>
      <div v-if="store.scanStatusText" class="scan-status" :class="{ active: store.scanBusy }">
        <div class="scan-status-head">
          <strong>{{ store.scanStatusText }}</strong>
          <div class="scan-status-actions">
            <span>{{ store.scanProgressPercent }}%</span>
            <button
              v-if="store.scanBusy"
              class="btn btn-secondary btn-small"
              type="button"
              @click="store.cancelActiveScan()"
            >
              Annuler
            </button>
          </div>
        </div>
        <div class="progress-track">
          <div class="progress-fill" :style="{ width: `${store.scanProgressPercent}%` }"></div>
        </div>
      </div>

      <div class="summary-grid">
        <div class="stat">
          <span>{{ $t('scan.stored') }}</span>
          <strong>{{ formatNumber(store.candidatePage?.totalCount) }}</strong>
        </div>
        <div class="stat">
          <span>{{ $t('scan.remaining') }}</span>
          <strong>{{ formatNumber(store.nextScanResult?.remaining) }}</strong>
        </div>
        <div class="stat">
          <span>Type</span>
          <strong>{{ store.exactScanType }}</strong>
        </div>
        <div class="stat">
          <span>Adresse</span>
          <strong>{{ store.selectedCandidateAddress ? `0x${store.selectedCandidateAddress}` : '-' }}</strong>
        </div>
      </div>

      <section v-if="store.expertRegionSize || store.expertRegionProtection" class="panel region-context">
        <div class="panel-title">
          <h2>Région active</h2>
          <span>{{ formatBytes(store.expertRegionSize) }}</span>
        </div>
        <div class="metrics">
          <span>Début: {{ store.expertStartAddress || '-' }}</span>
          <span>Fin: {{ store.expertStopAddress || '-' }}</span>
          <span>Protection: {{ store.expertRegionProtection || '-' }}</span>
          <span>État: {{ store.expertRegionState || '-' }}</span>
          <span>Type: {{ store.expertRegionType || '-' }}</span>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.exact') }}</h2>
          <div class="panel-actions">
            <span v-if="store.exactScanResult?.partial">{{ $t('scan.partial') }}</span>
            <button class="btn btn-secondary compact" type="button" :disabled="store.scanBusy" @click="store.resetWorkflow()">
              Nouveau scan
            </button>
          </div>
        </div>
        <div class="controls exact-controls">
          <input
            v-model="store.exactScanValue"
            :placeholder="$t('scan.value')"
            class="input"
            :disabled="store.scanBusy"
            @keyup.enter="store.doExactScan()"
          />
          <select v-model="store.exactScanType" class="input select" :disabled="store.scanBusy">
            <option value="Auto">Auto (multi-type)</option>
            <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
          </select>
          <button class="btn btn-primary" :disabled="!store.exactScanValue.trim() || store.scanBusy" @click="store.doExactScan()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Scan...' : exactScanButtonLabel }}</span>
          </button>
        </div>
        <div v-if="store.inferredExactTypes.length > 0" class="type-suggestions">
          <button
            v-for="item in store.inferredExactTypes"
            :key="String(item.type)"
            class="type-chip"
            type="button"
            :class="{ active: store.exactScanType === String(item.type).replace(/\\s+x100$/i, '') }"
            :title="`${item.confidence} · ${item.reason}`"
            @click="store.useInferredType(String(item.type))"
          >
            <strong>{{ item.type }}</strong>
            <span>{{ item.confidence }}</span>
          </button>
        </div>

        <!-- Mode Expert — Filtres avancés -->
        <div class="expert-toggle">
          <label class="checkbox-label">
            <input type="checkbox" v-model="store.expertModeEnabled" />
            Mode Expert (filtres avancés)
          </label>
        </div>
        <div v-if="store.expertModeEnabled" class="expert-filters">
          <div class="controls expert-address-controls">
            <input
              v-model="store.expertStartAddress"
              class="input"
              placeholder="Adresse début (0x...)"
            />
            <input
              v-model="store.expertStopAddress"
              class="input"
              placeholder="Adresse fin (0x...)"
            />
            <input
              v-model.number="store.expertAlignment"
              type="number"
              min="0"
              step="1"
              class="input"
              placeholder="Alignement (0 = auto)"
            />
          </div>
          <div class="expert-flags">
            <label class="checkbox-label">
              <input type="checkbox" v-model="store.expertWritableOnly" />
              Writable uniquement
            </label>
            <label class="checkbox-label">
              <input type="checkbox" v-model="store.expertExecutableOnly" />
              Exécutable uniquement
            </label>
            <label class="checkbox-label">
              <input type="checkbox" v-model="store.expertCopyOnWriteOnly" />
              Copy-on-write uniquement
            </label>
          </div>
          <p class="hint">
            Astuce : laisse les champs vides pour scanner tout. Alignement 0 active le fast scan automatique.
          </p>
        </div>

        <div v-if="store.exactScanResult" class="metrics">
          <span>{{ $t('scan.matches') }}: {{ formatNumber(store.exactScanResult.matchesFound) }}</span>
          <span>{{ $t('scan.regions') }}: {{ formatNumber(store.exactScanResult.regionsScanned) }}</span>
          <span>{{ $t('scan.stored') }}: {{ formatNumber(store.exactScanResult.candidateStoreSize) }}</span>
          <span v-if="store.exactScanResult.elapsedMs">Temps: {{ formatNumber(store.exactScanResult.elapsedMs) }} ms</span>
          <span v-if="store.exactScanResult.bytesPerSecond">Débit: {{ formatRate(store.exactScanResult.bytesPerSecond) }}</span>
        </div>
        <p v-if="store.exactScanResult?.error" class="error">{{ store.exactScanResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.nextScan') }}</h2>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="store.scanBusy || !store.candidatePage?.totalCount"
            @click="store.undoCandidateScan()"
          >
            Restaurer réduction
          </button>
        </div>
        <div class="controls next-controls">
          <select v-model="store.nextScanMode" class="input select" :disabled="store.scanBusy || !hasCandidateContext">
            <option value="exact">{{ $t('scan.modeExact') }}</option>
            <option value="changed">{{ $t('scan.modeChanged') }}</option>
            <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
            <option value="increased">{{ $t('scan.modeIncreased') }}</option>
            <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
            <option value="delta">{{ $t('scan.modeDelta') }}</option>
          </select>
          <input
            v-model="store.nextScanValue"
            :disabled="store.scanBusy || !hasCandidateContext || (store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta')"
            :placeholder="$t('scan.nextValue')"
            class="input"
            @keyup.enter="store.doNextScan()"
          />
          <button class="btn btn-primary" :disabled="store.scanBusy || !hasCandidateContext" @click="store.doNextScan()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Scan...' : $t('scan.nextScan') }}</span>
          </button>
        </div>
        <div v-if="store.nextScanResult" class="metrics">
          <span>{{ $t('scan.remaining') }}: {{ formatNumber(store.nextScanResult.remaining) }}</span>
          <span>{{ $t('scan.checked') }}: {{ formatNumber(store.nextScanResult.checked) }}</span>
          <span>{{ $t('scan.unreadable') }}: {{ formatNumber(store.nextScanResult.unreadable) }}</span>
          <span v-if="store.nextScanResult.elapsedMs">Temps: {{ formatNumber(store.nextScanResult.elapsedMs) }} ms</span>
          <span v-if="store.nextScanResult.candidatesPerSecond">Débit: {{ formatRate(store.nextScanResult.candidatesPerSecond) }}</span>
        </div>
        <p v-if="store.nextScanResult?.error" class="error">{{ store.nextScanResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('unknown.title') }}</h2>
        </div>
        <div class="controls unknown-controls">
          <select v-model="store.unknownScanType" class="input select" :disabled="store.scanBusy">
            <option value="Auto">Auto (multi-type)</option>
            <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
          </select>
          <label class="checkbox-label compact-toggle">
            <input v-model="store.unknownWritableOnly" type="checkbox" :disabled="store.scanBusy" />
            <span>Writable only</span>
          </label>
          <label class="checkbox-label compact-toggle">
            <input v-model="store.unknownCopyOnWriteOnly" type="checkbox" :disabled="store.scanBusy || !store.unknownWritableOnly" />
            <span>Copy-on-write</span>
          </label>
          <label class="compact-select">
            <span>Profondeur</span>
            <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select" :disabled="store.scanBusy">
              <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
            </select>
          </label>
          <button class="btn btn-secondary" :disabled="store.scanBusy" @click="store.captureUnknownSnapshot()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Capture...' : $t('unknown.capture') }}</span>
          </button>
        </div>
        <div class="unknown-guide">
          <button
            v-for="action in unknownGuideActions"
            :key="action.mode"
            class="btn btn-secondary compact guide-btn"
            type="button"
            :class="{ active: store.unknownScanMode === action.mode }"
            :disabled="store.scanBusy || !unknownGuideReady"
            @click="store.runUnknownGuideStep(action.mode)"
          >
            {{ action.label }}
          </button>
        </div>
        <div v-if="store.unknownSnapshotResult || store.unknownNextScanResult" class="metrics">
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.regions') }}: {{ formatNumber(store.unknownSnapshotResult.regionsCaptured) }}</span>
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.bytes') }}: {{ formatNumber(store.unknownSnapshotResult.bytesCaptured) }}</span>
          <span v-if="store.unknownSnapshotResult?.captureLimitBytes">Limite: {{ formatBytes(store.unknownSnapshotResult.captureLimitBytes) }}</span>
          <span v-if="store.unknownSnapshotResult?.captureLimitReached" class="warning-text">limite atteinte</span>
          <span v-if="store.unknownSnapshotResult?.compressedBytes !== undefined">Compressé: {{ formatBytes(store.unknownSnapshotResult.compressedBytes) }}</span>
          <span v-if="store.unknownSnapshotResult?.mappedStorage">Stockage fichier temporaire</span>
          <span v-if="store.unknownSnapshotResult?.writableOnly">Writable only</span>
          <span v-if="store.unknownSnapshotResult?.copyOnWriteOnly">Copy-on-write</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.matches') }}: {{ formatNumber(store.unknownNextScanResult.matchesFound) }}</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.stored') }}: {{ formatNumber(store.unknownNextScanResult.stored) }}</span>
        </div>
        <div v-if="store.unknownNextScanResult?.typePasses?.length" class="metrics">
          <span v-for="pass in store.unknownNextScanResult.typePasses" :key="pass.type">
            {{ pass.type }}: {{ formatNumber(pass.stored ?? pass.matchesFound ?? 0) }}
          </span>
        </div>
        <div v-if="store.unknownSnapshotResult?.captureLimitReached" class="warning depth-warning">
          <p>
            <strong>Capture limitée</strong> : seulement {{ formatBytes(store.unknownSnapshotResult.bytesCaptured) }} capturés sur une limite de {{ formatBytes(store.unknownSnapshotResult.captureLimitBytes) }}.
          </p>
          <p v-if="(store.unknownSnapshotResult.relevantBytes ?? 0) > (store.unknownSnapshotResult.bytesCaptured ?? 0)">
            Mémoire pertinente totale : {{ formatBytes(store.unknownSnapshotResult.relevantBytes) }}.
            Tu ne couvres que {{ (((store.unknownSnapshotResult.bytesCaptured ?? 0) / (store.unknownSnapshotResult.relevantBytes ?? 1)) * 100).toFixed(1) }}% — la ressource est probablement dans les {{ (100 - (((store.unknownSnapshotResult.bytesCaptured ?? 0) / (store.unknownSnapshotResult.relevantBytes ?? 1)) * 100)).toFixed(0) }}% manquants.
          </p>
          <p v-if="(store.unknownSnapshotResult.suggestedDepthMb ?? 0) > 0">
            <strong>Recommandation</strong> : passe la profondeur à <strong>{{ store.unknownSnapshotResult.suggestedDepthMb }} Mo</strong> (ou <strong>Auto</strong>) puis refais la capture.
          </p>
        </div>
        <div v-else-if="store.unknownSnapshotResult?.autoDepthApplied && (store.unknownSnapshotResult.suggestedDepthMb ?? 0) > 0" class="hint depth-info">
          Mode Auto : profondeur calculée à {{ store.unknownSnapshotResult.suggestedDepthMb }} Mo pour {{ formatBytes(store.unknownSnapshotResult.relevantBytes) }} de mémoire pertinente.
        </div>
        <div v-if="store.unknownGuideSteps.length > 0" class="unknown-timeline">
          <div
            v-for="step in store.unknownGuideSteps"
            :key="step.id"
            class="unknown-step"
            :class="step.status"
          >
            <span>{{ step.time }}</span>
            <strong>{{ step.label }}</strong>
            <em>{{ step.detail }}</em>
          </div>
        </div>
        <p class="hint">
          Capture d'abord, fais varier la ressource, puis indique comment elle a bougé. Stable sert surtout après une première réduction.
        </p>
      </section>

      <section class="panel ui-string-panel">
        <div class="panel-title">
          <h2>Trace UI string</h2>
          <span v-if="uiStringResult">{{ formatNumber(uiStringCandidates.length) }} candidat(s)</span>
        </div>
        <div class="controls ui-string-controls">
          <input
            v-model="uiStringValue"
            class="input"
            placeholder="Texte affiché (ex: 50)"
            :disabled="uiStringBusy || store.scanBusy"
            @keyup.enter="scanUiStrings()"
          />
          <button class="btn btn-primary" :disabled="uiStringBusy || store.scanBusy || !(uiStringValue || store.exactScanValue).trim()" @click="scanUiStrings()">
            <span v-if="uiStringBusy" class="btn-spinner" aria-hidden="true"></span>
            Scanner texte
          </button>
          <input
            v-model="uiStringNextValue"
            class="input"
            placeholder="Nouvelle valeur affichée"
            :disabled="uiStringBusy || uiStringCandidates.length === 0"
            @keyup.enter="trackUiStrings()"
          />
          <button class="btn btn-secondary" :disabled="uiStringBusy || uiStringCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStrings()">
            Filtrer strings
          </button>
          <button class="btn btn-primary" :disabled="uiStringBusy || uiStringSourceCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStringSources()">
            Tracker sources
          </button>
        </div>
        <div class="ui-investigation" :class="{ active: uiStringLiveInvestigation }">
          <div class="scanner-visual" aria-hidden="true">
            <div class="scanner-ring"></div>
            <div class="scanner-sweep"></div>
            <div class="scanner-core"></div>
          </div>
          <div class="investigation-copy">
            <strong>{{ uiStringLiveInvestigation ? 'Enquête armée' : 'Enquête live prête' }}</strong>
            <span>
              {{ uiStringLiveInvestigation
                ? `Snapshot capturé. Modifie la valeur dans SC2, puis clique Arrêter et comparer · ${formatNumber(uiStringInvestigationElapsed)} s`
                : 'Démarre avant de modifier la ressource pour chercher au-delà de la simple string UI.' }}
            </span>
          </div>
          <div class="investigation-steps">
            <span>strings</span>
            <span>sources</span>
            <span>backrefs</span>
          </div>
          <button class="btn compact" :class="uiStringLiveInvestigation ? 'btn-secondary' : 'btn-primary'" type="button" :disabled="uiStringBusy" @click="toggleUiStringLiveInvestigation()">
            {{ uiStringLiveInvestigation ? 'Arrêter et comparer' : 'Démarrer enquête' }}
          </button>
        </div>
        <div v-if="uiStringInvestigationStartResult" class="metrics">
          <span>Fenêtres enquête: {{ formatNumber(uiStringInvestigationStartResult.windows) }}</span>
          <span>Capturé: {{ formatBytes(uiStringInvestigationStartResult.bytesCaptured) }}</span>
          <span v-if="uiStringInvestigationStartResult.probeBlocks">Blocs radar: {{ formatNumber(uiStringInvestigationStartResult.probeBlocks) }}</span>
          <span v-if="uiStringInvestigationStartResult.probeBytesCaptured">Radar: {{ formatBytes(uiStringInvestigationStartResult.probeBytesCaptured) }}</span>
          <span v-if="uiStringInvestigationStartResult.radiusBytes">Rayon: {{ formatBytes(uiStringInvestigationStartResult.radiusBytes) }}</span>
          <span v-if="uiStringInvestigationStartResult.unreadable">Illisibles: {{ formatNumber(uiStringInvestigationStartResult.unreadable) }}</span>
        </div>
        <p v-if="uiStringInvestigationStartResult?.error" class="error">{{ uiStringInvestigationStartResult.error }}</p>
        <div v-if="uiStringInvestigationFinishResult" class="metrics">
          <span>Changements: {{ formatNumber(uiStringInvestigationFinishResult.changesFound) }}</span>
          <span>Octets modifiés: {{ formatNumber(uiStringInvestigationFinishResult.changedBytes) }}</span>
          <span>Fenêtres lues: {{ formatNumber(uiStringInvestigationFinishResult.windowsChecked) }}</span>
          <span v-if="uiStringInvestigationFinishResult.probeBlocksChanged !== undefined">Blocs modifiés: {{ formatNumber(uiStringInvestigationFinishResult.probeBlocksChanged) }}</span>
          <span v-if="uiStringInvestigationFinishResult.globalValueHitsFound !== undefined">Valeurs radar: {{ formatNumber(uiStringInvestigationFinishResult.globalValueHitsFound) }}</span>
          <span v-if="uiStringInvestigationFinishResult.partial" class="warning-text">résultats limités</span>
        </div>
        <p v-if="uiStringInvestigationFinishResult?.error" class="error">{{ uiStringInvestigationFinishResult.error }}</p>
        <div v-if="uiStringInvestigationFinishResult && !uiStringInvestigationFinishResult.error && uiStringInvestigationFinishResult.changesFound === 0 && !(uiStringInvestigationFinishResult.globalValueHits?.length)" class="investigation-empty">
          <strong>Aucun changement capturé dans les fenêtres suivies.</strong>
          <span>
            Ça veut dire que les strings suivies ont été relues, mais que la vraie valeur modifiée n'a pas été retrouvée dans les blocs modifiés.
            Relance l'enquête en indiquant la nouvelle valeur affichée, puis modifie la ressource pendant que l'état est armé.
          </span>
        </div>
        <div v-if="uiStringInvestigationFinishResult?.globalValueHits?.length" class="investigation-hit-summary">
          <strong>Valeurs trouvées dans des blocs modifiés</strong>
          <span>
            Ces adresses sont automatiquement ajoutées et cochées dans Sources numériques. Tu peux les envoyer vers Write, puis tester une écriture/freeze.
          </span>
        </div>
        <div v-if="uiStringInvestigationFinishResult?.changes.length" class="investigation-change-list">
          <div class="source-list-title">
            <strong>Changements pendant l'enquête</strong>
            <span>{{ formatNumber(uiStringInvestigationFinishResult.changes.length) }} piste(s)</span>
          </div>
          <div
            v-for="change in uiStringInvestigationFinishResult.changes.slice(0, 80)"
            :key="`${change.address}:${change.offset}:${change.length}`"
            class="investigation-change-row"
          >
            <code>0x{{ change.address }}</code>
            <span>{{ change.reason || '-' }}</span>
            <span>{{ change.beforeHex || '-' }}</span>
            <strong>{{ change.afterHex || '-' }}</strong>
            <span>{{ change.afterInt32 !== undefined ? `i32 ${formatNumber(change.afterInt32)}` : '-' }}</span>
            <span>{{ change.afterFloat32 !== undefined ? `f32 ${change.afterFloat32.toFixed(3)}` : '-' }}</span>
          </div>
        </div>
        <div class="expert-flags ui-string-flags">
          <label class="checkbox-label">
            <input v-model="uiStringAscii" type="checkbox" :disabled="uiStringBusy" />
            ASCII
          </label>
          <label class="checkbox-label">
            <input v-model="uiStringUtf16" type="checkbox" :disabled="uiStringBusy" />
            UTF-16
          </label>
          <label class="checkbox-label">
            <input v-model="uiStringWritableOnly" type="checkbox" :disabled="uiStringBusy" />
            Writable only
          </label>
          <label class="checkbox-label">
            <input v-model="uiStringBoundary" type="checkbox" :disabled="uiStringBusy" />
            Nombre isolé
          </label>
          <label class="compact-select">
            <span>Rayon sources</span>
            <select v-model.number="uiStringSourceRadiusBytes" class="input select" :disabled="uiStringBusy">
              <option v-for="option in uiStringSourceRadiusOptions" :key="option.value" :value="option.value">
                {{ option.label }}
              </option>
            </select>
          </label>
        </div>
        <div v-if="uiStringResult" class="metrics">
          <span>Matches: {{ formatNumber(uiStringResult.matchesFound) }}</span>
          <span>Régions: {{ formatNumber(uiStringResult.regionsScanned) }}</span>
          <span>Lu: {{ formatBytes(uiStringResult.bytesScanned) }}</span>
          <span v-if="uiStringResult.partial" class="warning-text">limite atteinte</span>
        </div>
        <div v-if="uiStringTrackResult" class="metrics">
          <span>Testés: {{ formatNumber(uiStringTrackResult.checked) }}</span>
          <span>Restants: {{ formatNumber(uiStringTrackResult.remaining) }}</span>
          <span>Illisibles: {{ formatNumber(uiStringTrackResult.unreadable) }}</span>
          <span v-if="uiStringTrackResult.moved">Déplacés: {{ formatNumber(uiStringTrackResult.moved) }}</span>
        </div>
        <div v-if="uiStringSourceResult" class="metrics">
          <span>Sources: {{ formatNumber(uiStringSourceResult.matchesReturned) }}</span>
          <span>Fenêtres lues: {{ formatBytes(uiStringSourceResult.bytesScanned) }}</span>
          <span v-if="uiStringSourceResult.radiusBytes">Rayon: {{ formatBytes(uiStringSourceResult.radiusBytes) }}</span>
          <span v-if="uiStringSourceResult.partial" class="warning-text">résultats limités</span>
        </div>
        <div v-if="uiStringSourceTrackResult" class="metrics">
          <span>Sources testées: {{ formatNumber(uiStringSourceTrackResult.checked) }}</span>
          <span>Sources restantes: {{ formatNumber(uiStringSourceTrackResult.remaining) }}</span>
          <span>Illisibles: {{ formatNumber(uiStringSourceTrackResult.unreadable) }}</span>
          <span v-if="uiStringSourceTrackResult.incompatible">Incompatibles: {{ formatNumber(uiStringSourceTrackResult.incompatible) }}</span>
        </div>
        <p v-if="uiStringResult?.error" class="error">{{ uiStringResult.error }}</p>
        <p v-if="uiStringTrackResult?.error" class="error">{{ uiStringTrackResult.error }}</p>
        <p v-if="uiStringSourceResult?.error" class="error">{{ uiStringSourceResult.error }}</p>
        <p v-if="uiStringSourceTrackResult?.error" class="error">{{ uiStringSourceTrackResult.error }}</p>
        <div v-if="uiStringCandidates.length > 0" class="selection-toolbar">
          <button class="btn btn-secondary compact" type="button" @click="toggleAllUiStringSelection()">
            {{ selectedUiStringAddresses.length === uiStringCandidates.length ? 'Tout décocher' : 'Tout cocher' }}
          </button>
          <button class="btn btn-primary compact" type="button" :disabled="uiStringBusy" @click="analyzeUiStringSources()">
            Analyser sources
          </button>
          <button class="btn btn-primary compact" type="button" :disabled="uiStringBusy" @click="autoInspectUiStrings()">
            Auto origine
          </button>
          <button class="btn btn-secondary compact" type="button" :disabled="uiStringBusy" @click="inspectUiStringOrigins()">
            Backrefs
          </button>
          <span>{{ selectedUiStringAddresses.length || uiStringCandidates.length }} suivi(s) au prochain filtre</span>
        </div>
        <div v-if="uiStringCandidates.length > 0" class="ui-string-list">
          <div v-for="candidate in uiStringCandidates" :key="uiStringKey(candidate)" class="ui-string-row">
            <label class="candidate-check">
              <input
                type="checkbox"
                :checked="isUiStringSelected(candidate)"
                @change="toggleUiStringSelection(candidate)"
              />
            </label>
            <code>0x{{ candidate.address }}</code>
            <span>{{ candidate.encoding }}</span>
            <strong>{{ candidate.text }}</strong>
            <span>{{ candidate.movedFrom ? `+${formatNumber(candidate.movedDistanceBytes)} o` : (candidate.protection || '-') }}</span>
            <span>{{ candidate.memoryType || '-' }}</span>
            <button class="btn btn-primary compact" type="button" @click="analyzeUiStringSources(candidate)">Sources</button>
            <button class="btn btn-secondary compact" type="button" @click="inspectUiStringOrigins(candidate)">Origine</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiStringCandidate(candidate)">Watch</button>
            <button class="btn btn-secondary compact" type="button" @click="useUiStringCandidate(candidate)">Assistant</button>
          </div>
        </div>
        <div v-if="uiStringOriginResult" class="metrics">
          <span>Cluster: {{ uiStringOriginResult.clusterStart ? `0x${uiStringOriginResult.clusterStart}` : '-' }}</span>
          <span>Cibles: {{ formatNumber(uiStringOriginResult.targetCount) }}</span>
          <span>Span: {{ formatBytes(uiStringOriginResult.clusterSpanBytes) }}</span>
          <span v-if="uiStringOriginResult.commonStrideBytes">Stride: {{ formatBytes(uiStringOriginResult.commonStrideBytes) }}</span>
          <span>Backrefs: {{ formatNumber(uiStringOriginResult.pointerRefsFound) }}</span>
          <span>Lu: {{ formatBytes(uiStringOriginResult.bytesScanned) }}</span>
        </div>
        <p v-if="uiStringOriginResult?.error" class="error">{{ uiStringOriginResult.error }}</p>
        <div v-if="uiStringOriginResult?.pointerRefs.length" class="origin-list">
          <div class="source-list-title">
            <strong>Pointeurs vers les strings</strong>
            <span>{{ formatNumber(uiStringOriginResult.pointerRefs.length) }} ref(s)</span>
          </div>
          <div v-for="ref in uiStringOriginResult.pointerRefs.slice(0, 80)" :key="`${ref.address}:${ref.pointsTo}`" class="origin-row">
            <code>0x{{ ref.address }}</code>
            <span>→ 0x{{ ref.pointsTo }}</span>
            <span>{{ ref.distanceToString ? `${formatNumber(ref.distanceToString)} o` : 'exact' }}</span>
            <span>{{ ref.memoryType || '-' }}</span>
            <span>{{ ref.protection || '-' }}</span>
          </div>
        </div>
        <div v-if="uiStringSourceCandidates.length > 0" class="source-list">
          <div class="source-list-title">
            <strong>Sources numériques proches</strong>
            <span>{{ formatNumber(filteredUiStringSourceCandidates.length) }}/{{ formatNumber(uiStringSourceCandidates.length) }} source(s) · {{ formatNumber(selectedUiSourceAddresses.length) }} cochée(s)</span>
            <div class="source-filter-bar">
              <select v-model="uiStringSourceTypeFilter" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option value="all">Tous types</option>
                <option v-for="type in uiStringSourceTypeOptions" :key="type" :value="type">{{ type }}</option>
              </select>
              <select v-model="uiStringSourceVariantFilter" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option value="all">Tous encodages</option>
                <option v-for="variant in uiStringSourceVariantOptions" :key="variant" :value="variant">{{ variant }}</option>
              </select>
              <select v-model.number="uiStringSourceBatchSize" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option v-for="size in uiStringSourceBatchSizeOptions" :key="size" :value="size">{{ size }}/lot</option>
              </select>
            </div>
            <div class="source-actions">
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="previousUiSourceBatch()">
                Prec
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectUiSourceBatch()">
                Lot {{ boundedUiStringSourceBatchIndex + 1 }}/{{ uiStringSourceBatchCount }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="nextUiSourceBatch()">
                Suiv
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectTopUiSources()">
                Top {{ uiStringSourceSafeSelectionLimit }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectAllUiSources()">
                Tout cocher sûr
              </button>
              <button class="btn btn-secondary compact" type="button" @click="clearUiSourceSelection()">
                Tout décocher
              </button>
              <button
                class="btn btn-primary compact"
                type="button"
                :disabled="selectedUiSourceAddresses.length === 0"
                @click="useSelectedUiSourcesForWrite()"
              >
                Envoyer {{ formatNumber(selectedUiSourceAddresses.length) }} vers Write
              </button>
            </div>
          </div>
          <p v-if="selectedUiSourceAddresses.length > 50" class="source-warning">
            Sélection massive : écrire beaucoup d'adresses peut rendre SC2 instable. Teste plutôt par petits paquets.
          </p>
          <div
            v-for="candidate in filteredUiStringSourceCandidates"
            :key="sourceKey(candidate)"
            class="source-row"
          >
            <label class="candidate-check">
              <input
                type="checkbox"
                :checked="isUiSourceSelected(candidate)"
                @change="toggleUiSourceSelection(candidate)"
              />
            </label>
            <code>0x{{ candidate.address }}</code>
            <span>{{ candidate.type }}</span>
            <span>{{ candidate.variantLabel || '-' }}</span>
            <strong>{{ candidate.lastValueNumber }}</strong>
            <span>{{ candidate.trackHits ? `${candidate.trackHits} hit(s)` : `${formatNumber(candidate.distanceBytes)} o` }}</span>
            <button class="btn btn-primary compact" type="button" @click="useUiSourceCandidate(candidate)">Utiliser</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiSourceCandidate(candidate)">Watch</button>
          </div>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>Candidats</h2>
          <span>{{ formatNumber(store.candidatePage?.totalCount) }} · {{ selectedCandidateAddresses.length }} sélectionné(s)</span>
        </div>
        <div class="candidate-toolbar">
          <input
            v-model="store.candidateFilter"
            :placeholder="$t('scan.filterAddress')"
            class="input"
            @input="store.candidatePageIndex = 0; store.refreshCandidates()"
          />
          <button class="btn btn-secondary" :disabled="store.candidatePageIndex === 0" @click="store.previousCandidatePage()">
            {{ $t('scan.previous') }}
          </button>
          <button
            class="btn btn-secondary"
            :disabled="!store.candidatePage || store.candidatePage.displaySuppressed || (store.candidatePageIndex + 1) * store.candidatePageSize >= store.candidatePage.totalCount"
            @click="store.nextCandidatePage()"
          >
            {{ $t('scan.next') }}
          </button>
        </div>
        <div class="selection-toolbar">
          <button class="btn btn-secondary compact" :disabled="store.candidatePage?.displaySuppressed || currentPageCandidates.length === 0" @click="toggleCurrentPageSelection()">
            Sélection page
          </button>
          <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="clearCandidateSelection()">
            Effacer
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0" @click="useSelectedCandidatesInAssistant()">
            Utiliser sélection
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0 || !store.writeValue.trim()" @click="writeSelectedCandidates()">
            Écrire sur sélection
          </button>
        </div>
        <div class="page-info">
          {{ store.candidatePage ? store.candidatePage.pageIndex + 1 : 1 }} / {{ candidatePageTotal }}
        </div>
        <div v-if="store.candidatePage" class="metrics candidate-storage">
          <span>{{ store.candidatePage.fileBacked ? 'Stockage fichier' : 'Stockage RAM' }}</span>
          <span>Fichier: {{ formatBytes(store.candidatePage.candidateStoreBytes) }}</span>
          <span>RAM estimée: {{ formatBytes(store.candidatePage.candidateStoreMemoryBytes) }}</span>
        </div>
        <div v-if="store.candidatePage?.displaySuppressed" class="candidate-suppressed">
          {{ formatNumber(store.candidatePage.totalCount) }} candidats trouvés. Réduis avec un next scan ou filtre une adresse pour afficher une page.
        </div>
        <div class="candidate-list">
          <div
            v-for="match in displayedCandidates"
            :key="match.address"
            class="candidate-row"
            :class="[`candidate-${store.candidateVisualState(match).replace(' ', '-')}`]"
          >
            <label class="candidate-check">
              <input
                type="checkbox"
                :checked="isCandidateSelected(match.address)"
                @change="toggleCandidateSelection(match.address)"
              />
            </label>
            <button class="address-btn" @click="store.selectCandidate(match.address, match.type)">
              0x{{ match.address }}
            </button>
            <div class="candidate-meta">
              <span class="candidate-type">{{ match.type }}</span>
              <span
                v-if="match.confidence !== undefined && match.confidence < 1"
                class="confidence-badge"
                :class="confidenceClass(match.confidence)"
                :title="match.variantLabel"
              >
                {{ confidencePercent(match.confidence) }}%
              </span>
              <span v-if="match.variantLabel" class="variant-label">{{ match.variantLabel }}</span>
              <span class="visual-state">{{ store.candidateVisualState(match) }}</span>
              <span v-if="store.watchedAddresses.some((item) => item.address === match.address)" class="live-dot">watch</span>
            </div>
            <div class="candidate-value" :class="{ error: candidateReadError(match.address) }" :title="candidateReadError(match.address) || match.lastValueHex">
              <span>Valeur</span>
              <strong>{{ candidateCurrentValue(match.address) }}</strong>
            </div>
            <div class="candidate-actions">
              <button class="btn btn-secondary compact" @click="useCandidateInAssistant(match.address, match.type)">
                Utiliser
              </button>
              <button class="btn btn-secondary compact" @click="readCandidateValue(match.address, match.type)">
                Lire
              </button>
              <button class="btn btn-secondary compact" @click="watchCandidate(match.address, match.type)">
                Watch
              </button>
              <button class="btn btn-secondary compact" @click="freezeCandidateCurrent(match.address, match.type)">
                Freeze actuel
              </button>
              <button class="btn btn-secondary compact" @click="store.keepCandidate(match.address)">
                Garder
              </button>
              <button class="btn btn-secondary compact" @click="store.ignoreCandidate(match.address)">
                Ignorer
              </button>
            </div>
          </div>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('write.title') }}</h2>
          <span v-if="store.writeResult">{{ store.writeResult.success ? 'OK' : 'FAIL' }}</span>
        </div>
        <div class="controls write-controls">
          <div v-if="hasSelectedWriteTargets" class="input multi-target-summary" :title="selectedCandidateAddresses.map((address) => `0x${address}`).join(', ')">
            <strong>{{ writeTargetLabel }}</strong>
            <button class="inline-clear" type="button" @click="clearCandidateSelection()">manuel</button>
          </div>
          <input
            v-else
            v-model="store.selectedCandidateAddress"
            class="input"
            :placeholder="$t('write.address')"
            @input="store.updateWriteSafetyWarning()"
            @blur="store.updateWriteSafetyWarning()"
          />
          <select v-model="store.exactScanType" class="input select">
            <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
          </select>
          <input v-model="store.writeValue" class="input" :placeholder="$t('write.value')" @keyup.enter="writeFromPanel()" />
          <button class="btn btn-primary" :disabled="!canWriteFromPanel" @click="writeFromPanel()">
            {{ writeButtonLabel }}
          </button>
          <button class="btn btn-secondary" @click="store.rollbackLastWrite()">
            {{ $t('write.rollback') }}
          </button>
          <label class="freeze-interval-control">
            <span>Freeze</span>
            <select
              v-model.number="store.freezeIntervalMs"
              class="input select"
              @change="store.setFreezeInterval(store.freezeIntervalMs)"
            >
              <option v-for="ms in freezeIntervalPresets" :key="ms" :value="ms">{{ ms }} ms</option>
            </select>
          </label>
          <button class="btn btn-secondary" :disabled="hasSelectedWriteTargets || (store.freezeEnabled ? !store.selectedCandidateAddress : !store.canWriteSelectedValue)" @click="store.toggleFreeze()">
            {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
          </button>
        </div>
        <div v-if="hasSelectedWriteTargets" class="write-plan">
          <div class="write-plan-title">
            <strong>Plan d'écriture</strong>
            <span>{{ writePlan.length }} cible(s) · valeur affichée {{ store.writeValue.trim() || '-' }}</span>
          </div>
          <div class="write-plan-list">
            <div v-for="target in writePlan.slice(0, 12)" :key="`${target.address}:${target.mode}`" class="write-plan-row">
              <code>0x{{ target.address }}</code>
              <span>{{ target.mode }}</span>
              <strong>{{ target.encodedValue }}</strong>
            </div>
          </div>
          <span v-if="writePlan.length > 12" class="muted">+ {{ writePlan.length - 12 }} autre(s) cible(s) avec le même calcul automatique.</span>
        </div>
        <div v-if="store.writeSafetyWarning" class="write-safety">
          <p class="warning">{{ store.writeSafetyWarning }}</p>
          <label class="safety-ack">
            <input v-model="store.writeSafetyAcknowledged" type="checkbox" />
            Je confirme cette écriture mémoire
          </label>
        </div>
        <div v-if="store.writeResult || store.freezeIntervalResult" class="metrics">
          <span v-if="store.writeResult">{{ store.writeResult.bytesWritten }} B</span>
          <span v-if="store.writeResult?.written !== undefined">Écrites: {{ formatNumber(store.writeResult.written) }}/{{ formatNumber(store.writeResult.total) }}</span>
          <span v-if="store.writeResult?.protectionChanged">VirtualProtectEx{{ store.writeResult.protectionChangedCount ? `: ${formatNumber(store.writeResult.protectionChangedCount)}` : '' }}</span>
          <span v-if="store.writeResult?.verified">{{ $t('write.verified') }}</span>
          <span v-if="store.writeResult?.enabled !== undefined">freeze: {{ store.writeResult.enabled ? 'on' : 'off' }}</span>
          <span v-if="store.freezeIntervalResult">intervalle: {{ store.freezeIntervalMs }} ms</span>
        </div>
        <p v-if="store.writeResult?.error" class="error">{{ store.writeResult.error }}</p>
        <div v-if="writeFailures.length" class="write-fail-list">
          <div class="source-list-title">
            <strong>Échecs d'écriture</strong>
            <span>{{ formatNumber(writeFailures.length) }} fail(s)</span>
          </div>
          <div v-for="failure in writeFailures.slice(0, 16)" :key="`${failure.address}:${failure.type}:${failure.variantLabel}`" class="write-fail-row">
            <code>0x{{ failure.address || '-' }}</code>
            <span>{{ failure.variantLabel || failure.type || '-' }}</span>
            <strong>{{ failure.encodedHex || '-' }}</strong>
            <span>{{ failure.error || '-' }}</span>
          </div>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>Watch live</h2>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="store.watchedAddresses.length === 0"
            @click="store.setWatchLiveEnabled(!store.watchLiveEnabled)"
          >
            {{ store.watchLiveEnabled ? 'Arrêter' : 'Démarrer' }}
          </button>
        </div>
        <div v-if="store.watchedAddresses.length === 0" class="hint">Sélectionne un candidat ou clique Watch pour surveiller une adresse.</div>
        <div v-else class="watch-list">
          <div v-for="item in store.watchedAddresses" :key="item.address" class="watch-row" :class="{ changed: item.changed }">
            <code>0x{{ item.address }}</code>
            <span>{{ item.type }}</span>
            <strong>{{ item.value || '-' }}</strong>
            <span v-if="item.previousValue">avant: {{ item.previousValue }}</span>
            <span>{{ item.updatedAt }}</span>
            <button class="btn btn-secondary compact" @click="store.removeAddressFromWatch(item.address)">Retirer</button>
          </div>
        </div>
      </section>

      <section class="panel pointer-chain-panel">
        <div class="panel-title">
          <h2>Pointer Chains <span class="hint-inline">(StarCraft 2 / jeux modernes)</span></h2>
          <span v-if="pointerScanResult">{{ formatNumber(pointerScanResult.chainCount) }} chaine(s)</span>
        </div>
        <p class="hint">
          Pour les jeux modernes (StarCraft 2, etc.), les ressources sont allouees dynamiquement.
          Trouve d'abord l'adresse avec un scan normal, puis utilise le scanner de pointeurs pour
          decouvrir une chaine stable qui survivra aux redemarrages.
        </p>
        <div class="controls pointer-chain-controls">
          <input
            v-model="pointerScanAddress"
            class="input"
            placeholder="Adresse cible (0x...)"
            :disabled="store.scanBusy || !store.isAttached"
          />
          <select v-model="pointerScanValueType" class="input select">
            <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
          </select>
          <input
            v-model.number="pointerScanMaxDepth"
            type="number"
            min="1"
            max="5"
            class="input"
            placeholder="Profondeur"
            title="Nombre de niveaux de dereferencement"
          />
          <input
            v-model.number="pointerScanMaxOffset"
            type="number"
            min="0"
            step="16"
            class="input"
            placeholder="Offset max"
            title="Offset maximum entre pointeur et cible"
          />
          <button
            class="btn btn-primary"
            :disabled="!pointerScanAddress.trim() || store.scanBusy || !store.isAttached"
            @click="runPointerScan()"
          >
            <span v-if="pointerScanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ pointerScanBusy ? 'Scan...' : 'Scanner les pointeurs' }}</span>
          </button>
        </div>
        <div v-if="pointerScanResult" class="metrics">
          <span>Chaines: {{ formatNumber(pointerScanResult.chainCount) }}</span>
          <span>Pointeurs scannes: {{ formatNumber(pointerScanResult.pointersScanned) }}</span>
          <span>Bytes: {{ formatBytes(pointerScanResult.bytesScanned) }}</span>
          <span v-if="pointerScanResult.elapsedMs">Temps: {{ formatNumber(pointerScanResult.elapsedMs) }} ms</span>
          <span v-if="pointerScanResult.partial">Resultat partiel</span>
        </div>
        <div v-if="pointerScanResult?.chains?.length" class="pointer-chain-list">
          <div
            v-for="(chain, index) in pointerScanResult.chains.slice(0, 20)"
            :key="index"
            class="pointer-chain-row"
            :class="{ selected: selectedPointerChainIndex === index }"
          >
            <label class="candidate-check">
              <input
                type="radio"
                :value="index"
                v-model.number="selectedPointerChainIndex"
              />
            </label>
            <div class="pointer-chain-info">
              <strong>{{ chain.label }}</strong>
              <span class="chain-depth">profondeur {{ chain.depth }}</span>
            </div>
            <div class="pointer-chain-actions">
              <button class="btn btn-secondary compact" @click="testPointerChain(chain)">
                Tester
              </button>
              <button
                class="btn btn-secondary compact"
                @click="usePointerChainAsCandidate(chain)"
              >
                Utiliser
              </button>
              <button
                class="btn btn-primary compact"
                @click="savePointerChain(chain)"
              >
                Sauver profil
              </button>
            </div>
          </div>
        </div>
        <p v-if="pointerScanResult?.error" class="error">{{ pointerScanResult.error }}</p>
        <p v-if="pointerResolveResult" class="hint">
          Resolution : {{ pointerResolveResult.success ? 'OK 0x' + pointerResolveResult.finalAddress : 'ECHEC ' + pointerResolveResult.error }}
        </p>
      </section>

      <section v-if="expertDense" class="panel">
        <div class="panel-title">
          <h2>Journal utilisateur</h2>
          <span>{{ store.actionLog.length }} entrée(s)</span>
        </div>
        <div class="action-log">
          <div v-for="entry in store.actionLog.slice(0, 18)" :key="entry.id" class="action-entry" :class="entry.status">
            <span>{{ entry.time }}</span>
            <strong>{{ entry.title }}</strong>
            <em>{{ entry.detail }}</em>
          </div>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.expert-view {
  max-width: 1180px;
  padding: 24px 32px;
}

.header,
.panel-title,
.candidate-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.header {
  margin-bottom: 18px;
}

.header-actions,
.panel-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p,
.panel-title span,
.page-info {
  color: var(--text-dim);
  font-size: 12px;
}

.empty-state {
  padding: 32px;
  color: var(--text-dim);
  text-align: center;
}

.summary-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(150px, 1fr));
  gap: 8px;
  margin-bottom: 14px;
}

.scan-status {
  min-height: 54px;
  margin-bottom: 12px;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.scan-status.active {
  border-color: rgba(122, 162, 247, 0.55);
}

.scan-status-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 8px;
}

.scan-status-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.scan-status-head span {
  color: var(--text-secondary);
  font-size: 12px;
}

.scan-status-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.btn-small {
  min-height: 26px;
  padding: 4px 10px;
  font-size: 12px;
}

.btn-spinner {
  display: inline-block;
  width: 14px;
  height: 14px;
  margin-right: 6px;
  border: 2px solid currentColor;
  border-right-color: transparent;
  border-radius: 999px;
  animation: spin 0.75s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.progress-track {
  overflow: hidden;
  width: 100%;
  height: 6px;
  border-radius: 999px;
  background: var(--bg-primary);
}

.progress-fill {
  height: 100%;
  border-radius: inherit;
  background: var(--accent);
  transition: width 0.2s ease;
}

.stat,
.panel {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.stat {
  min-height: 78px;
  padding: 12px;
}

.stat span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.stat strong {
  display: block;
  overflow: hidden;
  margin-top: 8px;
  color: var(--text-primary);
  font-size: 18px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.panel {
  margin-bottom: 12px;
  padding: 12px;
}

.panel-title {
  margin-bottom: 10px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.controls {
  display: grid;
  gap: 8px;
}

.exact-controls {
  grid-template-columns: 1fr 120px auto;
}

.expert-toggle {
  margin-top: 10px;
}

.checkbox-label {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 12px;
  cursor: pointer;
}

.checkbox-label input[type="checkbox"] {
  width: 14px;
  height: 14px;
  accent-color: var(--accent);
}

.expert-filters {
  margin-top: 8px;
  padding: 8px;
  border: 1px dashed var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.expert-address-controls {
  grid-template-columns: 1fr 1fr 140px;
  margin-bottom: 6px;
}

.expert-flags {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}

.hint {
  margin-top: 6px;
  color: var(--text-dim);
  font-size: 11px;
  line-height: 1.4;
}

.type-suggestions {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
  margin-top: 7px;
}

.type-chip {
  display: inline-flex;
  min-width: 0;
  align-items: center;
  gap: 6px;
  min-height: 26px;
  padding: 4px 8px;
  border: 1px solid var(--border);
  border-radius: 999px;
  background: rgba(36, 40, 59, 0.72);
  color: var(--text-secondary);
  cursor: pointer;
  font-size: 11px;
}

.type-chip strong {
  color: var(--text-primary);
  font-size: 11px;
  line-height: 1;
}

.type-chip span {
  overflow: hidden;
  max-width: 58px;
  color: var(--text-dim);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.type-chip:hover,
.type-chip.active {
  border-color: rgba(122, 162, 247, 0.55);
  background: rgba(122, 162, 247, 0.12);
  color: var(--accent);
}

.type-chip.active strong {
  color: var(--accent);
}

.next-controls {
  grid-template-columns: 150px 1fr auto;
}

.unknown-controls {
  grid-template-columns: 120px minmax(110px, auto) minmax(120px, auto) minmax(150px, auto) auto;
  align-items: center;
}

.compact-toggle {
  min-height: 32px;
}

.compact-select {
  display: grid;
  grid-template-columns: auto minmax(90px, 1fr);
  gap: 6px;
  align-items: center;
  color: var(--text-dim);
  font-size: 12px;
}

.warning-text {
  color: var(--warning);
}

.unknown-guide {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.guide-btn.active {
  background: rgba(122, 162, 247, 0.18);
  color: var(--accent);
}

.unknown-timeline {
  display: grid;
  gap: 6px;
  margin-top: 9px;
}

.unknown-step {
  display: grid;
  grid-template-columns: 70px 96px 1fr;
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.unknown-step strong {
  color: var(--text-primary);
}

.unknown-step em {
  overflow: hidden;
  color: var(--text-secondary);
  font-style: normal;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.unknown-step.capture {
  border-color: rgba(122, 162, 247, 0.42);
}

.unknown-step.compare,
.unknown-step.refine {
  border-color: rgba(158, 206, 106, 0.38);
}

.unknown-step.error {
  border-color: rgba(247, 118, 142, 0.45);
}

.ui-string-controls {
  grid-template-columns: minmax(130px, 1fr) auto minmax(150px, 1fr) auto auto;
  align-items: center;
}

.ui-string-flags {
  margin-top: 8px;
}

.ui-investigation {
  display: grid;
  grid-template-columns: 54px minmax(180px, 1fr) auto auto;
  gap: 12px;
  align-items: center;
  min-height: 64px;
  margin-top: 10px;
  padding: 9px 12px;
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 6px;
  background: rgba(13, 17, 32, 0.46);
}

.ui-investigation.active {
  border-color: rgba(158, 206, 106, 0.46);
  box-shadow: inset 0 0 0 1px rgba(158, 206, 106, 0.08);
}

.scanner-visual {
  position: relative;
  width: 44px;
  height: 44px;
}

.scanner-ring,
.scanner-sweep,
.scanner-core {
  position: absolute;
  inset: 0;
  border-radius: 50%;
}

.scanner-ring {
  border: 1px solid rgba(122, 162, 247, 0.44);
  background:
    linear-gradient(90deg, rgba(122, 162, 247, 0.22) 1px, transparent 1px),
    linear-gradient(rgba(122, 162, 247, 0.22) 1px, transparent 1px);
  background-size: 11px 11px;
}

.scanner-sweep {
  background: conic-gradient(from 0deg, rgba(158, 206, 106, 0.74), rgba(158, 206, 106, 0.08) 42deg, transparent 80deg);
  opacity: 0.28;
}

.ui-investigation.active .scanner-sweep {
  animation: scanner-spin 1.1s linear infinite;
  opacity: 0.72;
}

.scanner-core {
  inset: 17px;
  background: var(--accent);
  box-shadow: 0 0 12px rgba(122, 162, 247, 0.62);
}

.ui-investigation.active .scanner-core {
  background: var(--success);
  box-shadow: 0 0 14px rgba(158, 206, 106, 0.78);
}

.investigation-copy {
  display: grid;
  gap: 3px;
}

.investigation-copy strong {
  color: var(--text-primary);
  font-size: 13px;
}

.investigation-copy span,
.investigation-steps span {
  color: var(--text-dim);
  font-size: 12px;
}

.investigation-steps {
  display: flex;
  gap: 6px;
  align-items: center;
}

.investigation-steps span {
  padding: 4px 7px;
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 4px;
  background: var(--bg-primary);
}

.ui-investigation.active .investigation-steps span {
  border-color: rgba(158, 206, 106, 0.28);
  color: var(--success);
}

@keyframes scanner-spin {
  to {
    transform: rotate(360deg);
  }
}

.investigation-change-list {
  display: grid;
  gap: 4px;
  margin-top: 10px;
}

.investigation-empty {
  display: grid;
  gap: 4px;
  margin-top: 10px;
  padding: 10px 12px;
  border: 1px solid rgba(255, 199, 119, 0.28);
  border-radius: 4px;
  background: rgba(255, 199, 119, 0.06);
  color: var(--text-dim);
  font-size: 13px;
}

.investigation-empty strong {
  color: var(--warning);
}

.investigation-hit-summary {
  display: grid;
  gap: 4px;
  margin-top: 10px;
  padding: 10px 12px;
  border: 1px solid rgba(158, 206, 106, 0.24);
  border-radius: 4px;
  background: rgba(158, 206, 106, 0.06);
  color: var(--text-dim);
  font-size: 13px;
}

.investigation-hit-summary strong {
  color: var(--success);
}

.investigation-change-row {
  display: grid;
  grid-template-columns: minmax(140px, 1fr) minmax(90px, 120px) minmax(110px, 1fr) minmax(110px, 1fr) 86px 86px;
  gap: 8px;
  align-items: center;
  min-height: 32px;
  padding: 6px 8px;
  border: 1px solid rgba(158, 206, 106, 0.18);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.investigation-change-row code,
.investigation-change-row span,
.investigation-change-row strong {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.investigation-change-row code {
  color: var(--text-primary);
}

.investigation-change-row strong {
  color: var(--success);
}

.ui-string-list {
  display: flex;
  max-height: 220px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.ui-string-row {
  display: grid;
  grid-template-columns: 28px minmax(140px, 1fr) 58px 70px 92px 78px auto auto auto auto;
  gap: 8px;
  align-items: center;
  min-height: 38px;
  padding: 7px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.ui-string-row code {
  overflow: hidden;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ui-string-row strong {
  color: var(--accent);
}

.source-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 10px;
}

.origin-list {
  display: flex;
  max-height: 190px;
  flex-direction: column;
  gap: 4px;
  margin-top: 10px;
  overflow-y: auto;
}

.source-list-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  flex-wrap: wrap;
  color: var(--text-secondary);
  font-size: 12px;
}

.source-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  justify-content: flex-end;
}

.source-filter-bar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  justify-content: flex-end;
}

.compact-input {
  min-height: 28px;
  max-width: 150px;
  padding: 4px 8px;
  font-size: 12px;
}

.source-warning {
  margin: 2px 0 6px;
  color: var(--warning);
  font-size: 12px;
}

.source-row {
  display: grid;
  grid-template-columns: 28px minmax(140px, 1fr) 74px minmax(120px, 1fr) 70px 80px auto auto;
  gap: 8px;
  align-items: center;
  min-height: 38px;
  padding: 7px 8px;
  border: 1px solid rgba(158, 206, 106, 0.18);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.source-row code {
  overflow: hidden;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.source-row strong {
  color: var(--success);
}

.origin-row {
  display: grid;
  grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) 72px 78px 92px;
  gap: 8px;
  align-items: center;
  min-height: 32px;
  padding: 6px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.origin-row code {
  overflow: hidden;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-controls {
  grid-template-columns: minmax(170px, 1fr) 110px minmax(140px, 1fr) auto auto minmax(118px, auto) auto;
}

.freeze-interval-control {
  display: grid;
  grid-template-columns: auto 76px;
  gap: 6px;
  align-items: center;
  color: var(--text-dim);
  font-size: 12px;
}

.multi-target-summary {
  display: flex;
  gap: 8px;
  align-items: center;
  justify-content: space-between;
  overflow: hidden;
}

.multi-target-summary strong {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.inline-clear {
  flex: 0 0 auto;
  border: 0;
  background: transparent;
  color: var(--accent);
  cursor: pointer;
  font-size: 12px;
}

.write-plan {
  display: grid;
  gap: 8px;
  margin-top: 10px;
  padding: 10px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: rgba(13, 17, 32, 0.42);
}

.write-plan-title,
.write-plan-row {
  display: grid;
  grid-template-columns: minmax(150px, 1fr) minmax(90px, 140px) minmax(90px, 140px);
  gap: 10px;
  align-items: center;
}

.write-plan-title {
  color: var(--text-dim);
  font-size: 12px;
}

.write-plan-title strong {
  color: var(--text-primary);
  font-size: 13px;
}

.write-plan-list {
  display: grid;
  gap: 4px;
}

.write-plan-row {
  min-height: 28px;
  padding: 4px 6px;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.write-plan-row code {
  overflow: hidden;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-plan-row strong {
  color: var(--success);
}

.write-fail-list {
  display: grid;
  gap: 4px;
  margin-top: 10px;
}

.write-fail-row {
  display: grid;
  grid-template-columns: minmax(140px, 1fr) minmax(110px, 150px) minmax(110px, 160px) minmax(180px, 2fr);
  gap: 8px;
  align-items: center;
  min-height: 32px;
  padding: 6px 8px;
  border: 1px solid rgba(255, 117, 127, 0.22);
  border-radius: 4px;
  background: rgba(255, 117, 127, 0.05);
  color: var(--text-dim);
  font-size: 12px;
}

.write-fail-row code,
.write-fail-row span,
.write-fail-row strong {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-fail-row code {
  color: var(--text-primary);
}

.write-fail-row strong {
  color: var(--warning);
}

.input {
  min-width: 0;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
}

.input:disabled {
  opacity: 0.45;
}

.btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.compact {
  padding: 5px 9px;
  font-size: 12px;
}

.metrics {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-top: 9px;
  color: var(--text-dim);
  font-size: 12px;
}

.warning {
  margin-top: 8px;
  color: var(--warning);
  font-size: 12px;
}

.write-safety {
  margin-top: 8px;
  padding: 9px 10px;
  border: 1px solid color-mix(in srgb, var(--warning) 45%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--warning) 8%, var(--bg-secondary));
}

.write-safety .warning {
  margin-top: 0;
}

.safety-ack {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  margin-top: 8px;
  color: var(--text-secondary);
  font-size: 12px;
}

.candidate-storage {
  margin-top: 0;
  margin-bottom: 8px;
}

.error {
  margin-top: 7px;
  color: var(--error);
  font-size: 12px;
}

.candidate-toolbar {
  display: grid;
  grid-template-columns: 1fr auto auto;
  margin-bottom: 6px;
}

.selection-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 6px;
}

.page-info {
  margin-bottom: 6px;
  text-align: right;
}

.candidate-suppressed {
  margin-bottom: 8px;
  padding: 10px 12px;
  border: 1px solid rgba(122, 162, 247, 0.28);
  border-radius: 6px;
  background: rgba(122, 162, 247, 0.08);
  color: var(--text-secondary);
  font-size: 12px;
}

.candidate-list {
  display: flex;
  max-height: 260px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.candidate-row {
  display: grid;
  grid-template-columns: 28px minmax(160px, 1fr) minmax(210px, 1fr) minmax(120px, 0.6fr) minmax(390px, auto);
  gap: 8px;
  align-items: center;
  padding: 7px 8px;
  border: 1px solid transparent;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.candidate-meta,
.candidate-value,
.candidate-actions {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 6px;
}

.candidate-meta {
  overflow: hidden;
}

.candidate-actions {
  justify-content: flex-end;
}

.candidate-value {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 11px;
}

.candidate-value strong {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.candidate-value.error strong {
  color: var(--error);
}

.candidate-actions .btn {
  flex: 0 0 auto;
  white-space: nowrap;
}

.candidate-très-probable {
  border-color: color-mix(in srgb, var(--success) 35%, var(--border));
}

.candidate-à-vérifier {
  border-color: color-mix(in srgb, var(--warning) 35%, var(--border));
}

.candidate-faible,
.candidate-ignoré {
  opacity: 0.65;
}

.candidate-gardé {
  border-color: color-mix(in srgb, var(--accent) 55%, var(--border));
}

.candidate-check {
  display: flex;
  align-items: center;
  justify-content: center;
}

.candidate-check input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}

.address-btn {
  overflow: hidden;
  border: none;
  background: transparent;
  color: var(--text-primary);
  cursor: pointer;
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  text-align: left;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.address-btn:hover {
  color: var(--accent);
}

.candidate-type {
  color: var(--text-dim);
  font-size: 11px;
}

.confidence-badge {
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 10px;
  font-weight: 600;
  font-family: 'Segoe UI', sans-serif;
  white-space: nowrap;
}

.confidence-badge.conf-high {
  background: rgba(76, 175, 80, 0.18);
  color: #66bb6a;
}

.confidence-badge.conf-medium {
  background: rgba(255, 193, 7, 0.18);
  color: #ffa726;
}

.confidence-badge.conf-low {
  background: rgba(244, 67, 54, 0.18);
  color: #ef5350;
}

.variant-label {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 10px;
  font-style: italic;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.visual-state,
.live-dot {
  color: var(--text-dim);
  font-size: 11px;
}

.live-dot {
  color: var(--success);
}

.watch-list,
.action-log {
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.watch-row,
.action-entry {
  display: grid;
  grid-template-columns: 150px 80px minmax(90px, 1fr) minmax(90px, 1fr) 70px auto;
  gap: 8px;
  align-items: center;
  padding: 7px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.watch-row.changed {
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.watch-row code,
.watch-row strong {
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
}

.action-entry {
  grid-template-columns: 70px minmax(160px, 0.9fr) minmax(200px, 1.5fr);
}

.action-entry strong {
  color: var(--text-primary);
}

.action-entry em {
  overflow: hidden;
  color: var(--text-dim);
  font-style: normal;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.action-entry.success {
  border-color: color-mix(in srgb, var(--success) 30%, var(--border));
}

.action-entry.warning {
  border-color: color-mix(in srgb, var(--warning) 30%, var(--border));
}

.action-entry.error {
  border-color: color-mix(in srgb, var(--error) 30%, var(--border));
}

.hint-inline {
  color: var(--text-dim);
  font-size: 11px;
  font-weight: normal;
}

.pointer-chain-controls {
  grid-template-columns: minmax(170px, 1fr) 110px 100px 110px auto;
}

.pointer-chain-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 8px;
}

.pointer-chain-row {
  display: grid;
  grid-template-columns: 28px 1fr auto;
  gap: 8px;
  align-items: center;
  padding: 7px 8px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.pointer-chain-row.selected {
  border-color: color-mix(in srgb, var(--accent) 55%, var(--border));
}

.pointer-chain-info {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.pointer-chain-info strong {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.chain-depth {
  color: var(--text-dim);
  font-size: 10px;
}

.pointer-chain-actions {
  display: flex;
  gap: 4px;
}

@media (max-width: 980px) {
  .summary-grid,
  .exact-controls,
  .next-controls,
  .unknown-controls,
  .ui-string-controls,
  .ui-investigation,
  .investigation-change-row,
  .ui-string-row,
  .origin-row,
  .source-row,
  .write-plan-title,
  .write-plan-row,
  .write-controls,
  .candidate-toolbar,
  .candidate-row,
  .pointer-chain-controls {
    grid-template-columns: 1fr;
  }
}
</style>
