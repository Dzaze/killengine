import { defineStore } from 'pinia'
import { ref, computed } from 'vue'
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
  type MemoryWriteResult,
  type ProcessInfo,
  type ProcessModuleInfo,
  type SmartSearchContextResult,
  type SmartSearchDebugEventsResult,
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
  suggestions?: Array<Record<string, unknown>>
  filteredWriteCandidates?: Array<Record<string, unknown>>
  autoWriteResults?: Array<Record<string, unknown>>
  autoWriteOk?: boolean
  requiresConfirmation?: boolean
  confirmationReason?: string
  isError?: boolean
}

export const useAppStore = defineStore('app', () => {
  // State
  const version = ref('...')
  const isConnected = ref(false)
  const isAttached = ref(false)
  const processName = ref('')
  const processes = ref<ProcessInfo[]>([])
  const processModules = ref<ProcessModuleInfo[]>([])
  const memoryMap = ref<MemoryMapResult | null>(null)
  const memoryPreview = ref<MemoryReadPreview | null>(null)
  const pingResult = ref('')
  const logFilePath = ref('')
  const logLines = ref<string[]>([])
  const logError = ref('')
  const diagnosticExportPath = ref('')
  const diagnosticExportError = ref('')
  const smartSearchDebugFilePath = ref('')
  const smartSearchDebugEvents = ref<Array<Record<string, unknown>>>([])
  const smartSearchDebugError = ref('')
  const settingsLoaded = ref(false)
  const settingsSaving = ref(false)
  const settingsStatus = ref('')
  const appLanguage = ref<'fr' | 'en'>('fr')
  const settingDefaultValueType = ref('Int32')
  const settingScanMaxResults = ref(1000000)
  const settingScanChunkSizeMb = ref(1)
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
  const unknownSnapshotResult = ref<UnknownSnapshotResult | null>(null)
  const unknownNextScanResult = ref<UnknownNextScanResult | null>(null)
  const selectedCandidateAddress = ref('')
  const writeValue = ref('')
  const writeResult = ref<MemoryWriteResult | null>(null)
  const freezeEnabled = ref(false)
  const finalCandidateTargets = ref<Array<Record<string, unknown>>>([])

  // Chat / guided workflow state
  const messages = ref<ChatMessage[]>([])
  const messageIdCounter = ref(0)
  const workflowStatus = ref<string>('idle')
  const targetValueGuided = ref<string>('')
  const candidateHistory = ref<number[]>([])
  const isSearching = ref(false)
  const scanBusy = ref(false)
  const scanStatusText = ref('')
  const scanProgressPercent = ref(0)

  // Getters
  const statusText = computed(() => {
    if (!isConnected.value) return 'Déconnecté'
    if (!isAttached.value) return 'Prêt'
    return `Attaché: ${processName.value}`
  })

  function nowTime(): string {
    return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
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

  function resetWorkflow() {
    workflowStatus.value = 'idle'
    targetValueGuided.value = ''
    candidateHistory.value = []
  }

  async function tellNewValue(value: string) {
    if (!value.trim()) return
    searchQuery.value = value.trim()
    await doSearch()
  }

  async function doGuidedChange() {
    // Bouton "J'ai changé" — invite l'utilisateur à donner la nouvelle valeur.
    pushMessage('assistant', 'Parfait ! Donne-moi maintenant la nouvelle valeur affichée dans le jeu.')
    workflowStatus.value = 'awaiting_new_value'
  }

  // Actions
  async function init() {
    try {
      await backend.connect()
      isConnected.value = backend.isConnected
      version.value = await backend.getController().getVersion()
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
    try {
      memoryPreview.value = await backend.getController().readMemoryPreview(addressHex, size)
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
      logFilePath.value = await backend.getController().getLogFilePath()
      smartSearchDebugFilePath.value = await backend.getController().getSmartSearchDebugFilePath()
      await refreshLogTail()
      const debugResult: SmartSearchDebugEventsResult = await backend
        .getController()
        .getSmartSearchDebugEvents(settingSmartSearchDebugMaxEvents.value)
      smartSearchDebugEvents.value = debugResult.events ?? []
      smartSearchDebugError.value = debugResult.error ?? ''
    } catch (e) {
      logFilePath.value = ''
      logLines.value = []
      logError.value = String(e)
      smartSearchDebugFilePath.value = ''
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

  function applySettings(settings: AppSettings) {
    appLanguage.value = settings.language === 'en' ? 'en' : 'fr'
    settingDefaultValueType.value = settings.defaultValueType || 'Int32'
    exactScanType.value = settingDefaultValueType.value
    unknownScanType.value = settingDefaultValueType.value
    settingScanMaxResults.value = Number(settings.scanMaxResults || 1000000)
    settingScanChunkSizeMb.value = Number(settings.scanChunkSizeMb || 1)
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
    searchQuery.value = ''
    isSearching.value = true

    try {
      const result = await backend.getController().startSmartSearch(query)
      searchResult.value = result.message ?? JSON.stringify(result, null, 2)

      // Synchronise les résultats déterministes
      if (result.actionStatus === 'executed' && result.actionResult) {
        if (result.tool === 'exact_scan') {
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

      if (result.autoWriteResults) {
        const results = result.autoWriteResults as Array<Record<string, unknown>>
        extras.autoWriteResults = results
        extras.autoWriteOk = results.length > 0 && results.every((r) => r.success === true)
      }

      if (result.error) extras.isError = true

      const assistantText = String(
        result.message
        ?? result.error
        ?? "Je n'ai pas assez d'informations pour agir. Donne-moi une valeur à chercher ou une adresse à utiliser.",
      ).trim()
      pushMessage(
        'assistant',
        assistantText || "Je n'ai pas assez d'informations pour agir. Donne-moi une valeur à chercher ou une adresse à utiliser.",
        extras,
      )
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
    } catch (e) {
      pushMessage('assistant', 'Erreur de recherche : ' + String(e), { isError: true })
    } finally {
      isSearching.value = false
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
      scanProgressPercent.value = 8
      scanStatusText.value = 'Scan exact en cours...'
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
      scanProgressPercent.value = 85
      candidatePageIndex.value = 0
      scanStatusText.value = 'Chargement des candidats...'
      await refreshCandidates()
      scanProgressPercent.value = 100
      scanStatusText.value = exactScanResult.value.cancelled ? 'Scan annulé.' : 'Scan terminé.'
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
    } finally {
      scanBusy.value = false
    }
  }

  async function refreshCandidates() {
    try {
      candidatePage.value = await backend
        .getController()
        .getCandidates(candidatePageIndex.value, candidatePageSize.value, candidateFilter.value)
    } catch (e) {
      console.error('[KillEngine] Failed to get candidates:', e)
      candidatePage.value = null
    }
  }

  async function doNextScan() {
    if (scanBusy.value) return
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      scanStatusText.value = 'Réduction des candidats...'
      nextScanResult.value = await backend.startNextScanAsync(nextScanMode.value, nextScanValue.value)
      scanProgressPercent.value = 85
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      scanProgressPercent.value = 100
      scanStatusText.value = nextScanResult.value.cancelled ? 'Scan annulé.' : 'Next scan terminé.'
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
      } else {
        scanStatusText.value = undoCandidateScanResult.value.error || 'Aucune réduction à restaurer.'
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
      unknownSnapshotResult.value = await backend.captureUnknownSnapshotAsync()
      scanProgressPercent.value = 100
      scanStatusText.value = unknownSnapshotResult.value.cancelled ? 'Scan annulé.' : 'Snapshot capturé.'
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
    } finally {
      scanBusy.value = false
    }
  }

  async function doUnknownNextScan() {
    if (scanBusy.value) return
    try {
      scanBusy.value = true
      scanProgressPercent.value = 15
      scanStatusText.value = 'Comparaison unknown en cours...'
      unknownNextScanResult.value = await backend.unknownNextScanAsync(unknownScanMode.value, unknownScanType.value)
      scanProgressPercent.value = 85
      candidatePageIndex.value = 0
      scanStatusText.value = 'Actualisation des candidats...'
      await refreshCandidates()
      scanProgressPercent.value = 100
      scanStatusText.value = unknownNextScanResult.value.cancelled ? 'Scan annulé.' : 'Comparaison unknown terminée.'
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
    } finally {
      scanBusy.value = false
    }
  }

  function selectCandidate(address: string, type: string) {
    selectedCandidateAddress.value = address
    exactScanType.value = type
  }

  async function writeSelectedValue() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    try {
      writeResult.value = await backend
        .getController()
        .writeMemoryValue(selectedCandidateAddress.value, exactScanType.value, writeValue.value)
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
    }
  }

  async function rollbackLastWrite() {
    try {
      writeResult.value = await backend.getController().rollbackLastWrite()
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
    }
  }

  async function rollbackLastWriteBatch() {
    try {
      const result = await backend.getController().rollbackLastWriteBatch()
      pushMessage('assistant', result.success
        ? `Rollback batch réussi : ${result.rolledBack}/${result.total} écritures restaurées.`
        : `Rollback batch partiel : ${String(result.rolledBack ?? 0)}/${String(result.total ?? 0)} restaurées.`)
      return result
    } catch (e) {
      pushMessage('assistant', 'Rollback batch échoué : ' + String(e), { isError: true })
      return { success: false, error: String(e) }
    }
  }

  async function toggleFreeze() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    const nextState = !freezeEnabled.value
    try {
      writeResult.value = await backend
        .getController()
        .setFreezeValue(selectedCandidateAddress.value, exactScanType.value, writeValue.value, nextState)
      if (writeResult.value.success) {
        freezeEnabled.value = nextState
      }
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: freezeEnabled.value }
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
    isConnected,
    isAttached,
    processName,
    processes,
    processModules,
    memoryMap,
    memoryPreview,
    pingResult,
    logFilePath,
    logLines,
    logError,
    diagnosticExportPath,
    diagnosticExportError,
    smartSearchDebugFilePath,
    smartSearchDebugEvents,
    smartSearchDebugError,
    settingsLoaded,
    settingsSaving,
    settingsStatus,
    appLanguage,
    settingDefaultValueType,
    settingScanMaxResults,
    settingScanChunkSizeMb,
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
    unknownSnapshotResult,
    unknownNextScanResult,
    selectedCandidateAddress,
    writeValue,
    writeResult,
    freezeEnabled,
    finalCandidateTargets,
    messages,
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
    refreshActiveChatMemoryTargets,
    refreshSmartSearchContext,
    clearActiveChatMemoryTargets,
    clearSmartSearchDebug,
    doSearch,
    doExactScan,
    cancelActiveScan,
    refreshCandidates,
    nextCandidatePage,
    previousCandidatePage,
    doNextScan,
    undoCandidateScan,
    captureUnknownSnapshot,
    doUnknownNextScan,
    selectCandidate,
    writeSelectedValue,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    toggleFreeze,
    pushMessage,
    tellNewValue,
    doGuidedChange,
    resetWorkflow,
  }
})
