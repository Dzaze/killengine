<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useAppStore } from '@/stores/app'
import {
  backend,
  type AobScanResult,
  type AobSignatureResult,
  type BackwardDisassemblyResult,
  type CandidateFieldTestResult,
  type CodePatchResult,
  type CodePatchSuggestion,
  type CodePatchSuggestionResult,
  type MemoryWriteTarget,
  type PointerChainInfo,
  type PointerChainResolveResult,
  type PointerScanResult,
  type StableLocatorSuggestion,
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

import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import type { RiskLevel } from '@/components/expert/risk'
import RegionPanel from '@/components/expert/RegionPanel.vue'
import NextScanPanel from '@/components/expert/NextScanPanel.vue'
import WatchLivePanel from '@/components/expert/WatchLivePanel.vue'
import GroupScanPanel from '@/components/expert/GroupScanPanel.vue'
import PointerChainWatchPanel from '@/components/expert/PointerChainWatchPanel.vue'
import ActionLogPanel from '@/components/expert/ActionLogPanel.vue'
import InjectionPanel from '@/components/expert/InjectionPanel.vue'
import { formatNumber, formatRate, formatBytes } from '@/utils/format'
import { valueTypeOptions } from '@/utils/valueTypes'

const store = useAppStore()
const selectedCandidateAddresses = ref<string[]>([])
const selectedWriteTargetOverrides = ref<Record<string, MemoryWriteTarget>>({})
const uiStringSourcesPanelRef = ref<HTMLElement | null>(null)
const writePanelRef = ref<HTMLElement | null>(null)

// Phase 14 — Pointer Chains
const pointerScanAddress = ref('')
const pointerScanValueType = ref('Int32')
const pointerScanMaxDepth = ref(3)
const pointerScanMaxOffset = ref(0x1000)
const pointerScanResult = ref<PointerScanResult | null>(null)
const pointerScanBusy = ref(false)
const pointerResolveResult = ref<PointerChainResolveResult | null>(null)
const selectedPointerChainIndex = ref<number>(-1)

// Suggestion de chaîne de pointeurs après une écriture confirmée sur une seule
// adresse : évite de repasser manuellement par le panneau Pointer Chains.
const stableLocatorResult = ref<StableLocatorSuggestion | null>(null)
const stableLocatorBusy = ref(false)
const stableLocatorForAddress = ref('')

// Tenue live du freeze BP : BreakpointFreezeManager collecte deja hits/
// rewrites/errors, mais rien ne les affichait avant l'arret. Sondage leger
// (1s) pendant que le freeze BP est actif, arrete des qu'il ne l'est plus.
const breakpointFreezeStats = ref<Record<string, unknown> | null>(null)
let breakpointFreezeStatsTimer: ReturnType<typeof setInterval> | null = null

function stopBreakpointFreezeStatsPolling() {
  if (breakpointFreezeStatsTimer !== null) {
    clearInterval(breakpointFreezeStatsTimer)
    breakpointFreezeStatsTimer = null
  }
}

async function pollBreakpointFreezeStats() {
  const controller = backend.getController()
  if (!controller.getBreakpointFreezeStats) return
  breakpointFreezeStats.value = await controller.getBreakpointFreezeStats()
}

watch(() => store.breakpointFreezeEnabled, (enabled) => {
  stopBreakpointFreezeStatsPolling()
  if (enabled) {
    void pollBreakpointFreezeStats()
    breakpointFreezeStatsTimer = setInterval(() => { void pollBreakpointFreezeStats() }, 1000)
  } else {
    breakpointFreezeStats.value = null
  }
})

// AOB signatures — base du futur trainer engine.
const aobPattern = ref('')
const aobExecutableOnly = ref(true)
const aobImageOnly = ref(true)
const aobMaxResults = ref(200)
const aobBusy = ref(false)
const aobResult = ref<AobScanResult | null>(null)
const aobStabilizeBusy = ref(false)
const aobStabilizeResult = ref<Record<string, unknown> | null>(null)
const aobSignatureBusy = ref(false)
const aobSignatureResult = ref<AobSignatureResult | null>(null)
const disassembleBackwardBusy = ref(false)
const disassembleBackwardResult = ref<BackwardDisassemblyResult | null>(null)
const testCandidateFieldsBusy = ref(false)
const testCandidateFieldsResult = ref<CandidateFieldTestResult | null>(null)
// Message affiché quand generateAobSignatureFromHit() a délibérément SAUTÉ le
// scan auto-enchaîné parce que le pattern stable est trop faible (level
// "weak") pour être fiable — le pattern reste pré-rempli dans aobPattern, le
// bouton "Scanner AOB" manuel reste disponible si l'utilisateur veut quand même.
const aobAutoScanSkippedReason = ref('')
const codePatchAddress = ref('')
const codePatchBytes = ref('90 90')
const codePatchBusy = ref(false)
const codePatchResult = ref<CodePatchResult | null>(null)
const codePatchSuggestBusy = ref(false)
const codePatchSuggestionResult = ref<CodePatchSuggestionResult | null>(null)
// Suggestion "Forcer une valeur" en attente de saisie (needsValueInput) et
// valeur tapée par l'utilisateur pour elle — séparés de codePatchBytes tant
// que la valeur n'a pas été appliquée, pour ne jamais écraser silencieusement
// des bytes déjà choisis manuellement.
const valueOverrideSuggestion = ref<CodePatchSuggestion | null>(null)
const valueOverrideInput = ref('')
const valueOverrideError = ref('')
// "Forcer une valeur (hook)" : marche même quand la source de l'écriture est
// un registre (donc sans immédiat à substituer par valueOverrideSuggestion
// ci-dessus) — installe un trampoline + redirige le site via
// forceWriteInstructionValue. hit gardé pour ré-afficher le RIP ciblé.
const forceHookTargetHit = ref<Record<string, unknown> | null>(null)
const forceHookValueInput = ref('')
const forceHookBusy = ref(false)
const forceHookResult = ref<Record<string, unknown> | null>(null)
const codePatchProfileName = ref('')
const codePatchProfilePatchName = ref('')
const codePatchProfileDescription = ref('')
const codePatchProfileBusy = ref(false)
const codePatchProfileResult = ref<Record<string, unknown> | null>(null)
const codePatchTrainerFlowBusy = ref(false)

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
const findWhatWritesResult = ref<Record<string, unknown> | null>(null)
const findWhatWritesBusy = ref(false)
const findWhatWritesAcknowledged = ref(false)
const findWhatWritesTimeoutMs = ref(7000)
// Alternative à Find What Writes qui ne passe pas par le canal de debug Win32
// (PAGE_GUARD + handler injecté) — utile quand un autre débogueur tient déjà
// ce canal, mais moins précis (granularité page de 4 Ko, hits rapprochés
// potentiellement fusionnés).
const pageGuardResult = ref<Record<string, unknown> | null>(null)
const pageGuardBusy = ref(false)
const structureProbeResult = ref<Record<string, unknown> | null>(null)
const structureCaptureA = ref<StructureProbeRow[] | null>(null)
const structureCaptureB = ref<StructureProbeRow[] | null>(null)
const structureCaptureAName = ref('')
const structureCaptureBName = ref('')
const structureTemplateName = ref('')
const selectedFindWhatWritesRip = ref('')
const uiStringCandidates = ref<UiStringCandidate[]>([])
const uiStringSourceCandidates = ref<UiStringSourceCandidate[]>([])
const selectedUiStringAddresses = ref<string[]>([])
const selectedUiSourceAddresses = ref<string[]>([])
const uiStringLiveTexts = ref<Record<string, {
  current: string
  previous: string
  changed: boolean
  error: string
  updatedAt: string
}>>({})
const uiStringTextLiveEnabled = ref(false)
const uiStringTextLiveRefreshing = ref(false)
const uiStringLiveInvestigation = ref(false)
const uiStringLiveStartedAt = ref<number | null>(null)
const uiStringInvestigationElapsed = ref(0)
const uiStringInvestigationStartResult = ref<UiStringInvestigationStartResult | null>(null)
const uiStringInvestigationFinishResult = ref<UiStringInvestigationFinishResult | null>(null)
let uiStringInvestigationTimer: ReturnType<typeof setInterval> | null = null
let uiStringTextLiveTimer: ReturnType<typeof setInterval> | null = null
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
const findWhatWritesTimeoutOptions = [3000, 5000, 7000, 10000, 15000]

interface IntelligentCandidate {
  key: string
  address: string
  type: string
  variantLabel?: string
  score: number
  scorePercent: number
  currentValue: string
  reasons: string[]
  source: UiStringSourceCandidate
}

interface StructureProbeRow {
  offset: number
  address: string
  type?: string
  value?: unknown
  valueText?: string
  rawHex?: string
  int32?: number
  float32?: number
  marker: string
}

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
  if (uiStringTextLiveTimer) {
    clearInterval(uiStringTextLiveTimer)
    uiStringTextLiveTimer = null
  }
  stopBreakpointFreezeStatsPolling()
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
  const profileName = window.prompt('Nom du profil :', cleanTrainerName(store.processName || 'Jeu cible', 'Jeu cible'))
  if (!profileName) return
  const targetName = window.prompt('Nom de la cible :', 'Ressource')
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

async function watchPointerChain(chain: PointerChainInfo) {
  await store.addWatchedPointerChain(
    { module: chain.module, baseOffset: chain.baseOffset, offsets: chain.offsets },
    pointerScanValueType.value,
    chain.label || `Chaine 0x${pointerScanAddress.value}`,
  )
}

async function suggestStableLocator(addressHex: string) {
  if (!addressHex.trim()) return
  stableLocatorBusy.value = true
  stableLocatorResult.value = null
  stableLocatorForAddress.value = addressHex
  try {
    const controller = backend.getController()
    if (controller.suggestStableLocatorForAddress) {
      stableLocatorResult.value = await controller.suggestStableLocatorForAddress(addressHex, {})
    } else {
      stableLocatorResult.value = { success: false, chainCount: 0, error: 'Methode backend indisponible (mock mode).' }
    }
  } catch (e) {
    stableLocatorResult.value = { success: false, chainCount: 0, error: String(e) }
  } finally {
    stableLocatorBusy.value = false
  }
}

function saveStableLocator() {
  if (stableLocatorResult.value?.bestChain) {
    // savePointerChain() sauvegarde avec le type actuellement affiché dans le
    // panneau Pointer Chains ; on l'aligne sur le type réellement écrit avant.
    pointerScanValueType.value = store.exactScanType
    void savePointerChain(stableLocatorResult.value.bestChain)
  }
}

function bookmarkPointerChain(chain: PointerChainInfo) {
  store.addWorkspaceBookmark({
    kind: 'pointer',
    label: chain.label || `Pointer chain ${chain.depth}`,
    address: pointerScanAddress.value,
    type: pointerScanValueType.value,
    note: `profondeur ${chain.depth}`,
    payload: {
      chain,
      targetAddress: pointerScanAddress.value,
      maxDepth: pointerScanMaxDepth.value,
      maxOffset: pointerScanMaxOffset.value,
      resolved: pointerResolveResult.value?.success ? pointerResolveResult.value.finalAddress : undefined,
    },
  })
}

async function scanAobSignature() {
  const pattern = aobPattern.value.trim()
  if (!pattern) return
  aobBusy.value = true
  aobResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.scanAobPattern) {
      aobResult.value = { success: false, matches: [], error: 'Methode backend indisponible.' }
      return
    }
    aobResult.value = await controller.scanAobPattern(pattern, {
      executableOnly: aobExecutableOnly.value,
      imageOnly: aobImageOnly.value,
      maxResults: aobMaxResults.value,
    })
  } catch (e) {
    aobResult.value = { success: false, matches: [], error: String(e) }
  } finally {
    aobBusy.value = false
  }
}

async function scanAobPatternCandidate(pattern: string, maxResults = 1000): Promise<AobScanResult> {
  const controller = backend.getController()
  if (!controller.scanAobPattern) {
    return { success: false, matches: [], error: 'Methode backend indisponible.' }
  }
  return controller.scanAobPattern(pattern, {
    executableOnly: aobExecutableOnly.value,
    imageOnly: aobImageOnly.value,
    maxResults,
  })
}

async function stabilizeSelectedAobSignature() {
  const address = codePatchAddress.value.trim() || selectedFindWhatWritesRip.value.trim()
  if (!address) return
  aobStabilizeBusy.value = true
  aobStabilizeResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.generateAobSignature) {
      aobStabilizeResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }

    const tested: Array<Record<string, unknown>> = []
    const patterns: string[] = []
    const stablePattern = codePatchSuggestionResult.value?.stableAobPattern?.trim()
    if (stablePattern) patterns.push(stablePattern)

    for (const beforeBytes of [0, 4, 8, 12]) {
      for (const length of [16, 24, 32, 48, 64]) {
        const signature = await controller.generateAobSignature(address, { beforeBytes, length })
        if (signature.success && signature.pattern && !patterns.includes(signature.pattern)) {
          patterns.push(signature.pattern)
        }
      }
    }

    let best: { pattern: string, scan: AobScanResult } | null = null
    for (const pattern of patterns) {
      const scan = await scanAobPatternCandidate(pattern, 1000)
      const matchesFound = Number(scan.matchesFound ?? scan.matches?.length ?? 0)
      tested.push({
        pattern,
        matchesFound,
        patternBytes: scan.patternBytes,
        success: scan.success,
        partial: scan.partial,
        error: scan.error,
      })
      if (scan.success && matchesFound === 1) {
        best = { pattern, scan }
        break
      }
      if (scan.success && matchesFound > 0 && (!best || matchesFound < Number(best.scan.matchesFound ?? Number.MAX_SAFE_INTEGER))) {
        best = { pattern, scan }
      }
    }

    if (best) {
      aobPattern.value = best.pattern
      aobResult.value = best.scan
    }
    const matchesFound = Number(best?.scan.matchesFound ?? best?.scan.matches?.length ?? 0)
    aobStabilizeResult.value = {
      success: Boolean(best && matchesFound === 1),
      pattern: best?.pattern ?? '',
      matchesFound,
      tested,
      error: best && matchesFound !== 1
        ? `Aucune signature unique. Meilleure piste: ${formatNumber(matchesFound)} match(es).`
        : (!best ? 'Aucune signature exploitable générée.' : ''),
    }
  } catch (e) {
    aobStabilizeResult.value = { success: false, error: String(e) }
  } finally {
    aobStabilizeBusy.value = false
  }
}

async function generateAobSignatureFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  aobSignatureBusy.value = true
  aobSignatureResult.value = null
  aobAutoScanSkippedReason.value = ''
  codePatchSuggestBusy.value = true
  codePatchSuggestionResult.value = null
  valueOverrideSuggestion.value = null
  valueOverrideInput.value = ''
  valueOverrideError.value = ''
  forceHookTargetHit.value = null
  forceHookResult.value = null
  codePatchAddress.value = rip
  store.memoryPreviewAddress = rip
  try {
    const controller = backend.getController()
    if (!controller.generateAobSignature) {
      aobSignatureResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }
    const result = await controller.generateAobSignature(rip, {
      beforeBytes: 0,
      length: 24,
    })
    aobSignatureResult.value = result
    let stablePatternIsWeak = false
    if (controller.suggestCodePatches) {
      const suggestionResult = await controller.suggestCodePatches(rip, { maxBytes: 16 })
      codePatchSuggestionResult.value = suggestionResult
      const firstSafe = suggestionResult.suggestions?.find((suggestion) => !suggestion.risky)
      if (suggestionResult.success && firstSafe) {
        codePatchBytes.value = firstSafe.bytesText
      }
      if (suggestionResult.success && suggestionResult.stableAobPattern) {
        aobPattern.value = suggestionResult.stableAobPattern
        // Le pattern "stable" vient du decodage d'UNE seule instruction : pour
        // un mov [mem], reg typique, il ne reste souvent que 2-3 octets fixes
        // (opcode + ModRM) une fois les offsets/registres wildcardes. Un scan
        // executable+image avec un pattern aussi court remonte des centaines
        // de matches sans rapport — pas une vraie signature. On ne lance pas
        // le scan auto dans ce cas, on prévient l'utilisateur pourquoi.
        stablePatternIsWeak = suggestionResult.signatureQuality?.level === 'weak'
      }
    } else {
      codePatchSuggestionResult.value = { success: false, suggestions: [], error: 'Methode backend indisponible.' }
    }
    if (!aobPattern.value.trim() && result.success && result.pattern) {
      aobPattern.value = result.pattern
    }
    if (aobPattern.value.trim()) {
      if (stablePatternIsWeak) {
        aobAutoScanSkippedReason.value =
          "Signature trop faible pour lancer le scan automatiquement (peu d'octets fixes sur cette seule instruction — risque élevé de multi-match). " +
          'Le pattern est pré-rempli ci-dessous : élargis-le (plus de contexte autour de l\'instruction) ou clique "Scanner AOB" si tu veux quand même essayer.'
      } else {
        await scanAobSignature()
      }
      void nextTick(() => {
        document.querySelector('.aob-panel')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
      })
    }
  } catch (e) {
    aobSignatureResult.value = { success: false, error: String(e) }
    codePatchSuggestionResult.value = { success: false, suggestions: [], error: String(e) }
  } finally {
    aobSignatureBusy.value = false
    codePatchSuggestBusy.value = false
  }
}

