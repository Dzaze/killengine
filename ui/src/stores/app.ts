import { defineStore } from 'pinia'
import { ref, computed, nextTick, watch } from 'vue'
import {
  backend,
  type AppSettings,
  type CandidatePage,
  type ChatMemoryTargetsResult,
  type ExactScanResult,
  type LogTailResult,
  type MemoryMapResult,
  type NextScanResult,
  type MemoryReadPreview,
  type MemoryWriteBatchResult,
  type MemoryWriteResult,
  type MemoryWriteTarget,
  type ProcessInfo,
  type ProcessModuleInfo,
  type SmartSearchContextResult,
  type SmartSearchDebugEventsResult,
  type TemporaryStorageStatus,
  type UndoCandidateScanResult,
  type UnknownNextScanResult,
  type UnknownSnapshotResult,
} from '@/services/backend'

export interface ChatMessage {
  id: number
  role: 'user' | 'assistant'
  text: string
  time: string
  workflowStatus?: string
  candidateCount?: number
  targetValue?: string
  intent?: string
  intentRationale?: string
  suggestions?: Array<Record<string, unknown>>
  filteredWriteCandidates?: Array<Record<string, unknown>>
  autoWriteResults?: Array<Record<string, unknown>>
  autoWriteOk?: boolean
  previousTargetValue?: string
  writeHistory?: string[]
  activeTargetCount?: number
  recoveryActions?: Array<Record<string, unknown>>
  requiresConfirmation?: boolean
  confirmationReason?: string
  isThinking?: boolean
  isError?: boolean
}

export interface UserActionLogEntry {
  id: number
  time: string
  kind: string
  title: string
  detail: string
  status: 'info' | 'success' | 'warning' | 'error'
}

export interface MemoryPreviewDecodedValue {
  label: string
  value: string
}

export interface WatchedAddress {
  address: string
  type: string
  value: string
  previousValue: string
  changed: boolean
  error: string
  updatedAt: string
}

export interface UnknownGuideStep {
  id: number
  time: string
  mode: string
  label: string
  beforeCount: number
  afterCount: number
  status: 'capture' | 'compare' | 'refine' | 'error'
  detail: string
}

