<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import {
  backend,
  type ChangedPagesConsensusResult,
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
import PanelIntro from '@/components/common/PanelIntro.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import type { RiskLevel } from '@/components/expert/risk'
import RegionPanel from '@/components/expert/RegionPanel.vue'
import NextScanPanel from '@/components/expert/NextScanPanel.vue'
import WatchLivePanel from '@/components/expert/WatchLivePanel.vue'
import GroupScanPanel from '@/components/expert/GroupScanPanel.vue'
import FindWhatAccessesPanel from '@/components/expert/FindWhatAccessesPanel.vue'
import AutoDissectPanel from '@/components/expert/AutoDissectPanel.vue'
import PointerChainWatchPanel from '@/components/expert/PointerChainWatchPanel.vue'
import ActionLogPanel from '@/components/expert/ActionLogPanel.vue'
import SessionPanel from '@/components/expert/SessionPanel.vue'
import InjectionPanel from '@/components/expert/InjectionPanel.vue'
import SaveFilesPanel from '@/components/expert/SaveFilesPanel.vue'
import UnknownScanPanel from '@/components/expert/UnknownScanPanel.vue'
import ExactScanPanel from '@/components/expert/ExactScanPanel.vue'
import CandidatePanel from '@/components/expert/CandidatePanel.vue'
import PointerChainScanPanel from '@/components/expert/PointerChainScanPanel.vue'
import AobSignaturePanel from '@/components/expert/AobSignaturePanel.vue'
import WritePanel from '@/components/expert/WritePanel.vue'
import { useExpertWriteSelection } from '@/composables/useExpertWriteSelection'
import { useExpertPointerChain } from '@/composables/useExpertPointerChain'
import { useExpertAobFlow } from '@/composables/useExpertAobFlow'
import { formatNumber, formatBytes } from '@/utils/format'
import { findWhatWritesSizeForType } from '@/utils/valueTypes'

const { t } = useI18n()
const store = useAppStore()
const {
  selectedCandidateAddresses,
  setSelectedWriteTargets,
  clearCandidateSelection,
  scrollToWritePanel,
  watchedCandidate,
} = useExpertWriteSelection()

const { resetPointerChainState } = useExpertPointerChain()
const {
  aobPattern,
  aobResult,
  aobSignatureBusy,
  disassembleBackwardBusy,
  disassembleBackwardResult,
  testCandidateFieldsBusy,
  testCandidateFieldsResult,
  codePatchSuggestionResult,
  forceHookTargetHit,
  forceHookValueInput,
  forceHookBusy,
  forceHookResult,
  codePatchProfileBusy,
  codePatchTrainerFlowBusy,
  generateAobSignatureFromHit,
  disassembleBackwardFromHit,
  testCandidateFieldsFromHit,
  findWhatWritesHitKey,
  isSelectedFindWhatWritesHit,
  previewFindWhatWritesHit,
  copyFindWhatWritesRip,
  selectForceHookTarget,
  applyForceHookValue,
  saveTrainerPatchFromHit,
  resetAobFlowState,
} = useExpertAobFlow()
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
// PHASE 250 : Consensus multi-round pour Changed Pages Diff
const changedPagesSessionActive = ref(false)
const changedPagesSessionBusy = ref(false)
const changedPagesConsensusResult = ref<ChangedPagesConsensusResult | null>(null)
const changedPagesRoundPreviousValue = ref('')
const changedPagesRoundCurrentValue = ref('')
const changedPagesRoundBusy = ref(false)
const changedPagesStabilityResult = ref<Record<string, unknown> | null>(null)
const changedPagesStabilityBusy = ref(false)
const structureProbeResult = ref<Record<string, unknown> | null>(null)
const structureCaptureA = ref<StructureProbeRow[] | null>(null)
const structureCaptureB = ref<StructureProbeRow[] | null>(null)
const structureCaptureAName = ref('')
const structureCaptureBName = ref('')
const structureCaptureABase = ref('')
const structureCaptureAField = ref('')
const structureCaptureBBase = ref('')
const structureCaptureBField = ref('')
const structureTemplateName = ref('')
const structureDeltaResult = ref<Record<string, unknown> | null>(null)
const structureDeltaBusy = ref(false)
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
        uiStringInvestigationStartResult.value = { success: false, windows: 0, error: t('expert.backendUnavailableMock') }
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
        error: t('expert.backendUnavailableMock'),
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
        error: t('expert.backendUnavailableMock'),
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
      uiStringTrackResult.value = { success: false, checked: 0, unreadable: 0, remaining: 0, survivors: [], error: t('expert.backendUnavailableMock') }
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

async function analyzeStructureAroundAddress(sourceAddress: string) {
  const address = addressNumber(sourceAddress)
  if (!Number.isFinite(address)) return
  const base = Math.max(0, address - 128)
  const targetValue = displayedNumericValue()
  structureProbeResult.value = null
  try {
    const controller = backend.getController()
    if (controller.analyzeStructureMemory) {
      const result = await controller.analyzeStructureMemory(base.toString(16).toUpperCase(), 256)
      if (result.success !== true) {
        structureProbeResult.value = { success: false, error: String(result.error || t('expert.structureAnalysisFailed')) }
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
            if (Number.isFinite(numeric) && Math.abs(numeric - targetValue) < 0.001) markerParts.push(t('expert.markerDisplayedValue'))
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
        address: sourceAddress,
        rows,
        rowCount: rows.length,
        fieldCount: result.fieldCount,
      }
      return
    }

    const preview = await store.readMemoryPreviewByMode(base.toString(16).toUpperCase(), 256)
    if (!preview.success && !preview.partial) {
      structureProbeResult.value = { success: false, error: preview.error || t('expert.structureReadFailed') }
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
      if (targetValue !== null && int32 === Math.trunc(targetValue)) markers.push(t('expert.markerInt32Displayed'))
      if (targetValue !== null && Math.abs(float32 - targetValue) < 0.001) markers.push(t('expert.markerFloat32Displayed'))
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
      address: sourceAddress,
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
  store.createTrainerFeature({
    name: `Struct ${row.type} 0x${row.address}`,
    action: 'write',
    address: row.address,
    valueType: String(row.type ?? 'Int32'),
    value: String(row.value ?? ''),
  })
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
  const baseAddress = String(structureProbeResult.value?.base ?? '').replace(/^0x/i, '').toUpperCase()
  const fieldAddress = String(structureProbeResult.value?.address ?? '').replace(/^0x/i, '').toUpperCase()
  const label = `0x${String(structureProbeResult.value?.address ?? structureProbeResult.value?.base ?? '')} · ${new Date().toLocaleTimeString('fr-FR')}`
  if (slot === 'A') {
    structureCaptureA.value = rows
    structureCaptureAName.value = label
    structureCaptureABase.value = baseAddress
    structureCaptureAField.value = fieldAddress
  } else {
    structureCaptureB.value = rows
    structureCaptureBName.value = label
    structureCaptureBBase.value = baseAddress
    structureCaptureBField.value = fieldAddress
  }
  structureDeltaResult.value = null
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

async function analyzeStructureAroundSource(candidate: UiStringSourceCandidate) {
  await analyzeStructureAroundAddress(candidate.address)
}

async function inferStructureDelta() {
  const controller = backend.getController()
  if (!controller.inferStructureInstanceDelta) {
    structureDeltaResult.value = { success: false, error: t('expert.backendUnavailable') }
    return
  }
  if (!structureCaptureABase.value || !structureCaptureAField.value || !structureCaptureBBase.value || !structureCaptureBField.value) {
    structureDeltaResult.value = { success: false, error: t('expert.captureAbIncomplete') }
    return
  }

  structureDeltaBusy.value = true
  try {
    structureDeltaResult.value = await controller.inferStructureInstanceDelta(
      structureCaptureABase.value,
      structureCaptureAField.value,
      structureCaptureBBase.value,
      structureCaptureBField.value,
      { beforeCount: 2, afterCount: 6 },
    )
  } catch (e) {
    structureDeltaResult.value = { success: false, error: String(e), candidates: [] }
  } finally {
    structureDeltaBusy.value = false
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
          error: preview.error || t('expert.readFailed'),
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
    reasons.push(t('expert.reasonBase', { percent: confidencePercent(score) }))

    const distance = Number(source.distanceBytes ?? Number.MAX_SAFE_INTEGER)
    if (Number.isFinite(distance) && distance <= 4096) {
      score += 0.12
      reasons.push(t('expert.reasonCloseString'))
    } else if (Number.isFinite(distance) && distance <= 1024 * 1024) {
      score += 0.04
      reasons.push(t('expert.reasonSameWindow'))
    }

    const trackHits = Number(source.trackHits ?? 0)
    if (trackHits > 0) {
      score += Math.min(0.18, 0.06 * trackHits)
      reasons.push(t('expert.reasonTrackedOk', { count: trackHits }))
    }

    if ((uiStringInvestigationFinishResult.value?.globalValueHits ?? []).some((hit) => sourceKey(hit) === sourceKey(source))) {
      score += 0.16
      reasons.push(t('expert.reasonRadarChanged'))
    }

    const watched = watchedCandidate(source.address)
    if (watched?.changed) {
      score += 0.1
      reasons.push(t('expert.reasonWatchMoves'))
    }
    if (isUiSourceSelected(source)) {
      score += 0.04
      reasons.push(t('expert.reasonSelected'))
    }
    if (debuggerTargets.has(normalizeAddress(source.address))) {
      score += 0.22
      reasons.push(t('expert.reasonWriterCaptured'))
    }
    if ((source.variantLabel || '').includes('x')) {
      score -= 0.03
      reasons.push(t('expert.reasonEncoding'))
    }
    if (source.type.endsWith('8')) {
      score -= 0.08
      reasons.push(t('expert.reasonCompactNoisy'))
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
        error: t('expert.backendUnavailableMock'),
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
        error: t('expert.backendUnavailableMock'),
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
        error: t('expert.backendUnavailableMock'),
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
  setSelectedWriteTargets([{
    address: candidate.address,
    type: candidate.type,
    variantLabel: candidate.variantLabel,
  }])
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
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: t('expert.enableDebuggerBeforeWrittenBy') }
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
  if (!await store.confirmRiskAction('debug', 'Find what writes', t('expert.addressTimeoutDescription', { address: address.replace(/^0x/i, ''), timeout: String(options.timeoutMs ?? '?') }))) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: t('expert.captureCancelledByUser'),
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
          error: t('expert.timeoutFindWhatWrites'),
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
            error: String(start.error ?? t('expert.unableToStartFindWhatWrites')),
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
    return { success: false, hitCount: 0, hits: [], error: t('expert.backendUnavailable') }
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
      error: t('expert.cancelFindWhatWritesUnavailable'),
    }
    return
  }
  const result = await controller.cancelFindWhatWrites()
  if (result.success !== true) {
    findWhatWritesResult.value = {
      success: false,
      hitCount: 0,
      hits: [],
      error: String(result.error ?? t('expert.cancelFindWhatWritesFailed')),
    }
  }
}

async function runPageGuardWatch(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('injection', t('expert.pageGuardNoDebuggerLabel'), t('expert.pageGuardInjectDescription', { address: address.replace(/^0x/i, '') }))) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: t('expert.pageGuardCancelledByUser'),
    }
  }
  const controller = backend.getController()
  const fn = controller.startPageGuardWatchAsync
  const sig = controller.pageGuardWatchFinished
  if (!fn || !sig) {
    return { success: false, hitCount: 0, hits: [], error: t('expert.pageGuardUnavailableBackend') }
  }
  return new Promise<Record<string, unknown>>((resolve) => {
    let requestId: number | null = null
    let settled = false
    const earlyPayloads: Array<Record<string, unknown>> = []
    const timeout = window.setTimeout(() => {
      settled = true
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: t('expert.timeoutPageGuard') })
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
        resolve({ success: false, hitCount: 0, hits: [], error: String(start.error ?? t('expert.unableToStartPageGuard')) })
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
    pageGuardResult.value = { success: false, hitCount: 0, hits: [], error: t('expert.cancelPageGuardUnavailable') }
    return
  }
  const result = await controller.cancelPageGuardWatch()
  if (result.success !== true) {
    pageGuardResult.value = { success: false, hitCount: 0, hits: [], error: String(result.error ?? t('expert.cancelPageGuardFailed')) }
  }
}

// ---- PHASE 250 : Consensus multi-round pour Changed Pages Diff ----
async function startChangedPagesSession() {
  const controller = backend.getController()
  if (!controller.startChangedPagesSession) {
    changedPagesConsensusResult.value = { success: false, error: t('expert.consensusMultiRoundUnavailable') }
    return
  }
  changedPagesSessionBusy.value = true
  try {
    const result = await controller.startChangedPagesSession({})
    changedPagesSessionActive.value = result.success === true
    changedPagesConsensusResult.value = result
  } catch (e) {
    changedPagesConsensusResult.value = { success: false, error: String(e) }
  } finally {
    changedPagesSessionBusy.value = false
  }
}

async function applyChangedPagesRound() {
  const controller = backend.getController()
  if (!controller.applyChangedPagesRound) {
    changedPagesConsensusResult.value = { success: false, error: t('expert.roundMultiRoundUnavailable') }
    return
  }
  changedPagesRoundBusy.value = true
  try {
    const result = await controller.applyChangedPagesRound(
      changedPagesRoundPreviousValue.value,
      changedPagesRoundCurrentValue.value,
      {}
    )
    changedPagesConsensusResult.value = result
  } catch (e) {
    changedPagesConsensusResult.value = { success: false, error: String(e) }
  } finally {
    changedPagesRoundBusy.value = false
  }
}

async function getChangedPagesConsensus() {
  const controller = backend.getController()
  if (!controller.getChangedPagesConsensus) {
    changedPagesConsensusResult.value = { success: false, error: t('expert.consensusUnavailable') }
    return
  }
  try {
    const result = await controller.getChangedPagesConsensus({})
    changedPagesConsensusResult.value = result
  } catch (e) {
    changedPagesConsensusResult.value = { success: false, error: String(e) }
  }
}

async function stopChangedPagesSession() {
  const controller = backend.getController()
  if (!controller.stopChangedPagesSession) {
    changedPagesConsensusResult.value = { success: false, error: t('expert.stopSessionUnavailable') }
    return
  }
  try {
    const result = await controller.stopChangedPagesSession()
    changedPagesConsensusResult.value = result
    changedPagesSessionActive.value = false
  } catch (e) {
    changedPagesConsensusResult.value = { success: false, error: String(e) }
  }
}

async function validatePageStability(addressHex: string) {
  const controller = backend.getController()
  if (!controller.validatePageStability) {
    changedPagesStabilityResult.value = { success: false, error: t('expert.stabilityValidationUnavailable') }
    return
  }
  changedPagesStabilityBusy.value = true
  try {
    const result = await controller.validatePageStability(addressHex, { readCount: 5, intervalMs: 200, pageSize: 4096 })
    changedPagesStabilityResult.value = result
  } catch (e) {
    changedPagesStabilityResult.value = { success: false, error: String(e) }
  } finally {
    changedPagesStabilityBusy.value = false
  }
}

// ---- Find What Accesses (P1) : instructions qui LISSENT l'adresse ----
const findWhatAccessesResult = ref<Record<string, unknown> | null>(null)
const findWhatAccessesBusy = ref(false)

async function runFindWhatAccesses(address: string, options: Record<string, unknown>) {
  if (!await store.confirmRiskAction('debug', 'Find what accesses', t('expert.addressTimeoutDescription', { address: address.replace(/^0x/i, ''), timeout: String(options.timeoutMs ?? '?') }))) {
    return {
      success: false,
      hitCount: 0,
      hits: [],
      cancelled: true,
      error: t('expert.captureCancelledByUser'),
    }
  }
  const controller = backend.getController()
  const fn = controller.findWhatAccessesAsync
  const sig = controller.findWhatAccessesFinished
  if (!fn || !sig) {
    return { success: false, hitCount: 0, hits: [], error: t('expert.findWhatAccessesUnavailableBackend') }
  }
  return new Promise<Record<string, unknown>>((resolve) => {
    let requestId: number | null = null
    let settled = false
    const earlyPayloads: Array<Record<string, unknown>> = []
    const timeout = window.setTimeout(() => {
      settled = true
      sig.disconnect?.(handler)
      resolve({ success: false, hitCount: 0, hits: [], error: t('expert.timeoutFindWhatAccesses') })
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
        resolve({ success: false, hitCount: 0, hits: [], error: String(start.error ?? t('expert.unableToStartFindWhatAccesses')) })
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
    findWhatAccessesResult.value = { success: false, hitCount: 0, hits: [], error: t('expert.enableDebuggerBeforeReadBy') }
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
    findWhatWritesResult.value = { success: false, hitCount: 0, hits: [], error: t('expert.enableDebuggerBeforeWrittenBy') }
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
  setSelectedWriteTargets(chosen.map((candidate) => ({
    address: candidate.address,
    type: candidate.type,
    variantLabel: candidate.variantLabel,
  })))
  const types = Array.from(new Set(chosen.map((candidate) => candidate.type)))
  if (types.length === 1) {
    store.exactScanType = types[0]
  }
  store.selectedCandidateAddress = chosen[0].address
  store.writeValue = uiStringValue.value || store.exactScanValue
  scrollToWritePanel()
}

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
const structureDeltaCandidates = computed(() => {
  const candidates = structureDeltaResult.value?.candidates
  return Array.isArray(candidates) ? candidates as Array<Record<string, unknown>> : []
})
const expertScenarioPresets = computed(() => store.workflowPresets.filter((preset) => preset.id.startsWith('scenario-')))

function formatSignedDelta(value: unknown) {
  const numeric = Number(value)
  if (!Number.isFinite(numeric)) return '-'
  const sign = numeric >= 0 ? '+' : '-'
  return `${sign}0x${Math.abs(Math.trunc(numeric)).toString(16).toUpperCase()}`
}

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

  clearCandidateSelection()

  resetPointerChainState()

  resetAobFlowState()

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
  findWhatWritesAcknowledged.value = false
  findWhatAccessesResult.value = null

  structureProbeResult.value = null
  structureCaptureA.value = null
  structureCaptureB.value = null
  structureCaptureAName.value = ''
  structureCaptureBName.value = ''
  structureCaptureABase.value = ''
  structureCaptureAField.value = ''
  structureCaptureBBase.value = ''
  structureCaptureBField.value = ''
  structureTemplateName.value = ''
  structureDeltaResult.value = null
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

function candidateCurrentValue(address: string): string {
  return watchedCandidate(address)?.value || '-'
}

onMounted(() => {
  if (store.candidatePage) return
  void store.refreshCandidates()
})

// PHASE 120-B : consomme une demande d'etape venant d'un bouton de
// recommandation chat (recoveryActions -> open_expert/open_pointer_scan avec
// expertStep) -- une seule fois, puis remis a null pour ne jamais affecter
// une visite manuelle ulterieure d'Expert. Si une ancre precise est fournie
// en plus (une section est loin dans une longue etape, ex. "Ecrit par"),
// scrolle dessus apres que le changement d'etape ait mis a jour le DOM
// (nextTick) -- sans ca, l'element vise peut ne pas encore etre repositionne.
onMounted(() => {
  if (store.pendingExpertStep) {
    activeStep.value = store.pendingExpertStep
    store.pendingExpertStep = null
  }
  if (store.pendingExpertAnchor) {
    const anchorId = store.pendingExpertAnchor
    store.pendingExpertAnchor = null
    void nextTick(() => {
      document.getElementById(anchorId)?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    })
  }
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
          {{ $t('expert.newScan') }}
        </button>
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
      </div>
    </div>

    <PanelIntro
      :what="$t('expert.intro.what')"
      :purpose="$t('expert.intro.purpose')"
      :how="$t('expert.intro.how')"
    />

    <div v-if="!store.isAttached" class="empty-state">
      <p>{{ $t('expert.attachToUse') }}</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">{{ $t('expert.goToProcess') }}</button>
    </div>

    <template v-else>
      <div class="preset-row">
        <span class="hint preset-row-label">{{ $t('expert.commonScenarios') }}</span>
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
              {{ $t('expert.cancel') }}
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
          <span>{{ $t('expert.address') }}</span>
          <strong>{{ store.selectedCandidateAddress ? `0x${store.selectedCandidateAddress}` : '-' }}</strong>
        </div>
      </div>

      <nav class="workflow-steps" :aria-label="$t('expert.stepsAriaLabel')">
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
            <span class="workflow-step-title">{{ $t('expert.all') }}</span>
          </span>
          <span class="workflow-step-what">{{ $t('expert.showAllFourSteps') }}</span>
        </button>
      </nav>

      <RegionPanel v-show="showStep('inspect')" v-if="store.expertRegionSize || store.expertRegionProtection" />

      <SaveFilesPanel v-show="showStep('inspect')" />

      <ExactScanPanel v-show="showStep('find')" :on-start-new-scan="startNewScan" />

      <NextScanPanel v-show="showStep('find')" />

      <UnknownScanPanel v-show="showStep('find')" />

      <section v-show="showStep('find')" class="panel ui-string-panel risk-read">
        <div class="panel-title">
          <div class="panel-heading">
            <h2>Trace UI string</h2>
            <InfoDot topic="uiString" />
            <RiskBadge level="read" />
          </div>
          <span v-if="uiStringResult">{{ $t('expert.candidateCount', { count: formatNumber(uiStringCandidates.length) }) }}</span>
        </div>
        <p class="panel-hint">{{ $t('help.uiString.when') }}</p>
        <p class="hint">{{ $t('expert.traceUiStringHint') }}</p>
        <div class="controls ui-string-controls">
          <input
            v-model="uiStringValue"
            class="input"
            :placeholder="$t('expert.step1Placeholder')"
            :disabled="uiStringBusy || store.scanBusy"
            @keyup.enter="scanUiStrings()"
          />
          <button class="btn btn-primary" :disabled="uiStringBusy || store.scanBusy || !(uiStringValue || store.exactScanValue).trim()" @click="scanUiStrings()">
            <span v-if="uiStringBusy" class="btn-spinner" aria-hidden="true"></span>
            {{ $t('expert.scanText') }}
          </button>
          <input
            v-model="uiStringNextValue"
            class="input"
            :placeholder="$t('expert.step2Placeholder')"
            :disabled="uiStringBusy || uiStringCandidates.length === 0"
            @keyup.enter="trackUiStrings()"
          />
          <button class="btn btn-primary" :disabled="uiStringBusy || uiStringCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStrings()">
            {{ $t('expert.nextScanText') }}
          </button>
          <button class="btn btn-primary" :disabled="uiStringBusy || uiStringSourceCandidates.length === 0 || !uiStringNextValue.trim()" @click="trackUiStringSources()">
            {{ $t('expert.nextScanSources') }}
          </button>
        </div>
        <div class="ui-investigation" :class="{ active: uiStringLiveInvestigation }">
          <div class="scanner-visual" aria-hidden="true">
            <div class="scanner-ring"></div>
            <div class="scanner-sweep"></div>
            <div class="scanner-core"></div>
          </div>
          <div class="investigation-copy">
            <strong>{{ uiStringLiveInvestigation ? $t('expert.investigationArmed') : $t('expert.investigationLiveReady') }}</strong>
            <span>
              {{ uiStringLiveInvestigation
                ? $t('expert.snapshotCapturedHint', { seconds: formatNumber(uiStringInvestigationElapsed) })
                : $t('expert.startBeforeModifyingHint') }}
            </span>
          </div>
          <div class="investigation-steps">
            <span>strings</span>
            <span>sources</span>
            <span>backrefs</span>
          </div>
          <button class="btn compact" :class="uiStringLiveInvestigation ? 'btn-secondary' : 'btn-primary'" type="button" :disabled="uiStringBusy" @click="toggleUiStringLiveInvestigation()">
            {{ uiStringLiveInvestigation ? $t('expert.stopAndCompare') : $t('expert.startInvestigation') }}
          </button>
        </div>
        <div v-if="uiStringInvestigationStartResult" class="metrics">
          <span>{{ $t('expert.investigationWindows') }} {{ formatNumber(uiStringInvestigationStartResult.windows) }}</span>
          <span>{{ $t('expert.captured') }} {{ formatBytes(uiStringInvestigationStartResult.bytesCaptured) }}</span>
          <span v-if="uiStringInvestigationStartResult.probeBlocks">{{ $t('expert.radarBlocks') }} {{ formatNumber(uiStringInvestigationStartResult.probeBlocks) }}</span>
          <span v-if="uiStringInvestigationStartResult.probeBytesCaptured">{{ $t('expert.radar') }} {{ formatBytes(uiStringInvestigationStartResult.probeBytesCaptured) }}</span>
          <span v-if="uiStringInvestigationStartResult.radiusBytes">{{ $t('expert.radius') }} {{ formatBytes(uiStringInvestigationStartResult.radiusBytes) }}</span>
          <span v-if="uiStringInvestigationStartResult.unreadable">{{ $t('expert.unreadable') }} {{ formatNumber(uiStringInvestigationStartResult.unreadable) }}</span>
        </div>
        <p v-if="uiStringInvestigationStartResult?.error" class="error">{{ uiStringInvestigationStartResult.error }}</p>
        <div v-if="uiStringInvestigationFinishResult" class="metrics">
          <span>{{ $t('expert.changes') }} {{ formatNumber(uiStringInvestigationFinishResult.changesFound) }}</span>
          <span>{{ $t('expert.modifiedBytes') }} {{ formatNumber(uiStringInvestigationFinishResult.changedBytes) }}</span>
          <span>{{ $t('expert.windowsRead') }} {{ formatNumber(uiStringInvestigationFinishResult.windowsChecked) }}</span>
          <span v-if="uiStringInvestigationFinishResult.probeBlocksChanged !== undefined">{{ $t('expert.modifiedBlocks') }} {{ formatNumber(uiStringInvestigationFinishResult.probeBlocksChanged) }}</span>
          <span v-if="uiStringInvestigationFinishResult.globalValueHitsFound !== undefined">{{ $t('expert.radarValues') }} {{ formatNumber(uiStringInvestigationFinishResult.globalValueHitsFound) }}</span>
          <span v-if="uiStringInvestigationFinishResult.partial" class="warning-text">{{ $t('expert.limitedResults') }}</span>
        </div>
        <p v-if="uiStringInvestigationFinishResult?.error" class="error">{{ uiStringInvestigationFinishResult.error }}</p>
        <div v-if="uiStringInvestigationFinishResult && !uiStringInvestigationFinishResult.error && uiStringInvestigationFinishResult.changesFound === 0 && !(uiStringInvestigationFinishResult.globalValueHits?.length)" class="investigation-empty">
          <strong>{{ $t('expert.noChangeCaptured') }}</strong>
          <span>
            {{ $t('expert.noChangeCapturedDetail') }}
          </span>
        </div>
        <div v-if="uiStringInvestigationFinishResult?.globalValueHits?.length" class="investigation-hit-summary">
          <strong>{{ $t('expert.valuesFoundInModifiedBlocks') }}</strong>
          <span>
            {{ $t('expert.valuesFoundInModifiedBlocksDetail') }}
          </span>
        </div>
        <div v-if="uiStringInvestigationFinishResult?.changes.length" class="investigation-change-list">
          <div class="source-list-title">
            <strong>{{ $t('expert.changesDuringInvestigation') }}</strong>
            <span>{{ $t('expert.leadCount', { count: formatNumber(uiStringInvestigationFinishResult.changes.length) }) }}</span>
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
            {{ $t('expert.isolatedNumber') }}
          </label>
          <label class="compact-select">
            <span>{{ $t('expert.sourceRadius') }}</span>
            <select v-model.number="uiStringSourceRadiusBytes" class="input select" :disabled="uiStringBusy">
              <option v-for="option in uiStringSourceRadiusOptions" :key="option.value" :value="option.value">
                {{ option.label }}
              </option>
            </select>
          </label>
          <label class="checkbox-label debugger-check">
            <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
            {{ $t('expert.debuggerAuthorized') }}
          </label>
          <label class="compact-select">
            <span>{{ $t('expert.writtenBy') }}</span>
            <InfoDot topic="findWhatWrites" />
            <select v-model.number="findWhatWritesTimeoutMs" class="input select" :disabled="findWhatWritesBusy">
              <option v-for="timeout in findWhatWritesTimeoutOptions" :key="timeout" :value="timeout">
                {{ timeout / 1000 }} s
              </option>
            </select>
          </label>
        </div>
        <p id="expert-anchor-find-what-writes" class="debugger-guard">
          <strong>{{ $t('expert.writtenBy') }}</strong> {{ $t('expert.writtenByGuardText') }}
          {{ $t('expert.writtenByGuardDetail') }}
        </p>
        <div v-if="uiStringResult" class="metrics">
          <span>Matches: {{ formatNumber(uiStringResult.matchesFound) }}</span>
          <span>{{ $t('expert.regions') }} {{ formatNumber(uiStringResult.regionsScanned) }}</span>
          <span>{{ $t('expert.readShort') }} {{ formatBytes(uiStringResult.bytesScanned) }}</span>
          <span v-if="uiStringResult.partial" class="warning-text">{{ $t('expert.limitReached') }}</span>
        </div>
        <div v-if="uiStringTrackResult" class="metrics">
          <span>{{ $t('expert.tested') }} {{ formatNumber(uiStringTrackResult.checked) }}</span>
          <span>{{ $t('expert.remaining') }} {{ formatNumber(uiStringTrackResult.remaining) }}</span>
          <span>{{ $t('expert.unreadable') }} {{ formatNumber(uiStringTrackResult.unreadable) }}</span>
          <span v-if="uiStringTrackResult.moved">{{ $t('expert.moved') }} {{ formatNumber(uiStringTrackResult.moved) }}</span>
        </div>
        <div v-if="uiStringSourceResult" class="metrics">
          <span>{{ $t('expert.sources') }} {{ formatNumber(uiStringSourceResult.matchesReturned) }}</span>
          <span>{{ $t('expert.windowsRead') }} {{ formatBytes(uiStringSourceResult.bytesScanned) }}</span>
          <span v-if="uiStringSourceResult.radiusBytes">{{ $t('expert.radius') }} {{ formatBytes(uiStringSourceResult.radiusBytes) }}</span>
          <span v-if="uiStringSourceResult.partial" class="warning-text">{{ $t('expert.limitedResults') }}</span>
        </div>
        <div v-if="uiStringSourceTrackResult" class="metrics">
          <span>{{ $t('expert.sourcesTested') }} {{ formatNumber(uiStringSourceTrackResult.checked) }}</span>
          <span>{{ $t('expert.sourcesRemaining') }} {{ formatNumber(uiStringSourceTrackResult.remaining) }}</span>
          <span>{{ $t('expert.unreadable') }} {{ formatNumber(uiStringSourceTrackResult.unreadable) }}</span>
          <span v-if="uiStringSourceTrackResult.incompatible">{{ $t('expert.incompatible') }} {{ formatNumber(uiStringSourceTrackResult.incompatible) }}</span>
        </div>
        <p v-if="uiStringResult?.error" class="error">{{ uiStringResult.error }}</p>
        <p v-if="uiStringTrackResult?.error" class="error">{{ uiStringTrackResult.error }}</p>
        <p v-if="uiStringSourceResult?.error" class="error">{{ uiStringSourceResult.error }}</p>
        <p v-if="uiStringSourceTrackResult?.error" class="error">{{ uiStringSourceTrackResult.error }}</p>
        <p v-if="uiStringCandidates.length > 0" class="hint">
          {{ $t('expert.step3Hint') }}
        </p>
        <div v-if="uiStringCandidates.length > 0" class="selection-toolbar">
          <button class="btn btn-secondary compact" type="button" @click="toggleAllUiStringSelection()">
            {{ selectedUiStringAddresses.length === uiStringCandidates.length ? $t('expert.uncheckAll') : $t('expert.checkAll') }}
          </button>
          <button class="btn btn-primary compact" type="button" :disabled="uiStringBusy" @click="analyzeUiStringSources()">
            {{ $t('expert.analyzeSources') }}
          </button>
          <button class="btn btn-primary compact" type="button" :disabled="uiStringBusy" @click="autoInspectUiStrings()">
            {{ $t('expert.autoOrigin') }}
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
            {{ uiStringTextLiveEnabled ? $t('expert.liveStringsStop') : $t('expert.liveStrings') }}
          </button>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="uiStringBusy || uiStringTextLiveRefreshing || uiStringCandidates.length === 0"
            @click="refreshSelectedUiStringTexts()"
          >
            {{ $t('expert.refreshStrings') }}
          </button>
          <span>{{ $t('expert.trackedNextFilter', { count: selectedUiStringAddresses.length || uiStringCandidates.length }) }}</span>
          <label class="checkbox-label debugger-check" :title="$t('expert.debuggerRequiredTitle')">
            <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
            {{ $t('expert.debuggerAuthorized') }}
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
              {{ $t('expert.current') }} {{ uiStringLiveState(candidate)?.current || '-' }}
            </span>
            <span class="ui-string-live-previous">
              {{ $t('expert.before') }} {{ uiStringLiveState(candidate)?.previous || '-' }}
            </span>
            <span>{{ candidate.movedFrom ? `+${formatNumber(candidate.movedDistanceBytes)} o` : (candidate.protection || '-') }}</span>
            <span>{{ candidate.memoryType || '-' }}</span>
            <button class="btn btn-primary compact" type="button" @click="analyzeUiStringSources(candidate)">{{ $t('expert.sources') }}</button>
            <button class="btn btn-secondary compact" type="button" @click="inspectUiStringOrigins(candidate)">{{ $t('expert.origin') }}</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiStringCandidate(candidate)">{{ $t('expert.watchBytes') }}</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForUiString(candidate)">
              {{ $t('expert.writtenBy') }}
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
              {{ $t('expert.cancelCapture') }}
            </button>
          </div>
          <p v-if="findWhatWritesBusy" class="hint">{{ $t('expert.captureInProgress', { seconds: findWhatWritesTimeoutMs / 1000 }) }}</p>
          <p v-if="findWhatWritesResult?.error" class="error">{{ findWhatWritesResult.error }}</p>
          <div
            v-for="hit in findWhatWritesHits.slice(0, 12)"
            :key="findWhatWritesHitKey(hit)"
            class="find-writes-row"
            :class="{ selected: isSelectedFindWhatWritesHit(hit) }"
          >
            <code>RIP 0x{{ hit.instructionPointer }}</code>
            <span>{{ $t('expert.targetShort') }} 0x{{ hit.address }}</span>
            <span>{{ hit.module || '-' }}</span>
            <span>+0x{{ hit.moduleOffset || '0' }}</span>
            <span>T{{ hit.threadId }}</span>
            <span>{{ $t('expert.beforeShort') }} {{ formatNumber(Number(hit.valueBefore ?? 0)) }}</span>
            <strong>{{ $t('expert.currentShort') }} {{ formatNumber(Number(hit.valueAfter ?? 0)) }}</strong>
            <button class="btn btn-secondary compact" type="button" @click="previewFindWhatWritesHit(hit)">
              {{ $t('expert.preview') }}
            </button>
            <button class="btn btn-secondary compact" type="button" @click="copyFindWhatWritesRip(hit)">
              {{ $t('expert.copy') }}
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="aobSignatureBusy" @click="generateAobSignatureFromHit(hit)">
              {{ $t('expert.analyze') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="disassembleBackwardBusy" @click="disassembleBackwardFromHit(hit)">
              {{ $t('expert.disassembleBackward') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="testCandidateFieldsBusy" @click="testCandidateFieldsFromHit(hit)">
              {{ $t('expert.testAutomatically') }}
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
              :title="$t('expert.forceValueHookTitle')"
              @click="selectForceHookTarget(hit)"
            >
              {{ $t('expert.forceValueHook') }}
            </button>
          </div>
          <div v-if="forceHookTargetHit" class="controls value-override-controls">
            <span class="hint">
              {{ $t('expert.forceValueAt', { address: forceHookTargetHit.address, rip: forceHookTargetHit.instructionPointer }) }}
              {{ $t('expert.forceValueTypeUsed', { type: store.exactScanType }) }}
            </span>
            <input
              v-model="forceHookValueInput"
              class="input"
              :placeholder="$t('expert.valuePlaceholder')"
              :disabled="forceHookBusy"
              @keyup.enter="applyForceHookValue()"
            />
            <button class="btn btn-primary compact" type="button" :disabled="forceHookBusy || !forceHookValueInput.trim()" @click="applyForceHookValue()">
              {{ $t('expert.apply') }}
            </button>
          </div>
          <p v-if="forceHookResult" :class="forceHookResult.success ? 'hint' : 'error'">
            {{ forceHookResult.success
              ? $t('expert.forceValueSuccess', { address: forceHookResult.patchAddress })
              : forceHookResult.error }}
          </p>
        </div>
        <div v-if="pageGuardResult || pageGuardBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>Page Guard ({{ $t('expert.withoutDebugger') }})</strong>
            <span>{{ formatNumber(Number(pageGuardResult?.hitCount ?? 0)) }} hit(s)</span>
            <button
              v-if="pageGuardBusy"
              class="btn btn-secondary compact"
              type="button"
              @click="cancelPageGuardWatchCapture()"
            >
              {{ $t('expert.cancelCapture') }}
            </button>
          </div>
          <p class="hint">{{ $t('expert.pageGuardAlternativeHint') }}</p>
          <p v-if="pageGuardBusy" class="hint">{{ $t('expert.captureInProgress', { seconds: findWhatWritesTimeoutMs / 1000 }) }}</p>
          <p v-if="pageGuardResult?.warning" class="hint">{{ pageGuardResult.warning }}</p>
          <p v-if="pageGuardResult?.error" class="error">{{ pageGuardResult.error }}</p>
          <div
            v-for="hit in pageGuardHits.slice(0, 12)"
            :key="findWhatWritesHitKey(hit)"
            class="find-writes-row"
          >
            <code>RIP 0x{{ hit.instructionPointer }}</code>
            <span>{{ $t('expert.targetShort') }} 0x{{ hit.address }}</span>
            <span>{{ hit.module || '-' }}</span>
            <span>+0x{{ hit.moduleOffset || '0' }}</span>
            <span>T{{ hit.threadId }}</span>
            <span>{{ hit.isWrite ? $t('expert.write') : $t('expert.read') }}</span>
            <button class="btn btn-secondary compact" type="button" @click="previewFindWhatWritesHit(hit)">
              {{ $t('expert.preview') }}
            </button>
            <button class="btn btn-secondary compact" type="button" @click="copyFindWhatWritesRip(hit)">
              {{ $t('expert.copy') }}
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="aobSignatureBusy" @click="generateAobSignatureFromHit(hit)">
              {{ $t('expert.analyze') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="disassembleBackwardBusy" @click="disassembleBackwardFromHit(hit)">
              {{ $t('expert.disassembleBackward') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="testCandidateFieldsBusy" @click="testCandidateFieldsFromHit(hit)">
              {{ $t('expert.testAutomatically') }}
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
        <!-- PHASE 250 : Consensus multi-round pour Changed Pages Diff -->
        <div v-if="changedPagesSessionActive || changedPagesSessionBusy || changedPagesConsensusResult" class="find-writes-panel">
          <div class="source-list-title">
            <strong>{{ $t('expert.multiRoundConsensus') }}</strong>
            <span v-if="changedPagesConsensusResult?.roundsApplied">{{ changedPagesConsensusResult.roundsApplied }} round(s)</span>
            <span v-if="changedPagesConsensusResult?.entriesConfirmed">{{ $t('expert.confirmedCount', { count: changedPagesConsensusResult.entriesConfirmed }) }}</span>
            <button
              v-if="changedPagesSessionActive"
              class="btn btn-secondary compact"
              type="button"
              :disabled="changedPagesSessionBusy"
              @click="stopChangedPagesSession()"
            >
              {{ $t('expert.stopSession') }}
            </button>
          </div>
          <p class="hint">{{ $t('expert.multiRoundSessionHint') }}</p>
          <p v-if="changedPagesSessionBusy" class="hint">{{ $t('expert.sessionInProgress') }}</p>
          <p v-if="changedPagesConsensusResult?.error" class="error">{{ changedPagesConsensusResult.error }}</p>
          <div class="controls" style="margin-bottom: 8px;">
            <button class="btn btn-primary compact" type="button" :disabled="changedPagesSessionBusy || changedPagesSessionActive" @click="startChangedPagesSession()">
              {{ $t('expert.startSession') }}
            </button>
            <button class="btn btn-primary compact" type="button" :disabled="changedPagesRoundBusy || !changedPagesSessionActive" @click="applyChangedPagesRound()">
              {{ $t('expert.applyRound') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="!changedPagesSessionActive" @click="getChangedPagesConsensus()">
              {{ $t('expert.consensus') }}
            </button>
          </div>
          <div class="controls" style="margin-bottom: 8px;">
            <input
              v-model="changedPagesRoundPreviousValue"
              class="input"
              :placeholder="$t('expert.beforeValuePlaceholder')"
              :disabled="changedPagesRoundBusy || !changedPagesSessionActive"
            />
            <input
              v-model="changedPagesRoundCurrentValue"
              class="input"
              :placeholder="$t('expert.afterValuePlaceholder')"
              :disabled="changedPagesRoundBusy || !changedPagesSessionActive"
            />
          </div>
          <div v-if="changedPagesConsensusResult?.confirmedEntries?.length" class="consensus-entries">
            <div class="source-list-title">
              <strong>{{ $t('expert.rankedEntries') }}</strong>
              <span>{{ $t('expert.addressCount', { count: changedPagesConsensusResult.confirmedEntries.length }) }}</span>
            </div>
            <div
              v-for="(entry, index) in changedPagesConsensusResult.confirmedEntries.slice(0, 30)"
              :key="`${entry.address}-${index}`"
              class="find-writes-row"
              :class="{ selected: entry.confirmed }"
            >
              <code>0x{{ entry.address }}</code>
              <span>{{ entry.type || '-' }}</span>
              <span>{{ entry.variantLabel || '-' }}</span>
              <span>{{ $t('expert.seenCount', { count: entry.roundsSeen }) }}</span>
              <span>{{ $t('expert.confirmedTimes', { count: entry.roundsConfirmed }) }}</span>
              <span v-if="entry.staleRounds > 0" class="warning-text">{{ $t('expert.staleTimes', { count: entry.staleRounds }) }}</span>
              <span v-if="entry.contradictionRounds > 0" class="error">{{ $t('expert.contradictionTimes', { count: entry.contradictionRounds }) }}</span>
              <span>{{ $t('expert.score') }} {{ entry.score?.toFixed(2) }}</span>
              <span v-if="entry.lastValueNumber !== undefined">{{ $t('expert.valueShort') }} {{ entry.lastValueNumber }}</span>
              <button class="btn btn-secondary compact" type="button" @click="validatePageStability(entry.address)">
                {{ $t('expert.stability') }}
              </button>
            </div>
          </div>
          <div v-if="changedPagesStabilityResult" class="stability-result" style="margin-top: 8px;">
            <p :class="changedPagesStabilityResult.stable ? 'hint' : 'error'">
              {{ changedPagesStabilityResult.reason || $t('expert.unknownStabilityResult') }}
            </p>
            <p v-if="changedPagesStabilityResult.readCount" class="hint">
              {{ $t('expert.stabilitySummary', { reads: changedPagesStabilityResult.readCount, changes: changedPagesStabilityResult.changeCount, unreadable: changedPagesStabilityResult.unreadable }) }}
            </p>
          </div>
        </div>
        <div v-if="disassembleBackwardResult || disassembleBackwardBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>{{ $t('expert.disassembleBackward') }}</strong>
            <InfoDot topic="disassembleBackward" />
            <span v-if="disassembleBackwardResult?.instructions">{{ $t('expert.instructionCount', { count: disassembleBackwardResult.instructions.length }) }}</span>
          </div>
          <p class="hint">{{ $t('expert.disassembleBackwardHint') }}</p>
          <p v-if="disassembleBackwardBusy" class="hint">{{ $t('expert.memoryReadInProgress') }}</p>
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
              {{ $t('expert.candidateFieldLabel', { register: instr.memBaseRegister, offset: instr.memDisplacement?.toString(16) }) }}
            </span>
          </div>
        </div>
        <div v-if="testCandidateFieldsResult || testCandidateFieldsBusy" class="find-writes-panel">
          <div class="source-list-title">
            <strong>{{ $t('expert.testAutomatically') }}</strong>
            <InfoDot topic="testCandidateFields" />
            <span v-if="testCandidateFieldsResult?.results">{{ $t('expert.fieldsTestedCount', { count: testCandidateFieldsResult.results.length }) }}</span>
          </div>
          <p class="hint">{{ $t('expert.testCandidateFieldsHint') }}</p>
          <p v-if="testCandidateFieldsBusy" class="hint">{{ $t('expert.testInProgress') }}</p>
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
              {{ outcome.verdict === 'holds' ? $t('expert.holdsWithTicks', { ticks: outcome.ticksSurvived ?? 0 }) : outcome.verdict === 'reverts' ? $t('expert.verdictReverts') : (outcome.error || $t('expert.verdictError')) }}
            </span>
          </div>
        </div>
        <div v-if="uiStringOriginResult" class="metrics">
          <span>Cluster: {{ uiStringOriginResult.clusterStart ? `0x${uiStringOriginResult.clusterStart}` : '-' }}</span>
          <span>{{ $t('expert.targets') }} {{ formatNumber(uiStringOriginResult.targetCount) }}</span>
          <span>Span: {{ formatBytes(uiStringOriginResult.clusterSpanBytes) }}</span>
          <span v-if="uiStringOriginResult.commonStrideBytes">Stride: {{ formatBytes(uiStringOriginResult.commonStrideBytes) }}</span>
          <span>Backrefs: {{ formatNumber(uiStringOriginResult.pointerRefsFound) }}</span>
          <span>{{ $t('expert.readShort') }} {{ formatBytes(uiStringOriginResult.bytesScanned) }}</span>
        </div>
        <p v-if="uiStringOriginResult?.error" class="error">{{ uiStringOriginResult.error }}</p>
        <div v-if="uiStringOriginResult?.pointerRefs.length" class="origin-list">
          <div class="source-list-title">
            <strong>{{ $t('expert.pointersToStrings') }}</strong>
            <span>{{ formatNumber(uiStringOriginResult.pointerRefs.length) }} ref(s)</span>
          </div>
          <div v-for="ref in uiStringOriginResult.pointerRefs.slice(0, 80)" :key="`${ref.address}:${ref.pointsTo}`" class="origin-row">
            <code>0x{{ ref.address }}</code>
            <span>→ 0x{{ ref.pointsTo }}</span>
            <span>{{ ref.distanceToString ? `${formatNumber(ref.distanceToString)} o` : $t('expert.exact') }}</span>
            <span>{{ ref.memoryType || '-' }}</span>
            <span>{{ ref.protection || '-' }}</span>
          </div>
        </div>
        <div v-if="intelligentUiCandidates.length > 0" class="intelligence-panel">
          <div class="source-list-title">
            <strong>{{ $t('expert.smartLeads') }}</strong>
            <span>top {{ formatNumber(Math.min(12, intelligentUiCandidates.length)) }}/{{ formatNumber(intelligentUiCandidates.length) }}</span>
            <div class="source-actions">
              <button class="btn btn-secondary compact" type="button" @click="selectTopIntelligentUiCandidates()">
                {{ $t('expert.checkTopAi') }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="watchSelectedUiSources()">
                {{ $t('expert.watchChecked') }}
              </button>
              <button class="btn btn-primary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="useSelectedUiSourcesForWrite()">
                {{ $t('expert.writeChecked') }}
              </button>
              <button class="btn btn-secondary compact" type="button" @click="copyInvestigationReport()">
                {{ $t('expert.copyReport') }}
              </button>
              <label class="checkbox-label debugger-check" :title="$t('expert.debuggerRequiredTitle')">
                <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
                {{ $t('expert.debuggerAuthorized') }}
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
            <button class="btn btn-secondary compact" type="button" @click="selectIntelligentUiCandidate(candidate)">{{ $t('expert.check') }}</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiSourceCandidate(candidate.source)">Watch</button>
            <button class="btn btn-secondary compact" type="button" @click="analyzeStructureAroundSource(candidate.source)">Struct</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForSource(candidate.source)">{{ $t('expert.writtenBy') }}</button>
            <button class="btn btn-secondary compact" type="button" :disabled="pageGuardBusy" :title="$t('expert.withoutWin32DebugChannel')" @click="pageGuardWatchForSource(candidate.source)">{{ $t('expert.writtenByNoDebugger') }}</button>
          </div>
        </div>
        <div v-if="structureProbeResult" class="structure-panel">
          <div class="source-list-title">
            <strong>{{ $t('expert.structureAroundSource') }}</strong>
            <span v-if="structureProbeResult.address">0x{{ structureProbeResult.address }} · {{ $t('expert.lineCount', { count: formatNumber(Number(structureProbeResult.rowCount ?? 0)) }) }}</span>
          </div>
          <div class="structure-actions">
            <button class="btn btn-secondary compact" type="button" :disabled="structureProbeRows.length === 0" @click="captureStructure('A')">
              {{ $t('expert.captureA') }}
            </button>
            <button class="btn btn-secondary compact" type="button" :disabled="structureProbeRows.length === 0" @click="captureStructure('B')">
              {{ $t('expert.captureB') }}
            </button>
            <input v-model="structureTemplateName" class="input compact-input" :placeholder="$t('expert.templateNamePlaceholder')" />
            <button class="btn btn-primary compact" type="button" :disabled="structureProbeRows.length === 0" @click="saveCurrentStructureTemplate()">
              {{ $t('expert.saveTemplate') }}
            </button>
            <button
              class="btn btn-secondary compact"
              type="button"
              :disabled="structureDeltaBusy || !structureCaptureA || !structureCaptureB"
              @click="inferStructureDelta()"
            >
              {{ $t('expert.instanceDelta') }}
            </button>
            <span v-if="structureCaptureAName">A: {{ structureCaptureAName }}</span>
            <span v-if="structureCaptureBName">B: {{ structureCaptureBName }}</span>
          </div>
          <p v-if="structureProbeResult.error" class="error">{{ structureProbeResult.error }}</p>
          <div v-if="structureDeltaResult" class="structure-delta">
            <div class="source-list-title">
              <strong>{{ $t('expert.instanceSpacing') }}</strong>
              <span v-if="structureDeltaResult.success">
                {{ $t('expert.strideFieldSummary', { stride: formatSignedDelta(structureDeltaResult.instanceDelta), offset: Number(structureDeltaResult.fieldOffsetA ?? 0).toString(16).toUpperCase() }) }}
              </span>
            </div>
            <p v-if="structureDeltaResult.error" class="error">{{ structureDeltaResult.error }}</p>
            <p v-else-if="structureDeltaResult.warning" class="error">{{ structureDeltaResult.warning }}</p>
            <div
              v-for="candidate in structureDeltaCandidates"
              :key="`delta:${candidate.relativeIndex}`"
              class="structure-row structure-delta-row"
              :class="{ marked: candidate.inputInstance }"
            >
              <span>#{{ candidate.relativeIndex }}</span>
              <code>{{ $t('expert.baseShort') }} 0x{{ candidate.baseAddress }}</code>
              <code>{{ $t('expert.fieldShort') }} 0x{{ candidate.fieldAddress }}</code>
              <span>{{ candidate.inputInstance ? $t('expert.capture') : $t('expert.probable') }}</span>
              <button
                class="btn btn-secondary compact"
                type="button"
                @click="analyzeStructureAroundAddress(String(candidate.fieldAddress ?? ''))"
              >
                Struct
              </button>
            </div>
          </div>
          <div v-if="structureDiffRows.length > 0" class="structure-diff">
            <div class="source-list-title">
              <strong>Diff A/B</strong>
              <span>{{ $t('expert.changeCount', { count: formatNumber(structureDiffRows.filter((row) => row.changed).length) }) }}</span>
            </div>
            <div
              v-for="row in structureDiffRows"
              :key="`diff:${row.offset}:${row.type}`"
              class="structure-row"
              :class="{ marked: row.changed }"
            >
              <code>0x{{ row.address }}</code>
              <span>{{ row.offset >= 0 ? '+' : '' }}{{ row.offset }}</span>
              <strong>{{ row.type || $t('expert.field') }}</strong>
              <span>{{ row.beforeValue || '-' }} → {{ row.afterValue || '-' }}</span>
              <span>{{ row.changed ? $t('expert.changed') : $t('expert.stable') }}</span>
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
              {{ $t('expert.note') }}
            </button>
          </div>
        </div>
        <div v-if="uiStringSourceCandidates.length > 0" class="source-list">
          <div class="source-list-title">
            <strong>{{ $t('expert.nearbyNumericSources') }}</strong>
            <span>{{ $t('expert.sourcesFilterSummary', { shown: formatNumber(filteredUiStringSourceCandidates.length), total: formatNumber(uiStringSourceCandidates.length), checked: formatNumber(selectedUiSourceAddresses.length) }) }}</span>
            <div class="source-filter-bar">
              <select v-model="uiStringSourceTypeFilter" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option value="all">{{ $t('expert.allTypes') }}</option>
                <option v-for="type in uiStringSourceTypeOptions" :key="type" :value="type">{{ type }}</option>
              </select>
              <select v-model="uiStringSourceVariantFilter" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option value="all">{{ $t('expert.allEncodings') }}</option>
                <option v-for="variant in uiStringSourceVariantOptions" :key="variant" :value="variant">{{ variant }}</option>
              </select>
              <select v-model.number="uiStringSourceBatchSize" class="input select compact-input" @change="uiStringSourceBatchIndex = 0">
                <option v-for="size in uiStringSourceBatchSizeOptions" :key="size" :value="size">{{ $t('expert.perBatch', { size }) }}</option>
              </select>
            </div>
            <div class="source-actions">
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="previousUiSourceBatch()">
                {{ $t('expert.prev') }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectUiSourceBatch()">
                {{ $t('expert.batch') }} {{ boundedUiStringSourceBatchIndex + 1 }}/{{ uiStringSourceBatchCount }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="nextUiSourceBatch()">
                {{ $t('expert.next') }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectTopUiSources()">
                {{ $t('expert.top') }} {{ uiStringSourceSafeSelectionLimit }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="currentUiStringSourceBatch.length === 0" @click="watchCurrentUiSourceBatch()">
                {{ $t('expert.watchBatch') }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="selectedUiSourceAddresses.length === 0" @click="watchSelectedUiSources()">
                {{ $t('expert.watchChecked') }}
              </button>
              <button class="btn btn-secondary compact" type="button" :disabled="safeFilteredUiStringSources.length === 0" @click="selectAllUiSources()">
                {{ $t('expert.checkAllSafe') }}
              </button>
              <button class="btn btn-secondary compact" type="button" @click="clearUiSourceSelection()">
                {{ $t('expert.uncheckAll') }}
              </button>
              <button
                class="btn btn-primary compact"
                type="button"
                :disabled="selectedUiSourceAddresses.length === 0"
                @click="useSelectedUiSourcesForWrite()"
              >
                {{ $t('expert.sendToWrite', { count: formatNumber(selectedUiSourceAddresses.length) }) }}
              </button>
              <label class="checkbox-label debugger-check" :title="$t('expert.debuggerRequiredTitle')">
                <input v-model="findWhatWritesAcknowledged" type="checkbox" :disabled="findWhatWritesBusy" />
                {{ $t('expert.debuggerAuthorized') }}
              </label>
            </div>
          </div>
          <p v-if="selectedUiSourceAddresses.length > 50" class="source-warning">
            {{ $t('expert.massiveSelectionWarning') }}
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
            <button class="btn btn-primary compact" type="button" @click="useUiSourceCandidate(candidate)">{{ $t('expert.use') }}</button>
            <button class="btn btn-secondary compact" type="button" @click="watchUiSourceCandidate(candidate)">Watch</button>
            <button class="btn btn-secondary compact" type="button" @click="analyzeStructureAroundSource(candidate)">Struct</button>
            <button class="btn btn-primary compact" type="button" :disabled="findWhatWritesBusy || !findWhatWritesAcknowledged" @click="findWhatWritesForSource(candidate)">
              {{ $t('expert.writtenBy') }}
            </button> <button class="btn btn-secondary compact" type="button" :disabled="findWhatAccessesBusy || !findWhatWritesAcknowledged" @click="findWhatAccessesForSource(candidate)">{{ $t('expert.readBy') }}</button>
          </div>
        </div>
      </section>

      <CandidatePanel v-show="showStep('inspect')" />

      <WritePanel v-show="showStep('act')" />
      <WatchLivePanel v-show="showStep('inspect')" />

      <AobSignaturePanel v-show="showStep('persist')" />
      <InjectionPanel v-show="showStep('persist')" />

      <PointerChainScanPanel v-show="showStep('inspect')" />

      <GroupScanPanel v-show="showStep('find')" :find-what-accesses-result="findWhatAccessesResult" />
      <FindWhatAccessesPanel v-show="showStep('find')" :find-what-accesses-result="findWhatAccessesResult" />
      <AutoDissectPanel v-show="showStep('find')" />
      <PointerChainWatchPanel v-show="showStep('inspect')" />
      <SessionPanel v-show="showStep('persist')" />
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
  color: var(--text-dim);
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

.structure-delta {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 6px 0;
  border-bottom: 1px solid rgba(255, 255, 255, 0.08);
}

.structure-delta-row {
  grid-template-columns: 52px minmax(130px, 1fr) minmax(130px, 1fr) 74px minmax(58px, auto);
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

.candidate-stability-result {
  grid-column: 1 / -1;
  color: var(--text-secondary);
  font-size: 11px;
  padding-left: 36px;
}

.candidate-very-likely {
  border-color: color-mix(in srgb, var(--success) 35%, var(--border));
}

.candidate-to-verify {
  border-color: color-mix(in srgb, var(--warning) 35%, var(--border));
}

.candidate-weak,
.candidate-ignored {
  opacity: 0.65;
}

.candidate-kept {
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