async function disassembleBackwardFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  disassembleBackwardBusy.value = true
  disassembleBackwardResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.disassembleBackward) {
      disassembleBackwardResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }
    disassembleBackwardResult.value = await controller.disassembleBackward(rip, {})
  } catch (e) {
    disassembleBackwardResult.value = { success: false, error: String(e) }
  } finally {
    disassembleBackwardBusy.value = false
  }
}

// Teste automatiquement lequel des champs candidats tient réellement (écrit
// une valeur test, attend, relit, restaure) — pas besoin d'avoir cliqué
// "Désassembler en amont" d'abord, testCandidateFieldsAsync refait la
// résolution des champs en interne à partir du RIP et de l'adresse écrite.
async function testCandidateFieldsFromHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  const watchedAddress = String(hit.address ?? '').trim()
  if (!rip || !watchedAddress) return
  testCandidateFieldsBusy.value = true
  testCandidateFieldsResult.value = null
  try {
    testCandidateFieldsResult.value = await store.executeCandidateFieldTest(rip, watchedAddress)
  } catch (e) {
    testCandidateFieldsResult.value = { success: false, error: String(e) }
  } finally {
    testCandidateFieldsBusy.value = false
  }
}

function findWhatWritesHitKey(hit: Record<string, unknown>) {
  return `${hit.instructionPointer}:${hit.threadId}:${hit.address}`
}

function isSelectedFindWhatWritesHit(hit: Record<string, unknown>) {
  return selectedFindWhatWritesRip.value !== '' && selectedFindWhatWritesRip.value === String(hit.instructionPointer ?? '').trim()
}

function previewFindWhatWritesHit(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  codePatchAddress.value = rip
  store.memoryPreviewAddress = rip
  void store.readMemoryPreview(rip, 128)
  void nextTick(() => {
    document.querySelector('.aob-panel')?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  })
}

async function copyFindWhatWritesRip(hit: Record<string, unknown>) {
  const rip = String(hit.instructionPointer ?? '').trim()
  if (!rip) return
  selectedFindWhatWritesRip.value = rip
  await navigator.clipboard?.writeText(`0x${rip}`)
}

function useAobMatchAddress(address: string) {
  store.memoryPreviewAddress = address
  codePatchAddress.value = address
  void store.readMemoryPreview(address, 128)
}

function bookmarkAobMatch(match: Record<string, unknown>) {
  const address = String(match.address ?? '').replace(/^0x/i, '').toUpperCase()
  if (!address) return
  store.addWorkspaceBookmark({
    kind: 'aob',
    label: `AOB 0x${address}`,
    address,
    type: 'Code',
    note: `${String(match.module || match.memoryType || 'code')} ${match.moduleOffset ? `+0x${String(match.moduleOffset)}` : ''}`.trim(),
    payload: {
      aobPattern: aobPattern.value.trim(),
      module: match.module,
      moduleOffset: match.moduleOffset,
      protection: match.protection,
      executableOnly: aobExecutableOnly.value,
      imageOnly: aobImageOnly.value,
    },
  })
}

function bookmarkCurrentCodePatch() {
  const address = codePatchAddress.value.trim().replace(/^0x/i, '').toUpperCase()
  if (!address) return
  const suggestion = selectedPatchSuggestion()
  const quality = currentAobQuality()
  store.addWorkspaceBookmark({
    kind: 'aob',
    label: codePatchProfilePatchName.value.trim() || `Patch 0x${address}`,
    address,
    type: 'CodePatch',
    value: codePatchBytes.value.trim(),
    note: codePatchSuggestionResult.value?.disassembly || suggestion?.description || codePatchProfileDescription.value.trim(),
    payload: {
      patchBytes: codePatchBytes.value.trim(),
      aobPattern: (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim(),
      originalBytes: codePatchResult.value?.originalBytes || codePatchSuggestionResult.value?.bytes || '',
      disassembly: codePatchSuggestionResult.value?.disassembly || '',
      riskLevel: suggestion?.riskLevel || '',
      profileName: codePatchProfileName.value.trim(),
      signatureQuality: quality,
      signatureScore: quality?.score,
      signatureLevel: quality?.level,
      signatureWarning: quality?.warning,
      signatureFixedBytes: quality?.fixedBytes,
      signatureWildcardBytes: quality?.wildcardBytes,
      signatureUniqueFixedBytes: quality?.uniqueFixedBytes,
      signatureFixedRatio: quality?.fixedRatio,
      trainerSafe: quality?.trainerSafe,
      signatureMatches: Number(aobResult.value?.matchesFound ?? 0) || undefined,
    },
  })
}

async function selectAobPatchAddress(address: string) {
  codePatchAddress.value = address
  store.memoryPreviewAddress = address
  await suggestSelectedCodePatches()
}

function useCodePatchSuggestion(suggestion: CodePatchSuggestion) {
  if (suggestion.needsValueInput) {
    valueOverrideSuggestion.value = suggestion
    valueOverrideInput.value = ''
    valueOverrideError.value = ''
    return
  }
  valueOverrideSuggestion.value = null
  codePatchBytes.value = suggestion.bytesText
}

function parseValueOverrideInput(text: string): bigint | null {
  const trimmed = text.trim()
  if (!trimmed) return null
  try {
    return BigInt(trimmed)
  } catch {
    return null
  }
}

function applyValueOverrideSuggestion() {
  const suggestion = valueOverrideSuggestion.value
  valueOverrideError.value = ''
  if (!suggestion || suggestion.valueOffset == null || !suggestion.valueSize) return
  const originalHex = (codePatchSuggestionResult.value?.bytes || suggestion.bytesText).replace(/\s+/g, '')
  const originalBytes = originalHex.match(/../g)?.map((byte) => parseInt(byte, 16)) ?? []
  if (originalBytes.length < suggestion.valueOffset + suggestion.valueSize) {
    valueOverrideError.value = "Bytes d'instruction insuffisants pour appliquer la valeur."
    return
  }
  const parsed = parseValueOverrideInput(valueOverrideInput.value)
  if (parsed === null) {
    valueOverrideError.value = 'Valeur invalide (entier décimal ou 0x hexadécimal attendu).'
    return
  }
  // Tronque a la largeur du champ immediat (modulo 2^(size*8), les BigInt
  // negatifs se masquent en complement a deux) plutot que de rejeter
  // silencieusement : le comportement est le meme qu'un patch manuel "je
  // sais ce que je fais", avec un avertissement explicite si la valeur
  // demandee ne rentrait pas telle quelle dans le champ.
  const widthBits = BigInt(suggestion.valueSize * 8)
  const mask = (1n << widthBits) - 1n
  const truncated = parsed & mask
  const minSigned = -(1n << (widthBits - 1n))
  const maxUnsigned = (1n << widthBits) - 1n
  if (parsed < minSigned || parsed > maxUnsigned) {
    valueOverrideError.value = `Valeur hors plage pour un champ de ${suggestion.valueSize} octet(s) (tronquée à 0x${truncated.toString(16)}). Corrige la valeur si ce n'est pas voulu.`
  }
  const patched = [...originalBytes]
  for (let i = 0; i < suggestion.valueSize; ++i) {
    patched[suggestion.valueOffset + i] = Number((truncated >> BigInt(i * 8)) & 0xffn)
  }
  codePatchBytes.value = patched.map((byte) => byte.toString(16).padStart(2, '0').toUpperCase()).join(' ')
}

function selectForceHookTarget(hit: Record<string, unknown>) {
  forceHookTargetHit.value = hit
  forceHookValueInput.value = ''
  forceHookResult.value = null
}

async function applyForceHookValue() {
  const hit = forceHookTargetHit.value
  const suggestion = codePatchSuggestionResult.value
  if (!hit || !suggestion?.memBaseRegister || !forceHookValueInput.value.trim()) return
  forceHookBusy.value = true
  forceHookResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.forceWriteInstructionValue) {
      forceHookResult.value = { success: false, error: 'Méthode backend indisponible.' }
      return
    }
    forceHookResult.value = await controller.forceWriteInstructionValue(
      String(hit.instructionPointer ?? ''),
      Number(suggestion.instructionLength ?? 0),
      suggestion.memBaseRegister,
      Number(suggestion.memDisplacement ?? 0),
      store.exactScanType,
      forceHookValueInput.value.trim(),
    )
  } catch (e) {
    forceHookResult.value = { success: false, error: String(e) }
  } finally {
    forceHookBusy.value = false
  }
}

function cleanTrainerName(value: string, fallback: string) {
  const cleaned = value
    .replace(/\.[^.]+$/, '')
    .replace(/[^a-z0-9_-]+/gi, '_')
    .replace(/^_+|_+$/g, '')
  return cleaned || fallback
}

function defaultTrainerProfileName() {
  return cleanTrainerName(store.processName || 'Trainer', 'Trainer')
}

function defaultPatchNameFromHit(hit: Record<string, unknown>) {
  const moduleName = cleanTrainerName(String(hit.module || aobSignatureResult.value?.module || 'Patch'), 'Patch')
  const offset = String(hit.moduleOffset || aobSignatureResult.value?.moduleOffset || selectedFindWhatWritesRip.value || '0').toUpperCase()
  return `${moduleName}_${offset}`
}

function selectedPatchSuggestion() {
  const patchBytes = codePatchBytes.value.trim()
  return codePatchSuggestionResult.value?.suggestions?.find((suggestion) => suggestion.bytesText === patchBytes)
}

function currentAobQuality() {
  return codePatchSuggestionResult.value?.signatureQuality
    || aobResult.value?.signatureQuality
    || aobSignatureResult.value?.signatureQuality
}

function aobQualityBlocksTrainer() {
  const quality = currentAobQuality()
  if (!quality) return ''
  const score = Number(quality.score ?? 0)
  const fixedBytes = Number(quality.fixedBytes ?? 0)
  if (fixedBytes < 3 || score < 35) {
    return `Signature AOB trop faible (${score}/100, ${fixedBytes} octet(s) fixe(s)). Allonge la signature ou régénère une AOB plus stable.`
  }
  return ''
}

async function suggestSelectedCodePatches() {
  const address = codePatchAddress.value.trim()
  if (!address) return
  codePatchSuggestBusy.value = true
  codePatchSuggestionResult.value = null
  valueOverrideSuggestion.value = null
  valueOverrideInput.value = ''
  valueOverrideError.value = ''
  try {
    const controller = backend.getController()
    if (!controller.suggestCodePatches) {
      codePatchSuggestionResult.value = { success: false, suggestions: [], error: 'Methode backend indisponible.' }
      return
    }
    const result = await controller.suggestCodePatches(address, { maxBytes: 16 })
    codePatchSuggestionResult.value = result
    const firstSafe = result.suggestions?.find((suggestion) => !suggestion.risky)
    if (result.success && firstSafe) {
      codePatchBytes.value = firstSafe.bytesText
    }
    if (result.success && result.stableAobPattern) {
      aobPattern.value = result.stableAobPattern
    }
  } catch (e) {
    codePatchSuggestionResult.value = { success: false, suggestions: [], error: String(e) }
  } finally {
    codePatchSuggestBusy.value = false
  }
}

async function applySelectedCodePatch() {
  const address = codePatchAddress.value.trim()
  const bytes = codePatchBytes.value.trim()
  if (!address || !bytes) return
  if (!await store.confirmRiskAction('patch', 'Patch code', `Adresse 0x${address.replace(/^0x/i, '')}, bytes ${bytes}.`)) return
  codePatchBusy.value = true
  codePatchResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.applyCodePatch) {
      codePatchResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }
    codePatchResult.value = await controller.applyCodePatch(address, bytes, { verify: true })
  } catch (e) {
    codePatchResult.value = { success: false, error: String(e) }
  } finally {
    codePatchBusy.value = false
  }
}

async function restoreSelectedCodePatch() {
  const address = codePatchAddress.value.trim()
  if (!address) return
  codePatchBusy.value = true
  codePatchResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.restoreCodePatch) {
      codePatchResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }
    codePatchResult.value = await controller.restoreCodePatch(address)
  } catch (e) {
    codePatchResult.value = { success: false, error: String(e) }
  } finally {
    codePatchBusy.value = false
  }
}

async function saveSelectedCodePatchProfile() {
  const profileName = codePatchProfileName.value.trim()
  const patchName = codePatchProfilePatchName.value.trim()
  const address = codePatchAddress.value.trim()
  const pattern = (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim()
  const patchBytes = codePatchBytes.value.trim()
  if (!profileName || !patchName || !address || !pattern || !patchBytes) return
  const qualityError = aobQualityBlocksTrainer()
  if (qualityError) {
    codePatchProfileResult.value = {
      success: false,
      profileName,
      patchName,
      error: qualityError,
    }
    return
  }

  codePatchProfileBusy.value = true
  codePatchProfileResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.saveProfileCodePatch) {
      codePatchProfileResult.value = { success: false, error: 'Methode backend indisponible.' }
      return
    }
    codePatchProfileResult.value = await controller.saveProfileCodePatch(
      profileName,
      patchName,
      address,
      pattern,
      patchBytes,
      {
        originalBytes: codePatchResult.value?.originalBytes || codePatchSuggestionResult.value?.bytes || '',
        disassembly: codePatchSuggestionResult.value?.disassembly || '',
        riskLevel: selectedPatchSuggestion()?.riskLevel || '',
        description: codePatchProfileDescription.value.trim(),
        signatureQuality: currentAobQuality(),
      },
    )
  } catch (e) {
    codePatchProfileResult.value = { success: false, error: String(e) }
  } finally {
    codePatchProfileBusy.value = false
  }
}