export const useAppStore = defineStore('app', () => {
  // State
  const activeView = ref<'assistant' | 'process' | 'memory' | 'profiles' | 'expert' | 'settings'>('assistant')
  const uiMode = ref<'beginner' | 'expert'>('beginner')
  const version = ref('...')
  const isConnected = ref(false)
  const isAttached = ref(false)
  const processName = ref('')
  const processes = ref<ProcessInfo[]>([])
  const processModules = ref<ProcessModuleInfo[]>([])
  const memoryMap = ref<MemoryMapResult | null>(null)
  const memoryPreview = ref<MemoryReadPreview | null>(null)
  const memoryPreviewAddress = ref('')
  const memoryPreviewLoading = ref(false)
  const memoryPreviewAscii = computed(() => bytesToAscii(hexToBytes(memoryPreview.value?.hex ?? '')))
  const memoryPreviewDecoded = computed(() => decodePreviewValues(hexToBytes(memoryPreview.value?.hex ?? '')))
  const selectedMemoryRegion = ref<Record<string, unknown> | null>(null)
  const pingResult = ref('')
  const logFilePath = ref('')
  const logLines = ref<string[]>([])
  const logError = ref('')
  const diagnosticExportPath = ref('')
  const diagnosticExportError = ref('')
  const temporaryStorageStatus = ref<TemporaryStorageStatus | null>(null)
  const temporaryStorageCleanupResult = ref<Record<string, unknown> | null>(null)
  const temporaryStorageError = ref('')
  const smartSearchDebugFilePath = ref('')
  const scanTelemetryFilePath = ref('')
  const smartSearchDebugEvents = ref<Array<Record<string, unknown>>>([])
  const smartSearchDebugError = ref('')
  const settingsLoaded = ref(false)
  const settingsSaving = ref(false)
  const settingsStatus = ref('')
  const appLanguage = ref<'fr' | 'en'>('fr')
  const settingDefaultValueType = ref('Int32')
  const settingScanMaxResults = ref(1000000)
  const settingScanChunkSizeMb = ref(0)
  const settingPerformanceMode = ref<AppSettings['performanceMode']>('Auto')
  const settingScanMaxWorkerThreads = ref(0)
  const settingScanMaxInFlightMb = ref(0)
  const settingCandidateFileBackedThreshold = ref(250000)
  const settingUnknownSnapshotMaxMb = ref(128)
  const settingFastScan = ref(true)
  const settingSmartSearchDebugEnabled = ref(true)
  const settingSmartSearchDebugMaxEvents = ref(30)
  const settingModelPath = ref('')
  const settingModelThreads = ref(4)
  const activeChatMemoryTargets = ref<Array<Record<string, unknown>>>([])
  const smartSearchContext = ref<SmartSearchContextResult | null>(null)
  const searchQuery = ref('')
  const searchResult = ref('')
  const exactScanValue = ref('')
  const exactScanType = ref('Int32')
  const exactScanResult = ref<ExactScanResult | null>(null)

  // Mode Expert (Phase 12)
  const expertModeEnabled = ref(false)
  const expertStartAddress = ref('')
  const expertStopAddress = ref('')
  const expertAlignment = ref(0)
  const expertWritableOnly = ref(false)
  const expertExecutableOnly = ref(false)
  const expertCopyOnWriteOnly = ref(false)
  const expertRegionSize = ref(0)
  const expertRegionProtection = ref('')
  const expertRegionState = ref('')
  const expertRegionType = ref('')
  const candidatePage = ref<CandidatePage | null>(null)
  const candidatePageIndex = ref(0)
  const candidatePageSize = ref(100)
  const candidateFilter = ref('')
  const nextScanMode = ref('exact')
  const nextScanValue = ref('')
  const nextScanResult = ref<NextScanResult | null>(null)
  const undoCandidateScanResult = ref<UndoCandidateScanResult | null>(null)
  const unknownScanMode = ref('changed')
  const unknownScanType = ref('Int32')
  const unknownWritableOnly = ref(true)
  const unknownCopyOnWriteOnly = ref(false)
  const unknownSnapshotResult = ref<UnknownSnapshotResult | null>(null)
  const unknownNextScanResult = ref<UnknownNextScanResult | null>(null)
  const unknownGuideSteps = ref<UnknownGuideStep[]>([])
  const unknownGuideStepIdCounter = ref(0)
  const selectedCandidateAddress = ref('')
  const writeValue = ref('')
  const writeResult = ref<MemoryWriteResult | null>(null)
  const writeSafetyWarning = ref('')
  const writeSafetyAcknowledged = ref(false)
  const freezeEnabled = ref(false)
  const finalCandidateTargets = ref<Array<Record<string, unknown>>>([])
  const ignoredCandidateAddresses = ref<string[]>([])
  const keptCandidateAddresses = ref<string[]>([])
  const watchLiveEnabled = ref(false)
  const watchedAddresses = ref<WatchedAddress[]>([])
  let watchLiveTimer: ReturnType<typeof setInterval> | null = null

  // Chat / guided workflow state
  const messages = ref<ChatMessage[]>([])
  const messageIdCounter = ref(0)
  const actionLog = ref<UserActionLogEntry[]>([])
  const actionLogIdCounter = ref(0)
  const workflowStatus = ref<string>('idle')
  const targetValueGuided = ref<string>('')
  const candidateHistory = ref<number[]>([])
  const isSearching = ref(false)
  const scanBusy = ref(false)
  const scanStatusText = ref('')
  const scanProgressPercent = ref(0)
  let backendScanSignalsConnected = false

  // Getters
  const statusText = computed(() => {
    if (!isConnected.value) return 'Déconnecté'
    if (!isAttached.value) return 'Prêt'
    return `Attaché: ${processName.value}`
  })

  function nowTime(): string {
    return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
  }

  function formatCount(value: number | undefined): string {
    return new Intl.NumberFormat('fr-FR').format(value ?? 0)
  }

  function unknownModeLabel(mode: string): string {
    if (mode === 'increased') return 'ça augmente'
    if (mode === 'decreased') return 'ça diminue'
    if (mode === 'unchanged') return 'stable'
    if (mode === 'changed') return 'ça change'
    return mode
  }

  function pushUnknownGuideStep(step: Omit<UnknownGuideStep, 'id' | 'time'>) {
    unknownGuideStepIdCounter.value += 1
    unknownGuideSteps.value.unshift({
      id: unknownGuideStepIdCounter.value,
      time: nowTime(),
      ...step,
    })
    unknownGuideSteps.value = unknownGuideSteps.value.slice(0, 12)
  }

  function addActionLog(
    kind: string,
    title: string,
    detail = '',
    status: UserActionLogEntry['status'] = 'info',
  ) {
    actionLogIdCounter.value += 1
    actionLog.value.unshift({
      id: actionLogIdCounter.value,
      time: nowTime(),
      kind,
      title,
      detail,
      status,
    })
    actionLog.value = actionLog.value.slice(0, 80)
  }

  function hexToBytes(hex: string): number[] {
    return hex
      .trim()
      .split(/\s+/)
      .map((chunk) => Number.parseInt(chunk, 16))
      .filter((byte) => Number.isFinite(byte) && byte >= 0 && byte <= 255)
  }

  function bytesToAscii(bytes: number[]): string {
    if (bytes.length === 0) return ''
    return bytes
      .map((byte) => (byte >= 32 && byte <= 126 ? String.fromCharCode(byte) : '.'))
      .join('')
  }

  function readInt(bytes: number[], offset: number, size: number, signed: boolean): string {
    if (bytes.length < offset + size) return '-'
    let value = 0n
    for (let i = 0; i < size; i += 1) {
      value |= BigInt(bytes[offset + i]) << BigInt(8 * i)
    }
    if (signed) {
      const signBit = 1n << BigInt(size * 8 - 1)
      if ((value & signBit) !== 0n) value -= 1n << BigInt(size * 8)
    }
    return value.toString()
  }

  function decodeFloat(bytes: number[], size: 4 | 8): string {
    if (bytes.length < size) return '-'
    const buffer = new ArrayBuffer(size)
    const view = new DataView(buffer)
    bytes.slice(0, size).forEach((byte, index) => view.setUint8(index, byte))
    const value = size === 4 ? view.getFloat32(0, true) : view.getFloat64(0, true)
    return Number.isFinite(value) ? String(Number(value.toPrecision(8))) : String(value)
  }

  function decodePreviewValues(bytes: number[]): MemoryPreviewDecodedValue[] {
    return [
      { label: 'Int8', value: readInt(bytes, 0, 1, true) },
      { label: 'UInt8', value: readInt(bytes, 0, 1, false) },
      { label: 'Int16', value: readInt(bytes, 0, 2, true) },
      { label: 'UInt16', value: readInt(bytes, 0, 2, false) },
      { label: 'Int32', value: readInt(bytes, 0, 4, true) },
      { label: 'UInt32', value: readInt(bytes, 0, 4, false) },
      { label: 'Int64', value: readInt(bytes, 0, 8, true) },
      { label: 'UInt64', value: readInt(bytes, 0, 8, false) },
      { label: 'Float32', value: decodeFloat(bytes, 4) },
      { label: 'Float64', value: decodeFloat(bytes, 8) },
    ]
  }

  function valueTypeReadSize(type: string): number {
    if (/8/.test(type)) return 1
    if (/16/.test(type)) return 2
    if (/64/.test(type)) return 8
    return 4
  }

  function decodeTypedPreviewValue(preview: MemoryReadPreview, type: string): string {
    const decoded = decodePreviewValues(hexToBytes(preview.hex))
    return decoded.find((item) => item.label === type)?.value
      ?? decoded.find((item) => item.label === 'Int32')?.value
      ?? ''
  }

  function setScanProgress(percent: number) {
    if (!Number.isFinite(percent)) return
    scanProgressPercent.value = Math.max(0, Math.min(100, Math.round(percent)))
  }

  function pushMessage(
    role: 'user' | 'assistant',
    text: string,
    extras: Partial<ChatMessage> = {},
  ): ChatMessage {
    messageIdCounter.value += 1
    const msg: ChatMessage = {
      id: messageIdCounter.value,
      role,
      text,
      time: nowTime(),
      ...extras,
    }
    messages.value.push(msg)
    return msg
  }

  function updateMessage(id: number, text: string, extras: Partial<ChatMessage> = {}) {
    const index = messages.value.findIndex((message) => message.id === id)
    if (index < 0) return
    messages.value[index] = {
      ...messages.value[index],
      ...extras,
      text,
      time: nowTime(),
    }
  }

  async function letChatRenderBeforeBackendWork() {
    await nextTick()
    await new Promise<void>((resolve) => window.setTimeout(resolve, 50))
  }

  async function resetWorkflow() {
    try {
      const result = await backend.getController().clearScanContext()
      addActionLog('scan', 'Nouveau scan', String(result.message ?? 'Contexte de scan vidé.'), 'success')
    } catch (e) {
      addActionLog('scan', 'Nouveau scan local', `Backend non purgé : ${String(e)}`, 'warning')
    }
    workflowStatus.value = 'idle'
    targetValueGuided.value = ''
    candidateHistory.value = []
    unknownGuideSteps.value = []
    candidatePage.value = null
    candidatePageIndex.value = 0
    candidateFilter.value = ''
    exactScanResult.value = null
    nextScanResult.value = null
    unknownSnapshotResult.value = null
    unknownNextScanResult.value = null
    nextScanValue.value = ''
    scanStatusText.value = 'Nouveau scan prêt.'
    setScanProgress(0)
    await refreshSmartSearchContext()
  }

  async function tellNewValue(value: string) {
    if (!value.trim()) return
    searchQuery.value = value.trim()
    await doSearch()
  }

  async function doGuidedChange() {
    // Bouton "J'ai changé" - invite l'utilisateur à donner la nouvelle valeur.
    pushMessage('assistant', 'Parfait ! Donne-moi maintenant la nouvelle valeur affichée dans le jeu.')
    workflowStatus.value = 'awaiting_new_value'
  }

  // Actions
  async function init() {
    try {
      await backend.connect()
      isConnected.value = backend.isConnected
      const controller = backend.getController()
      if (!backendScanSignalsConnected) {
        controller.scanStarted?.connect(() => {
          setScanProgress(0)
        })
        controller.scanProgress?.connect((percent) => {
          setScanProgress(Number(percent))
        })
        backendScanSignalsConnected = true
      }
      version.value = await controller.getVersion()
      await loadSettings()
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
      console.log('[KillEngine] Version:', version.value)
    } catch (e) {
      console.error('[KillEngine] Backend connection failed:', e)
    }
  }

  async function refreshProcesses() {
    try {
      processes.value = await backend.getController().getProcesses()
    } catch (e) {
      console.error('[KillEngine] Failed to get processes:', e)
    }
  }

  async function attach(pid: number) {
    try {
      const ok = await backend.getController().attachProcess(pid)
      if (ok) {
        isAttached.value = true
        const proc = processes.value.find((p) => p.pid === pid)
        processName.value = proc?.name ?? `PID ${pid}`
      }
    } catch (e) {
      console.error('[KillEngine] Attach failed:', e)
    }
  }

  async function refreshProcessModules(pid: number) {
    try {
      processModules.value = await backend.getController().getProcessModules(pid)
    } catch (e) {
      processModules.value = []
      console.error('[KillEngine] Failed to get process modules:', e)
    }
  }

  async function refreshMemoryMap() {
    try {
      memoryMap.value = await backend.getController().getMemoryMap()
    } catch (e) {
      memoryMap.value = null
      console.error('[KillEngine] Failed to get memory map:', e)
    }
  }

  async function readMemoryPreview(addressHex: string, size = 64) {
    const normalizedAddress = addressHex.trim().replace(/^0x/i, '')
    memoryPreviewAddress.value = normalizedAddress
    memoryPreviewLoading.value = true
    memoryPreview.value = {
      success: false,
      partial: false,
      cancelled: false,
      bytesRead: 0,
      requestedBytes: size,
      error: '',
      hex: '',
    }
    try {
      memoryPreview.value = await backend.getController().readMemoryPreview(addressHex, size)
      addActionLog(
        'memory_preview',
        `Aperçu mémoire 0x${normalizedAddress}`,
        memoryPreview.value.success
          ? `${memoryPreview.value.bytesRead}/${memoryPreview.value.requestedBytes} octets lus.`
          : memoryPreview.value.error || 'Lecture sans donnée.',
        memoryPreview.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      memoryPreview.value = {
        success: false,
        partial: false,
        cancelled: false,
        bytesRead: 0,
        requestedBytes: size,
        error: String(e),
        hex: '',
      }
      addActionLog('memory_preview', `Aperçu mémoire 0x${normalizedAddress}`, String(e), 'error')
    } finally {
      memoryPreviewLoading.value = false
    }
  }

  async function detach() {
    try {
      await backend.getController().detachProcess()
      isAttached.value = false
      processName.value = ''
      processModules.value = []
      memoryMap.value = null
      memoryPreview.value = null
      memoryPreviewAddress.value = ''
      memoryPreviewLoading.value = false
    } catch (e) {
      console.error('[KillEngine] Detach failed:', e)
    }
  }

  async function doPing() {
    try {
      pingResult.value = await backend.getController().ping('hello from Vue')
    } catch (e) {
      pingResult.value = 'Ping failed: ' + String(e)
    }
  }

  async function refreshDiagnostics() {
    try {
      const controller = backend.getController()
      logFilePath.value = await controller.getLogFilePath()
      smartSearchDebugFilePath.value = await controller.getSmartSearchDebugFilePath()
      scanTelemetryFilePath.value = controller.getScanTelemetryFilePath
        ? await controller.getScanTelemetryFilePath()
        : ''
      await refreshLogTail()
      await refreshTemporaryStorageStatus()
      const debugResult: SmartSearchDebugEventsResult = await backend
        .getController()
        .getSmartSearchDebugEvents(settingSmartSearchDebugMaxEvents.value)
      smartSearchDebugEvents.value = debugResult.events ?? []
      smartSearchDebugError.value = debugResult.error ?? ''
    } catch (e) {
      logFilePath.value = ''
      logLines.value = []
      logError.value = String(e)
      temporaryStorageStatus.value = null
      temporaryStorageError.value = String(e)
      smartSearchDebugFilePath.value = ''
      scanTelemetryFilePath.value = ''
      smartSearchDebugEvents.value = []
      smartSearchDebugError.value = String(e)
      console.error('[KillEngine] Failed to refresh diagnostics:', e)
    }
  }

  async function refreshLogTail() {
    try {
      const result: LogTailResult = await backend.getController().getLogTail(80)
      logFilePath.value = result.path || logFilePath.value
      logLines.value = result.lines ?? []
      logError.value = result.error ?? ''
      return result
    } catch (e) {
      logLines.value = []
      logError.value = String(e)
      return { success: false, path: logFilePath.value, lines: [], error: String(e) }
    }
  }

  async function exportDiagnostics() {
    diagnosticExportPath.value = ''
    diagnosticExportError.value = ''
    try {
      const result = await backend.getController().exportDiagnostics()
      if (result.success === true) {
        diagnosticExportPath.value = String(result.path ?? '')
      } else {
        diagnosticExportError.value = String(result.error ?? 'Export diagnostic impossible.')
      }
      return result
    } catch (e) {
      diagnosticExportError.value = String(e)
      return { success: false, error: String(e) }
    }
  }

  async function refreshTemporaryStorageStatus() {
    try {
      temporaryStorageStatus.value = await backend.getController().getTemporaryStorageStatus()
      temporaryStorageError.value = temporaryStorageStatus.value.error ?? ''
      return temporaryStorageStatus.value
    } catch (e) {
      temporaryStorageStatus.value = null
      temporaryStorageError.value = String(e)
      return null
    }
  }

  async function clearTemporaryStorage() {
    if (scanBusy.value) return { success: false, error: 'Un scan est actif.' }
    try {
      temporaryStorageCleanupResult.value = await backend.getController().clearTemporaryStorage()
      await refreshTemporaryStorageStatus()
      candidatePage.value = null
      candidatePageIndex.value = 0
      exactScanResult.value = null
      nextScanResult.value = null
      unknownSnapshotResult.value = null
      unknownNextScanResult.value = null
      unknownGuideSteps.value = []
      scanStatusText.value = String(temporaryStorageCleanupResult.value.message ?? 'Stockage temporaire nettoyé.')
      addActionLog(
        'cleanup',
        'Temporaire nettoyé',
        `${String(temporaryStorageCleanupResult.value.removedFileCount ?? 0)} fichier(s), ${String(temporaryStorageCleanupResult.value.removedBytes ?? 0)} octet(s).`,
        temporaryStorageCleanupResult.value.success === true ? 'success' : 'warning',
      )
      return temporaryStorageCleanupResult.value
    } catch (e) {
      temporaryStorageCleanupResult.value = { success: false, error: String(e) }
      temporaryStorageError.value = String(e)
      addActionLog('cleanup', 'Nettoyage temporaire échoué', String(e), 'error')
      return temporaryStorageCleanupResult.value
    }
  }

  function applySettings(settings: AppSettings) {
    appLanguage.value = settings.language === 'en' ? 'en' : 'fr'
    settingDefaultValueType.value = settings.defaultValueType || 'Int32'
    exactScanType.value = settingDefaultValueType.value
    unknownScanType.value = settingDefaultValueType.value
    settingScanMaxResults.value = Number(settings.scanMaxResults || 1000000)
    settingScanChunkSizeMb.value = Number(settings.scanChunkSizeMb ?? 0)
    settingPerformanceMode.value = settings.performanceMode || 'Auto'
    settingScanMaxWorkerThreads.value = Number(settings.scanMaxWorkerThreads ?? 0)
    settingScanMaxInFlightMb.value = Number(settings.scanMaxInFlightMb ?? 0)
    settingCandidateFileBackedThreshold.value = Number(settings.candidateFileBackedThreshold || 250000)
    settingUnknownSnapshotMaxMb.value = Number(settings.unknownSnapshotMaxMb || 128)
    settingFastScan.value = settings.fastScan !== false
    settingSmartSearchDebugEnabled.value = settings.smartSearchDebugEnabled !== false
    settingSmartSearchDebugMaxEvents.value = Number(settings.smartSearchDebugMaxEvents || 30)
    settingModelPath.value = settings.modelPath || ''
    settingModelThreads.value = Number(settings.modelThreads || 4)
  }

  function currentSettings(): AppSettings {
    return {
      language: appLanguage.value,
      defaultValueType: settingDefaultValueType.value,
      scanMaxResults: settingScanMaxResults.value,
      scanChunkSizeMb: settingScanChunkSizeMb.value,
      performanceMode: settingPerformanceMode.value,
      scanMaxWorkerThreads: settingScanMaxWorkerThreads.value,
      scanMaxInFlightMb: settingScanMaxInFlightMb.value,
      candidateFileBackedThreshold: settingCandidateFileBackedThreshold.value,
      unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      fastScan: settingFastScan.value,
      smartSearchDebugEnabled: settingSmartSearchDebugEnabled.value,
      smartSearchDebugMaxEvents: settingSmartSearchDebugMaxEvents.value,
      modelPath: settingModelPath.value,
      modelThreads: settingModelThreads.value,
    }
  }

  async function loadSettings() {
    try {
      const settings = await backend.getController().getSettings()
      applySettings(settings)
      settingsLoaded.value = true
      settingsStatus.value = ''
      return settings
    } catch (e) {
      settingsStatus.value = 'Impossible de charger les paramètres : ' + String(e)
      return null
    }
  }

  async function saveSettings() {
    settingsSaving.value = true
    try {
      const saved = await backend.getController().saveSettings(currentSettings())
      applySettings(saved)
      settingsLoaded.value = true
      settingsStatus.value = 'Paramètres sauvegardés.'
      await refreshDiagnostics()
      return saved
    } catch (e) {
      settingsStatus.value = 'Sauvegarde impossible : ' + String(e)
      return null
    } finally {
      settingsSaving.value = false
    }
  }

  async function refreshActiveChatMemoryTargets() {
    try {
      const result: ChatMemoryTargetsResult = await backend.getController().getActiveChatMemoryTargets()
      activeChatMemoryTargets.value = result.targets ?? []
    } catch (e) {
      activeChatMemoryTargets.value = []
      console.error('[KillEngine] Failed to refresh active chat memory targets:', e)
    }
  }

  async function refreshSmartSearchContext() {
    try {
      smartSearchContext.value = await backend.getController().getSmartSearchContext()
    } catch (e) {
      smartSearchContext.value = null
      console.error('[KillEngine] Failed to refresh Smart Search context:', e)
    }
  }

  async function clearActiveChatMemoryTargets() {
    try {
      const result = await backend.getController().clearActiveChatMemoryTargets()
      activeChatMemoryTargets.value = []
      await refreshSmartSearchContext()
      pushMessage('assistant', `J'ai oublié ${String(result.cleared ?? 0)} adresse(s) mémoire active(s).`)
      return result
    } catch (e) {
      pushMessage('assistant', "Impossible d'oublier les adresses mémoire : " + String(e), { isError: true })
      return { success: false, error: String(e), count: 0, targets: [] }
    }
  }

  async function clearSmartSearchDebug() {
    try {
      const result = await backend.getController().clearSmartSearchDebugEvents()
      if (result.success !== true) {
        smartSearchDebugError.value = String(result.error ?? 'Impossible de vider le debug Smart Search.')
        return result
      }
      smartSearchDebugEvents.value = []
      smartSearchDebugError.value = ''
      await refreshDiagnostics()
      return result
    } catch (e) {
      smartSearchDebugError.value = String(e)
      return { success: false, error: String(e) }
    }
  }

  async function doSearch() {
    const query = searchQuery.value.trim()
    if (!query || isSearching.value) return

    // Message utilisateur
    pushMessage('user', query)
    const thinkingMessage = pushMessage('assistant', 'Je vais rechercher ça en mémoire...', { isThinking: true })
    searchQuery.value = ''
    isSearching.value = true
    await letChatRenderBeforeBackendWork()

    try {
      const result = await backend.getController().startSmartSearch(query)
      searchResult.value = result.message ?? JSON.stringify(result, null, 2)

      // Synchronise les résultats déterministes
      if (result.actionStatus === 'executed' && result.actionResult) {
        if (result.tool === 'exact_scan' || result.tool === 'exact_scan_multi_type') {
          exactScanResult.value = result.actionResult as unknown as ExactScanResult
          candidatePageIndex.value = 0
          await refreshCandidates()
        } else if (result.tool === 'next_scan') {
          nextScanResult.value = result.actionResult as unknown as NextScanResult
          candidatePageIndex.value = 0
          await refreshCandidates()
        } else if (result.tool === 'unknown_capture') {
          unknownSnapshotResult.value = result.actionResult as unknown as UnknownSnapshotResult
        } else if (result.tool === 'unknown_compare') {
          unknownNextScanResult.value = result.actionResult as unknown as UnknownNextScanResult
          candidatePageIndex.value = 0
          await refreshCandidates()
        }
      }

      // Suivi workflow
      const wfStatus = (result.workflowStatus as string | undefined) ?? ''
      if (wfStatus) workflowStatus.value = wfStatus

      if (result.targetValue) {
        targetValueGuided.value = String(result.targetValue)
      }

      const candidateCount = extractCandidateCount(result)
      if (candidateCount !== undefined && candidateCount >= 0) {
        candidateHistory.value.push(candidateCount)
      }

      // Suggestions d'écriture
      if (result.suggestedWrite) {
        const suggestion = result.suggestedWrite
        selectedCandidateAddress.value = String(suggestion.address ?? '')
        writeValue.value = String(suggestion.value ?? '')
        exactScanType.value = String(suggestion.type ?? exactScanType.value)
      }

      if (result.autoWriteResult) {
        writeResult.value = result.autoWriteResult as unknown as MemoryWriteResult
      }

      // Construction du message assistant
      const extras: Partial<ChatMessage> = {
        workflowStatus: wfStatus || undefined,
        candidateCount,
        targetValue: result.targetValue ? String(result.targetValue) : undefined,
        intent: result.intent ? String(result.intent) : undefined,
        intentRationale: result.intentRationale ? String(result.intentRationale) : undefined,
      }

      if (result.requiresConfirmation) {
        extras.requiresConfirmation = true
        extras.confirmationReason = result.confirmationReason as string | undefined
      }

      if (result.suggestedWrites) {
        const suggestions = result.suggestedWrites as Array<Record<string, unknown>>
        finalCandidateTargets.value = suggestions
        extras.suggestions = suggestions
      }

      if (result.filteredWriteCandidates) {
        extras.filteredWriteCandidates = result.filteredWriteCandidates as Array<Record<string, unknown>>
      }

      if (result.recoveryActions) {
        extras.recoveryActions = result.recoveryActions as Array<Record<string, unknown>>
      }

      if (result.autoWriteResults) {
        const results = result.autoWriteResults as Array<Record<string, unknown>>
        extras.autoWriteResults = results
        extras.autoWriteOk = results.length > 0 && results.every((r) => r.success === true)
        extras.previousTargetValue = result.previousTargetValue ? String(result.previousTargetValue) : undefined
        extras.activeTargetCount = typeof result.activeTargetCount === 'number' ? result.activeTargetCount : undefined
        extras.writeHistory = Array.isArray(result.writeHistory)
          ? result.writeHistory.map((value) => String(value))
          : undefined
      }

      if (result.error) extras.isError = true

      const actionResultError = typeof result.actionResult === 'object' && result.actionResult !== null
        ? String((result.actionResult as Record<string, unknown>).error ?? '').trim()
        : ''
      const assistantText = String(
        result.message
        ?? result.error
        ?? result.actionError
        ?? actionResultError
        ?? "Je n'ai pas assez d'informations pour agir. Donne-moi une valeur à chercher ou une adresse à utiliser.",
      ).trim()
      updateMessage(
        thinkingMessage.id,
        assistantText || "Je n'ai pas assez d'informations pour agir. Donne-moi une valeur à chercher ou une adresse à utiliser.",
        { ...extras, isThinking: false },
      )
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
    } catch (e) {
      updateMessage(thinkingMessage.id, 'Erreur de recherche : ' + String(e), { isThinking: false, isError: true })
    } finally {
      isSearching.value = false
    }
  }

  async function startNewSearchContext() {
    if (isSearching.value) return
    searchQuery.value = 'nouvelle recherche'
    await doSearch()
  }

  async function useSuggestedAddresses(suggestions: Array<Record<string, unknown>>) {
    const addresses = suggestions
      .map((suggestion) => String(suggestion.address ?? '').trim())
      .filter(Boolean)
      .map((address) => address.startsWith('0x') ? address : `0x${address}`)
    if (addresses.length === 0 || isSearching.value) return
    searchQuery.value = `j'utilise ces mémoires ${addresses.join(' ')}`
    await doSearch()
  }

  async function searchValueElsewhere(value: string) {
    const trimmed = value.trim()
    if (!trimmed || isSearching.value) return
    searchQuery.value = `nouvelle recherche ${trimmed}`
    await doSearch()
  }

  async function searchValueAsType(value: string, type: string, target?: string) {
    const trimmed = value.trim()
    if (!trimmed || isSearching.value) return
    const targetText = target?.trim() ?? ''
    exactScanValue.value = trimmed
    exactScanType.value = type
    searchQuery.value = targetText
      ? `nouvelle recherche ${trimmed} en ${type} cible ${targetText}`
      : `nouvelle recherche ${trimmed} en ${type}`
    await doSearch()
    addActionLog('assistant', `Recherche Assistant en ${type}`, targetText ? `${trimmed} -> ${targetText}` : trimmed, 'info')
  }

  async function testSingleSuggestedAddress(suggestion: Record<string, unknown>) {
    const address = String(suggestion.address ?? '').trim()
    const type = String(suggestion.type ?? exactScanType.value)
    const value = String(suggestion.value ?? targetValueGuided.value ?? '').trim()
    if (!address || !value || isSearching.value) return

    const normalizedAddress = address.startsWith('0x') ? address.slice(2) : address
    pushMessage('user', `tester uniquement 0x${normalizedAddress} avec ${value}`)
    try {
      const result = await backend.getController().writeMemoryValue(normalizedAddress, type, value)
      writeResult.value = result
      pushMessage('assistant',
        result.success
          ? `J'ai écrit ${value} uniquement sur 0x${normalizedAddress}. Vérifie dans le jeu si c'est la bonne adresse.`
          : `L'écriture sur 0x${normalizedAddress} a échoué : ${result.error}`,
        { isError: !result.success })
    } catch (e) {
      pushMessage('assistant', `L'écriture sur 0x${normalizedAddress} a échoué : ${String(e)}`, { isError: true })
    }
  }

  function extractCandidateCount(result: Record<string, unknown>): number | undefined {
    if (typeof result.candidateStoreSize === 'number') return result.candidateStoreSize
    const ar = result.actionResult as Record<string, unknown> | undefined
    if (ar) {
      if (typeof ar.remaining === 'number') return ar.remaining
      if (typeof ar.candidateStoreSize === 'number') return ar.candidateStoreSize
      if (typeof ar.stored === 'number') return ar.stored
    }
    return undefined
  }

  async function doExactScan() {
    if (!exactScanValue.value.trim() || scanBusy.value) return

    // Le mode Auto (multi-type) est crucial pour StarCraft 2 : il cherche
    // Int/UInt, Float et variantes fixed-point en une fois.
    const isAutoType = exactScanType.value.toLowerCase() === 'auto'

    // Si le Mode Expert est activé et qu'au moins un filtre est défini, on utilise l'API expert.
    const hasExpertFilter =
      expertModeEnabled.value
      && (expertStartAddress.value.trim()
        || expertStopAddress.value.trim()
        || expertAlignment.value > 0
        || expertWritableOnly.value
        || expertExecutableOnly.value
        || expertCopyOnWriteOnly.value)
    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = isAutoType ? 'Scan multi-type en cours...' : 'Scan exact en cours...'
      addActionLog('scan', `Scan ${isAutoType ? 'multi-type' : 'exact'} ${exactScanValue.value}`, `${exactScanType.value}${hasExpertFilter ? ' · filtres expert actifs' : ''}.`, 'info')

      if (isAutoType) {
        const controller = backend.getController()
        if (controller.startExactScanMultiType) {
          exactScanResult.value = await controller.startExactScanMultiType(
            exactScanValue.value,
            exactScanType.value,
          )
        } else {
          // Fallback : si le backend n'expose pas le scan multi-type, on utilise le scan simple.
          exactScanResult.value = await backend.startExactScanAsync(
            exactScanValue.value,
            'Int32',
            {},
          )
        }
      } else {
        exactScanResult.value = await backend.startExactScanAsync(
          exactScanValue.value,
          exactScanType.value,
          hasExpertFilter
            ? {
                startAddress: expertStartAddress.value.trim() || undefined,
                stopAddress: expertStopAddress.value.trim() || undefined,
                alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
                writableOnly: expertWritableOnly.value,
                executableOnly: expertExecutableOnly.value,
                copyOnWriteOnly: expertCopyOnWriteOnly.value,
              }
            : {},
        )
      }
      setScanProgress(Math.max(scanProgressPercent.value, 95))
      candidatePageIndex.value = 0
      scanStatusText.value = 'Chargement des candidats...'
      await refreshCandidates()
      setScanProgress(100)
      scanStatusText.value = exactScanResult.value.cancelled ? 'Scan annulé.' : 'Scan terminé.'
      addActionLog(
        'scan',
        exactScanResult.value.cancelled ? 'Scan exact annulé' : 'Scan exact terminé',
        `${exactScanResult.value.candidateStoreSize} candidat(s), ${exactScanResult.value.regionsScanned} région(s).`,
        exactScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      exactScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: String(e),
        matches: [],
        candidateStoreSize: 0,
      }
      scanStatusText.value = 'Scan échoué.'
      addActionLog('scan', 'Scan exact échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function refreshCandidates() {
    try {
      candidatePage.value = await backend
        .getController()
        .getCandidates(candidatePageIndex.value, candidatePageSize.value, candidateFilter.value)
      if (
        candidatePage.value
        && !candidatePage.value.displaySuppressed
        && candidatePage.value.totalCount > 0
        && candidatePage.value.totalCount <= 20
      ) {
        for (const candidate of candidatePage.value.candidates) {
          addAddressToWatch(candidate.address, candidate.type)
        }
      }
    } catch (e) {
      console.error('[KillEngine] Failed to get candidates:', e)
      candidatePage.value = null
    }
  }

  async function doNextScan() {
    if (scanBusy.value) return
    if ((candidatePage.value?.totalCount ?? 0) <= 0) {
      scanStatusText.value = 'Aucun candidat à réduire. Lance d’abord un premier scan.'
      nextScanResult.value = {
        success: false,
        checked: 0,
        unreadable: 0,
        remaining: 0,
        error: scanStatusText.value,
      }
      addActionLog('scan', 'Next scan refusé', scanStatusText.value, 'warning')
      return
    }
    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Réduction des candidats...'
      addActionLog('scan', `Next scan ${nextScanMode.value}`, nextScanValue.value ? `Valeur ${nextScanValue.value}.` : 'Sans valeur explicite.', 'info')
      nextScanResult.value = await backend.startNextScanAsync(nextScanMode.value, nextScanValue.value)
      setScanProgress(Math.max(scanProgressPercent.value, 95))
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      setScanProgress(100)
      scanStatusText.value = nextScanResult.value.cancelled ? 'Scan annulé.' : 'Next scan terminé.'
      addActionLog(
        'scan',
        nextScanResult.value.cancelled ? 'Next scan annulé' : 'Next scan terminé',
        `${nextScanResult.value.remaining} candidat(s) restant(s).`,
        nextScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      nextScanResult.value = {
        success: false,
        cancelled: false,
        checked: 0,
        unreadable: 0,
        remaining: 0,
        error: String(e),
      }
      scanStatusText.value = 'Next scan échoué.'
      addActionLog('scan', 'Next scan échoué', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function undoCandidateScan() {
    if (scanBusy.value) return null
    try {
      undoCandidateScanResult.value = await backend.getController().undoCandidateScan()
      if (undoCandidateScanResult.value.success) {
        candidatePageIndex.value = 0
        await refreshCandidates()
        scanStatusText.value = `Réduction restaurée : ${undoCandidateScanResult.value.count} candidat(s).`
        addActionLog('rollback', 'Réduction restaurée', `${undoCandidateScanResult.value.count} candidat(s).`, 'success')
      } else {
        scanStatusText.value = undoCandidateScanResult.value.error || 'Aucune réduction à restaurer.'
        addActionLog('rollback', 'Réduction non restaurée', scanStatusText.value, 'warning')
      }
      return undoCandidateScanResult.value
    } catch (e) {
      undoCandidateScanResult.value = {
        success: false,
        restored: false,
        count: candidatePage.value?.totalCount ?? 0,
        error: String(e),
      }
      scanStatusText.value = 'Restauration impossible.'
      addActionLog('rollback', 'Restauration impossible', String(e), 'error')
      return undoCandidateScanResult.value
    }
  }

  async function cancelActiveScan() {
    if (!scanBusy.value) return
    scanStatusText.value = 'Annulation demandée...'
    try {
      const result = await backend.cancelActiveScan()
      if (result.success !== true && result.error) {
        scanStatusText.value = String(result.error)
      }
    } catch (e) {
      scanStatusText.value = 'Annulation impossible : ' + String(e)
    }
  }

  async function captureUnknownSnapshot() {
    if (scanBusy.value) return
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      scanStatusText.value = 'Capture unknown en cours...'
      addActionLog('scan', 'Capture unknown', `${unknownScanType.value}.`, 'info')
      unknownSnapshotResult.value = await backend.captureUnknownSnapshotAsync({
        writableOnly: unknownWritableOnly.value,
        copyOnWriteOnly: unknownWritableOnly.value && unknownCopyOnWriteOnly.value,
        unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      })
      scanProgressPercent.value = 100
      scanStatusText.value = unknownSnapshotResult.value.cancelled ? 'Scan annulé.' : 'Snapshot capturé.'
      candidatePage.value = null
      candidatePageIndex.value = 0
      nextScanResult.value = null
      unknownNextScanResult.value = null
      unknownScanMode.value = 'changed'
      unknownGuideSteps.value = []
      pushUnknownGuideStep({
        mode: 'capture',
        label: 'capture',
        beforeCount: 0,
        afterCount: 0,
        status: unknownSnapshotResult.value.success ? 'capture' : 'error',
        detail: unknownSnapshotResult.value.success
          ? `${unknownSnapshotResult.value.regionsCaptured} région(s), ${unknownSnapshotResult.value.bytesCaptured} octet(s) / limite ${unknownSnapshotResult.value.captureLimitBytes ?? 0}.${unknownSnapshotResult.value.captureLimitReached ? ' Limite atteinte.' : ''}${unknownSnapshotResult.value.writableOnly ? ' Writable only.' : ''}`
          : unknownSnapshotResult.value.error || 'Capture refusée.',
      })
      addActionLog('scan', 'Snapshot unknown capturé', `${unknownSnapshotResult.value.regionsCaptured} région(s).`, 'success')
    } catch (e) {
      unknownSnapshotResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        regionsCaptured: 0,
        regionsSkipped: 0,
        bytesCaptured: 0,
        error: String(e),
      }
      scanStatusText.value = 'Capture unknown échouée.'
      pushUnknownGuideStep({
        mode: 'capture',
        label: 'capture',
        beforeCount: 0,
        afterCount: 0,
        status: 'error',
        detail: String(e),
      })
      addActionLog('scan', 'Capture unknown échouée', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  async function doUnknownNextScan() {
    if (scanBusy.value) return
    if (!unknownSnapshotResult.value?.success) {
      scanStatusText.value = 'Capture d’abord une image unknown avant de comparer.'
      unknownNextScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: scanStatusText.value,
      }
      addActionLog('scan', 'Comparaison unknown refusée', scanStatusText.value, 'warning')
      return
    }
    if (unknownScanMode.value === 'unchanged') {
      scanStatusText.value = 'Le mode stable est réservé au raffinage après une première réduction. Utilise d’abord ça change, ça augmente ou ça diminue.'
      unknownNextScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: scanStatusText.value,
      }
      addActionLog('scan', 'Unknown stable refusé', scanStatusText.value, 'warning')
      return
    }
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      const candidateCount = candidatePage.value?.totalCount ?? 0
      const isRefine = candidateCount > 0
      scanStatusText.value = isRefine
        ? `Raffinage unknown ${unknownScanMode.value}...`
        : 'Comparaison unknown en cours...'
      addActionLog(
        'scan',
        isRefine ? `Raffinage unknown ${unknownScanMode.value}` : `Comparaison unknown ${unknownScanMode.value}`,
        isRefine ? `${candidateCount} candidat(s).` : unknownScanType.value,
        'info',
      )
      unknownNextScanResult.value = await backend.unknownNextScanAsync(unknownScanMode.value, unknownScanType.value)
      scanProgressPercent.value = 85
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      scanProgressPercent.value = 100
      scanStatusText.value = unknownNextScanResult.value.cancelled ? 'Scan annulé.' : 'Comparaison unknown terminée.'
      addActionLog('scan', 'Comparaison unknown terminée', `${unknownNextScanResult.value.stored} candidat(s).`, 'success')
    } catch (e) {
      unknownNextScanResult.value = {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: String(e),
      }
      scanStatusText.value = 'Comparaison unknown échouée.'
      addActionLog('scan', 'Comparaison unknown échouée', String(e), 'error')
    } finally {
      scanBusy.value = false
    }
  }

  function selectCandidate(address: string, type: string) {
    selectedCandidateAddress.value = address
    exactScanType.value = type
    addAddressToWatch(address, type)
    addActionLog('select', `Adresse sélectionnée 0x${address}`, `Type ${type}.`, 'info')
  }

  async function runUnknownGuideStep(mode: 'increased' | 'decreased' | 'unchanged' | 'changed') {
    if (scanBusy.value) return
    if (!unknownSnapshotResult.value?.success && (candidatePage.value?.totalCount ?? 0) <= 0) {
      scanStatusText.value = 'Capture d’abord une valeur unknown.'
      addActionLog('scan', 'Unknown guidé refusé', scanStatusText.value, 'warning')
      pushUnknownGuideStep({
        mode,
        label: unknownModeLabel(mode),
        beforeCount: 0,
        afterCount: 0,
        status: 'error',
        detail: scanStatusText.value,
      })
      return
    }

    const beforeCount = candidatePage.value?.totalCount ?? 0
    unknownScanMode.value = mode
    await doUnknownNextScan()

    const afterCount = candidatePage.value?.totalCount
      ?? unknownNextScanResult.value?.stored
      ?? nextScanResult.value?.remaining
      ?? 0
    const usedRefine = beforeCount > 0
    const error = unknownNextScanResult.value?.error
    const cancelled = unknownNextScanResult.value?.cancelled
    const status: UnknownGuideStep['status'] = error || cancelled ? 'error' : usedRefine ? 'refine' : 'compare'
    const detail = error
      ? String(error)
      : cancelled
        ? 'Opération annulée.'
        : usedRefine
          ? `${beforeCount} -> ${afterCount} candidat(s).`
          : `${formatCount(unknownNextScanResult.value?.matchesFound)} trouvé(s), ${afterCount} stocké(s).`

    pushUnknownGuideStep({
      mode,
      label: unknownModeLabel(mode),
      beforeCount,
      afterCount,
      status,
      detail,
    })
  }

  function openExpertAtAddress(address: string, type = exactScanType.value) {
    const normalizedAddress = address.trim().replace(/^0x/i, '')
    if (!normalizedAddress) return
    selectCandidate(normalizedAddress, type)
    activeView.value = 'expert'
    addActionLog('navigation', `Mode Expert ouvert sur 0x${normalizedAddress}`, `Type ${type}.`, 'info')
  }

  function openExpertForRegion(region: Record<string, unknown>, type = exactScanType.value) {
    const address = String(region.baseAddress ?? '').trim().replace(/^0x/i, '')
    const size = Number(region.size ?? 0)
    if (!address) return

    selectedMemoryRegion.value = region
    selectedCandidateAddress.value = address
    exactScanType.value = type
    expertModeEnabled.value = true
    expertStartAddress.value = `0x${address}`
    expertStopAddress.value = size > 0 ? `0x${(Number.parseInt(address, 16) + size).toString(16)}` : ''
    expertWritableOnly.value = region.writable === true
    expertExecutableOnly.value = region.executable === true
    expertCopyOnWriteOnly.value = String(region.type ?? '').toLowerCase().includes('copy')
    expertRegionSize.value = size
    expertRegionProtection.value = String(region.protection ?? '')
    expertRegionState.value = String(region.state ?? '')
    expertRegionType.value = String(region.type ?? '')
    addAddressToWatch(address, type)
    activeView.value = 'expert'
    addActionLog(
      'navigation',
      `Région ouverte en Expert 0x${address}`,
      `${expertRegionProtection.value || '?'} · ${expertRegionState.value || '?'} · ${expertRegionType.value || '?'}.`,
      'info',
    )
  }

  async function scanAroundPreview(value?: string, type = exactScanType.value) {
    const scanValue = (value ?? exactScanValue.value).trim()
    if (!scanValue || !memoryPreviewAddress.value) return
    expertModeEnabled.value = true
    expertStartAddress.value = `0x${memoryPreviewAddress.value}`
    if (selectedMemoryRegion.value && Number(selectedMemoryRegion.value.size ?? 0) > 0) {
      const size = Number(selectedMemoryRegion.value.size)
      expertStopAddress.value = `0x${(Number.parseInt(memoryPreviewAddress.value, 16) + size).toString(16)}`
    }
    exactScanValue.value = scanValue
    exactScanType.value = type
    activeView.value = 'expert'
    addActionLog('scan', `Scan autour de 0x${memoryPreviewAddress.value}`, `${type} = ${scanValue}.`, 'info')
    await doExactScan()
  }

  function regionForAddress(address: string): Record<string, unknown> | undefined {
    const normalized = address.trim().replace(/^0x/i, '')
    const numericAddress = Number.parseInt(normalized, 16)
    if (!Number.isFinite(numericAddress)) return undefined
    return memoryMap.value?.regions.find((region) => {
      const base = Number.parseInt(String(region.baseAddress ?? '').replace(/^0x/i, ''), 16)
      const size = Number(region.size ?? 0)
      return Number.isFinite(base) && size > 0 && numericAddress >= base && numericAddress < base + size
    }) as Record<string, unknown> | undefined
  }

  function updateWriteSafetyWarning() {
    writeSafetyWarning.value = ''
    const address = selectedCandidateAddress.value.trim()
    if (!address) return
    const region = regionForAddress(address)
    if (!region) {
      writeSafetyWarning.value = 'Région inconnue : actualise la carte mémoire avant écriture.'
      return
    }
    if (region.writable !== true) {
      writeSafetyWarning.value = `Attention : la région 0x${region.baseAddress} n'est pas marquée writable (${region.protection ?? '?'}).`
      return
    }
    if (String(region.state ?? '').toLowerCase() !== 'committed') {
      writeSafetyWarning.value = `Attention : état mémoire ${String(region.state ?? '?')}, écriture risquée.`
    }
  }

  function inferredTypesForValue(value: string): Array<Record<string, unknown>> {
    const normalized = value.trim().replace(',', '.')
    const numberValue = Number(normalized)
    if (!Number.isFinite(numberValue)) return []
    const integer = Number.isInteger(numberValue)
    const results: Array<Record<string, unknown>> = []
    if (integer && numberValue >= 0 && numberValue <= 255) {
      results.push({ type: 'UInt8', confidence: 'faible', reason: 'petite valeur compacte possible' })
    }
    if (integer && numberValue >= -128 && numberValue <= 127) {
      results.push({ type: 'Int8', confidence: 'faible', reason: 'petite valeur signée compacte possible' })
    }
    if (integer && numberValue >= 0 && numberValue <= 65535) {
      results.push({ type: 'UInt16', confidence: 'moyenne', reason: 'ressource compacte possible' })
    }
    if (integer && numberValue >= -32768 && numberValue <= 32767) {
      results.push({ type: 'Int16', confidence: 'moyenne', reason: 'entier court possible' })
    }
    if (integer && numberValue >= -2147483648 && numberValue <= 2147483647) {
      results.push({ type: 'Int32', confidence: 'élevée', reason: 'entier courant dans les jeux' })
    }
    if (integer && numberValue >= 0 && numberValue <= 4294967295) {
      results.push({ type: 'UInt32', confidence: 'élevée', reason: 'entier non signé courant pour ressources' })
    }
    if (integer) results.push({ type: 'Int64', confidence: 'moyenne', reason: 'entier large possible' })
    if (integer && numberValue >= 0) results.push({ type: 'UInt64', confidence: 'faible', reason: 'entier non signé large possible' })
    results.push({ type: 'Float32', confidence: integer ? 'moyenne' : 'élevée', reason: 'valeur affichée parfois stockée en float' })
    results.push({ type: 'Float64', confidence: 'faible', reason: 'moins fréquent, utile pour jeux/outils spécifiques' })
    for (const scale of [10, 100, 1000, 4096, 65536]) {
      const scaled = numberValue * scale
      if (integer && Math.abs(scaled) <= 2147483647) {
        results.push({ type: `Int32 x${scale}`, confidence: scale >= 4096 ? 'moyenne' : 'moyenne', reason: `valeur affichée ${value}, stock possible ${scaled}` })
      }
    }
    return results
  }

  const inferredExactTypes = computed(() => inferredTypesForValue(exactScanValue.value))
  const canWriteSelectedValue = computed(() => {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return false
    return !writeSafetyWarning.value || writeSafetyAcknowledged.value
  })

  watch([selectedCandidateAddress, writeValue, writeSafetyWarning], () => {
    writeSafetyAcknowledged.value = false
  })

  function useInferredType(typeLabel: string) {
    const type = typeLabel.replace(/\s+x100$/i, '')
    if (typeLabel.endsWith('x100')) {
      const numeric = Number(exactScanValue.value.trim().replace(',', '.'))
      if (Number.isFinite(numeric)) exactScanValue.value = String(numeric * 100)
    }
    exactScanType.value = type
    addActionLog('scan_type', `Type de scan choisi : ${typeLabel}`, `Valeur ${exactScanValue.value}.`, 'info')
  }

  function addAddressToWatch(address: string, type = exactScanType.value) {
    const normalized = address.trim().replace(/^0x/i, '')
    if (!normalized) return
    const existing = watchedAddresses.value.find((item) => item.address === normalized)
    if (existing) {
      existing.type = type
      void refreshWatchedAddress(normalized)
      return
    }
    watchedAddresses.value.push({
      address: normalized,
      type,
      value: '',
      previousValue: '',
      changed: false,
      error: '',
      updatedAt: '',
    })
    void refreshWatchedAddress(normalized)
  }

  function removeAddressFromWatch(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    watchedAddresses.value = watchedAddresses.value.filter((item) => item.address !== normalized)
  }

  async function refreshWatchedAddress(address: string): Promise<WatchedAddress | null> {
    const normalized = address.trim().replace(/^0x/i, '')
    const watched = watchedAddresses.value.find((item) => item.address === normalized)
    if (!watched) return null

    let updated: WatchedAddress
    try {
      const preview = await backend.getController().readMemoryPreview(
        watched.address,
        valueTypeReadSize(watched.type),
      )
      const value = decodeTypedPreviewValue(preview, watched.type)
      updated = {
        ...watched,
        previousValue: watched.value,
        value,
        changed: watched.value !== '' && value !== watched.value,
        error: preview.success ? '' : preview.error,
        updatedAt: nowTime(),
      }
    } catch (e) {
      updated = { ...watched, previousValue: watched.value, changed: false, error: String(e), updatedAt: nowTime() }
    }

    watchedAddresses.value = watchedAddresses.value.map((item) => (
      item.address === normalized ? updated : item
    ))
    return updated
  }

  async function refreshWatchedAddresses() {
    for (const watched of watchedAddresses.value.slice(0, 20)) {
      await refreshWatchedAddress(watched.address)
    }
  }

  function setWatchLiveEnabled(enabled: boolean) {
    watchLiveEnabled.value = enabled
    if (watchLiveTimer) {
      clearInterval(watchLiveTimer)
      watchLiveTimer = null
    }
    if (enabled) {
      void refreshWatchedAddresses()
      watchLiveTimer = setInterval(() => {
        void refreshWatchedAddresses()
      }, 1000)
    }
    addActionLog('watch', enabled ? 'Watch live activé' : 'Watch live arrêté', `${watchedAddresses.value.length} adresse(s).`, enabled ? 'success' : 'info')
  }

  function keepCandidate(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    keptCandidateAddresses.value = Array.from(new Set([...keptCandidateAddresses.value, normalized]))
    ignoredCandidateAddresses.value = ignoredCandidateAddresses.value.filter((item) => item !== normalized)
    candidateFilter.value = normalized
    candidatePageIndex.value = 0
    void refreshCandidates()
    addActionLog('candidate', `Candidat gardé 0x${normalized}`, 'Filtre appliqué sur cette adresse.', 'success')
  }

  function ignoreCandidate(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    ignoredCandidateAddresses.value = Array.from(new Set([...ignoredCandidateAddresses.value, normalized]))
    keptCandidateAddresses.value = keptCandidateAddresses.value.filter((item) => item !== normalized)
    addActionLog('candidate', `Candidat ignoré 0x${normalized}`, 'Masqué dans la comparaison visuelle locale.', 'warning')
  }

  function candidateVisualState(candidate: Record<string, unknown>): string {
    const address = String(candidate.address ?? '').replace(/^0x/i, '')
    if (keptCandidateAddresses.value.includes(address)) return 'gardé'
    if (ignoredCandidateAddresses.value.includes(address)) return 'ignoré'
    const confidence = Number(candidate.confidence ?? Number.NaN)
    if (Number.isFinite(confidence)) {
      if (confidence >= 0.8) return 'très probable'
      if (confidence >= 0.5) return 'à vérifier'
      return 'faible'
    }
    return 'standard'
  }

  async function writeSelectedAddresses(addresses: string[], type: string, value: string) {
    if (addresses.length === 0 || !value.trim()) return
    try {
      const results: MemoryWriteResult[] = []
      for (const address of addresses) {
        const result = await backend.getController().writeMemoryValue(address, type, value)
        results.push(result)
      }
      writeResult.value = results[results.length - 1]
      scanStatusText.value = results.every((r) => r.success)
        ? `${results.length} adresse(s) écrite(s).`
        : `Écriture partielle: ${results.filter((r) => r.success).length}/${results.length} réussie(s).`
      addActionLog('write', `Écriture multiple ${value}`, `${results.filter((r) => r.success).length}/${results.length} réussie(s).`, results.every((r) => r.success) ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture multiple échouée.'
      addActionLog('write', 'Écriture multiple échouée', String(e), 'error')
    }
  }

  async function writeSelectedTargets(targets: MemoryWriteTarget[], value: string) {
    if (targets.length === 0 || !value.trim()) return
    try {
      const controller = backend.getController()
      if (controller.writeMemoryValuesWithVariants) {
        const result: MemoryWriteBatchResult = await controller.writeMemoryValuesWithVariants(targets, value)
        writeResult.value = result
        const written = result.written ?? result.results?.filter((r) => r.success).length ?? 0
        scanStatusText.value = result.success
          ? `${written} adresse(s) écrite(s) avec encodage auto.`
          : `Écriture auto partielle: ${written}/${targets.length} réussie(s).`
        for (const target of targets) {
          addAddressToWatch(target.address, target.type)
        }
        addActionLog('write', `Écriture auto ${value}`, `${written}/${targets.length} réussie(s).`, result.success ? 'success' : 'warning')
        return
      }
      await writeSelectedAddresses(targets.map((target) => target.address), targets[0]?.type ?? exactScanType.value, value)
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture auto échouée.'
      addActionLog('write', 'Écriture auto échouée', String(e), 'error')
    }
  }

  async function writeSelectedValue() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Écriture bloquée', writeSafetyWarning.value, 'warning')
      return
    }
    try {
      writeResult.value = await backend
        .getController()
        .writeMemoryValue(selectedCandidateAddress.value, exactScanType.value, writeValue.value)
      addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
      addActionLog(
        'write',
        `Écriture 0x${selectedCandidateAddress.value}`,
        `${exactScanType.value} = ${writeValue.value}${writeSafetyWarning.value ? ` · ${writeSafetyWarning.value}` : ''}.`,
        writeResult.value.success ? 'success' : 'error',
      )
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      addActionLog('write', `Écriture échouée 0x${selectedCandidateAddress.value}`, String(e), 'error')
    }
  }

  async function rollbackLastWrite() {
    try {
      writeResult.value = await backend.getController().rollbackLastWrite()
      addActionLog('rollback', 'Rollback dernière écriture', writeResult.value.success ? 'Adresse restaurée.' : writeResult.value.error, writeResult.value.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      addActionLog('rollback', 'Rollback échoué', String(e), 'error')
    }
  }

  async function rollbackLastWriteBatch() {
    try {
      const result = await backend.getController().rollbackLastWriteBatch()
      const restored = Array.isArray(result.restoredWrites)
        ? result.restoredWrites
          .map((item: unknown) => {
            const record = item as Record<string, unknown>
            const prefix = record.success === true ? '✓' : '✗'
            return `${prefix} 0x${record.address}: ${String(record.from ?? '?')} -> ${String(record.to ?? '?')}`
          })
          .join('\n')
        : ''
      pushMessage('assistant', result.success
        ? `Rollback batch réussi : ${result.rolledBack}/${result.total} écritures restaurées.${restored ? `\n${restored}` : ''}`
        : `Rollback batch partiel : ${String(result.rolledBack ?? 0)}/${String(result.total ?? 0)} restaurées.${restored ? `\n${restored}` : ''}`)
      addActionLog('rollback', 'Rollback batch', `${String(result.rolledBack ?? 0)}/${String(result.total ?? 0)} restaurée(s).`, result.success ? 'success' : 'warning')
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
      return result
    } catch (e) {
      pushMessage('assistant', 'Rollback batch échoué : ' + String(e), { isError: true })
      return { success: false, error: String(e) }
    }
  }

  async function freezeCandidateCurrent(address: string, type: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    if (!normalized) return

    selectedCandidateAddress.value = normalized
    exactScanType.value = type
    addAddressToWatch(normalized, type)

    const watched = await refreshWatchedAddress(normalized)
    const currentValue = watched?.value.trim() ?? ''
    if (!currentValue || currentValue === '-') {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: watched?.error || 'Valeur actuelle illisible.',
        enabled: freezeEnabled.value,
      }
      addActionLog('freeze', `Freeze impossible 0x${normalized}`, writeResult.value.error, 'error')
      return
    }

    writeValue.value = currentValue
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze bloqué', writeSafetyWarning.value, 'warning')
      return
    }

    try {
      writeResult.value = await backend
        .getController()
        .setFreezeValue(normalized, type, currentValue, true)
      if (writeResult.value.success) {
        freezeEnabled.value = true
        await refreshWatchedAddress(normalized)
      }
      addActionLog(
        'freeze',
        writeResult.value.success ? 'Freeze actuel activé' : 'Freeze actuel échoué',
        `0x${normalized} ${type} = ${currentValue}.`,
        writeResult.value.success ? 'success' : 'error',
      )
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: freezeEnabled.value }
      addActionLog('freeze', `Freeze échoué 0x${normalized}`, String(e), 'error')
    }
  }

  async function toggleFreeze() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    const nextState = !freezeEnabled.value
    updateWriteSafetyWarning()
    if (nextState && writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze bloqué', writeSafetyWarning.value, 'warning')
      return
    }
    try {
      writeResult.value = await backend
        .getController()
        .setFreezeValue(selectedCandidateAddress.value, exactScanType.value, writeValue.value, nextState)
      if (writeResult.value.success) {
        freezeEnabled.value = nextState
        addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
      }
      addActionLog('freeze', nextState ? 'Freeze activé' : 'Freeze arrêté', `0x${selectedCandidateAddress.value} = ${writeValue.value}.`, writeResult.value.success ? 'success' : 'error')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: freezeEnabled.value }
      addActionLog('freeze', 'Freeze échoué', String(e), 'error')
    }
  }

  async function nextCandidatePage() {
    if (!candidatePage.value) return
    const nextStart = (candidatePageIndex.value + 1) * candidatePageSize.value
    if (nextStart >= candidatePage.value.totalCount) return
    candidatePageIndex.value += 1
    await refreshCandidates()
  }

  async function previousCandidatePage() {
    if (candidatePageIndex.value === 0) return
    candidatePageIndex.value -= 1
    await refreshCandidates()
  }

  return {
    version,
    activeView,
    uiMode,
    isConnected,
    isAttached,
    processName,
    processes,
    processModules,
    memoryMap,
    memoryPreview,
    memoryPreviewAddress,
    memoryPreviewLoading,
    memoryPreviewAscii,
    memoryPreviewDecoded,
    selectedMemoryRegion,
    pingResult,
    logFilePath,
    logLines,
    logError,
    diagnosticExportPath,
    diagnosticExportError,
    temporaryStorageStatus,
    temporaryStorageCleanupResult,
    temporaryStorageError,
    smartSearchDebugFilePath,
    scanTelemetryFilePath,
    smartSearchDebugEvents,
    smartSearchDebugError,
    settingsLoaded,
    settingsSaving,
    settingsStatus,
    appLanguage,
    settingDefaultValueType,
    settingScanMaxResults,
    settingScanChunkSizeMb,
    settingPerformanceMode,
    settingScanMaxWorkerThreads,
    settingScanMaxInFlightMb,
    settingCandidateFileBackedThreshold,
    settingUnknownSnapshotMaxMb,
    settingFastScan,
    settingSmartSearchDebugEnabled,
    settingSmartSearchDebugMaxEvents,
    settingModelPath,
    settingModelThreads,
    activeChatMemoryTargets,
    smartSearchContext,
    searchQuery,
    searchResult,
    exactScanValue,
    exactScanType,
    exactScanResult,
    expertModeEnabled,
    expertStartAddress,
    expertStopAddress,
    expertAlignment,
    expertWritableOnly,
    expertExecutableOnly,
    expertCopyOnWriteOnly,
    expertRegionSize,
    expertRegionProtection,
    expertRegionState,
    expertRegionType,
    candidatePage,
    candidatePageIndex,
    candidatePageSize,
    candidateFilter,
    nextScanMode,
    nextScanValue,
    nextScanResult,
    undoCandidateScanResult,
    unknownScanMode,
    unknownScanType,
    unknownWritableOnly,
    unknownCopyOnWriteOnly,
    unknownSnapshotResult,
    unknownNextScanResult,
    unknownGuideSteps,
    selectedCandidateAddress,
    writeValue,
    writeResult,
    writeSafetyWarning,
    writeSafetyAcknowledged,
    canWriteSelectedValue,
    freezeEnabled,
    finalCandidateTargets,
    ignoredCandidateAddresses,
    keptCandidateAddresses,
    watchLiveEnabled,
    watchedAddresses,
    messages,
    actionLog,
    workflowStatus,
    targetValueGuided,
    candidateHistory,
    isSearching,
    scanBusy,
    scanStatusText,
    scanProgressPercent,
    statusText,
    init,
    refreshProcesses,
    refreshProcessModules,
    refreshMemoryMap,
    readMemoryPreview,
    attach,
    detach,
    doPing,
    loadSettings,
    saveSettings,
    refreshDiagnostics,
    refreshLogTail,
    exportDiagnostics,
    refreshTemporaryStorageStatus,
    clearTemporaryStorage,
    refreshActiveChatMemoryTargets,
    refreshSmartSearchContext,
    clearActiveChatMemoryTargets,
    clearSmartSearchDebug,
    doSearch,
    updateMessage,
    startNewSearchContext,
    useSuggestedAddresses,
    searchValueElsewhere,
    searchValueAsType,
    testSingleSuggestedAddress,
    doExactScan,
    cancelActiveScan,
    refreshCandidates,
    nextCandidatePage,
    previousCandidatePage,
    doNextScan,
    undoCandidateScan,
    captureUnknownSnapshot,
    doUnknownNextScan,
    runUnknownGuideStep,
    selectCandidate,
    openExpertAtAddress,
    openExpertForRegion,
    scanAroundPreview,
    inferredExactTypes,
    useInferredType,
    updateWriteSafetyWarning,
    addAddressToWatch,
    removeAddressFromWatch,
    refreshWatchedAddress,
    refreshWatchedAddresses,
    setWatchLiveEnabled,
    keepCandidate,
    ignoreCandidate,
    candidateVisualState,
    writeSelectedValue,
    writeSelectedAddresses,
    writeSelectedTargets,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    freezeCandidateCurrent,
    toggleFreeze,
    pushMessage,
    tellNewValue,
    doGuidedChange,
    resetWorkflow,
  }
})