async function saveTrainerPatchFromHit(hit: Record<string, unknown>) {
  if (codePatchTrainerFlowBusy.value) return
  codePatchTrainerFlowBusy.value = true
  codePatchProfileResult.value = null
  try {
    await generateAobSignatureFromHit(hit)
    if (Number(aobResult.value?.matchesFound ?? 0) !== 1) {
      await stabilizeSelectedAobSignature()
    }

    const patchBytes = codePatchBytes.value.trim()
    const pattern = (codePatchSuggestionResult.value?.stableAobPattern || aobPattern.value).trim()
    if (!patchBytes || !pattern || !codePatchAddress.value.trim()) {
      codePatchProfileResult.value = {
        success: false,
        error: 'Analyse incomplète : patch, adresse ou AOB stable manquant.',
      }
      return
    }

    if (!codePatchProfileName.value.trim()) {
      codePatchProfileName.value = defaultTrainerProfileName()
    }
    if (!codePatchProfilePatchName.value.trim()) {
      codePatchProfilePatchName.value = defaultPatchNameFromHit(hit)
    }
    if (!codePatchProfileDescription.value.trim()) {
      const suggestion = selectedPatchSuggestion()
      codePatchProfileDescription.value = [
        codePatchSuggestionResult.value?.disassembly || 'Patch issu Find What Writes',
        suggestion?.label ? `Suggestion: ${suggestion.label}` : '',
        hit.address ? `Cible observée: 0x${hit.address}` : '',
      ].filter(Boolean).join(' | ')
    }

    const matchesFound = Number(aobResult.value?.matchesFound ?? 0)
    const qualityError = aobQualityBlocksTrainer()
    if (qualityError) {
      codePatchProfileResult.value = {
        success: false,
        profileName: codePatchProfileName.value.trim(),
        patchName: codePatchProfilePatchName.value.trim(),
        error: qualityError,
      }
      return
    }
    if (!aobResult.value?.success || matchesFound !== 1) {
      codePatchProfileResult.value = {
        success: false,
        profileName: codePatchProfileName.value.trim(),
        patchName: codePatchProfilePatchName.value.trim(),
        error: matchesFound === 0
          ? 'Signature AOB introuvable : ajuste le pattern avant de sauver le trainer.'
          : `Signature AOB non unique (${formatNumber(matchesFound)} matches) : sauvegarde bloquée pour éviter un patch dangereux.`,
      }
      return
    }

    await saveSelectedCodePatchProfile()
  } finally {
    codePatchTrainerFlowBusy.value = false
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

function localNowTime(): string {
  return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function formatSaveFileTime(value?: string): string {
  if (!value) return '-'
  const date = new Date(value)
  if (Number.isNaN(date.getTime())) return value
  return date.toLocaleString('fr-FR')
}

function saveFileName(path: string): string {
  return path.split(/[\\/]/).filter(Boolean).pop() || path
}

function discoverSaveFilesFromExpert() {
  void store.discoverSaveFiles(50)
}

function readSaveFileFromExpert(path: string) {
  void store.readSaveFileText(path, 65536)
}

function previewHexToBytes(hex: string): number[] {
  return hex
    .trim()
    .split(/\s+/)
    .map((chunk) => Number.parseInt(chunk, 16))
    .filter((byte) => Number.isFinite(byte) && byte >= 0 && byte <= 255)
}

function readInt32Le(bytes: number[], offset: number) {
  const value = (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) | 0
  return value
}

function readFloat32Le(bytes: number[], offset: number) {
  const buffer = new ArrayBuffer(4)
  const view = new DataView(buffer)
  for (let i = 0; i < 4; i += 1) view.setUint8(i, bytes[offset + i] ?? 0)
  return view.getFloat32(0, true)
}

function displayedNumericValue() {
  const raw = (uiStringNextValue.value || uiStringValue.value || store.exactScanValue).trim().replace(',', '.')
  const value = Number(raw)
  return Number.isFinite(value) ? value : null
}

async function analyzeStructureAroundSource(candidate: UiStringSourceCandidate) {
  const address = addressNumber(candidate.address)
  if (!Number.isFinite(address)) return
  const base = Math.max(0, address - 128)
  const targetValue = displayedNumericValue()
  structureProbeResult.value = null
  try {
    const controller = backend.getController()
    if (controller.analyzeStructureMemory) {
      const result = await controller.analyzeStructureMemory(base.toString(16).toUpperCase(), 256)
      if (result.success !== true) {
        structureProbeResult.value = { success: false, error: String(result.error || 'Analyse structure impossible.') }
        return
      }
      const fields = Array.isArray(result.fields) ? result.fields as Array<Record<string, unknown>> : []
      const rows = fields
        .map((field): StructureProbeRow => {
          const fieldAddress = addressNumber(String(field.address ?? ''))
          const offset = Number(field.offset ?? 0)
          const value = field.value
          const markerParts: string[] = []
          if (fieldAddress === address) markerParts.push('source')
          if (targetValue !== null) {
            const numeric = Number(value)
            if (Number.isFinite(numeric) && Math.abs(numeric - targetValue) < 0.001) markerParts.push('valeur affichée')
            if (Number.isFinite(numeric) && [10, 100, 1000, 4096, 65536].some((scale) => Math.trunc(numeric) === Math.trunc(targetValue * scale))) {
              markerParts.push('fixed-point')
            }
          }
          return {
            offset: fieldAddress === 0 ? offset - 128 : fieldAddress - address,
            address: String(field.address ?? '').toUpperCase(),
            type: String(field.type ?? ''),
            value,
            valueText: String(field.valueText ?? ''),
            rawHex: String(field.rawHex ?? ''),
            marker: markerParts.join(' · '),
          }
        })
        .filter((row) => row.marker || Math.abs(row.offset) <= 32)
        .slice(0, 160)
      structureProbeResult.value = {
        success: true,
        base: String(result.baseAddress ?? base.toString(16).toUpperCase()),
        address: candidate.address,
        rows,
        rowCount: rows.length,
        fieldCount: result.fieldCount,
      }
      return
    }

    const preview = await store.readMemoryPreviewByMode(base.toString(16).toUpperCase(), 256)
    if (!preview.success && !preview.partial) {
      structureProbeResult.value = { success: false, error: preview.error || 'Lecture structure impossible.' }
      return
    }
    const bytes = previewHexToBytes(preview.hex)
    const rows: StructureProbeRow[] = []
    for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
      const rowAddress = base + offset
      const int32 = readInt32Le(bytes, offset)
      const float32 = readFloat32Le(bytes, offset)
      const markers: string[] = []
      if (rowAddress === address) markers.push('source')
      if (targetValue !== null && int32 === Math.trunc(targetValue)) markers.push('i32 affiché')
      if (targetValue !== null && Math.abs(float32 - targetValue) < 0.001) markers.push('f32 affiché')
      if (targetValue !== null && [10, 100, 1000, 4096, 65536].some((scale) => int32 === Math.trunc(targetValue * scale))) {
        markers.push('fixed-point')
      }
      if (markers.length > 0 || (Math.abs(rowAddress - address) <= 32 && int32 !== 0)) {
        rows.push({
          offset: rowAddress - address,
          address: rowAddress.toString(16).toUpperCase(),
          int32,
          float32,
          marker: markers.join(' · '),
        })
      }
    }
    structureProbeResult.value = {
      success: true,
      base: base.toString(16).toUpperCase(),
      address: candidate.address,
      rows,
      rowCount: rows.length,
    }
  } catch (e) {
    structureProbeResult.value = { success: false, error: String(e) }
  }
}

function structureRowCanBecomeTrainer(row: StructureProbeRow): boolean {
  const type = String(row.type ?? '')
  return Boolean(row.address && row.value !== undefined && /^(Int|UInt|Float)/.test(type))
}

function createTrainerFromStructureRow(row: StructureProbeRow) {
  if (!structureRowCanBecomeTrainer(row)) return
  const feature = store.createTrainerFeature({
    name: `Struct ${row.type} 0x${row.address}`,
    action: 'write',
    address: row.address,
    valueType: String(row.type ?? 'Int32'),
    value: String(row.value ?? ''),
  })
  if (feature) store.activeView = 'trainer'
}

function bookmarkStructureRow(row: StructureProbeRow) {
  store.addWorkspaceBookmark({
    kind: 'structure_field',
    label: `Struct ${row.type || 'field'} 0x${row.address}`,
    address: row.address,
    type: row.type,
    value: String(row.value ?? row.int32 ?? ''),
    note: row.valueText || row.marker || '',
    payload: {
      offset: row.offset,
      rawHex: row.rawHex,
      marker: row.marker,
    },
  })
}

function cloneStructureRows() {
  return structureProbeRows.value.map((row) => ({ ...row }))
}

function captureStructure(slot: 'A' | 'B') {
  const rows = cloneStructureRows()
  if (rows.length === 0) return
  const label = `0x${String(structureProbeResult.value?.address ?? structureProbeResult.value?.base ?? '')} · ${new Date().toLocaleTimeString('fr-FR')}`
  if (slot === 'A') {
    structureCaptureA.value = rows
    structureCaptureAName.value = label
  } else {
    structureCaptureB.value = rows
    structureCaptureBName.value = label
  }
}

function saveCurrentStructureTemplate() {
  const rows = structureProbeRows.value
  if (rows.length === 0) return
  const baseAddress = String(structureProbeResult.value?.base ?? structureProbeResult.value?.address ?? '').replace(/^0x/i, '').toUpperCase()
  const template = store.saveStructureTemplate({
    name: structureTemplateName.value || `Structure 0x${String(structureProbeResult.value?.address ?? baseAddress)}`,
    baseAddress,
    size: Number(structureProbeResult.value?.bytesRead ?? 256),
    fields: rows
      .filter((row) => row.type)
      .map((row) => ({
        offset: row.offset,
        type: String(row.type ?? ''),
        label: row.marker || String(row.type ?? ''),
        note: row.valueText || '',
        sampleValue: String(row.value ?? ''),
        rawHex: row.rawHex,
      })),
  })
  if (template) {
    structureTemplateName.value = ''
    store.activeView = 'settings'
  }
}

function decodeUiStringBytes(candidate: UiStringCandidate, bytes: number[]): string {
  const chars: string[] = []
  const maxChars = 32
  if (candidate.encoding === 'utf16') {
    for (let i = 0; i + 1 < bytes.length && chars.length < maxChars; i += 2) {
      const code = bytes[i] | (bytes[i + 1] << 8)
      if (code === 0) break
      if (code < 32 || code > 126) break
      chars.push(String.fromCharCode(code))
    }
  } else {
    for (const byte of bytes) {
      if (chars.length >= maxChars) break
      if (byte === 0) break
      if (byte < 32 || byte > 126) break
      chars.push(String.fromCharCode(byte))
    }
  }
  return chars.join('')
}

function uiStringLiveState(candidate: UiStringCandidate) {
  return uiStringLiveTexts.value[uiStringKey(candidate)]
}

async function refreshUiStringLiveCandidate(candidate: UiStringCandidate) {
  const key = uiStringKey(candidate)
  const previousState = uiStringLiveTexts.value[key]
  try {
    const minBytes = Math.max(1, Number(candidate.byteLength ?? 0))
    const readSize = Math.min(96, Math.max(minBytes + 8, candidate.encoding === 'utf16' ? 64 : 32))
    const preview = await store.readMemoryPreviewByMode(candidate.address, readSize)
    if (!preview.success && !preview.partial) {
      uiStringLiveTexts.value = {
        ...uiStringLiveTexts.value,
        [key]: {
          current: previousState?.current ?? '',
          previous: previousState?.previous ?? '',
          changed: false,
          error: preview.error || 'Lecture impossible.',
          updatedAt: localNowTime(),
        },
      }
      return
    }
    const current = decodeUiStringBytes(candidate, previewHexToBytes(preview.hex))
    uiStringLiveTexts.value = {
      ...uiStringLiveTexts.value,
      [key]: {
        current,
        previous: previousState?.current ?? '',
        changed: Boolean(previousState?.current) && previousState.current !== current,
        error: '',
        updatedAt: localNowTime(),
      },
    }
  } catch (e) {
    uiStringLiveTexts.value = {
      ...uiStringLiveTexts.value,
      [key]: {
        current: previousState?.current ?? '',
        previous: previousState?.previous ?? '',
        changed: false,
        error: String(e),
        updatedAt: localNowTime(),
      },
    }
  }
}

async function refreshSelectedUiStringTexts() {
  const targets = selectedUiStringCandidates().slice(0, 80)
  if (targets.length === 0 || uiStringTextLiveRefreshing.value) return
  uiStringTextLiveRefreshing.value = true
  try {
    for (const target of targets) {
      await refreshUiStringLiveCandidate(target)
    }
  } finally {
    uiStringTextLiveRefreshing.value = false
  }
}

function setUiStringTextLiveEnabled(enabled: boolean) {
  uiStringTextLiveEnabled.value = enabled
  if (uiStringTextLiveTimer) {
    clearInterval(uiStringTextLiveTimer)
    uiStringTextLiveTimer = null
  }
  if (enabled) {
    void refreshSelectedUiStringTexts()
    uiStringTextLiveTimer = setInterval(() => {
      void refreshSelectedUiStringTexts()
    }, 1000)
  }
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

function findWhatWritesSizeForType(type: string) {
  if (type.endsWith('8')) return 1
  if (type.endsWith('16')) return 2
  if (type.endsWith('64') || type === 'Float64') return 8
  return 4
}

function findWhatWritesSizeForUiString(candidate: UiStringCandidate) {
  if (candidate.encoding === 'utf16') return 2
  return 1
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

function normalizeAddress(address: string) {
  return address.replace(/^0x/i, '').toUpperCase()
}

function addIntelligenceCandidate(
  map: Map<string, UiStringSourceCandidate>,
  candidate: UiStringSourceCandidate,
) {
  const existing = map.get(sourceKey(candidate))
  if (!existing || Number(candidate.confidence ?? 0) > Number(existing.confidence ?? 0)) {
    map.set(sourceKey(candidate), candidate)
  }
}

const findWhatWritesHits = computed(() => (findWhatWritesResult.value?.hits as Array<Record<string, unknown>> | undefined) ?? [])
const pageGuardHits = computed(() => (pageGuardResult.value?.hits as Array<Record<string, unknown>> | undefined) ?? [])

const intelligentUiCandidates = computed<IntelligentCandidate[]>(() => {
  const merged = new Map<string, UiStringSourceCandidate>()
  for (const source of uiStringSourceCandidates.value) addIntelligenceCandidate(merged, source)
  for (const source of uiStringInvestigationFinishResult.value?.globalValueHits ?? []) addIntelligenceCandidate(merged, source)

  const debuggerTargets = new Set(findWhatWritesHits.value.map((hit) => normalizeAddress(String(hit.address ?? ''))))
  return Array.from(merged.values()).map((source) => {
    const reasons: string[] = []
    let score = Number(source.confidence ?? 0.45)
    reasons.push(`base ${confidencePercent(score)}%`)

    const distance = Number(source.distanceBytes ?? Number.MAX_SAFE_INTEGER)
    if (Number.isFinite(distance) && distance <= 4096) {
      score += 0.12
      reasons.push('proche string')
    } else if (Number.isFinite(distance) && distance <= 1024 * 1024) {
      score += 0.04
      reasons.push('même fenêtre')
    }

    const trackHits = Number(source.trackHits ?? 0)
    if (trackHits > 0) {
      score += Math.min(0.18, 0.06 * trackHits)
      reasons.push(`${trackHits} suivi(s) OK`)
    }

    if ((uiStringInvestigationFinishResult.value?.globalValueHits ?? []).some((hit) => sourceKey(hit) === sourceKey(source))) {
      score += 0.16
      reasons.push('radar modifié')
    }

    const watched = watchedCandidate(source.address)
    if (watched?.changed) {
      score += 0.1
      reasons.push('watch bouge')
    }
    if (isUiSourceSelected(source)) {
      score += 0.04
      reasons.push('sélectionné')
    }
    if (debuggerTargets.has(normalizeAddress(source.address))) {
      score += 0.22
      reasons.push('writer capturé')
    }
    if ((source.variantLabel || '').includes('x')) {
      score -= 0.03
      reasons.push('encodage')
    }
    if (source.type.endsWith('8')) {
      score -= 0.08
      reasons.push('compact bruyant')
    }

    score = Math.max(0.01, Math.min(1, score))
    return {
      key: sourceKey(source),
      address: source.address,
      type: source.type,
      variantLabel: source.variantLabel,
      score,
      scorePercent: Math.round(score * 100),
      currentValue: candidateCurrentValue(source.address),
      reasons,
      source,
    }
  }).sort((a, b) => {
    if (Math.abs(b.score - a.score) > 0.000001) return b.score - a.score
    return addressNumber(a.address) - addressNumber(b.address)
  }).slice(0, 80)
})

const investigationReport = computed(() => ({
  kind: 'killengine_investigation_report',
  createdAt: new Date().toISOString(),
  processName: store.processName,
  displayedValue: uiStringValue.value.trim(),
  nextDisplayedValue: uiStringNextValue.value.trim(),
  uiStrings: {
    count: uiStringCandidates.value.length,
    selected: selectedUiStringAddresses.value.length,
    matchesFound: uiStringResult.value?.matchesFound,
    trackedRemaining: uiStringTrackResult.value?.remaining,
  },
  numericSources: {
    count: uiStringSourceCandidates.value.length,
    selected: selectedUiSourceAddresses.value.length,
    trackedRemaining: uiStringSourceTrackResult.value?.remaining,
    top: intelligentUiCandidates.value.slice(0, 12).map((candidate) => ({
      address: candidate.address,
      type: candidate.type,
      variantLabel: candidate.variantLabel,
      score: candidate.score,
      currentValue: candidate.currentValue,
      reasons: candidate.reasons,
    })),
  },
  liveInvestigation: uiStringInvestigationFinishResult.value ? {
    changesFound: uiStringInvestigationFinishResult.value.changesFound,
    changedBytes: uiStringInvestigationFinishResult.value.changedBytes,
    globalValueHitsFound: uiStringInvestigationFinishResult.value.globalValueHitsFound,
    probeBlocksChanged: uiStringInvestigationFinishResult.value.probeBlocksChanged,
    partial: uiStringInvestigationFinishResult.value.partial,
  } : null,
  debugger: {
    hitCount: Number(findWhatWritesResult.value?.hitCount ?? 0),
    cancelled: Boolean(findWhatWritesResult.value?.cancelled),
    hits: findWhatWritesHits.value.slice(0, 8).map((hit) => ({
      address: hit.address,
      instructionPointer: hit.instructionPointer,
      module: hit.module,
      moduleOffset: hit.moduleOffset,
      valueBefore: hit.valueBefore,
      valueAfter: hit.valueAfter,
    })),
  },
  aob: {
    pattern: aobPattern.value.trim(),
    matchesFound: aobResult.value?.matchesFound,
    stablePattern: codePatchSuggestionResult.value?.stableAobPattern,
    disassembly: codePatchSuggestionResult.value?.disassembly,
  },
}))

async function copyInvestigationReport() {
  await navigator.clipboard?.writeText(JSON.stringify(investigationReport.value, null, 2))
}

watch(investigationReport, (report) => {
  store.setInvestigationReport(report)
}, { deep: true })

function selectIntelligentUiCandidate(candidate: IntelligentCandidate) {
  selectedUiSourceAddresses.value = [candidate.key]
}

function selectTopIntelligentUiCandidates(limit = uiStringSourceSafeSelectionLimit) {
  selectedUiSourceAddresses.value = intelligentUiCandidates.value.slice(0, limit).map((candidate) => candidate.key)
}

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
  selectUiSourceBatch()
}

function nextUiSourceBatch() {
  uiStringSourceBatchIndex.value = Math.min(uiStringSourceBatchCount.value - 1, boundedUiStringSourceBatchIndex.value + 1)
  selectUiSourceBatch()
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
  scrollToWritePanel()
}

function watchUiSourceCandidate(candidate: UiStringSourceCandidate) {
  store.addAddressToWatch(candidate.address, candidate.type)
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function watchSelectedUiSources() {
  const selected = selectedUiSourceCandidates()
  if (selected.length === 0) return
  store.addAddressesToWatch(selected.map((candidate) => ({
    address: candidate.address,
    type: candidate.type,
  })))
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function watchCurrentUiSourceBatch() {
  if (currentUiStringSourceBatch.value.length === 0) return
  store.addAddressesToWatch(currentUiStringSourceBatch.value.map((candidate) => ({
    address: candidate.address,
    type: candidate.type,
  })))
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

// Chaînage réel : dès qu'une capture Find What Writes réussit, on enchaîne
// automatiquement sur la génération AOB + suggestions de patch du meilleur
// hit — au lieu d'attendre que l'utilisateur clique manuellement "Analyser"
// sur chaque ligne. Réutilise exactement generateAobSignatureFromHit() (le
// même chemin que le clic manuel), rien n'est dupliqué côté backend. Reste
// lecture seule : ni "Trainer" (sauvegarde) ni "Patcher" (écriture) ne sont
// déclenchés automatiquement, l'utilisateur garde la main sur ces étapes.
async function autoChainFindWhatWritesResult() {
  if (!findWhatWritesResult.value?.success) return
  const hits = findWhatWritesHits.value
  if (hits.length === 0) return
  await generateAobSignatureFromHit(hits[0])
}

async function findWhatWritesForSource(candidate: UiStringSourceCandidate) {
  if (!findWhatWritesAcknowledged.value) {
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: 'Active "Debugger autorisé" avant de lancer Écrit par.' }
    return
  }
  findWhatWritesBusy.value = true
  findWhatWritesResult.value = null
  forceHookTargetHit.value = null
  forceHookResult.value = null
  try {
    findWhatWritesResult.value = await runFindWhatWrites(candidate.address, {
      size: findWhatWritesSizeForType(candidate.type),
      timeoutMs: findWhatWritesTimeoutMs.value,
      maxHits: 12,
    })
    await autoChainFindWhatWritesResult()
  } catch (e) {
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: String(e) }
  } finally {
    findWhatWritesBusy.value = false
  }
}

async function runFindWhatWrites(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('debug', 'Find what writes', `Adresse 0x${address.replace(/^0x/i, '')}, timeout ${String(options.timeoutMs ?? '?')} ms.`)) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: store.lastRiskBlockReason || 'Capture debugger annulée par l’utilisateur.',
    }
  }
  const controller = backend.getController()
  const findWhatWritesAsync = controller.findWhatWritesAsync
  const findWhatWritesFinished = controller.findWhatWritesFinished
  if (findWhatWritesAsync && findWhatWritesFinished) {
    return new Promise<Record<string, unknown>>((resolve) => {
      let requestId: number | null = null
      let settled = false
      const earlyPayloads: Array<Record<string, unknown>> = []
      const timeout = window.setTimeout(() => {
        settled = true
        findWhatWritesFinished.disconnect?.(handler)
        resolve({
          requestId: requestId ?? undefined,
          success: false,
          hitCount: 0,
          hits: [],
          error: 'Timeout de la capture Find What Writes.',
        })
      }, 20000)

      const handler = (payload: Record<string, unknown>) => {
        if (requestId === null) {
          earlyPayloads.push(payload)
          return
        }
        if (Number(payload.requestId) !== requestId) return
        settled = true
        window.clearTimeout(timeout)
        findWhatWritesFinished.disconnect?.(handler)
        resolve(payload)
      }
      findWhatWritesFinished.connect(handler)

      void findWhatWritesAsync(address, options).then((start) => {
        if (settled) return
        if (start.success !== true || start.started !== true) {
          settled = true
          window.clearTimeout(timeout)
          findWhatWritesFinished.disconnect?.(handler)
          resolve({
            success: false,
            hitCount: 0,
            hits: [],
            error: String(start.error ?? 'Impossible de démarrer Find What Writes async.'),
          })
          return
        }
        requestId = Number(start.requestId)
        for (const payload of earlyPayloads.splice(0)) {
          handler(payload)
          if (settled) break
        }
      }).catch((error) => {
        if (settled) return
        settled = true
        window.clearTimeout(timeout)
        findWhatWritesFinished.disconnect?.(handler)
        resolve({
          success: false,
          hitCount: 0,
          hits: [],
          error: String(error),
        })
      })
    })
  }
  if (!controller.findWhatWrites) {
    return { success: false, hitCount: 0, hits: [], error: 'Methode backend indisponible.' }
  }
  return controller.findWhatWrites(address, options)
}

async function cancelFindWhatWritesCapture() {
  const controller = backend.getController()
  if (!controller.cancelFindWhatWrites) {
    findWhatWritesResult.value = {
      success: false,
      hitCount: 0,
      hits: [],
      error: 'Annulation Find What Writes indisponible côté backend.',
    }
    return
  }
  const result = await controller.cancelFindWhatWrites()
  if (result.success !== true) {
    findWhatWritesResult.value = {
      success: false,
      hitCount: 0,
      hits: [],
      error: String(result.error ?? 'Annulation Find What Writes impossible.'),
    }
  }
}

async function runPageGuardWatch(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('injection', 'Page Guard (sans debugger)', 'Injecte un handler dans le processus cible pour surveiller 0x' + address.replace(/^0x/i, '') + ' sans passer par le canal de debug Win32.')) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: store.lastRiskBlockReason || 'Capture Page Guard annulée par l’utilisateur.',
    }
  }
  const controller = backend.getController()
  const fn = controller.startPageGuardWatchAsync
  const sig = controller.pageGuardWatchFinished
  if (!fn || !sig) {
    return { success: false, hitCount: 0, hits: [], error: 'Page Guard non disponible dans ce backend.' }
  }
  return new Promise<Record<string, unknown>>((resolve) => {
    let requestId: number | null = null
    let settled = false
    const earlyPayloads: Array<Record<string, unknown>> = []
    const timeout = window.setTimeout(() => {
      settled = true
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: 'Timeout de la capture Page Guard.' })
    }, 20000)

    const handler = (payload: Record<string, unknown>) => {
      if (requestId === null) {
        earlyPayloads.push(payload)
        return
      }
      if (Number(payload.requestId) !== requestId) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve(payload)
    }
    sig.connect(handler)

    void fn(address, options).then((start) => {
      if (settled) return
      if (start.success !== true || start.started !== true) {
        settled = true
        window.clearTimeout(timeout)
        sig.disconnect?.(handler)
        resolve({ success: false, hitCount: 0, hits: [], error: String(start.error ?? 'Impossible de démarrer Page Guard async.') })
        return
      }
      requestId = Number(start.requestId)
      for (const payload of earlyPayloads.splice(0)) {
        handler(payload)
        if (settled) break
      }
    }).catch((error) => {
      if (settled) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: String(error) })
    })
  })
}

async function pageGuardWatchForSource(candidate: UiStringSourceCandidate) {
  pageGuardBusy.value = true
  pageGuardResult.value = null
  try {
    pageGuardResult.value = await runPageGuardWatch(candidate.address, {
      size: findWhatWritesSizeForType(candidate.type),
      timeoutMs: findWhatWritesTimeoutMs.value,
      maxHits: 12,
    })
  } catch (e) {
    pageGuardResult.value = { success: false, hitCount: 0, hits: [], error: String(e) }
  } finally {
    pageGuardBusy.value = false
  }
}

async function cancelPageGuardWatchCapture() {
  const controller = backend.getController()
  if (!controller.cancelPageGuardWatch) {
    pageGuardResult.value = { success: false, hitCount: 0, hits: [], error: 'Annulation Page Guard indisponible côté backend.' }
    return
  }
  const result = await controller.cancelPageGuardWatch()
  if (result.success !== true) {
    pageGuardResult.value = { success: false, hitCount: 0, hits: [], error: String(result.error ?? 'Annulation Page Guard impossible.') }
  }
}

// ---- Find What Accesses (P1) : instructions qui LISSENT l'adresse ----
const findWhatAccessesResult = ref<Record<string, unknown> | null>(null)
const findWhatAccessesBusy = ref(false)

async function runFindWhatAccesses(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('debug', 'Find what accesses', 'Adresse 0x' + address.replace(/^0x/i, '') + ', timeout ' + String(options.timeoutMs ?? '?') + ' ms.')) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: store.lastRiskBlockReason || 'Capture debugger annulee par l utilisateur.',
    }
  }
  const controller = backend.getController()
  const fn = controller.findWhatAccessesAsync
  const sig = controller.findWhatAccessesFinished
  if (!fn || !sig) {
    return { success: false, hitCount: 0, hits: [], error: 'Find What Accesses non disponible dans ce backend.' }
  }
  return new Promise<Record<string, unknown>>((resolve) => {
    let requestId: number | null = null
    let settled = false
    const earlyPayloads: Array<Record<string, unknown>> = []
    const timeout = window.setTimeout(() => {
      settled = true
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: 'Timeout de la capture Find What Accesses.' })
    }, 20000)

    const handler = (payload: Record<string, unknown>) => {
      if (requestId === null) {
        earlyPayloads.push(payload)
        return
      }
      if (Number(payload.requestId) !== requestId) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve(payload)
    }
    sig.connect(handler)

    void fn(address, options).then((start) => {
      if (settled) return
      if (start.success !== true || start.started !== true) {
        settled = true
        window.clearTimeout(timeout)
        sig.disconnect?.(handler)
        resolve({ success: false, hitCount: 0, hits: [], error: String(start.error ?? 'Impossible de demarrer Find What Accesses async.') })
        return
      }
      requestId = Number(start.requestId)
      for (const payload of earlyPayloads.splice(0)) {
        handler(payload)
        if (settled) break
      }
    }).catch((error) => {
      if (settled) return
      settled = true
      window.clearTimeout(timeout)
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: String(error) })
    })
  })
}

async function findWhatAccessesForSource(candidate: UiStringSourceCandidate) {
  if (!findWhatWritesAcknowledged.value) {
    findWhatAccessesResult.value = { success: false, hitCount: 0, hits: [], error: 'Active le consentement debugger avant de lancer Lu par.' }
    return
  }
  findWhatAccessesBusy.value = true
  findWhatAccessesResult.value = null
  try {
    findWhatAccessesResult.value = await runFindWhatAccesses(candidate.address, {
      size: findWhatWritesSizeForType(candidate.type),
      timeoutMs: findWhatWritesTimeoutMs.value,
      maxHits: 12,
    })
  } catch (e) {
    findWhatAccessesResult.value = { success: false, hitCount: 0, hits: [], error: String(e) }
  } finally {
    findWhatAccessesBusy.value = false
  }
}
async function findWhatWritesForUiString(candidate: UiStringCandidate) {
  if (!findWhatWritesAcknowledged.value) {
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: 'Active "Debugger autorisé" avant de lancer Écrit par.' }
    return
  }
  findWhatWritesBusy.value = true
  findWhatWritesResult.value = null
  forceHookTargetHit.value = null
  forceHookResult.value = null
  try {
    await refreshUiStringLiveCandidate(candidate)
    findWhatWritesResult.value = await runFindWhatWrites(candidate.address, {
      size: findWhatWritesSizeForUiString(candidate),
      timeoutMs: findWhatWritesTimeoutMs.value,
      maxHits: 16,
      origin: 'ui_string',
      encoding: candidate.encoding,
      text: candidate.text,
    })
    await autoChainFindWhatWritesResult()
  } catch (e) {
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: String(e) }
  } finally {
    findWhatWritesBusy.value = false
  }
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
  scrollToWritePanel()
}

function scrollToWritePanel() {
  void nextTick(() => {
    writePanelRef.value?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  })
}

function scrollToUiSources() {
  void nextTick(() => {
    uiStringSourcesPanelRef.value?.scrollIntoView({ behavior: 'smooth', block: 'start' })
  })
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
const structureProbeRows = computed(() => (structureProbeResult.value?.rows as StructureProbeRow[] | undefined) ?? [])
const structureDiffRows = computed(() => {
  const aRows = structureCaptureA.value ?? []
  const bRows = structureCaptureB.value ?? []
  if (aRows.length === 0 || bRows.length === 0) return []
  const keyFor = (row: StructureProbeRow) => `${row.offset}:${row.type ?? ''}`
  const aByKey = new Map(aRows.map((row) => [keyFor(row), row]))
  return bRows
    .map((after) => {
      const before = aByKey.get(keyFor(after))
      const beforeValue = before ? String(before.value ?? before.int32 ?? '') : ''
      const afterValue = String(after.value ?? after.int32 ?? '')
      return {
        ...after,
        beforeValue,
        afterValue,
        changed: beforeValue !== afterValue,
      }
    })
    .filter((row) => row.changed || Math.abs(row.offset) <= 32)
    .slice(0, 160)
})
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
const expertScenarioPresets = computed(() => store.workflowPresets.filter((preset) => preset.id.startsWith('scenario-')))

// P3 - Les 13 panneaux Expert sont regroupes en 4 etapes de workflow.
// L'ordre relatif des panneaux dans le template correspond deja aux etapes,
// donc un simple filtre suffit: aucun bloc n'a besoin d'etre deplace.
type ExpertStepId = 'find' | 'inspect' | 'act' | 'persist'

const expertSteps: Array<{ id: ExpertStepId; risk: RiskLevel }> = [
  { id: 'find', risk: 'read' },
  { id: 'inspect', risk: 'read' },
  { id: 'act', risk: 'write' },
  { id: 'persist', risk: 'code' },
]

// Défaut sur 'all' : le workflow réel va constamment de "trouver" à "agir"
// (trouver un candidat -> l'écrire tout de suite pour tester), donc masquer
// le panneau Write par défaut le rend invisible en pratique. Le filtre par
// étape reste disponible pour qui le veut, mais rien n'est caché sans un
// clic explicite de l'utilisateur.
const activeStep = ref<ExpertStepId | 'all'>('all')

function showStep(id: ExpertStepId) {
  return activeStep.value === 'all' || activeStep.value === id
}

// Compteur affiche sur l'onglet d'etape: montre ou en est le travail sans
// forcer une navigation automatique (un changement d'onglet subi est pire
// qu'un onglet a cliquer).
function stepCount(id: ExpertStepId): number {
  if (id === 'find') return store.candidatePage?.totalCount ?? 0
  if (id === 'inspect') return store.watchedAddresses.length
  if (id === 'act') return selectedCandidateAddresses.value.length
  return aobResult.value?.matchesFound ?? 0
}

// "Nouveau scan" ne videait avant que le candidate store guidé
// (store.resetWorkflow) : les résultats des autres panneaux (Trace UI
// string, AOB, patch, pointer chains, find what writes, structures) restaient
// affichés comme s'ils appartenaient à la nouvelle recherche. Ne touche
// volontairement PAS aux surveillances actives (watch, freeze polling/BP en
// cours) : ce sont des actions délibérées et indépendantes de "quelle valeur
// je cherche maintenant" — les arrêter silencieusement serait une surprise,
// pas une aide.
async function startNewScan() {
  await store.resetWorkflow()

  if (uiStringLiveInvestigation.value) {
    await toggleUiStringLiveInvestigation() // ferme proprement côté backend (finishUiStringInvestigation)
  }
  if (uiStringTextLiveEnabled.value) {
    setUiStringTextLiveEnabled(false)
  }

  selectedCandidateAddresses.value = []
  selectedWriteTargetOverrides.value = {}

  pointerScanAddress.value = ''
  pointerScanResult.value = null
  pointerResolveResult.value = null
  selectedPointerChainIndex.value = -1

  stableLocatorResult.value = null
  stableLocatorForAddress.value = ''

  aobPattern.value = ''
  aobResult.value = null
  aobStabilizeResult.value = null
  aobSignatureResult.value = null
  codePatchAddress.value = ''
  codePatchBytes.value = '90 90'
  codePatchResult.value = null
  codePatchSuggestionResult.value = null
  codePatchProfileName.value = ''
  codePatchProfilePatchName.value = ''
  codePatchProfileDescription.value = ''
  codePatchProfileResult.value = null

  uiStringValue.value = ''
  uiStringNextValue.value = ''
  uiStringResult.value = null
  uiStringTrackResult.value = null
  uiStringSourceResult.value = null
  uiStringSourceTrackResult.value = null
  uiStringOriginResult.value = null
  uiStringCandidates.value = []
  uiStringSourceCandidates.value = []
  selectedUiStringAddresses.value = []
  selectedUiSourceAddresses.value = []
  uiStringLiveTexts.value = {}
  uiStringInvestigationStartResult.value = null
  uiStringInvestigationFinishResult.value = null
  uiStringSourceBatchIndex.value = 0

  findWhatWritesResult.value = null
  forceHookTargetHit.value = null
  forceHookResult.value = null
  findWhatWritesAcknowledged.value = false
  selectedFindWhatWritesRip.value = ''
  findWhatAccessesResult.value = null

  structureProbeResult.value = null
  structureCaptureA.value = null
  structureCaptureB.value = null
  structureCaptureAName.value = ''
  structureCaptureBName.value = ''
  structureTemplateName.value = ''
}

const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
const exactScanButtonLabel = computed(() => hasCandidateContext.value ? 'Nouveau scan' : 'Premier scan')
const unknownGuideReady = computed(() => Boolean(store.unknownSnapshotResult?.success) || hasCandidateContext.value)

const unknownGuideActions = [
  { mode: 'increased', label: 'ça augmente' },
  { mode: 'decreased', label: 'ça diminue' },
  { mode: 'unchanged', label: 'stable' },
  { mode: 'changed', label: 'ça change' },
] as const
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096] // -1 = Auto
const unknownDepthLabel = (mb: number) => (mb === -1 ? 'Auto' : `${mb} Mo`)


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

function writeSelectedCandidatesAtomic() {
  if (selectedCandidateAddresses.value.length === 0 || !store.writeValue.trim()) return
  void store.writeSelectedAtomic(selectedCandidateAddresses.value, selectedWriteType.value, store.writeValue)
}

// Écriture d'escalade : contourne les protections mémoire usermode via le
// driver noyau. Un seul candidat à la fois (jamais de bulk) — c'est une
// escalade ciblée quand l'écriture normale ne tient pas, pas une alternative
// systématique à "Écrire sur sélection".
async function writeSelectedCandidateKernel() {
  const address = selectedCandidateAddresses.value[0]
  if (!address || !store.writeValue.trim()) return
  await store.executeCheckpointKernelWrite({ address, value: store.writeValue, type: selectedWriteType.value })
}

function writeFromPanel() {
  if (hasSelectedWriteTargets.value) {
    writeSelectedCandidates()
    return
  }
  void store.writeSelectedValue()
}

function watchOrRefreshCandidate(address: string, type: string) {
  const watched = watchedCandidate(address)
  if (watched) {
    void store.refreshWatchedAddress(address)
    return
  }
  store.addAddressToWatch(address, type)
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function watchSelectedCandidates() {
  if (selectedCandidateAddresses.value.length === 0) return
  store.addAddressesToWatch(selectedWriteTargets.value.map((target) => ({
    address: target.address,
    type: target.type,
  })))
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
}

function watchCurrentCandidatePage() {
  if (currentPageCandidates.value.length === 0) return
  store.addAddressesToWatch(currentPageCandidates.value.map((candidate) => ({
    address: candidate.address,
    type: String(candidate.type),
  })))
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
        <button class="btn btn-secondary" :disabled="store.scanBusy" @click="startNewScan()">
          Nouveau scan
        </button>
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
      </div>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache un processus pour utiliser les outils expert.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <div class="preset-row">
        <span class="hint preset-row-label">Scénarios courants :</span>
        <button
          v-for="preset in expertScenarioPresets"
          :key="preset.id"
          class="btn btn-secondary compact"
          type="button"
          @click="store.applyWorkflowPreset(preset.id)"
        >
          {{ preset.title }}
        </button>
      </div>

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

      <nav class="workflow-steps" aria-label="Étapes du Mode Expert">
        <button
          v-for="step in expertSteps"
          :key="step.id"
          type="button"
          class="workflow-step"
          :class="{ active: activeStep === step.id }"
          @click="activeStep = step.id"
        >
          <span class="workflow-step-head">
            <span class="workflow-step-title">{{ $t(`help.step.${step.id}.title`) }}</span>
            <RiskBadge v-if="step.risk !== 'read'" :level="step.risk" />
            <span v-if="stepCount(step.id)" class="workflow-step-count">{{ formatNumber(stepCount(step.id)) }}</span>
          </span>
          <span class="workflow-step-what">{{ $t(`help.step.${step.id}.what`) }}</span>
        </button>
        <button
          type="button"
          class="workflow-step compact-step"
          :class="{ active: activeStep === 'all' }"
          @click="activeStep = 'all'"
        >
          <span class="workflow-step-head">
            <span class="workflow-step-title">Tout</span>
          </span>
          <span class="workflow-step-what">Afficher les panneaux des quatre étapes en même temps.</span>
        </button>
      </nav>

      <RegionPanel v-show="showStep('inspect')" v-if="store.expertRegionSize || store.expertRegionProtection" />

      <section v-show="showStep('inspect')" class="panel save-file-panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>Fichiers de sauvegarde</h2>
            <RiskBadge level="read" />
          </div>
          <span v-if="store.discoveredSaveFiles.length > 0">{{ formatNumber(store.discoveredSaveFiles.length) }} fichier(s)</span>
        </div>
        <div class="panel-actions save-file-actions">
          <button
            class="btn btn-primary"
            type="button"
            :disabled="store.saveFilesBusy"
            @click="discoverSaveFilesFromExpert()"
          >
            <span v-if="store.saveFilesBusy" class="btn-spinner" aria-hidden="true"></span>
            {{ store.saveFilesBusy ? 'Découverte...' : 'Découvrir les fichiers de sauvegarde' }}
          </button>
          <span v-if="store.discoveredSaveFilesFamilyName">{{ store.discoveredSaveFilesFamilyName }}</span>
        </div>
        <p v-if="store.saveFileDiscoveryResult?.error" class="error">{{ store.saveFileDiscoveryResult.error }}</p>
        <div v-if="store.discoveredSaveFiles.length > 0" class="save-file-list">
          <button
            v-for="file in store.discoveredSaveFiles"
            :key="file.path"
            class="save-file-row"
            type="button"
            :class="{ selected: store.selectedSaveFilePath === file.path }"
            :disabled="store.saveFileTextBusy"
            @click="readSaveFileFromExpert(file.path)"
          >
            <strong :title="file.path">{{ saveFileName(file.path) }}</strong>
            <span :title="file.path">{{ file.path }}</span>
            <span>{{ formatBytes(file.sizeBytes) }}</span>
            <span>{{ formatSaveFileTime(file.lastWriteTime) }}</span>
          </button>
        </div>
        <div v-else-if="store.saveFileDiscoveryResult?.success" class="empty compact">
          Aucun fichier de sauvegarde probable trouvé.
        </div>
        <div v-if="store.saveFileTextBusy || store.selectedSaveFileText" class="save-file-preview">
          <div class="source-list-title">
            <strong>{{ store.saveFileTextBusy ? 'Lecture...' : saveFileName(store.selectedSaveFileText?.path || store.selectedSaveFilePath) }}</strong>
            <span v-if="store.selectedSaveFileText?.truncated" class="warning-text">aperçu tronqué à 64 Ko</span>
          </div>
          <p v-if="store.selectedSaveFileText?.error" class="error">{{ store.selectedSaveFileText.error }}</p>
          <textarea
            v-if="store.selectedSaveFileText?.success"
            class="input save-file-textarea"
            readonly
            :value="store.selectedSaveFileText.text || ''"
          ></textarea>

          <div class="save-file-watch-row">
            <button
              class="btn btn-secondary compact"
              type="button"
              :disabled="store.saveFileWatchBusy || !store.selectedSaveFilePath"
              @click="store.watchSelectedSaveFile(store.selectedSaveFilePath, 8000)"
            >
              <span v-if="store.saveFileWatchBusy" class="btn-spinner" aria-hidden="true"></span>
              {{ store.saveFileWatchBusy ? 'Surveillance (8s)...' : 'Surveiller ce fichier' }}
            </button>
            <button
              v-if="store.saveFileWatchBusy"
              class="btn btn-secondary compact"
              type="button"
              @click="store.cancelSaveFileWatchAction()"
            >
              Annuler
            </button>
            <span v-if="store.saveFileWatchResult && !store.saveFileWatchBusy" :class="store.saveFileWatchResult.changed ? 'hint' : 'warning-text'">
              {{ store.saveFileWatchResult.changed
                ? `Changement détecté (${store.saveFileWatchResult.changeType ?? '?'})`
                : (store.saveFileWatchResult.cancelled ? 'Surveillance annulée' : (store.saveFileWatchResult.error ?? 'Aucun changement avant timeout')) }}
            </span>
          </div>

          <div class="save-file-patch-row">
            <input v-model="store.saveFilePatchFindHex" class="input" placeholder="Octets à trouver (hex, ex: 35 38)" />
            <input v-model="store.saveFilePatchReplaceHex" class="input" placeholder="Octets de remplacement (même longueur)" />
            <button
              class="btn btn-danger compact"
              type="button"
              :disabled="store.saveFilePatchBusy || !store.saveFilePatchFindHex.trim() || !store.saveFilePatchReplaceHex.trim()"
              @click="store.patchSelectedSaveFileBytes(store.selectedSaveFilePath, store.saveFilePatchFindHex, store.saveFilePatchReplaceHex)"
            >
              {{ store.saveFilePatchBusy ? 'Patch...' : 'Patcher' }}
            </button>
          </div>
          <p v-if="store.saveFilePatchResult" :class="store.saveFilePatchResult.success ? 'hint' : 'error'">
            {{ store.saveFilePatchResult.success ? `Patché (${store.saveFilePatchResult.occurrencesFound ?? 1} occurrence).` : store.saveFilePatchResult.error }}
          </p>
        </div>

        <div class="local-settings-block">
          <div class="panel-actions">
            <button
              class="btn btn-secondary compact"
              type="button"
              :disabled="store.localSettingsBusy"
              @click="store.inspectLocalSettings(200)"
            >
              <span v-if="store.localSettingsBusy" class="btn-spinner" aria-hidden="true"></span>
              {{ store.localSettingsBusy ? 'Inspection...' : 'Inspecter LocalSettings' }}
            </button>
            <span v-if="store.localSettingsResult?.count !== undefined">{{ store.localSettingsResult.count }} valeur(s)</span>
          </div>
          <p v-if="store.localSettingsResult?.error" class="error">{{ store.localSettingsResult.error }}</p>
          <div v-if="store.localSettingsResult?.values?.length" class="save-file-list local-settings-list">
            <div v-for="value in store.localSettingsResult.values" :key="`${value.keyPath}/${value.name}`" class="local-settings-row">
              <strong :title="value.keyPath">{{ value.name }}</strong>
              <span>{{ value.type }}</span>
              <span :title="value.preview">{{ value.preview }}</span>
            </div>
          </div>
        </div>
      </section>

      <section v-show="showStep('find')" class="panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>{{ $t('scan.exact') }}</h2>
            <InfoDot topic="scanExact" />
            <RiskBadge level="read" />
          </div>
          <div class="panel-actions">
            <span v-if="store.exactScanResult?.partial">{{ $t('scan.partial') }}</span>
            <button class="btn btn-secondary compact" type="button" :disabled="store.scanBusy" @click="startNewScan()">
              Nouveau scan
            </button>
          </div>
        </div>
        <p class="panel-hint">{{ $t('help.scanExact.when') }}</p>
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
        <div class="encrypted-scan-panel">
          <div class="source-list-title">
            <strong>Scan chiffré</strong>
            <span>XOR/Add/Sub/NOT · writable</span>
          </div>
          <div class="controls encrypted-controls">
            <select v-model="store.encryptedScanMode" class="input select compact-input" :disabled="store.scanBusy">
              <option value="xor">XOR</option>
              <option value="add">Add</option>
              <option value="sub">Sub</option>
              <option value="not">NOT</option>
            </select>
            <input v-model="store.encryptedScanKey" class="input compact-input" placeholder="clé 0x42" :disabled="store.scanBusy || store.encryptedScanMode === 'not'" />
            <select v-model.number="store.encryptedScanKeySearchBits" class="input select compact-input" :disabled="store.scanBusy || store.encryptedScanMode === 'not'">
              <option :value="0">clé fixe</option>
              <option :value="8">brute 8-bit</option>
              <option :value="16">brute 16-bit</option>
            </select>
            <button class="btn btn-primary compact" type="button" :disabled="!store.exactScanValue.trim() || store.scanBusy" @click="store.doEncryptedScan()">
              Scanner
            </button>
          </div>
          <div v-if="store.encryptedScanResult" class="metrics">
            <span>Matches: {{ formatNumber(store.encryptedScanResult.matchesFound) }}</span>
            <span>Régions: {{ formatNumber(store.encryptedScanResult.regionsScanned) }}</span>
            <span>Lu: {{ formatBytes(store.encryptedScanResult.bytesScanned) }}</span>
            <span v-if="store.encryptedScanResult.partial">partiel</span>
          </div>
          <p v-if="store.encryptedScanResult?.error" class="error">{{ store.encryptedScanResult.error }}</p>
          <div v-if="store.encryptedScanResult?.matches.length" class="encrypted-result-list">
            <div v-for="match in store.encryptedScanResult.matches.slice(0, 12)" :key="`${match.address}:${match.variantLabel}`" class="source-row">
              <code>0x{{ match.address }}</code>
              <span>{{ match.type }}</span>
              <span>{{ match.variantLabel || '-' }}</span>
              <button class="btn btn-secondary compact" type="button" @click="store.selectCandidate(match.address, match.type)">Utiliser</button>
              <button class="btn btn-secondary compact" type="button" @click="store.addAddressToWatch(match.address, match.type)">Watch</button>
            </div>
          </div>
        </div>
      </section>

      <NextScanPanel v-show="showStep('find')" />

      <section v-show="showStep('find')" class="panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>{{ $t('unknown.title') }}</h2>
            <InfoDot topic="unknown" />
            <RiskBadge level="read" />
          </div>
        </div>
        <p class="panel-hint">{{ $t('help.unknown.when') }}</p>
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
          <button class="btn btn-primary" :disabled="store.scanBusy" @click="store.captureUnknownSnapshot()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Capture...' : $t('unknown.capture') }}</span>
          </button>
        </div>
        <p class="hint unknown-guide-warning">
          ⚠️ Avant de cliquer : as-tu bien fait l'action dans le jeu ? "Ça augmente/diminue/change" doit suivre un vrai changement, "stable" doit suivre l'absence de changement. Cliquer le mauvais bouton peut faire tomber tes candidats à 0 d'un coup — utilise "Restaurer réduction" (panneau Scan suivant, un peu plus haut) si ça arrive.
        </p>
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

      <section v-show="showStep('find')" class="panel ui-string-panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>Trace UI string</h2>
            <InfoDot topic="uiString" />
            <RiskBadge level="read" />
          </div>
          <span v-if="uiStringResult">{{ formatNumber(uiStringCandidates.length) }} candidat(s)</span>
        </div>
        <p class="panel-hint">{{ $t('help.uiString.when') }}</p>
        <p class="hint">Étape 1 : tape la valeur telle qu'affichée à l'écran, clique Scanner texte. Étape 2 : change cette valeur dans le jeu, tape la nouvelle valeur affichée, puis clique Scan suivant (texte) et, si tu as déjà lancé Analyser sources plus bas, Scan suivant (sources) aussi.</p>
        <div class="controls ui-string-controls">
          <input
            v-model="uiStringValue"
            class="input"
            placeholder="Étape 1 : texte affiché (ex: 50)"
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
            placeholder="Étape 2 : nouvelle valeur affichée"
            :disabled="uiStringBusy || uiStringCandidates.length === 0"
            @keyup.enter="trackUiStrings()"
          />
          <button class="btn btn-primary" :disabled="uiStringBusy || uiStringCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStrings()">
            Scan suivant (texte)
          </button>
          <button class="btn btn-primary" :disabled="uiStringBusy || uiStringSourceCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStringSources()">
            Scan suivant (sources)
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
                ? `Snapshot capturé. Modifie la valeur dans le jeu cible, puis clique Arrêter et comparer · ${formatNumber(uiStringInvestigationElapsed)} s`
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
            <span>{{ change.afterInt32 !== undefined ? `i32 ${formatNumber(change.beforeInt32)} -> ${formatNumber(change.afterInt32)}` : '-' }}</span>
            <span>{{ change.afterFloat32 !== undefined ? `f32 ${change.beforeFloat32?.toFixed(3)} -> ${change.afterFloat32.toFixed(3)}` : '-' }}</span>
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
          <label class="checkbox-label debugger-check">
            <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
            Debugger autorisé
          </label>
          <label class="compact-select">
            <span>Écrit par</span>
            <InfoDot topic="findWhatWrites" />
            <select v-model.number="findWhatWritesTimeoutMs" class="input select" :disabled="findWhatWritesBusy">
              <option v-for="timeout in findWhatWritesTimeoutOptions" :key="timeout" :value="timeout">
                {{ timeout / 1000 }} s
              </option>
            </select>
          </label>
        </div>
        <p class="debugger-guard">
          <strong>Écrit par</strong> attache le debugger Windows au processus pendant la capture. À utiliser sur une cible de test ou solo, puis fais varier la valeur pendant la fenêtre choisie.
          Dès qu'une instruction est capturée, la signature AOB et les suggestions de patch se génèrent automatiquement ci-dessous (lecture seule) — sauvegarder en Trainer ou patcher reste toujours un clic manuel séparé.
        </p>
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
        <p v-if="uiStringCandidates.length > 0" class="hint">
          Étape 3 (optionnelle) : clique Analyser sources pour chercher les nombres qui alimentent ce texte — ça débloque le bouton Scan suivant (sources) plus haut. Trop de résultats ? Change encore la valeur en jeu puis reclique Scan suivant (sources), ou essaie Auto origine qui enchaîne tout.
        </p>
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
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="uiStringBusy || uiStringCandidates.length === 0"
            @click="setUiStringTextLiveEnabled(!uiStringTextLiveEnabled)"
          >
            {{ uiStringTextLiveEnabled ? 'Live strings stop' : 'Live strings' }}
          </button>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="uiStringBusy || uiStringTextLiveRefreshing || uiStringCandidates.length === 0"
            @click="refreshSelectedUiStringTexts()"
          >
            Rafraîchir strings
          </button>
          <span>{{ selectedUiStringAddresses.length || uiStringCandidates.length }} suivi(s) au prochain filtre</span>
          <label class="checkbox-label debugger-check" title="Nécessaire pour utiliser Écrit par sur les candidats ci-dessous.">
            <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
            Debugger autorisé
          </label>
        </div>
        <div v-if="uiStringCandidates.length > 0" class="ui-string-list">
          <div
            v-for="candidate in uiStringCandidates"
            :key="uiStringKey(candidate)"
            class="ui-string-row"
            :class="{ changed: uiStringLiveState(candidate)?.changed }"
          >
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
            <span class="ui-string-live-current">
              actuel: {{ uiStringLiveState(candidate)?.current || '-' }}
            </span>
            <span class="ui-string-live-previous">
              avant: {{ uiStringLiveState(candidate)?.previous || '-' }}
            </span>
            <span>{{ candidate.movedFrom ? `+${formatNumber(candidate.movedDistanceBytes)} o` : (candidate.protection || '-') }}</span>
            <span>{{ candidate.memoryType || '-' }}</span>
            <button class="btn btn-primary compact" type="button" @click="analyzeUiStringSources(candidate)">Sources</button>
            <button class="btn btn-secondary compact" type="button" @click="inspectUiStringOrigins(candidate)">Origine</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiStringCandidate(candidate)">Watch octets</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForUiString(candidate)">
              Écrit par
            </button>
            <button class="btn btn-secondary compact" type="button" @click="useUiStringCandidate(candidate)">Assistant</button>
            <span v-if="uiStringLiveState(candidate)?.error" class="error-inline">{{ uiStringLiveState(candidate)?.error }}</span>
          </div>
        </div>
        <div v-if="findWhatWritesResult || findWhatWritesBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>Find what writes</strong>
            <span>{{ formatNumber(Number(findWhatWritesResult?.hitCount ?? 0)) }} hit(s)</span>
            <button
              v-if="findWhatWritesBusy"
              class="btn btn-secondary compact"
              type="button"
              @click="cancelFindWhatWritesCapture()"
            >
              Annuler capture
            </button>
          </div>
          <p v-if="findWhatWritesBusy" class="hint">Capture en cours : modifie la valeur dans le jeu cible pendant {{ findWhatWritesTimeoutMs / 1000 }} seconde(s).</p>
          <p v-if="findWhatWritesResult?.error" class="error">{{ findWhatWritesResult.error }}</p>
          <div
            v-for="hit in findWhatWritesHits.slice(0, 12)"
            :key="findWhatWritesHitKey(hit)"
            class="find-writes-row"
            :class="{ selected: isSelectedFindWhatWritesHit(hit) }"
          >
            <code>RIP 0x{{ hit.instructionPointer }}</code>
            <span>cible 0x{{ hit.address }}</span>
            <span>{{ hit.module || '-' }}</span>
            <span>+0x{{ hit.moduleOffset || '0' }}</span>
            <span>T{{ hit.threadId }}</span>
            <span>avant {{ formatNumber(Number(hit.valueBefore ?? 0)) }}</span>
            <strong>actuel {{ formatNumber(Number(hit.valueAfter ?? 0)) }}</strong>
            <button class="btn btn-secondary compact" type="button" @click="previewFindWhatWritesHit(hit)">
              Aperçu
            </button>
            <button class="btn btn-secondary compact" type="button" @click="copyFindWhatWritesRip(hit)">
              Copier
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="aobSignatureBusy" @click="generateAobSignatureFromHit(hit)">
              Analyser
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="disassembleBackwardBusy" @click="disassembleBackwardFromHit(hit)">
              Désassembler en amont
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="testCandidateFieldsBusy" @click="testCandidateFieldsFromHit(hit)">
              Tester automatiquement
            </button>
            <button
              class="btn btn-primary compact"
              type="button"
              :disabled="codePatchTrainerFlowBusy || aobSignatureBusy || codePatchProfileBusy"
              @click="saveTrainerPatchFromHit(hit)"
            >
              Trainer
            </button>
            <button
              v-if="isSelectedFindWhatWritesHit(hit) && codePatchSuggestionResult?.memBaseRegister"
              class="btn btn-primary compact"
              type="button"
              :disabled="forceHookBusy"
              :title="`Force une valeur à cette adresse quelle que soit la source de l'écriture (registre ou immédiat) — installe un trampoline et redirige le site, restaurable comme un script auto-assembleur.`"
              @click="selectForceHookTarget(hit)"
            >
              Forcer valeur (hook)
            </button>
          </div>
          <div v-if="forceHookTargetHit" class="controls value-override-controls">
            <span class="hint">
              Force une valeur à 0x{{ forceHookTargetHit.address }} (écrite par RIP 0x{{ forceHookTargetHit.instructionPointer }}),
              même si la source est un registre. Type utilisé : {{ store.exactScanType }} (sélecteur de type Expert).
            </span>
            <input
              v-model="forceHookValueInput"
              class="input"
              placeholder="Valeur : 999"
              :disabled="forceHookBusy"
              @keyup.enter="applyForceHookValue()"
            />
            <button class="btn btn-primary compact" type="button" :disabled="forceHookBusy || !forceHookValueInput.trim()" @click="applyForceHookValue()">
              Appliquer
            </button>
          </div>
          <p v-if="forceHookResult" :class="forceHookResult.success ? 'hint' : 'error'">
            {{ forceHookResult.success
              ? `Valeur forcée : trampoline actif à 0x${forceHookResult.patchAddress}. Restaurable via le bouton "Restaurer" du panneau Injection / Auto-assembler.`
              : forceHookResult.error }}
          </p>
        </div>
        <div v-if="pageGuardResult || pageGuardBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>Page Guard (sans debugger)</strong>
            <span>{{ formatNumber(Number(pageGuardResult?.hitCount ?? 0)) }} hit(s)</span>
            <button
              v-if="pageGuardBusy"
              class="btn btn-secondary compact"
              type="button"
              @click="cancelPageGuardWatchCapture()"
            >
              Annuler capture
            </button>
          </div>
          <p class="hint">Alternative sans debugger à "Écrit par" : utile quand un autre débogueur tient déjà le canal de debug Win32, mais moins précis (granularité page 4 Ko).</p>
          <p v-if="pageGuardBusy" class="hint">Capture en cours : modifie la valeur dans le jeu cible pendant {{ findWhatWritesTimeoutMs / 1000 }} seconde(s).</p>
          <p v-if="pageGuardResult?.warning" class="hint">{{ pageGuardResult.warning }}</p>
          <p v-if="pageGuardResult?.error" class="error">{{ pageGuardResult.error }}</p>
          <div
            v-for="hit in pageGuardHits.slice(0, 12)"
            :key="findWhatWritesHitKey(hit)"
            class="find-writes-row"
          >
            <code>RIP 0x{{ hit.instructionPointer }}</code>
            <span>cible 0x{{ hit.address }}</span>
            <span>{{ hit.module || '-' }}</span>
            <span>+0x{{ hit.moduleOffset || '0' }}</span>
            <span>T{{ hit.threadId }}</span>
            <span>{{ hit.isWrite ? 'écriture' : 'lecture' }}</span>
            <button class="btn btn-secondary compact" type="button" @click="previewFindWhatWritesHit(hit)">
              Aperçu
            </button>
            <button class="btn btn-secondary compact" type="button" @click="copyFindWhatWritesRip(hit)">
              Copier
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="aobSignatureBusy" @click="generateAobSignatureFromHit(hit)">
              Analyser
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="disassembleBackwardBusy" @click="disassembleBackwardFromHit(hit)">
              Désassembler en amont
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="testCandidateFieldsBusy" @click="testCandidateFieldsFromHit(hit)">
              Tester automatiquement
            </button>
            <button
              class="btn btn-primary compact"
              type="button"
              :disabled="codePatchTrainerFlowBusy || aobSignatureBusy || codePatchProfileBusy"
              @click="saveTrainerPatchFromHit(hit)"
            >
              Trainer
            </button>
          </div>
        </div>
        <div v-if="disassembleBackwardResult || disassembleBackwardBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>Désassembler en amont</strong>
            <InfoDot topic="disassembleBackward" />
            <span v-if="disassembleBackwardResult?.instructions">{{ disassembleBackwardResult.instructions.length }} instruction(s)</span>
          </div>
          <p class="hint">Instructions qui précèdent l'écriture capturée — utile pour trouver le vrai champ source (actuel/cible) d'un compteur animé, plutôt que le champ affiché.</p>
          <p v-if="disassembleBackwardBusy" class="hint">Lecture mémoire en cours...</p>
          <p v-if="disassembleBackwardResult?.error" class="error">{{ disassembleBackwardResult.error }}</p>
          <div
            v-for="(instr, index) in disassembleBackwardResult?.instructions ?? []"
            :key="`${instr.address}-${index}`"
            class="find-writes-row"
            :class="{ selected: instr.isCandidateField }"
          >
            <code>0x{{ instr.address }}</code>
            <span>{{ instr.bytes }}</span>
            <strong>{{ instr.disassembly || instr.mnemonicHint }}</strong>
            <span v-if="instr.isCandidateField" class="quality-strong">
              champ candidat : [{{ instr.memBaseRegister }}+0x{{ instr.memDisplacement?.toString(16) }}]
            </span>
          </div>
        </div>
        <div v-if="testCandidateFieldsResult || testCandidateFieldsBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>Tester automatiquement</strong>
            <InfoDot topic="testCandidateFields" />
            <span v-if="testCandidateFieldsResult?.results">{{ testCandidateFieldsResult.results.length }} champ(s) testé(s)</span>
          </div>
          <p class="hint">Écrit une valeur test transitoire sur chaque champ candidat, attend quelques secondes, relit, puis restaure — pour savoir lequel tient sans lire d'assembleur.</p>
          <p v-if="testCandidateFieldsBusy" class="hint">Test en cours (peut prendre jusqu'à une minute selon le nombre de champs)...</p>
          <p v-if="testCandidateFieldsResult?.error" class="error">{{ testCandidateFieldsResult.error }}</p>
          <div
            v-for="(outcome, index) in testCandidateFieldsResult?.results ?? []"
            :key="`${outcome.address}-${index}`"
            class="find-writes-row"
            :class="{ selected: outcome.verdict === 'holds' }"
          >
            <code>0x{{ outcome.address }}</code>
            <span>[{{ outcome.memBaseRegister }}+0x{{ outcome.memDisplacement?.toString(16) }}]</span>
            <span>{{ outcome.valueType }}</span>
            <span :class="outcome.verdict === 'holds' ? 'quality-strong' : outcome.verdict === 'reverts' ? 'hint' : 'error'">
              {{ outcome.verdict === 'holds' ? `tient (${outcome.ticksSurvived ?? 0} sondage(s))` : outcome.verdict === 'reverts' ? 'repart' : (outcome.error || 'erreur') }}
            </span>
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
        <div v-if="intelligentUiCandidates.length > 0" class="intelligence-panel">
          <div class="source-list-title">
            <strong>Pistes intelligentes</strong>
            <span>top {{ formatNumber(Math.min(12, intelligentUiCandidates.length)) }}/{{ formatNumber(intelligentUiCandidates.length) }}</span>
            <div class="source-actions">
              <button class="btn btn-secondary compact" type="button" @click="selectTopIntelligentUiCandidates()">
                Cocher Top IA
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="watchSelectedUiSources()">
                Watch cochés
              </button>
              <button class="btn btn-primary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="useSelectedUiSourcesForWrite()">
                Write cochés
              </button>
              <button class="btn btn-secondary compact" type="button" @click="copyInvestigationReport()">
                Copier rapport
              </button>
              <label class="checkbox-label debugger-check" title="Nécessaire pour utiliser Écrit par sur les candidats ci-dessous.">
                <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
                Debugger autorisé
              </label>
            </div>
          </div>
          <div
            v-for="candidate in intelligentUiCandidates.slice(0, 12)"
            :key="candidate.key"
            class="intelligence-row"
            :class="confidenceClass(candidate.score)"
          >
            <strong>{{ candidate.scorePercent }}%</strong>
            <code>0x{{ candidate.address }}</code>
            <span>{{ candidate.variantLabel || candidate.type }}</span>
            <span>{{ candidate.currentValue }}</span>
            <span class="intelligence-reasons">{{ candidate.reasons.join(' · ') }}</span>
            <button class="btn btn-secondary compact" type="button" @click="selectIntelligentUiCandidate(candidate)">Cocher</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiSourceCandidate(candidate.source)">Watch</button>
            <button class="btn btn-secondary compact" type="button" @click="analyzeStructureAroundSource(candidate.source)">Struct</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForSource(candidate.source)">Écrit par</button>
            <button class="btn btn-secondary compact" type="button" :disabled="pageGuardBusy" title="Sans passer par le canal de debug Win32 (DebugActiveProcess)" @click="pageGuardWatchForSource(candidate.source)">Écrit par (sans debugger)</button>
          </div>
        </div>
        <div v-if="structureProbeResult" class="structure-panel">
          <div class="source-list-title">
            <strong>Structure autour source</strong>
            <span v-if="structureProbeResult.address">0x{{ structureProbeResult.address }} · {{ formatNumber(Number(structureProbeResult.rowCount ?? 0)) }} ligne(s)</span>
          </div>
          <div class="structure-actions">
            <button class="btn btn-secondary compact" type="button" :disabled="structureProbeRows.length === 0" @click="captureStructure('A')">
              Capture A
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="structureProbeRows.length === 0" @click="captureStructure('B')">
              Capture B
            </button>
            <input v-model="structureTemplateName" class="input compact-input" placeholder="Nom template" />
            <button class="btn btn-primary compact" type="button" :disabled="structureProbeRows.length === 0" @click="saveCurrentStructureTemplate()">
              Sauver template
            </button>
            <span v-if="structureCaptureAName">A: {{ structureCaptureAName }}</span>
            <span v-if="structureCaptureBName">B: {{ structureCaptureBName }}</span>
          </div>
          <p v-if="structureProbeResult.error" class="error">{{ structureProbeResult.error }}</p>
          <div v-if="structureDiffRows.length > 0" class="structure-diff">
            <div class="source-list-title">
              <strong>Diff A/B</strong>
              <span>{{ formatNumber(structureDiffRows.filter((row) => row.changed).length) }} changement(s)</span>
            </div>
            <div
              v-for="row in structureDiffRows"
              :key="`diff:${row.offset}:${row.type}`"
              class="structure-row"
              :class="{ marked: row.changed }"
            >
              <code>0x{{ row.address }}</code>
              <span>{{ row.offset >= 0 ? '+' : '' }}{{ row.offset }}</span>
              <strong>{{ row.type || 'field' }}</strong>
              <span>{{ row.beforeValue || '-' }} → {{ row.afterValue || '-' }}</span>
              <span>{{ row.changed ? 'changé' : 'stable' }}</span>
            </div>
          </div>
          <div
            v-for="row in structureProbeRows"
            :key="row.address"
            class="structure-row"
            :class="{ marked: row.marker }"
          >
            <code>0x{{ row.address }}</code>
            <span>{{ row.offset >= 0 ? '+' : '' }}{{ row.offset }}</span>
            <strong>{{ row.type ? `${row.type} ${String(row.value ?? '')}` : `i32 ${formatNumber(Number(row.int32 ?? 0))}` }}</strong>
            <span>{{ row.valueText || (Number.isFinite(row.float32) ? `f32 ${Number(row.float32).toFixed(3)}` : '-') }}</span>
            <code v-if="row.rawHex">{{ row.rawHex }}</code>
            <span>{{ row.marker || '-' }}</span>
            <button
              v-if="structureRowCanBecomeTrainer(row)"
              class="btn btn-secondary compact"
              type="button"
              @click="createTrainerFromStructureRow(row)"
            >
              Trainer
            </button>
            <button
              class="btn btn-secondary compact"
              type="button"
              @click="bookmarkStructureRow(row)"
            >
              Note
            </button>
          </div>
        </div>
        <div v-if="uiStringSourceCandidates.length > 0" ref="uiStringSourcesPanelRef" class="source-list">
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
              <button class="btn btn-secondary compact" type="button" :disabled="currentUiStringSourceBatch.length === 0" @click="watchCurrentUiSourceBatch()">
                Watch lot
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="watchSelectedUiSources()">
                Watch cochés
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
              <label class="checkbox-label debugger-check" title="Nécessaire pour utiliser Écrit par sur les candidats ci-dessous.">
                <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
                Debugger autorisé
              </label>
            </div>
          </div>
          <p v-if="selectedUiSourceAddresses.length > 50" class="source-warning">
            Sélection massive : écrire beaucoup d'adresses peut rendre la cible instable. Teste plutôt par petits paquets.
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
            <strong class="live-value" :class="{ changed: watchedCandidate(candidate.address)?.changed }">
              {{ candidateCurrentValue(candidate.address) }}
            </strong>
            <span>{{ candidate.trackHits ? `${candidate.trackHits} hit(s)` : `${formatNumber(candidate.distanceBytes)} o` }}</span>
            <button class="btn btn-primary compact" type="button" @click="useUiSourceCandidate(candidate)">Utiliser</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiSourceCandidate(candidate)">Watch</button>
            <button class="btn btn-secondary compact" type="button" @click="analyzeStructureAroundSource(candidate)">Struct</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForSource(candidate)">
              Écrit par
            </button> <button class="btn btn-secondary compact" type="button" :disabled="findWhatAccessesBusy || !findWhatWritesAcknowledged" @click="findWhatAccessesForSource(candidate)">Lu par</button>
          </div>
        </div>
      </section>

      <section v-show="showStep('inspect')" class="panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>Candidats</h2>
            <InfoDot topic="candidates" />
            <RiskBadge level="read" />
          </div>
          <span>{{ formatNumber(store.candidatePage?.totalCount) }} · {{ selectedCandidateAddresses.length }} sélectionné(s)</span>
        </div>
        <p class="panel-hint">{{ $t('help.candidates.when') }}</p>
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
            {{ store.kernelMemoryModeActive ? 'Écrire via kernel' : 'Écrire sur sélection' }}
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length < 2 || !store.writeValue.trim()" @click="writeSelectedCandidatesAtomic()">
            Écrire ensemble (atomique)
          </button>
          <InfoDot topic="writeAtomic" align="right" />
          <button
            v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess && !store.kernelMemoryModeActive"
            class="btn btn-secondary compact"
            :title="`Contourne les protections mémoire usermode — pour une adresse qui refuse de tenir une écriture normale (ex: instabilité/compteur animé).`"
            :disabled="selectedCandidateAddresses.length !== 1 || !store.writeValue.trim()"
            @click="writeSelectedCandidateKernel()"
          >
            Écrire via kernel
          </button>
          <InfoDot topic="writeKernel" align="right" />
          <button class="btn btn-secondary compact" :disabled="store.candidatePage?.displaySuppressed || currentPageCandidates.length === 0" @click="watchCurrentCandidatePage()">
            Watch page
          </button>
          <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="watchSelectedCandidates()">
            Watch sélection
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
        <div
          v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess"
          class="kernel-escalation-guide"
        >
          <strong>Kernel prêt</strong>
          <span>Sélectionne 1 candidat, lis/écris via kernel, puis relis. Si la valeur revient, ce n’est probablement pas un blocage d’écriture : lance Écrit par puis Tester automatiquement.</span>
        </div>
        <div v-if="store.candidatePage?.displaySuppressed" class="candidate-suppressed">
          {{ formatNumber(store.candidatePage.totalCount) }} candidats trouvés. Réduis avec un next scan ou filtre une adresse pour afficher une page.
        </div>
        <p v-if="store.nextScanResult?.stableGroupHint" class="hint stable-group-hint">
          {{ store.nextScanResult.stableGroupHint }}
        </p>
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
              <span
                v-if="match.writeVerified"
                class="write-verified-badge"
                title="Cette adresse a déjà reçu une écriture confirmée avec succès — contrairement à un candidat juste stable au scan, celui-ci a été prouvé écrivable."
              >
                ✓ écrit
              </span>
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
              <button class="btn btn-secondary compact" @click="watchOrRefreshCandidate(match.address, match.type)">
                {{ watchedCandidate(match.address) ? 'Rafraîchir' : 'Watch' }}
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

      <section v-show="showStep('act')" ref="writePanelRef" class="panel risk-write">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>{{ $t('write.title') }}</h2>
            <InfoDot topic="write" />
            <RiskBadge level="write" />
          </div>
          <div class="panel-title-actions">
            <button
              v-if="uiStringSourceCandidates.length > 0"
              class="btn btn-secondary compact"
              type="button"
              @click="scrollToUiSources()"
            >
              Retour sources
            </button>
            <span v-if="store.writeResult">{{ store.writeResult.success ? 'OK' : 'FAIL' }}</span>
          </div>
        </div>
        <p class="panel-hint">{{ $t('help.write.when') }}</p>
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
          <button class="btn" :class="store.freezeEnabled ? 'btn-secondary' : 'btn-primary'" :disabled="hasSelectedWriteTargets || (store.freezeEnabled ? !store.selectedCandidateAddress : !store.canWriteSelectedValue)" @click="store.toggleFreeze()">
            {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
          </button>
          <button
            class="btn btn-secondary"
            :disabled="hasSelectedWriteTargets || store.breakpointFreezeEnabled || !store.canWriteSelectedValue"
            title="Hardware breakpoint: intercepte les écritures et réécrit immédiatement la valeur."
            @click="store.startBreakpointFreeze()"
          >
            Freeze BP
          </button>
          <InfoDot topic="freezeBp" align="right" />
          <button
            class="btn btn-secondary"
            :disabled="!store.breakpointFreezeEnabled"
            title="Arrêter le freeze par hardware breakpoint."
            @click="store.stopBreakpointFreeze()"
          >
            Stop BP
          </button>
          <span v-if="store.breakpointFreezeEnabled && breakpointFreezeStats" class="bp-live-stats" :class="{ warning: breakpointFreezeStats.healthy === false }">
            {{ formatNumber(Number(breakpointFreezeStats.hits ?? 0)) }} hits · {{ formatNumber(Number(breakpointFreezeStats.rewrites ?? 0)) }} corrigé(s)<template v-if="Number(breakpointFreezeStats.errors ?? 0) > 0"> · {{ formatNumber(Number(breakpointFreezeStats.errors ?? 0)) }} erreur(s)</template>
          </span>
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
          <span v-if="store.writeResult?.mode === 'breakpoint'">BP: {{ store.breakpointFreezeEnabled ? 'on' : 'off' }}</span>
          <span v-if="store.writeResult?.rewrites !== undefined">rewrites: {{ formatNumber(store.writeResult.rewrites) }}</span>
          <span v-if="store.freezeIntervalResult">intervalle: {{ store.freezeIntervalMs }} ms</span>
          <span v-if="store.writeResult?.suspendedThreadCount !== undefined" title="Threads du processus cible suspendues pendant l'écriture atomique">
            threads suspendues: {{ formatNumber(store.writeResult.suspendedThreadCount) }}
          </span>
        </div>
        <p v-if="store.writeResult?.error" class="error">{{ store.writeResult.error }}</p>
        <p v-if="store.writeResult?.warning" class="hint warning-hint">{{ store.writeResult.warning }}</p>
        <div
          v-if="store.writeResult?.success && !hasSelectedWriteTargets && store.selectedCandidateAddress"
          class="stable-locator"
        >
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="stableLocatorBusy"
            @click="suggestStableLocator(store.selectedCandidateAddress)"
          >
            <span v-if="stableLocatorBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ stableLocatorBusy ? 'Recherche...' : 'Stabiliser cette adresse' }}</span>
          </button>
          <InfoDot text="Cherche une chaîne de pointeurs stable (module + offsets) vers l'adresse qui vient d'être écrite, pour qu'elle survive à un redémarrage du processus cible. Lecture seule, bornée." />
          <span v-if="stableLocatorResult && stableLocatorForAddress === store.selectedCandidateAddress" class="stable-locator-result">
            <template v-if="stableLocatorResult.success && stableLocatorResult.bestChain">
              <span class="hint">{{ stableLocatorResult.message }}</span>
              <button class="btn btn-secondary compact" type="button" @click="saveStableLocator()">
                Sauvegarder dans un profil
              </button>
            </template>
            <template v-else-if="stableLocatorResult.success">
              <span class="hint">{{ stableLocatorResult.message }}</span>
            </template>
            <template v-else>
              <span class="hint">{{ stableLocatorResult.error || 'Recherche indisponible.' }}</span>
            </template>
          </span>
        </div>
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

      <WatchLivePanel v-show="showStep('inspect')" />

      <section v-show="showStep('persist')" class="panel aob-panel risk-code">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>AOB signatures</h2>
            <InfoDot topic="aob" />
            <RiskBadge level="code" />
          </div>
          <span v-if="aobResult">{{ formatNumber(aobResult.matchesFound) }} match(es)</span>
        </div>
        <p class="panel-hint">{{ $t('help.aob.when') }}</p>
        <div class="controls aob-controls">
          <input
            v-model="aobPattern"
            class="input"
            placeholder="Pattern: 48 8B ?? ?? 89"
            :disabled="aobBusy"
            @keyup.enter="scanAobSignature()"
          />
          <input v-model.number="aobMaxResults" class="input" type="number" min="1" max="10000" />
          <button class="btn btn-primary" :disabled="aobBusy || !aobPattern.trim()" @click="scanAobSignature()">
            <span v-if="aobBusy" class="btn-spinner" aria-hidden="true"></span>
            Scanner AOB
          </button>
        </div>
        <div class="expert-flags aob-flags">
          <label class="checkbox-label">
            <input v-model="aobExecutableOnly" type="checkbox" :disabled="aobBusy" />
            Code exécutable
          </label>
          <label class="checkbox-label">
            <input v-model="aobImageOnly" type="checkbox" :disabled="aobBusy" />
            Module image
          </label>
        </div>
        <div v-if="aobResult" class="metrics">
          <span>Régions: {{ formatNumber(aobResult.regionsScanned) }}</span>
          <span>Lu: {{ formatBytes(aobResult.bytesScanned) }}</span>
          <span v-if="aobResult.patternBytes">Pattern: {{ formatNumber(aobResult.patternBytes) }} o</span>
          <span v-if="aobResult.signatureQuality" :class="`quality-${aobResult.signatureQuality.level}`">
            Qualité: {{ aobResult.signatureQuality.level }} · {{ aobResult.signatureQuality.score }}/100
          </span>
          <span v-if="aobResult.signatureQuality">
            Fixes: {{ formatNumber(aobResult.signatureQuality.fixedBytes) }} / Wildcards: {{ formatNumber(aobResult.signatureQuality.wildcardBytes) }}
          </span>
          <span v-if="aobResult.partial" class="warning-text">résultats limités</span>
        </div>
        <div v-if="aobSignatureResult" class="metrics">
          <span>Signature: {{ aobSignatureResult.success ? 'OK' : 'FAIL' }}</span>
          <span v-if="aobSignatureResult.module">{{ aobSignatureResult.module }} +0x{{ aobSignatureResult.moduleOffset }}</span>
          <span v-if="aobSignatureResult.patternBytes">{{ formatNumber(aobSignatureResult.patternBytes) }} o</span>
          <span v-if="aobSignatureResult.signatureQuality" :class="`quality-${aobSignatureResult.signatureQuality.level}`">
            Qualité: {{ aobSignatureResult.signatureQuality.level }} · {{ aobSignatureResult.signatureQuality.score }}/100
          </span>
        </div>
        <p v-if="aobAutoScanSkippedReason" class="warning-text">{{ aobAutoScanSkippedReason }}</p>
        <p v-if="aobResult?.signatureWarning" class="hint">{{ aobResult.signatureWarning }}</p>
        <p v-if="aobSignatureResult?.warning" class="hint">{{ aobSignatureResult.warning }}</p>
        <p v-if="aobSignatureResult?.error" class="error">{{ aobSignatureResult.error }}</p>
        <p v-if="aobResult?.error" class="error">{{ aobResult.error }}</p>
        <div class="metrics patch-relay-status">
          <span
            class="quality-medium"
            title="Fallback PHASE 122 : si le patch code direct échoue en ERROR_ACCESS_DENIED sur la bascule RWX, KillEngine tente le relais PowerShell borné aux patchs code."
          >
            Relais patch prêt
          </span>
          <span title="Le relais ne s'applique pas aux écritures mémoire DATA génériques.">
            code uniquement
          </span>
        </div>
        <div class="controls code-patch-controls">
          <input
            v-model="codePatchAddress"
            class="input"
            placeholder="Adresse patch (0x...)"
            :disabled="codePatchBusy"
          />
          <input
            v-model="codePatchBytes"
            class="input"
            placeholder="Bytes exacts: 90 90"
            :disabled="codePatchBusy"
            @keyup.enter="applySelectedCodePatch()"
          />
          <button class="btn btn-primary" :disabled="codePatchBusy || !codePatchAddress.trim() || !codePatchBytes.trim()" @click="applySelectedCodePatch()">
            <span v-if="codePatchBusy" class="btn-spinner" aria-hidden="true"></span>
            Appliquer
          </button>
          <button class="btn btn-primary" :disabled="codePatchSuggestBusy || !codePatchAddress.trim()" @click="suggestSelectedCodePatches()">
            <span v-if="codePatchSuggestBusy" class="btn-spinner" aria-hidden="true"></span>
            Analyser
          </button>
          <button class="btn btn-secondary" :disabled="aobStabilizeBusy || !codePatchAddress.trim()" @click="stabilizeSelectedAobSignature()">
            <span v-if="aobStabilizeBusy" class="btn-spinner" aria-hidden="true"></span>
            Stabiliser AOB
          </button>
          <button class="btn btn-secondary" :disabled="codePatchBusy || !codePatchAddress.trim()" @click="restoreSelectedCodePatch()">
            Restaurer
          </button>
          <button class="btn btn-secondary" :disabled="!codePatchAddress.trim()" @click="bookmarkCurrentCodePatch()">
            Bookmark
          </button>
        </div>
        <div v-if="aobStabilizeResult" class="metrics">
          <span>Auto AOB: {{ aobStabilizeResult.success ? 'unique' : 'à ajuster' }}</span>
          <span v-if="aobStabilizeResult.matchesFound !== undefined">{{ formatNumber(Number(aobStabilizeResult.matchesFound)) }} match(es)</span>
          <span v-if="Array.isArray(aobStabilizeResult.tested)">{{ formatNumber(aobStabilizeResult.tested.length) }} pattern(s)</span>
        </div>
        <p v-if="aobStabilizeResult?.error" class="error">{{ aobStabilizeResult.error }}</p>
        <div v-if="codePatchSuggestionResult" class="metrics">
          <span>Instruction: {{ codePatchSuggestionResult.success ? 'OK' : 'FAIL' }}</span>
          <span v-if="codePatchSuggestionResult.instructionLength">{{ formatNumber(codePatchSuggestionResult.instructionLength) }} o</span>
          <span v-if="codePatchSuggestionResult.mnemonicHint">{{ codePatchSuggestionResult.mnemonicHint }}</span>
          <span v-if="codePatchSuggestionResult.category">{{ codePatchSuggestionResult.category }}</span>
          <span v-if="codePatchSuggestionResult.decoder">{{ codePatchSuggestionResult.decoder }}</span>
          <span v-if="codePatchSuggestionResult.signatureQuality" :class="`quality-${codePatchSuggestionResult.signatureQuality.level}`">
            AOB {{ codePatchSuggestionResult.signatureQuality.level }} · {{ codePatchSuggestionResult.signatureQuality.score }}/100
          </span>
        </div>
        <p v-if="codePatchSuggestionResult?.disassembly" class="hint">{{ codePatchSuggestionResult.disassembly }}</p>
        <p v-if="codePatchSuggestionResult?.stableAobPattern" class="hint">AOB stable: {{ codePatchSuggestionResult.stableAobPattern }}</p>
        <p v-if="codePatchSuggestionResult?.bytes" class="hint">Instruction: {{ codePatchSuggestionResult.bytes }}</p>
        <p v-if="codePatchSuggestionResult?.warning" class="hint">{{ codePatchSuggestionResult.warning }}</p>
        <div v-if="codePatchSuggestionResult?.suggestions?.length" class="patch-suggestion-list">
          <button
            v-for="suggestion in codePatchSuggestionResult.suggestions"
            :key="suggestion.label"
            class="btn compact"
            :class="suggestion.riskLevel === 'low' ? 'btn-primary' : 'btn-secondary'"
            type="button"
            :title="suggestion.description"
            @click="useCodePatchSuggestion(suggestion)"
          >
            {{ suggestion.label }}{{ suggestion.riskLevel ? ` · ${suggestion.riskLevel}` : '' }}
          </button>
        </div>
        <div v-if="valueOverrideSuggestion" class="controls value-override-controls">
          <span class="hint">
            {{ valueOverrideSuggestion.description }} ({{ valueOverrideSuggestion.valueSize }} octet(s), à l'offset {{ valueOverrideSuggestion.valueOffset }} de l'instruction).
          </span>
          <input
            v-model="valueOverrideInput"
            class="input"
            placeholder="Valeur : 999 ou 0x3E7"
            @keyup.enter="applyValueOverrideSuggestion()"
          />
          <button class="btn btn-primary compact" type="button" :disabled="!valueOverrideInput.trim()" @click="applyValueOverrideSuggestion()">
            Appliquer valeur
          </button>
        </div>
        <p v-if="valueOverrideError" class="warning-text">{{ valueOverrideError }}</p>
        <p v-if="codePatchSuggestionResult?.error" class="error">{{ codePatchSuggestionResult.error }}</p>
        <div v-if="codePatchResult" class="metrics">
          <span>Patch: {{ codePatchResult.success ? 'OK' : 'FAIL' }}</span>
          <span v-if="codePatchResult.bytesWritten">{{ formatNumber(codePatchResult.bytesWritten) }} o</span>
          <span v-if="codePatchResult.verified">vérifié</span>
          <span v-if="codePatchResult.protectionChanged" title="Bascule de protection directe ou fallback relais PowerShell selon le blocage runtime.">VirtualProtectEx/relais</span>
          <span v-if="codePatchResult.active">actif</span>
        </div>
        <p v-if="codePatchResult?.originalBytes" class="hint">Originaux: {{ codePatchResult.originalBytes }}</p>
        <p v-if="codePatchResult?.restoredBytes" class="hint">Restaurés: {{ codePatchResult.restoredBytes }}</p>
        <p v-if="codePatchResult?.error" class="error">{{ codePatchResult.error }}</p>
        <div class="controls code-patch-profile-controls">
          <input
            v-model="codePatchProfileName"
            class="input"
            placeholder="Profil trainer"
            :disabled="codePatchProfileBusy"
          />
          <input
            v-model="codePatchProfilePatchName"
            class="input"
            placeholder="Nom patch"
            :disabled="codePatchProfileBusy"
          />
          <input
            v-model="codePatchProfileDescription"
            class="input"
            placeholder="Description"
            :disabled="codePatchProfileBusy"
          />
          <button
            class="btn btn-primary"
            :disabled="codePatchProfileBusy || !codePatchProfileName.trim() || !codePatchProfilePatchName.trim() || !codePatchAddress.trim() || !codePatchBytes.trim()"
            @click="saveSelectedCodePatchProfile()"
          >
            <span v-if="codePatchProfileBusy" class="btn-spinner" aria-hidden="true"></span>
            Sauver trainer
          </button>
        </div>
        <div v-if="codePatchProfileResult" class="metrics">
          <span>Profil: {{ codePatchProfileResult.success ? 'OK' : 'FAIL' }}</span>
          <span v-if="codePatchProfileResult.profileName">{{ codePatchProfileResult.profileName }}</span>
          <span v-if="codePatchProfileResult.patchName">{{ codePatchProfileResult.patchName }}</span>
        </div>
        <p v-if="codePatchProfileResult?.error" class="error">{{ codePatchProfileResult.error }}</p>
        <div v-if="aobResult?.matches?.length" class="aob-list">
          <div v-for="match in aobResult.matches.slice(0, 80)" :key="match.address" class="aob-row">
            <code>0x{{ match.address }}</code>
            <span>{{ match.module || match.memoryType || '-' }}</span>
            <span>{{ match.moduleOffset ? `+0x${match.moduleOffset}` : match.protection || '-' }}</span>
            <button class="btn btn-secondary compact" type="button" @click="useAobMatchAddress(match.address)">Lire</button>
            <button class="btn btn-primary compact" type="button" @click="selectAobPatchAddress(match.address)">Patch</button>
            <button class="btn btn-secondary compact" type="button" @click="bookmarkAobMatch(match)">Note</button>
          </div>
        </div>
      </section>

      <InjectionPanel v-show="showStep('persist')" />

      <section v-show="showStep('inspect')" class="panel pointer-chain-panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>Pointer Chains <span class="hint-inline">(jeux modernes / applications dynamiques)</span></h2>
            <InfoDot topic="pointerChains" />
            <RiskBadge level="read" />
          </div>
          <span v-if="pointerScanResult">{{ formatNumber(pointerScanResult.chainCount) }} chaine(s)</span>
        </div>
        <p class="panel-hint">{{ $t('help.pointerChains.when') }}</p>
        <p class="hint">
          Pour les jeux modernes et applications avec allocations dynamiques, les ressources changent souvent d'adresse.
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
              <button
                class="btn btn-secondary compact"
                @click="bookmarkPointerChain(chain)"
              >
                Note
              </button>
              <button
                class="btn btn-secondary compact"
                title="Surveille cette chaine en live (adresse re-resolue a chaque cycle) dans le panneau Watch chaines de pointeurs."
                @click="watchPointerChain(chain)"
              >
                Watch
              </button>
            </div>
          </div>
        </div>
        <p v-if="pointerScanResult?.error" class="error">{{ pointerScanResult.error }}</p>
        <p v-if="pointerResolveResult" class="hint">
          Resolution : {{ pointerResolveResult.success ? 'OK 0x' + pointerResolveResult.finalAddress : 'ECHEC ' + pointerResolveResult.error }}
        </p>
      </section>

      <GroupScanPanel v-show="showStep('find')" :find-what-accesses-result="findWhatAccessesResult" />
      <PointerChainWatchPanel v-show="showStep('inspect')" />
      <ActionLogPanel v-show="showStep('persist')" />
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

.workflow-steps {
  display: grid;
  grid-template-columns: repeat(4, 1fr) auto;
  gap: 8px;
  margin-bottom: 16px;
}

.workflow-step {
  display: flex;
  flex-direction: column;
  gap: 5px;
  padding: 9px 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  text-align: left;
  cursor: pointer;
  transition: border-color 0.12s, background 0.12s;
}

.workflow-step:hover {
  border-color: var(--accent-hover);
}

.workflow-step.active {
  border-color: var(--accent);
  background: var(--bg-accent);
}

.workflow-step.compact-step {
  max-width: 150px;
}

.workflow-step-head {
  display: flex;
  align-items: center;
  gap: 7px;
}

.workflow-step-title {
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 600;
}

.workflow-step.active .workflow-step-title {
  color: var(--accent-hover);
}

.workflow-step-count {
  padding: 1px 6px;
  border-radius: 8px;
  background: var(--bg-primary);
  color: var(--text-secondary);
  font-size: 10px;
  font-weight: 700;
}

.workflow-step-what {
  color: var(--text-dim);
  font-size: 11px;
  line-height: 1.35;
}

.panel-heading {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  min-width: 0;
}

.empty-state {
  padding: 32px;
  color: var(--text-dim);
  text-align: center;
}

.empty-state .btn {
  margin-top: 10px;
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

/* Repère visuel par niveau de risque, memes couleurs que RiskBadge (lecture
   seule / ecrit en memoire / modifie le code) pour qu'un panneau se
   reconnaisse d'un coup d'oeil dans une page Expert autrement tres dense. */
.panel.risk-read {
  border-left: 3px solid rgba(158, 206, 106, 0.5);
  background: linear-gradient(90deg, rgba(158, 206, 106, 0.05), var(--bg-tertiary) 12%);
}

.panel.risk-write {
  border-left: 3px solid rgba(224, 175, 104, 0.55);
  background: linear-gradient(90deg, rgba(224, 175, 104, 0.07), var(--bg-tertiary) 12%);
}

.panel.risk-code {
  border-left: 3px solid rgba(247, 118, 142, 0.55);
  background: linear-gradient(90deg, rgba(247, 118, 142, 0.07), var(--bg-tertiary) 12%);
}

/* "Quand utiliser ce panneau" toujours visible, sans avoir a cliquer le "?"
   (le contenu complet what/cost/example reste dans InfoDot). Reprend la
   couleur du panneau pour rester coherent avec la bordure risk-*. */
.panel-hint {
  margin: 0 0 10px;
  padding: 5px 10px;
  border-left: 2px solid var(--border);
  color: var(--text-dim);
  font-size: 12px;
  font-style: italic;
  line-height: 1.4;
}

.panel.risk-read .panel-hint {
  border-left-color: rgba(158, 206, 106, 0.5);
}

.panel.risk-write .panel-hint {
  border-left-color: rgba(224, 175, 104, 0.55);
}

.panel.risk-code .panel-hint {
  border-left-color: rgba(247, 118, 142, 0.55);
}

.panel-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  margin-bottom: 10px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.panel-title-actions {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  color: var(--text-dim);
  font-size: 12px;
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

.quality-strong {
  color: var(--success);
}

.quality-medium {
  color: var(--warning);
}

.quality-weak,
.quality-invalid {
  color: var(--error);
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

.debugger-check {
  border-color: rgba(247, 118, 142, 0.28);
}

.debugger-guard {
  margin: 8px 0 0;
  padding: 8px 10px;
  border: 1px solid rgba(247, 118, 142, 0.26);
  border-radius: 6px;
  color: var(--muted);
  background: rgba(247, 118, 142, 0.08);
}

.debugger-guard strong {
  color: #ffb7c3;
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
  grid-template-columns: 28px minmax(120px, 1fr) 54px 54px minmax(76px, 0.7fr) minmax(76px, 0.7fr) 86px 74px auto auto auto auto auto;
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

.ui-string-row.changed {
  border-color: rgba(224, 175, 104, 0.55);
}

.ui-string-live-current,
.ui-string-live-previous,
.error-inline {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ui-string-live-current {
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
}

.ui-string-live-previous {
  color: var(--text-dim);
}

.error-inline {
  color: var(--error);
  font-size: 11px;
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

.intelligence-panel {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 10px;
  padding: 8px;
  border: 1px solid rgba(122, 162, 247, 0.2);
  border-radius: 6px;
  background: rgba(122, 162, 247, 0.06);
}

.intelligence-row {
  display: grid;
  grid-template-columns: 54px minmax(130px, 1fr) minmax(84px, 130px) 74px minmax(180px, 1.2fr) auto auto auto auto;
  gap: 8px;
  align-items: center;
  min-height: 34px;
  padding: 6px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.intelligence-row.conf-high {
  border-color: rgba(158, 206, 106, 0.35);
}

.intelligence-row.conf-medium {
  border-color: rgba(224, 175, 104, 0.3);
}

.intelligence-row.conf-low {
  border-color: rgba(247, 118, 142, 0.28);
}

.intelligence-row strong {
  color: var(--accent);
}

.intelligence-row code,
.intelligence-reasons {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.structure-panel {
  display: flex;
  max-height: 360px;
  flex-direction: column;
  gap: 4px;
  margin-top: 10px;
  padding: 8px;
  border: 1px solid rgba(158, 206, 106, 0.18);
  border-radius: 6px;
  background: rgba(13, 17, 32, 0.46);
  overflow-y: auto;
}

.structure-row {
  display: grid;
  grid-template-columns: minmax(130px, 1fr) 58px minmax(90px, 1fr) minmax(110px, 1fr) minmax(95px, 1fr) minmax(70px, auto) minmax(58px, auto);
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 7px;
  border: 1px solid rgba(122, 162, 247, 0.12);
  border-radius: 4px;
  color: var(--text-dim);
  font-size: 12px;
}

.structure-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  align-items: center;
}

.structure-actions span {
  color: var(--text-dim);
  font-size: 11px;
}

.structure-diff {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding-bottom: 6px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.08);
}

.structure-row.marked {
  border-color: rgba(158, 206, 106, 0.32);
  background: rgba(158, 206, 106, 0.07);
}

.structure-row code,
.structure-row span,
.structure-row strong {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
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

.save-file-panel {
  display: grid;
  gap: 10px;
}

.save-file-actions {
  justify-content: flex-start;
  flex-wrap: wrap;
}

.save-file-actions span {
  color: var(--text-dim);
  font-size: 12px;
}

.save-file-list {
  display: grid;
  gap: 6px;
}

.save-file-row {
  display: grid;
  grid-template-columns: minmax(130px, 220px) minmax(220px, 1fr) 86px minmax(150px, 190px);
  gap: 8px;
  align-items: center;
  min-height: 34px;
  padding: 7px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font: inherit;
  font-size: 12px;
  text-align: left;
  cursor: pointer;
}

.save-file-row:hover,
.save-file-row.selected {
  border-color: rgba(122, 162, 247, 0.48);
  background: rgba(122, 162, 247, 0.08);
}

.save-file-row:disabled {
  cursor: wait;
  opacity: 0.65;
}

.save-file-row strong,
.save-file-row span {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.save-file-row strong {
  color: var(--text-primary);
}

.save-file-preview {
  display: grid;
  gap: 6px;
  padding: 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: rgba(13, 17, 32, 0.42);
}

.save-file-textarea {
  width: 100%;
  min-height: 220px;
  resize: vertical;
  white-space: pre;
  font-family: var(--font-mono, monospace);
  font-size: 12px;
}

.save-file-watch-row,
.save-file-patch-row {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.save-file-patch-row .input {
  flex: 1;
  min-width: 140px;
}

.btn-danger {
  background: var(--error);
  color: white;
  border: none;
}

.btn-danger:hover:not(:disabled) {
  filter: brightness(1.08);
}

.local-settings-block {
  display: grid;
  gap: 6px;
  margin-top: 4px;
  padding-top: 10px;
  border-top: 1px solid rgba(122, 162, 247, 0.16);
}

.local-settings-list {
  max-height: 260px;
  overflow-y: auto;
}

.local-settings-row {
  display: grid;
  grid-template-columns: minmax(120px, 200px) 90px minmax(160px, 1fr);
  gap: 8px;
  align-items: center;
  min-height: 30px;
  padding: 6px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.local-settings-row strong {
  color: var(--text-primary);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.local-settings-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
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
  grid-template-columns: 28px minmax(140px, 1fr) 74px minmax(120px, 1fr) 70px 74px 80px auto auto auto auto;
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

.live-value {
  min-width: 0;
  padding: 2px 6px;
  border: 1px solid rgba(122, 162, 247, 0.25);
  border-radius: 999px;
  color: var(--text-primary);
  text-align: center;
}

.live-value.changed {
  border-color: rgba(224, 175, 104, 0.55);
  color: var(--warning);
}

.find-writes-panel {
  display: grid;
  gap: 6px;
  margin-top: 8px;
  padding: 8px;
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 6px;
  background: rgba(13, 17, 32, 0.46);
}

.find-writes-row {
  display: grid;
  grid-template-columns: minmax(150px, 1fr) minmax(140px, 1fr) minmax(90px, 150px) 78px 48px 88px 96px auto auto auto auto;
  gap: 8px;
  align-items: center;
  min-height: 30px;
  padding: 5px 8px;
  border: 1px solid rgba(122, 162, 247, 0.14);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.find-writes-row.selected {
  border-color: rgba(122, 162, 247, 0.48);
  background: rgba(122, 162, 247, 0.08);
}

.find-writes-row code,
.find-writes-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.find-writes-row code,
.find-writes-row strong {
  color: var(--text-primary);
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

.stable-locator {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.bp-live-stats {
  color: var(--text-dim);
  font-size: 11px;
  white-space: nowrap;
}

.bp-live-stats.warning {
  color: var(--warning);
}

.stable-locator-result {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
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

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
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

.patch-relay-status span {
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 3px 8px;
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

.kernel-escalation-guide {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  gap: 10px;
  align-items: center;
  margin-bottom: 8px;
  padding: 8px 10px;
  border: 1px solid color-mix(in srgb, var(--accent) 30%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-secondary));
  color: var(--text-dim);
  font-size: 12px;
}

.kernel-escalation-guide strong {
  color: var(--text-primary);
}

.stable-group-hint {
  margin-bottom: 8px;
  padding: 10px 12px;
  border: 1px solid rgba(224, 175, 104, 0.35);
  border-radius: 6px;
  background: rgba(224, 175, 104, 0.08);
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

.write-verified-badge {
  border: 1px solid rgba(158, 206, 106, 0.35);
  border-radius: 999px;
  color: var(--success);
  font-size: 10px;
  padding: 1px 6px;
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

.warning-hint {
  color: var(--warning);
}

.aob-controls {
  grid-template-columns: minmax(220px, 1fr) 120px auto;
  align-items: center;
}

.code-patch-controls {
  grid-template-columns: minmax(170px, 1fr) minmax(180px, 1fr) auto auto auto;
  align-items: center;
  margin-top: 10px;
}

.code-patch-profile-controls {
  grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) minmax(160px, 1fr) auto;
  align-items: center;
  margin-top: 10px;
}

.patch-suggestion-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.aob-flags {
  margin-top: 8px;
}

.aob-list {
  display: grid;
  gap: 5px;
  margin-top: 10px;
}

.aob-row {
  display: grid;
  grid-template-columns: minmax(150px, 1fr) minmax(120px, 180px) minmax(90px, 140px) auto auto;
  gap: 8px;
  align-items: center;
  min-height: 34px;
  padding: 6px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.aob-row code,
.aob-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.aob-row code {
  color: var(--text-primary);
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
  .aob-controls,
  .code-patch-controls,
  .code-patch-profile-controls,
  .aob-row,
  .kernel-escalation-guide,
  .pointer-chain-controls {
    grid-template-columns: 1fr;
  }
}

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
</style>
