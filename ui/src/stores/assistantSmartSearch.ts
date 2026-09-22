/**
 * KillEngine - store Assistant / Smart Search / Candidate Actions (extrait de
 * app.ts, candidats S10 + S11b de docs/REFACTOR_ROADMAP.md, PHASE 236,
 * 30/08/2026).
 *
 * Ces deux chantiers sont volontairement traites ensemble : le dispatch chat
 * Smart Search consomme directement les actions auto pilotees IA
 * (runAutoEncryptedScan, runAutoUnknownObservation, runAutoTraceUiString) et
 * la promotion candidat -> cible d'ecriture (finalCandidateTargets, candidats
 * gardes/ignores). Les separer forcerait des callbacks dans les deux sens et
 * fragiliserait exactement la zone qu'on veut clarifier.
 *
 * Le store importe les feuilles deja extraites (scanning.ts, trainer.ts,
 * writeFreeze.ts, investigation.ts, actionLog.ts) et recoit seulement les
 * dependances encore possedees par app.ts : navigation, process courant,
 * ecriture memoire mode standard/kernel et ajout au Watch live.
 */
import { defineStore, storeToRefs } from 'pinia'
import { computed, nextTick, ref, type Ref } from 'vue'
import {
  backend,
  type ChatMemoryTargetsResult,
  type EncryptedScanResult,
  type ExactScanResult,
  type MemoryWriteResult,
  type NextScanResult,
  type SmartSearchContextResult,
  type UiStringCandidate,
  type UiStringScanResult,
  type UiStringSourceCandidate,
  type UiStringSourceResult,
  type UnknownNextScanResult,
  type UnknownSnapshotResult,
} from '@/services/backend'
import { useActionLogStore, type UserActionLogEntry } from './actionLog'
import { useInvestigationStore, type InvestigationStep } from './investigation'
import { useScanningStore } from './scanning'
import { useTrainerStore, type TrainerFeature } from './trainer'
import { useWriteFreezeStore } from './writeFreeze'
import { i18n } from '@/i18n'

const { t } = i18n.global

export type AssistantView = 'assistant' | 'investigation' | 'trainer' | 'project' | 'process' | 'memory' | 'memory-timeline' | 'memory-heatmap' | 'pattern-learning' | 'clr' | 'webview2' | 'scripting' | 'speedhack' | 'network' | 'profiles' | 'expert' | 'lexicon' | 'modules' | 'settings'

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
  invalidated?: boolean
  previousTargetValue?: string
  writeHistory?: string[]
  activeTargetCount?: number
  recoveryActions?: Array<Record<string, unknown>>
  executedSafeSteps?: Array<Record<string, unknown>>
  requiresConfirmation?: boolean
  confirmationReason?: string
  isThinking?: boolean
  isError?: boolean
}

export interface WorkflowPreset {
  id: string
  title: string
  description: string
  prompt: string
  startView: AssistantView
  mode: 'auto' | 'manual'
  valueType?: string
  risk: 'safe' | 'write' | 'debug' | 'patch' | 'injection'
  nextStep: string
}

export interface InvestigationReport {
  kind?: string
  createdAt?: string
  processName?: string
  displayedValue?: string
  nextDisplayedValue?: string
  numericSources?: {
    count?: number
    selected?: number
    top?: Array<Record<string, unknown>>
  }
  liveInvestigation?: Record<string, unknown> | null
  debugger?: {
    hitCount?: number
    cancelled?: boolean
    hits?: Array<Record<string, unknown>>
  }
  aob?: Record<string, unknown>
}

export interface AssistantSmartSearchExternalDeps {
  activeView: Ref<AssistantView>
  processName: Ref<string>
  writeMemoryValueByMode: (address: string, type: string, value: string) => Promise<MemoryWriteResult>
  kernelMemoryModeActive: { readonly value: boolean }
  addAddressToWatch: (address: string, type?: string) => void
}

export const useAssistantSmartSearchStore = defineStore('assistantSmartSearch', () => {
  const actionLogStore = useActionLogStore()
  const investigationStore = useInvestigationStore()
  const scanningStore = useScanningStore()
  const trainerStore = useTrainerStore()
  const writeFreezeStore = useWriteFreezeStore()
  const { activeInvestigation } = storeToRefs(investigationStore)
  const {
    exactScanValue,
    exactScanType,
    exactScanResult,
    encryptedScanResult,
    encryptedScanMode,
    encryptedScanKey,
    encryptedScanKeySearchBits,
    groupScanResult,
    expertStartAddress,
    expertStopAddress,
    expertRegionSize,
    expertRegionProtection,
    expertRegionState,
    expertRegionType,
    selectedCandidateAddress,
    candidatePage,
    candidatePageIndex,
    candidateFilter,
    nextScanValue,
    nextScanResult,
    unknownSnapshotResult,
    unknownNextScanResult,
    unknownGuideSteps,
    autoUnknownAwaitingObservation,
    unknownScanType,
    candidateHistory,
    scanBusy,
    scanStatusText,
  } = storeToRefs(scanningStore)
  const {
    writeValue,
    writeResult,
    writeSafetyWarning,
    writeSafetyAcknowledged,
    freezeIntervalResult,
  } = storeToRefs(writeFreezeStore)
  const { getTrainerFeaturesSnapshot, createTrainerFeature, deleteTrainerFeature } = trainerStore

  let externalDeps: AssistantSmartSearchExternalDeps | null = null
  function configureAssistantSmartSearchContext(newDeps: AssistantSmartSearchExternalDeps) {
    externalDeps = newDeps
  }
  function deps(): AssistantSmartSearchExternalDeps {
    if (!externalDeps) throw new Error('assistantSmartSearch store used before configureAssistantSmartSearchContext')
    return externalDeps
  }

  const activeView = {
    get value() { return deps().activeView.value },
    set value(value: AssistantView) { deps().activeView.value = value },
  } as Ref<AssistantView>
  const processName = {
    get value() { return deps().processName.value },
    set value(value: string) { deps().processName.value = value },
  } as Ref<string>
  const kernelMemoryModeActive = {
    get value() { return deps().kernelMemoryModeActive.value },
  }

  function addAddressToWatch(address: string, type?: string) {
    deps().addAddressToWatch(address, type)
  }

  function writeMemoryValueByMode(address: string, type: string, value: string): Promise<MemoryWriteResult> {
    return deps().writeMemoryValueByMode(address, type, value)
  }

  function nowTime(): string {
    return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
  }

  const pendingAssistantAction = ref('')
  const workflowPresets = computed<WorkflowPreset[]>(() => [
    {
      id: 'exact-value',
      title: t('assistantSmartSearchStore.presets.exactValue.title'),
      description: t('assistantSmartSearchStore.presets.exactValue.description'),
      prompt: t('assistantSmartSearchStore.presets.exactValue.prompt'),
      startView: 'assistant',
      mode: 'auto',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.exactValue.nextStep'),
    },
    {
      id: 'unknown-change',
      title: t('assistantSmartSearchStore.presets.unknownChange.title'),
      description: t('assistantSmartSearchStore.presets.unknownChange.description'),
      prompt: t('assistantSmartSearchStore.presets.unknownChange.prompt'),
      startView: 'assistant',
      mode: 'auto',
      valueType: 'Auto',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.unknownChange.nextStep'),
    },
    {
      id: 'display-trace',
      title: t('assistantSmartSearchStore.presets.displayTrace.title'),
      description: t('assistantSmartSearchStore.presets.displayTrace.description'),
      prompt: t('assistantSmartSearchStore.presets.displayTrace.prompt'),
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.displayTrace.nextStep'),
    },
    {
      id: 'stable-trainer',
      title: t('assistantSmartSearchStore.presets.stableTrainer.title'),
      description: t('assistantSmartSearchStore.presets.stableTrainer.description'),
      prompt: t('assistantSmartSearchStore.presets.stableTrainer.prompt'),
      startView: 'trainer',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'write',
      nextStep: t('assistantSmartSearchStore.presets.stableTrainer.nextStep'),
    },
    {
      id: 'code-investigation',
      title: t('assistantSmartSearchStore.presets.codeInvestigation.title'),
      description: t('assistantSmartSearchStore.presets.codeInvestigation.description'),
      prompt: t('assistantSmartSearchStore.presets.codeInvestigation.prompt'),
      startView: 'investigation',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'debug',
      nextStep: t('assistantSmartSearchStore.presets.codeInvestigation.nextStep'),
    },
    // Scénarios courants Expert/Trainer (finition commerciale) : mêmes champs
    // que les presets Assistant ci-dessus, réutilisent applyWorkflowPreset()
    // tel quel — juste des données, pas un nouveau mécanisme.
    {
      id: 'scenario-money',
      title: t('assistantSmartSearchStore.presets.scenarioMoney.title'),
      description: t('assistantSmartSearchStore.presets.scenarioMoney.description'),
      prompt: t('assistantSmartSearchStore.presets.scenarioMoney.prompt'),
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.scenarioMoney.nextStep'),
    },
    {
      id: 'scenario-health',
      title: t('assistantSmartSearchStore.presets.scenarioHealth.title'),
      description: t('assistantSmartSearchStore.presets.scenarioHealth.description'),
      prompt: t('assistantSmartSearchStore.presets.scenarioHealth.prompt'),
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.scenarioHealth.nextStep'),
    },
    {
      id: 'scenario-score',
      title: t('assistantSmartSearchStore.presets.scenarioScore.title'),
      description: t('assistantSmartSearchStore.presets.scenarioScore.description'),
      prompt: t('assistantSmartSearchStore.presets.scenarioScore.prompt'),
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.scenarioScore.nextStep'),
    },
    {
      id: 'scenario-ammo',
      title: t('assistantSmartSearchStore.presets.scenarioAmmo.title'),
      description: t('assistantSmartSearchStore.presets.scenarioAmmo.description'),
      prompt: t('assistantSmartSearchStore.presets.scenarioAmmo.prompt'),
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: t('assistantSmartSearchStore.presets.scenarioAmmo.nextStep'),
    },
  ])
  const lastWorkflowPresetId = ref('')
  const activeChatMemoryTargets = ref<Array<Record<string, unknown>>>([])
  const smartSearchContext = ref<SmartSearchContextResult | null>(null)
  const investigationReport = ref<InvestigationReport | null>(null)
  const searchQuery = ref('')
  const searchResult = ref('')
  const autoUiStringScanResult = ref<UiStringScanResult | null>(null)
  const autoUiStringSourceResult = ref<UiStringSourceResult | null>(null)
  const autoUiStringSources = ref<UiStringSourceCandidate[]>([])
  const finalCandidateTargets = ref<Array<Record<string, unknown>>>([])
  const ignoredCandidateAddresses = ref<string[]>([])
  const keptCandidateAddresses = ref<string[]>([])
  const messages = ref<ChatMessage[]>([])
  const messageIdCounter = ref(0)
  const workflowStatus = ref<string>('idle')
  const targetValueGuided = ref<string>('')
  const isSearching = ref(false)

  function firstNumberFromText(text: string): number | null {
    const match = text.replace(',', '.').match(/-?\d+(?:\.\d+)?/)
    if (!match) return null
    const value = Number(match[0])
    return Number.isFinite(value) ? value : null
  }

  function inferUnknownModeFromObservation(observation: string): 'increased' | 'decreased' | 'unchanged' | 'changed' {
    const lower = observation.toLowerCase()
    if (/(stable|pareil|inchang|m[eê]me|unchanged)/.test(lower)) return 'unchanged'
    if (/(augmente|mont|plus|hausse|increase|increased|higher)/.test(lower)) return 'increased'
    if (/(diminue|baisse|moins|decrease|decreased|lower)/.test(lower)) return 'decreased'

    const observed = firstNumberFromText(observation)
    const previousText = targetValueGuided.value || smartSearchContext.value?.targetValue || smartSearchContext.value?.initialValue || exactScanValue.value
    const previous = firstNumberFromText(String(previousText ?? ''))
    if (observed !== null && previous !== null) {
      if (observed > previous) return 'increased'
      if (observed < previous) return 'decreased'
      return 'unchanged'
    }
    return 'changed'
  }

  function addActionLog(
    kind: string,
    title: string,
    detail = '',
    status: UserActionLogEntry['status'] = 'info',
  ) {
    actionLogStore.addActionLog(kind, title, detail, status)
  }

  function saveInvestigations() {
    investigationStore.saveInvestigations()
  }

  function startInvestigation(objective: string, title?: string) {
    investigationStore.startInvestigation(objective, title, processName.value)
  }

  function addInvestigationStep(step: Omit<InvestigationStep, 'id' | 'time'>) {
    investigationStore.addInvestigationStep(step, searchQuery.value || t('assistantSmartSearchStore.investigation.manualFallback'), processName.value)
  }

  function applyWorkflowPreset(id: string) {
    const preset = workflowPresets.value.find((item) => item.id === id)
    if (!preset) {
      addActionLog('workflow', t('assistantSmartSearchStore.workflow.presetMissing'), id, 'warning')
      return null
    }
    lastWorkflowPresetId.value = preset.id
    searchQuery.value = preset.prompt
    if (preset.valueType) {
      if (preset.valueType === 'Auto') unknownScanType.value = 'Auto'
      else exactScanType.value = preset.valueType
    }
    if (!activeInvestigation.value) {
      startInvestigation(preset.prompt, t('assistantSmartSearchStore.workflow.presetTitle', { title: preset.title }))
    }
    addInvestigationStep({
      title: t('assistantSmartSearchStore.workflow.presetLoadedTitle', { title: preset.title }),
      detail: t('assistantSmartSearchStore.workflow.presetLoadedDetail', { description: preset.description, nextStep: preset.nextStep }),
      status: 'planned',
      tool: 'applyWorkflowPreset',
      risk: preset.risk,
      payload: {
        presetId: preset.id,
        mode: preset.mode,
        prompt: preset.prompt,
        startView: preset.startView,
      },
    })
    pushMessage('assistant', t('assistantSmartSearchStore.workflow.presetLoadedMessage', { title: preset.title, nextStep: preset.nextStep }))
    activeView.value = preset.startView
    addActionLog('workflow', t('assistantSmartSearchStore.workflow.presetLoadedTitle', { title: preset.title }), preset.nextStep, 'success')
    return preset
  }

  function updateInvestigationFromAutoResult(result: Record<string, unknown>) {
    investigationStore.updateInvestigationFromAutoResult(result)
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

  function markAutoWriteMessagesInvalidated(addresses: string[]) {
    if (addresses.length === 0) return
    const normalized = new Set(addresses.map((a) => a.replace(/^0x/i, '').toLowerCase()))
    for (const msg of messages.value) {
      if (!msg.autoWriteResults || msg.autoWriteResults.length === 0 || msg.invalidated) continue
      const matches = msg.autoWriteResults.some((r) =>
        normalized.has(String(r.address ?? '').replace(/^0x/i, '').toLowerCase()))
      if (matches) msg.invalidated = true
    }
  }

  async function letChatRenderBeforeBackendWork() {
    await nextTick()
    await new Promise<void>((resolve) => window.setTimeout(resolve, 50))
  }

  async function resetWorkflow() {
    try {
      const result = await backend.getController().clearScanContext()
      addActionLog('scan', t('assistantSmartSearchStore.scan.newScan'), String(result.message ?? t('assistantSmartSearchStore.scan.contextCleared')), 'success')
    } catch (e) {
      addActionLog('scan', t('assistantSmartSearchStore.scan.localNewScan'), t('assistantSmartSearchStore.scan.backendNotCleared', { error: String(e) }), 'warning')
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
    scanStatusText.value = t('assistantSmartSearchStore.scan.ready')
    setScanProgress(0)

    // Avant : "Nouveau scan" ne vidait que le candidate store côté guidé, en
    // laissant la région active, la cible d'écriture, le dernier résultat de
    // write/freeze et le scan groupé d'une enquête précédente affichés comme
    // si c'était pour la nouvelle recherche. Ne touche PAS aux surveillances
    // actives (watch, freeze polling/BP en cours) : celles-ci sont des
    // actions délibérées et indépendantes, les arrêter silencieusement au
    // clic serait une surprise, pas une aide.
    expertStartAddress.value = ''
    expertStopAddress.value = ''
    expertRegionSize.value = 0
    expertRegionProtection.value = ''
    expertRegionState.value = ''
    expertRegionType.value = ''
    selectedCandidateAddress.value = ''
    writeValue.value = ''
    writeResult.value = null
    writeSafetyWarning.value = ''
    writeSafetyAcknowledged.value = false
    freezeIntervalResult.value = null
    groupScanResult.value = null

    await refreshSmartSearchContext()
  }

  async function tellNewValue(value: string) {
    if (!value.trim()) return
    searchQuery.value = value.trim()
    await doSearch()
  }

  async function doGuidedChange() {
    // Bouton "J'ai changé" - invite l'utilisateur à donner la nouvelle valeur.
    pushMessage('assistant', t('assistantSmartSearchStore.guidedChange.askNewValue'))
    workflowStatus.value = 'awaiting_new_value'
  }

  async function clearAutoResolveMemory(allProcesses = false) {
    const controller = backend.getController()
    if (!controller.clearAutoResolveMemory) {
      addActionLog('ai_memory', t('assistantSmartSearchStore.autoMemory.unavailableTitle'), t('assistantSmartSearchStore.errors.backendNotExposed'), 'warning')
      return { success: false, error: t('assistantSmartSearchStore.errors.backendNotExposed') }
    }
    const result = await controller.clearAutoResolveMemory(allProcesses)
    addActionLog('ai_memory', t('assistantSmartSearchStore.autoMemory.clearedTitle'), String(result.message ?? ''), result.success === false ? 'warning' : 'success')
    return result
  }

  function logAiAudit(event: string, payload: Record<string, unknown>) {
    const controller = backend.getController()
    if (!controller.logAiAudit) return
    void controller.logAiAudit(event, {
      ...payload,
      investigationId: activeInvestigation.value?.id ?? null,
      objective: activeInvestigation.value?.objective ?? searchQuery.value,
      timestamp: new Date().toISOString(),
    }).catch(() => {})
  }

  async function confirmChatMemoryWrite(value: string) {
    const controller = backend.getController()
    if (!controller.confirmChatMemoryWrite) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatWrite.unavailable'), t('assistantSmartSearchStore.errors.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.confirmChatMemoryWrite(value)
      addActionLog('checkpoint', result.success === true ? t('assistantSmartSearchStore.chatWrite.ok') : t('assistantSmartSearchStore.chatWrite.failed'), String(result.message ?? result.error ?? ''), result.success === true ? 'success' : 'error')
      addInvestigationStep({
        title: result.success === true ? t('assistantSmartSearchStore.chatWrite.executed') : t('assistantSmartSearchStore.chatWrite.failed'),
        detail: String(result.message ?? result.error ?? t('assistantSmartSearchStore.common.valueEquals', { value })),
        status: result.success === true ? 'success' : 'error',
        tool: 'confirmChatMemoryWrite',
        risk: 'write',
        payload: result as unknown as Record<string, unknown>,
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatWrite.failed'), String(e), 'error')
      return null
    }
  }

  async function confirmChatMemoryFreeze(value: string) {
    const controller = backend.getController()
    if (!controller.confirmChatMemoryFreeze) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatFreeze.unavailable'), t('assistantSmartSearchStore.errors.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.confirmChatMemoryFreeze(value)
      addActionLog('checkpoint', result.success === true ? t('assistantSmartSearchStore.chatFreeze.ok') : t('assistantSmartSearchStore.chatFreeze.failed'), String(result.message ?? result.error ?? ''), result.success === true ? 'success' : 'error')
      addInvestigationStep({
        title: result.success === true ? t('assistantSmartSearchStore.chatFreeze.executed') : t('assistantSmartSearchStore.chatFreeze.failed'),
        detail: String(result.message ?? result.error ?? t('assistantSmartSearchStore.common.valueEquals', { value })),
        status: result.success === true ? 'success' : 'error',
        tool: 'confirmChatMemoryFreeze',
        risk: 'write',
        payload: result as unknown as Record<string, unknown>,
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatFreeze.failed'), String(e), 'error')
      return null
    }
  }

  async function confirmRewriteLastAutoWrite(value: string) {
    const controller = backend.getController()
    if (!controller.confirmRewriteLastAutoWrite) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatRewrite.unavailable'), t('assistantSmartSearchStore.errors.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.confirmRewriteLastAutoWrite(value)
      addActionLog('checkpoint', result.success === true ? t('assistantSmartSearchStore.chatRewrite.ok') : t('assistantSmartSearchStore.chatRewrite.failed'), String(result.message ?? result.error ?? ''), result.success === true ? 'success' : 'error')
      addInvestigationStep({
        title: result.success === true ? t('assistantSmartSearchStore.chatRewrite.executed') : t('assistantSmartSearchStore.chatRewrite.failed'),
        detail: String(result.message ?? result.error ?? t('assistantSmartSearchStore.common.valueEquals', { value })),
        status: result.success === true ? 'success' : 'error',
        tool: 'confirmRewriteLastAutoWrite',
        risk: 'write',
        payload: result as unknown as Record<string, unknown>,
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('assistantSmartSearchStore.chatRewrite.failed'), String(e), 'error')
      return null
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

  async function acknowledgePendingSmartSearchRecovery() {
    // A appeler quand l'utilisateur repond a une relance de l'echelle de
    // secours (ex. "Tracer le texte affiche") en cliquant le bouton plutot
    // qu'en tapant dans le chat : ces boutons appellent leur propre pipeline
    // frontend (runAutoTraceUiString) sans jamais repasser par
    // startSmartSearch, seul endroit qui consommerait sinon
    // m_pendingRecoveryAction cote backend.
    try {
      await backend.getController().acknowledgePendingSmartSearchRecovery?.()
    } catch (e) {
      console.error('[KillEngine] Failed to acknowledge pending smart search recovery:', e)
    }
  }

  async function clearActiveChatMemoryTargets() {
    try {
      const result = await backend.getController().clearActiveChatMemoryTargets()
      activeChatMemoryTargets.value = []
      await refreshSmartSearchContext()
      pushMessage('assistant', t('assistantSmartSearchStore.memoryTargets.cleared', { count: String(result.cleared ?? 0) }))
      return result
    } catch (e) {
      pushMessage('assistant', t('assistantSmartSearchStore.memoryTargets.clearFailed', { error: String(e) }), { isError: true })
      return { success: false, error: String(e), count: 0, targets: [] }
    }
  }

  function setScanProgress(percent: number) {
    scanningStore.setScanProgress(percent)
  }

  async function refreshCandidates() {
    return scanningStore.refreshCandidates(addAddressToWatch)
  }

  function extractCandidateCount(result: Record<string, unknown>): number | undefined {
    return scanningStore.extractCandidateCount(result)
  }

  async function doEncryptedScan() {
    return scanningStore.doEncryptedScan()
  }

  async function captureUnknownSnapshot() {
    return scanningStore.captureUnknownSnapshot()
  }

  async function runUnknownGuideStep(mode: 'increased' | 'decreased' | 'unchanged' | 'changed') {
    return scanningStore.runUnknownGuideStep(mode)
  }

  function setInvestigationReport(report: InvestigationReport | null) {
    investigationReport.value = report
  }

  function topInvestigationCandidates() {
    const top = investigationReport.value?.numericSources?.top
    return Array.isArray(top) ? top : []
  }

  function promoteEncryptedMatchesToCheckpoints(value: string, matches: Array<Record<string, unknown>>) {
    if (!activeInvestigation.value || matches.length === 0) return
    const scored = matches
      .map((match) => {
        const confidence = Number(match.confidence ?? 0)
        const score = Math.max(35, Math.min(82, Math.round(confidence > 1 ? confidence : confidence * 100) || 55))
        return { match, score }
      })
      .sort((a, b) => b.score - a.score)
    activeInvestigation.value.checkpoints = [
      ...scored.slice(0, 8).map(({ match, score }) => ({
        kind: 'encrypted_hit',
        label: t('assistantSmartSearchStore.checkpoints.encryptedScanLabel', { address: String(match.address ?? '').replace(/^0x/i, '') }),
        address: String(match.address ?? '').replace(/^0x/i, ''),
        type: String(match.type ?? 'Int32'),
        value,
        mode: String(match.encryptedMode ?? match.mode ?? 'auto'),
        key: String(match.key ?? ''),
        confidenceScore: score,
        confidenceLabel: `score ${score}/100`,
        requiresConfirmation: true,
      })),
      ...activeInvestigation.value.checkpoints,
    ].sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0)).slice(0, 12)
    saveInvestigations()
  }

  function promoteUiStringMatchesToCheckpoints(value: string, matches: Array<Record<string, unknown>>) {
    if (!activeInvestigation.value || matches.length === 0) return
    const scored = matches
      .map((match) => {
        const encoding = String(match.encoding ?? '')
        const writableBonus = match.writable === true ? 8 : 0
        const score = Math.min(74, 44 + writableBonus + (encoding === 'utf16' ? 6 : 0))
        return { match, score }
      })
      .sort((a, b) => b.score - a.score)
    activeInvestigation.value.checkpoints = [
      ...scored.slice(0, 8).map(({ match, score }) => ({
        kind: 'ui_string_hit',
        label: `String UI 0x${String(match.address ?? '').replace(/^0x/i, '')}`,
        address: String(match.address ?? '').replace(/^0x/i, ''),
        type: 'String',
        value,
        encoding: String(match.encoding ?? ''),
        byteLength: match.byteLength,
        confidenceScore: score,
        confidenceLabel: `score ${score}/100`,
        requiresConfirmation: false,
        reason: t('assistantSmartSearchStore.checkpoints.uiStringReason'),
      })),
      ...activeInvestigation.value.checkpoints,
    ].sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0)).slice(0, 12)
    saveInvestigations()
  }

  async function analyzeAutoUiStringSourcesFromMatches(value: string, matches: UiStringCandidate[]) {
    const controller = backend.getController()
    if (!controller.analyzeUiStringSources || matches.length === 0) return
    const sourceMap = new Map<string, UiStringSourceCandidate>()
    for (const candidate of matches.slice(0, 5)) {
      const sourceResult = await controller.analyzeUiStringSources(candidate, value, {
        radiusBytes: 1024 * 1024,
        maxResults: 80,
        includeScaled: true,
      })
      if (!sourceResult.success) continue
      for (const source of sourceResult.candidates) {
        const key = `${source.address.replace(/^0x/i, '')}:${source.type}:${source.variantLabel ?? ''}`
        const existing = sourceMap.get(key)
        if (!existing || (source.confidence ?? 0) > (existing.confidence ?? 0)) {
          sourceMap.set(key, source)
        }
      }
    }
    autoUiStringSources.value = Array.from(sourceMap.values())
      .sort((a, b) => Number(b.confidence ?? 0) - Number(a.confidence ?? 0))
      .slice(0, 80)
    autoUiStringSourceResult.value = {
      success: autoUiStringSources.value.length > 0,
      partial: false,
      matchesFound: autoUiStringSources.value.length,
      matchesReturned: autoUiStringSources.value.length,
      bytesScanned: 0,
      error: '',
      candidates: autoUiStringSources.value,
    } as unknown as UiStringSourceResult
    if (autoUiStringSources.value.length > 0 && activeInvestigation.value) {
      activeInvestigation.value.checkpoints = [
        ...autoUiStringSources.value.slice(0, 8).map((source) => {
          const score = Math.min(95, Math.max(50, Math.round(Number(source.confidence ?? 0) * 100) || 65))
          return {
            kind: 'ui_numeric_source',
            label: `Source UI 0x${source.address.replace(/^0x/i, '')}`,
            address: source.address.replace(/^0x/i, ''),
            type: source.type,
            value,
            variantLabel: source.variantLabel,
            confidence: source.confidence,
            confidenceScore: score,
            confidenceLabel: `score ${score}/100`,
            requiresConfirmation: true,
          }
        }),
        ...activeInvestigation.value.checkpoints,
      ].sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0)).slice(0, 12)
      addInvestigationStep({
        title: t('assistantSmartSearchStore.uiSources.autoAnalyzedTitle'),
        detail: t('assistantSmartSearchStore.uiSources.autoAnalyzedDetail', { count: autoUiStringSources.value.length }),
        status: 'checkpoint',
        tool: 'analyzeUiStringSources',
        risk: 'safe',
        payload: autoUiStringSourceResult.value as unknown as Record<string, unknown>,
      })
      saveInvestigations()
    }
  }

  function investigationAssistantReply(query: string): string {
    const report = investigationReport.value
    if (!report) return ''
    const lower = query.toLowerCase()
    const asksInvestigation = /(meilleur|piste|tester|quoi|suivant|enqu[eê]te|rapport|debugger|aob|trainer|bloqu)/i.test(lower)
    if (!asksInvestigation) return ''

    const top = topInvestigationCandidates()
    const best = top[0]
    const lines: string[] = []
    if (best) {
      const address = String(best.address ?? '')
      const type = String(best.variantLabel ?? best.type ?? '')
      const score = Math.round(Number(best.score ?? 0) * 100)
      const reasons = Array.isArray(best.reasons) ? best.reasons.map(String).join(' · ') : ''
      lines.push(t('assistantSmartSearchStore.investigation.bestCandidate', { address, type, score }))
      if (reasons) lines.push(t('assistantSmartSearchStore.investigation.why', { reasons }))
      lines.push(t('assistantSmartSearchStore.investigation.nextActionWatch'))
    } else {
      lines.push(t('assistantSmartSearchStore.investigation.noScoredCandidate'))
    }

    const hitCount = Number(report.debugger?.hitCount ?? 0)
    if (hitCount > 0) {
      lines.push(t('assistantSmartSearchStore.investigation.debuggerHits', { count: hitCount }))
    } else if (report.debugger?.cancelled) {
      lines.push(t('assistantSmartSearchStore.investigation.debuggerCancelled'))
    }

    const matchesFound = Number(report.aob?.matchesFound ?? Number.NaN)
    if (Number.isFinite(matchesFound)) {
      const weakQuality = Number(report.aob?.weakQualityCount ?? 0)
      const blockedTrainer = Number(report.aob?.trainerBlockedCount ?? 0)
      if (weakQuality > 0 || blockedTrainer > 0) {
        lines.push(t('assistantSmartSearchStore.investigation.aobWeak', { weakQuality, blockedTrainer }))
      } else if (matchesFound === 1) lines.push(t('assistantSmartSearchStore.investigation.aobUnique'))
      else if (matchesFound > 1) lines.push(t('assistantSmartSearchStore.investigation.aobMultiple', { count: matchesFound }))
      else lines.push(t('assistantSmartSearchStore.investigation.aobNone'))
    }

    return lines.join('\n')
  }

  async function doSearch() {
    const query = searchQuery.value.trim()
    if (!query || isSearching.value) return

    // Etape 3/4 de l'echelle d'escalade (scan chiffre) : si l'assistant vient
    // de le proposer et que la reponse ressemble a une simple valeur plutot
    // qu'une commande explicite (nouvelle recherche, annuler...), on route
    // directement vers runAutoEncryptedScan au lieu de repartir sur
    // startSmartSearch, qui ne saurait pas quoi faire de ce texte libre.
    if (pendingAssistantAction.value === 'encrypted_scan') {
      pendingAssistantAction.value = ''
      if (!/nouvelle recherche|new search|oublie|forget|annule|cancel|rollback|abandonne|give up|laisse tomber|drop it/i.test(query)) {
        pushMessage('user', query)
        searchQuery.value = ''
        await runAutoEncryptedScan(query)
        return
      }
    }

    // Message utilisateur
    pushMessage('user', query)

    const localInvestigationReply = investigationAssistantReply(query)
    if (localInvestigationReply) {
      pushMessage('assistant', localInvestigationReply, {
        intent: 'InvestigationReport',
        intentRationale: t('assistantSmartSearchStore.intentRationale.investigationDecision'),
      })
      searchQuery.value = ''
      return
    }

    const thinkingMessage = pushMessage('assistant', t('assistantSmartSearchStore.search.thinking'), { isThinking: true })
    searchQuery.value = ''
    isSearching.value = true
    await letChatRenderBeforeBackendWork()

    try {
      const result = await backend.getController().startSmartSearch(query)

      // PHASE 169 : certains tools Trainer (trainer_list_features/
      // trainer_create_write/trainer_delete_feature) ne peuvent plus finir
      // leur travail cote C++ via callVueStoreAction() -- cet appel y
      // rappelle runJavaScript() en reentrance sur CETTE MEME page pendant
      // que ce startSmartSearch() est encore en vol, et le callback JS
      // n'arrive jamais dans les 5s (confirme en direct, PHASE 168/169,
      // docs/PHASE_TRACKER.md). Le C++ pose donc juste un marqueur
      // `needsLocalStoreAction` + les donnees necessaires ; on termine
      // l'action ici, directement dans le meme contexte JS que le store
      // (aucun aller-retour requis), avant que le message ne soit affiche.
      const pendingAction = result.needsLocalStoreAction as string | undefined
      if (pendingAction === 'trainer_list_features') {
        const features = getTrainerFeaturesSnapshot()
        result.workflowStatus = 'trainer_features_listed'
        result.message = features.length === 0
          ? t('assistantSmartSearchStore.trainer.noFeatures')
          : t('assistantSmartSearchStore.trainer.featuresFound', { count: features.length })
      } else if (pendingAction === 'trainer_create_write') {
        const pendingFeature = (result.pendingFeature ?? {}) as Partial<TrainerFeature>
        const created = createTrainerFeature(pendingFeature)
        const address = String(result.pendingAddress ?? pendingFeature.address ?? '')
        const valueType = String(result.pendingValueType ?? pendingFeature.valueType ?? 'Int32')
        const value = String(result.pendingValue ?? pendingFeature.value ?? '')
        const locatorSummary = String(result.locatorSummary ?? '')
        if (!created) {
          result.workflowStatus = 'trainer_feature_create_failed'
          result.message = t('assistantSmartSearchStore.trainer.createFailed')
        } else {
          result.workflowStatus = 'trainer_feature_created'
          result.message = created.locatorKind === 'absolute'
            ? t('assistantSmartSearchStore.trainer.createdAbsolute', { address, valueType, value })
            : t('assistantSmartSearchStore.trainer.createdWithLocator', { address, valueType, value, locatorSummary })
        }
      } else if (pendingAction === 'trainer_delete_feature') {
        const trainerId = Number(result.pendingTrainerId ?? 0)
        deleteTrainerFeature(trainerId)
        result.workflowStatus = 'trainer_feature_deleted'
        result.message = t('assistantSmartSearchStore.trainer.deleteRequested', { id: trainerId })
      }

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
      // `result.rationale` (AIEngine::makeToolCall, moteur IA complet) est plus
      // specifique quand il existe (retry multi-type, pivot chiffre, reduction
      // contextuelle...) que `result.intentRationale` (classifieur simple,
      // toujours present mais plus generique) : on prefere le premier.
      const decisionRationale = result.rationale
        ? String(result.rationale)
        : (result.intentRationale ? String(result.intentRationale) : undefined)
      const extras: Partial<ChatMessage> = {
        workflowStatus: wfStatus || undefined,
        candidateCount,
        targetValue: result.targetValue ? String(result.targetValue) : undefined,
        intent: result.intent ? String(result.intent) : undefined,
        intentRationale: decisionRationale,
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

      pendingAssistantAction.value = typeof result.pendingRecoveryAction === 'string' ? result.pendingRecoveryAction : ''

      if (Array.isArray(result.invalidatedAddresses) && result.invalidatedAddresses.length > 0) {
        markAutoWriteMessagesInvalidated(result.invalidatedAddresses.map((a) => String(a)))
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
        ?? t('assistantSmartSearchStore.search.notEnoughInfo'),
      ).trim()
      updateMessage(
        thinkingMessage.id,
        assistantText || t('assistantSmartSearchStore.search.notEnoughInfo'),
        { ...extras, isThinking: false },
      )
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
    } catch (e) {
      updateMessage(thinkingMessage.id, t('assistantSmartSearchStore.search.error', { error: String(e) }), { isThinking: false, isError: true })
    } finally {
      isSearching.value = false
    }
  }

  async function doAutoResolve() {
    const query = searchQuery.value.trim()
    if (!query || isSearching.value) return

    const controller = backend.getController()
    if (!controller.startAutoResolve) {
      pushMessage('assistant', t('assistantSmartSearchStore.autoResolve.unavailable'), { isError: true })
      return
    }

    pushMessage('user', `Auto: ${query}`)
    startInvestigation(query, t('assistantSmartSearchStore.autoResolve.investigationTitle'))
    addInvestigationStep({
      title: t('assistantSmartSearchStore.autoResolve.userObjective'),
      detail: query,
      status: 'running',
      tool: 'startAutoResolve',
      risk: 'safe',
    })
    const thinkingMessage = pushMessage('assistant', t('assistantSmartSearchStore.autoResolve.thinking'), { isThinking: true })
    searchQuery.value = ''
    isSearching.value = true
    await letChatRenderBeforeBackendWork()

    try {
      if (autoUnknownAwaitingObservation.value || (unknownSnapshotResult.value?.success === true && /^(changed|stable|inchang|augment|diminu|baisse|hausse|plus|moins|-?\d)/i.test(query))) {
        const handledUnknown = await runAutoUnknownObservation(query, thinkingMessage.id)
        if (handledUnknown) return
      }

      const result = await controller.startAutoResolve(query, { executeSafe: true, maxSafeSteps: 3 })
      updateInvestigationFromAutoResult(result as Record<string, unknown>)
      searchResult.value = result.message ?? JSON.stringify(result, null, 2)

      if (result.firstAction) {
        if (String(result.safeAction ?? '') === 'next_scan') {
          nextScanResult.value = result.firstAction as unknown as NextScanResult
        } else {
          exactScanResult.value = result.firstAction as unknown as ExactScanResult
        }
        candidatePageIndex.value = 0
        await refreshCandidates()
      }

      if (result.fallbackAction && typeof result.fallbackAction === 'object') {
        const fallback = result.fallbackAction as Record<string, unknown>
        if ('keySearchBits' in fallback || fallback.mode === 'xor' || fallback.mode === 'auto') {
          encryptedScanResult.value = fallback as unknown as EncryptedScanResult
          promoteEncryptedMatchesToCheckpoints(
            String(result.initialValue ?? query),
            Array.isArray(fallback.matches) ? fallback.matches as Array<Record<string, unknown>> : [],
          )
        }
      }
      if (result.fallbackTraceUiAction && typeof result.fallbackTraceUiAction === 'object') {
        const trace = result.fallbackTraceUiAction as Record<string, unknown>
        autoUiStringScanResult.value = trace as unknown as UiStringScanResult
        const traceMatches = Array.isArray(trace.matches) ? trace.matches as Array<Record<string, unknown>> : []
        promoteUiStringMatchesToCheckpoints(
          String(result.initialValue ?? query),
          traceMatches,
        )
        await analyzeAutoUiStringSourcesFromMatches(
          String(result.initialValue ?? query),
          traceMatches as unknown as UiStringCandidate[],
        )
      }
      if (result.unknownCaptureAction && typeof result.unknownCaptureAction === 'object') {
        unknownSnapshotResult.value = result.unknownCaptureAction as unknown as UnknownSnapshotResult
        autoUnknownAwaitingObservation.value = unknownSnapshotResult.value.success === true
      }

      if (Array.isArray(result.executedSafeSteps)) {
        for (const step of result.executedSafeSteps.slice(0, 5) as Array<Record<string, unknown>>) {
          addInvestigationStep({
            title: t('assistantSmartSearchStore.autoResolve.safeStepTitle', { tool: String(step.tool ?? t('assistantSmartSearchStore.autoResolve.toolFallback')) }),
            detail: String(step.detail ?? step.status ?? ''),
            status: String(step.status ?? '') === 'error' ? 'warning' : 'success',
            tool: String(step.tool ?? 'auto_safe_action'),
            risk: 'safe',
            payload: step,
          })
        }
      } else if (result.firstAction) {
        addInvestigationStep({
          title: String(result.safeAction ?? result.actionStatus ?? t('assistantSmartSearchStore.autoResolve.safeActionExecuted')),
          detail: t('assistantSmartSearchStore.autoResolve.candidateCount', { count: candidatePage.value?.totalCount ?? extractCandidateCount(result) ?? 0 }),
          status: result.success === false ? 'warning' : 'success',
          tool: String(result.safeAction ?? 'auto_safe_action'),
          risk: 'safe',
          payload: result.firstAction as Record<string, unknown>,
        })
      }

      const wfStatus = (result.workflowStatus as string | undefined) ?? ''
      if (wfStatus) workflowStatus.value = wfStatus
      if (result.targetValue) targetValueGuided.value = String(result.targetValue)

      const candidateCount = extractCandidateCount(result)
      if (candidateCount !== undefined && candidateCount >= 0) {
        candidateHistory.value.push(candidateCount)
      }

      const planLines = Array.isArray(result.plan)
        ? result.plan.slice(0, 6).map((step, index) => {
            const row = step as Record<string, unknown>
            return `${index + 1}. ${String(row.description ?? row.type ?? t('assistantSmartSearchStore.autoResolve.stepFallback'))}`
          })
        : []
      const executedSafeSteps = Array.isArray(result.executedSafeSteps)
        ? result.executedSafeSteps.slice(0, 5) as Array<Record<string, unknown>>
        : []

      const nextActions = Array.isArray(result.nextActions)
        ? (result.nextActions as Array<Record<string, unknown>>)
        : []
      const contextReport = typeof result.contextReport === 'object' && result.contextReport !== null
        ? result.contextReport as Record<string, unknown>
        : {}
      const nextBestAction = typeof contextReport.nextBestAction === 'object' && contextReport.nextBestAction !== null
        ? contextReport.nextBestAction as Record<string, unknown>
        : null
      const reportRecommendations = Array.isArray(contextReport.recommendations)
        ? contextReport.recommendations as Array<Record<string, unknown>>
        : []
      const telemetryInsights = Array.isArray(contextReport.telemetryInsights)
        ? contextReport.telemetryInsights as Array<Record<string, unknown>>
        : []
      const displayValueReport = typeof contextReport.displayValueReport === 'object' && contextReport.displayValueReport !== null
        ? contextReport.displayValueReport as Record<string, unknown>
        : {}
      const mergedActions = nextBestAction ? [nextBestAction, ...nextActions] : [...nextActions]
      for (const action of reportRecommendations) {
        const id = String(action.id ?? '')
        if (!id || mergedActions.some((existing) => String(existing.id ?? '') === id)) continue
        mergedActions.push(action)
      }
      const nextBestHint = nextBestAction
        ? t('assistantSmartSearchStore.autoResolve.nextBestHint', { label: String(nextBestAction.label ?? nextBestAction.id), confidence: Number.isFinite(Number(nextBestAction.confidence)) ? ` (${Number(nextBestAction.confidence)}/100)` : '', reason: String(nextBestAction.reason ?? '') })
        : ''
      const reportSummary = String(contextReport.summary ?? '').trim()
      const learnedProfile = typeof contextReport.learnedProfile === 'object' && contextReport.learnedProfile !== null
        ? contextReport.learnedProfile as Record<string, unknown>
        : {}
      const learnedStarts = Number(learnedProfile.starts ?? 0)
      const learnedNoCandidate = Number(learnedProfile.noCandidateCount ?? 0)
      const learnedHint = learnedStarts > 0
        ? t('assistantSmartSearchStore.autoResolve.learnedHint', { starts: learnedStarts, noCandidate: learnedNoCandidate })
        : ''
      const preferredStrategy = typeof contextReport.preferredStrategy === 'object' && contextReport.preferredStrategy !== null
        ? contextReport.preferredStrategy as Record<string, unknown>
        : {}
      const strategyLabel = String(preferredStrategy.label ?? '').trim()
      const strategyScore = Number(preferredStrategy.score ?? Number.NaN)
      const strategyReason = String(preferredStrategy.reason ?? '').trim()
      const strategyHint = strategyLabel
        ? t('assistantSmartSearchStore.autoResolve.strategyHint', { label: strategyLabel, score: Number.isFinite(strategyScore) ? ` (${strategyScore})` : '', reason: strategyReason ? ` — ${strategyReason}` : '' })
        : ''
      const insightHint = telemetryInsights.length > 0
        ? t('assistantSmartSearchStore.autoResolve.telemetryHint', { label: String(telemetryInsights[0].label ?? telemetryInsights[0].id), nextAction: String(telemetryInsights[0].nextAction ?? telemetryInsights[0].reason ?? '') })
        : ''
      const displayValueHint = displayValueReport.enabled === true
        ? t('assistantSmartSearchStore.autoResolve.displayValueHint', { recommendation: String(displayValueReport.recommendation ?? t('assistantSmartSearchStore.autoResolve.displayValueFallback')) })
        : ''

      const baseMessage = String(result.message ?? result.error ?? t('assistantSmartSearchStore.autoResolve.planGenerated')).trim()
      const messageParts = [baseMessage]
      if (nextBestHint) messageParts.push(nextBestHint)
      if (reportSummary) messageParts.push(t('assistantSmartSearchStore.autoResolve.context', { summary: reportSummary }))
      if (learnedHint) messageParts.push(learnedHint)
      if (strategyHint) messageParts.push(strategyHint)
      if (insightHint) messageParts.push(insightHint)
      if (displayValueHint) messageParts.push(displayValueHint)
      if (planLines.length > 0) messageParts.push(t('assistantSmartSearchStore.autoResolve.plan', { plan: planLines.join('\n') }))
      const message = messageParts.join('\n\n')

      updateMessage(thinkingMessage.id, message, {
        isThinking: false,
        isError: Boolean(result.error),
        workflowStatus: wfStatus || undefined,
        candidateCount,
        targetValue: result.targetValue ? String(result.targetValue) : undefined,
        recoveryActions: mergedActions,
        executedSafeSteps,
        intent: 'AutoResolve',
        intentRationale: t('assistantSmartSearchStore.intentRationale.autoResolvePlan'),
      })
      if (result.requiresConfirmation) {
        addInvestigationStep({
          title: t('assistantSmartSearchStore.autoResolve.checkpointConfirmation'),
          detail: String(result.confirmationReason ?? t('assistantSmartSearchStore.autoResolve.confirmationRequired')),
          status: 'checkpoint',
          risk: 'write',
          payload: result as Record<string, unknown>,
        })
      }
      await refreshSmartSearchContext()
    } catch (e) {
      addInvestigationStep({
        title: t('assistantSmartSearchStore.autoResolve.errorTitle'),
        detail: String(e),
        status: 'error',
        tool: 'startAutoResolve',
        risk: 'safe',
      })
      updateMessage(thinkingMessage.id, t('assistantSmartSearchStore.autoResolve.errorMessage', { error: String(e) }), { isThinking: false, isError: true })
    } finally {
      isSearching.value = false
    }
  }

  async function startNewSearchContext() {
    if (isSearching.value) return
    searchQuery.value = t('assistantSmartSearchStore.commands.newSearchEmpty')
    await doSearch()
  }

  async function useSuggestedAddresses(suggestions: Array<Record<string, unknown>>) {
    const addresses = suggestions
      .map((suggestion) => String(suggestion.address ?? '').trim())
      .filter(Boolean)
      .map((address) => address.startsWith('0x') ? address : `0x${address}`)
    if (addresses.length === 0 || isSearching.value) return
    searchQuery.value = t('assistantSmartSearchStore.commands.useMemories', { addresses: addresses.join(' ') })
    await doSearch()
  }

  async function searchValueElsewhere(value: string) {
    const trimmed = value.trim()
    if (!trimmed || isSearching.value) return
    searchQuery.value = t('assistantSmartSearchStore.commands.newSearch', { value: trimmed })
    await doSearch()
  }

  async function searchValueAsType(value: string, type: string, target?: string) {
    const trimmed = value.trim()
    if (!trimmed || isSearching.value) return
    const targetText = target?.trim() ?? ''
    exactScanValue.value = trimmed
    exactScanType.value = type
    searchQuery.value = targetText
      ? t('assistantSmartSearchStore.commands.newSearchTypedTarget', { value: trimmed, type, target: targetText })
      : t('assistantSmartSearchStore.commands.newSearchTyped', { value: trimmed, type })
    await doSearch()
    addActionLog('assistant', t('assistantSmartSearchStore.search.assistantSearchByType', { type }), targetText ? `${trimmed} -> ${targetText}` : trimmed, 'info')
  }

  async function testSingleSuggestedAddress(suggestion: Record<string, unknown>) {
    const address = String(suggestion.address ?? '').trim()
    const type = String(suggestion.type ?? exactScanType.value)
    const value = String(suggestion.value ?? targetValueGuided.value ?? '').trim()
    if (!address || !value || isSearching.value) return

    const normalizedAddress = address.startsWith('0x') ? address.slice(2) : address
    pushMessage('user', t('assistantSmartSearchStore.commands.testOnlyAddress', { address: normalizedAddress, value }))
    try {
      const result = await writeMemoryValueByMode(normalizedAddress, type, value)
      writeResult.value = result
      pushMessage('assistant',
        result.success
          ? t(kernelMemoryModeActive.value ? 'assistantSmartSearchStore.singleWrite.okKernel' : 'assistantSmartSearchStore.singleWrite.ok', { value, address: normalizedAddress })
          : t('assistantSmartSearchStore.singleWrite.failed', { address: normalizedAddress, error: result.error }),
        { isError: !result.success })
    } catch (e) {
      pushMessage('assistant', t('assistantSmartSearchStore.singleWrite.failed', { address: normalizedAddress, error: String(e) }), { isError: true })
    }
  }

  async function runAutoEncryptedScan(value: string) {
    const trimmed = value.trim()
    if (!trimmed || scanBusy.value) return
    exactScanValue.value = trimmed
    exactScanType.value = 'Int32'
    encryptedScanKey.value = '0'
    encryptedScanKeySearchBits.value = 16
    addInvestigationStep({
      title: t('assistantSmartSearchStore.encryptedScan.guideTitle'),
      detail: t('assistantSmartSearchStore.encryptedScan.guideDetail', { value: trimmed }),
      status: 'running',
      tool: 'scanEncryptedValue',
      risk: 'safe',
    })
    const modes: Array<typeof encryptedScanMode.value> = ['xor', 'add', 'sub', 'not']
    const mergedMatches = new Map<string, Record<string, unknown>>()
    let aggregateRegions = 0
    let aggregateBytes = 0
    let firstError = ''
    for (const mode of modes) {
      encryptedScanMode.value = mode
      await doEncryptedScan()
      const result = encryptedScanResult.value
      if (!result) continue
      aggregateRegions = Math.max(aggregateRegions, result.regionsScanned)
      aggregateBytes += result.bytesScanned
      if (!result.success && !firstError) firstError = result.error
      for (const match of result.matches ?? []) {
        const key = `${match.address}:${match.type}:${mode}:${String((match as Record<string, unknown>).key ?? result.key ?? '')}`
        mergedMatches.set(key, { ...match, encryptedMode: mode, key: result.key, keySearchBits: result.keySearchBits })
      }
      if (mergedMatches.size >= 500) break
    }
    const matches = Array.from(mergedMatches.values()).slice(0, 500)
    encryptedScanResult.value = {
      success: matches.length > 0,
      partial: mergedMatches.size >= 500,
      regionsScanned: aggregateRegions,
      bytesScanned: aggregateBytes,
      matchesFound: mergedMatches.size,
      matchesReturned: matches.length,
      maxResults: 500,
      error: firstError,
      matches: matches as unknown as EncryptedScanResult['matches'],
      mode: 'auto',
      key: '0',
      keySearchBits: 16,
    }
    promoteEncryptedMatchesToCheckpoints(trimmed, matches)
    addInvestigationStep({
      title: t('assistantSmartSearchStore.encryptedScan.finishedTitle'),
      detail: t('assistantSmartSearchStore.encryptedScan.finishedDetail', { matchesFound: encryptedScanResult.value?.matchesFound ?? 0, matchesReturned: encryptedScanResult.value?.matchesReturned ?? 0 }),
      status: encryptedScanResult.value?.success ? 'success' : 'warning',
      tool: 'scanEncryptedValue',
      risk: 'safe',
      payload: encryptedScanResult.value as unknown as Record<string, unknown>,
    })
    pushMessage(
      'assistant',
      encryptedScanResult.value?.success
        ? t('assistantSmartSearchStore.encryptedScan.messageOk', { count: encryptedScanResult.value.matchesFound })
        : t('assistantSmartSearchStore.encryptedScan.messageNoResult', { error: encryptedScanResult.value?.error ?? t('assistantSmartSearchStore.common.noMatch') }),
      { isError: encryptedScanResult.value?.success === false },
    )
  }

  async function runAutoTraceUiString(value: string) {
    const trimmed = value.trim()
    if (!trimmed || scanBusy.value) return
    const controller = backend.getController()
    if (!controller.scanUiStrings) {
      pushMessage('assistant', t('assistantSmartSearchStore.traceUi.unavailable'), { isError: true })
      return
    }
    scanBusy.value = true
    addInvestigationStep({
      title: t('assistantSmartSearchStore.traceUi.guideTitle'),
      detail: t('assistantSmartSearchStore.traceUi.guideDetail', { value: trimmed }),
      status: 'running',
      tool: 'scanUiStrings',
      risk: 'safe',
    })
    try {
      const previousStrings = autoUiStringScanResult.value?.matches ?? []
      if (previousStrings.length > 0 && controller.trackUiStringCandidates) {
        const tracked = await controller.trackUiStringCandidates(previousStrings.slice(0, 100), trimmed)
        autoUiStringScanResult.value = {
          success: tracked.success,
          matchesFound: tracked.remaining,
          matchesReturned: tracked.survivors.length,
          regionsScanned: 0,
          bytesScanned: 0,
          partial: false,
          error: tracked.error,
          matches: tracked.survivors,
        }
        addInvestigationStep({
          title: t('assistantSmartSearchStore.traceUi.trackingTitle'),
          detail: t('assistantSmartSearchStore.traceUi.trackingDetail', { count: tracked.survivors.length, value: trimmed }),
          status: tracked.success ? 'success' : 'warning',
          tool: 'trackUiStringCandidates',
          risk: 'safe',
          payload: tracked as unknown as Record<string, unknown>,
        })
      } else {
        autoUiStringScanResult.value = await controller.scanUiStrings(trimmed, {
          writableOnly: false,
          maxResults: 500,
        })
      }
      const sourceMap = new Map<string, UiStringSourceCandidate>()
      let sourcePartial = false
      let sourceError = ''
      if (autoUiStringScanResult.value.success && controller.analyzeUiStringSources) {
        const stringCandidates = autoUiStringScanResult.value.matches.slice(0, 5) as UiStringCandidate[]
        for (const candidate of stringCandidates) {
          const sourceResult = await controller.analyzeUiStringSources(candidate, trimmed, {
            radiusBytes: 4 * 1024 * 1024,
            maxResults: 250,
            alignment: 1,
          })
          sourcePartial = sourcePartial || Boolean(sourceResult.partial)
          if (!sourceResult.success && !sourceError) sourceError = sourceResult.error
          for (const source of sourceResult.candidates) {
            const key = `${source.address.replace(/^0x/i, '')}:${source.type}:${source.variantLabel ?? ''}`
            const existing = sourceMap.get(key)
            if (!existing || (source.confidence ?? 0) > (existing.confidence ?? 0)) {
              sourceMap.set(key, source)
            }
          }
        }
      }
      autoUiStringSources.value = Array.from(sourceMap.values())
        .sort((a, b) => (b.confidence ?? 0) - (a.confidence ?? 0))
      autoUiStringSourceResult.value = {
        success: autoUiStringSources.value.length > 0,
        partial: sourcePartial,
        matchesFound: autoUiStringSources.value.length,
        matchesReturned: autoUiStringSources.value.length,
        bytesScanned: 0,
        radiusBytes: 4 * 1024 * 1024,
        error: sourceError,
        candidates: autoUiStringSources.value,
      }
      addInvestigationStep({
        title: t('assistantSmartSearchStore.traceUi.finishedTitle'),
        detail: t('assistantSmartSearchStore.traceUi.finishedDetail', { strings: autoUiStringScanResult.value.matchesFound, sources: autoUiStringSources.value.length }),
        status: autoUiStringScanResult.value.success ? 'success' : 'warning',
        tool: 'scanUiStrings',
        risk: 'safe',
        payload: autoUiStringScanResult.value as unknown as Record<string, unknown>,
      })
      if (autoUiStringSources.value.length > 0 && activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...autoUiStringSources.value.slice(0, 8).map((source) => ({
            kind: 'ui_numeric_source',
            label: `Source UI 0x${source.address.replace(/^0x/i, '')}`,
            address: source.address.replace(/^0x/i, ''),
            type: source.type,
            value: trimmed,
            variantLabel: source.variantLabel,
            confidence: source.confidence,
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        activeInvestigation.value.hypotheses = [
          {
            id: 'trace_ui_sources',
            label: t('assistantSmartSearchStore.traceUi.sourcesHypothesisLabel'),
            reason: t('assistantSmartSearchStore.traceUi.sourcesHypothesisReason', { count: autoUiStringSources.value.length }),
            safe: false,
            requiresConfirmation: true,
          },
          ...activeInvestigation.value.hypotheses,
        ].slice(0, 8)
        saveInvestigations()
      }
      pushMessage(
        'assistant',
        autoUiStringScanResult.value.success
          ? t('assistantSmartSearchStore.traceUi.messageOk', { strings: autoUiStringScanResult.value.matchesFound, sources: autoUiStringSources.value.length })
          : t('assistantSmartSearchStore.traceUi.messageNoResult', { error: autoUiStringScanResult.value.error || t('assistantSmartSearchStore.common.noMatch') }),
        { isError: autoUiStringScanResult.value.success === false },
      )
    } catch (e) {
      autoUiStringScanResult.value = {
        success: false,
        matchesFound: 0,
        matchesReturned: 0,
        regionsScanned: 0,
        bytesScanned: 0,
        error: String(e),
        matches: [],
      }
      autoUiStringSourceResult.value = {
        success: false,
        matchesFound: 0,
        matchesReturned: 0,
        bytesScanned: 0,
        error: String(e),
        candidates: [],
      }
      autoUiStringSources.value = []
      addInvestigationStep({
        title: t('assistantSmartSearchStore.traceUi.failedTitle'),
        detail: String(e),
        status: 'error',
        tool: 'scanUiStrings',
        risk: 'safe',
      })
    } finally {
      scanBusy.value = false
    }
  }

  async function runAutoUnknownObservation(observation: string, thinkingMessageId?: number) {
    const trimmed = observation.trim()
    if (!trimmed) return false

    if (!unknownSnapshotResult.value?.success) {
      addInvestigationStep({
        title: t('assistantSmartSearchStore.unknown.captureTitle'),
        detail: t('assistantSmartSearchStore.unknown.captureDetail'),
        status: 'running',
        tool: 'captureUnknownSnapshotAsync',
        risk: 'safe',
      })
      await captureUnknownSnapshot()
      const message = unknownSnapshotResult.value?.success
        ? t('assistantSmartSearchStore.unknown.captureOk')
        : t('assistantSmartSearchStore.unknown.captureFailed', { error: unknownSnapshotResult.value?.error ?? t('assistantSmartSearchStore.common.unknownError') })
      if (thinkingMessageId !== undefined) {
        updateMessage(thinkingMessageId, message, { isThinking: false, isError: unknownSnapshotResult.value?.success !== true })
      } else {
        pushMessage('assistant', message, { isError: unknownSnapshotResult.value?.success !== true })
      }
      return true
    }

    const mode = inferUnknownModeFromObservation(trimmed)
    addInvestigationStep({
      title: t('assistantSmartSearchStore.unknown.compareTitle'),
      detail: t('assistantSmartSearchStore.unknown.compareDetail', { observation: trimmed, mode }),
      status: 'running',
      tool: 'unknownNextScanAsync',
      risk: 'safe',
    })
    await runUnknownGuideStep(mode)

    const total = candidatePage.value?.totalCount ?? unknownNextScanResult.value?.stored ?? 0
    const candidates = candidatePage.value?.candidates ?? []
    if (total > 0 && total <= 25 && activeInvestigation.value) {
      activeInvestigation.value.checkpoints = [
        ...candidates.slice(0, 10).map((candidate) => ({
          kind: 'unknown_candidate',
          label: `Unknown 0x${candidate.address}`,
          address: candidate.address,
          type: String(candidate.type ?? unknownScanType.value),
          value: firstNumberFromText(trimmed) !== null ? String(firstNumberFromText(trimmed)) : trimmed,
          confidence: candidate.confidence,
          variantLabel: candidate.variantLabel,
          requiresConfirmation: true,
        })),
        ...activeInvestigation.value.checkpoints,
      ].slice(0, 12)
      saveInvestigations()
    }

    const status = unknownNextScanResult.value?.error ? 'error' : total <= 25 && total > 0 ? 'checkpoint' : 'success'
    addInvestigationStep({
      title: t('assistantSmartSearchStore.unknown.finishedTitle'),
      detail: total > 0
        ? t(total <= 25 ? 'assistantSmartSearchStore.unknown.finishedDetailCheckpoint' : 'assistantSmartSearchStore.unknown.finishedDetailContinue', { count: total, mode })
        : t('assistantSmartSearchStore.unknown.noCandidateAfter', { mode }),
      status,
      tool: 'unknownNextScanAsync',
      risk: 'safe',
      payload: unknownNextScanResult.value as unknown as Record<string, unknown>,
    })
    logAiAudit('unknown_auto_observation', {
      observation: trimmed,
      mode,
      candidateCount: total,
      success: !unknownNextScanResult.value?.error,
    })

    const message = total > 0
      ? t(total <= 25 ? 'assistantSmartSearchStore.unknown.messageCheckpoint' : 'assistantSmartSearchStore.unknown.messageContinue', { count: total, mode })
      : t('assistantSmartSearchStore.unknown.messageNoCandidate', { mode })
    if (thinkingMessageId !== undefined) {
      updateMessage(thinkingMessageId, message, { isThinking: false, isError: Boolean(unknownNextScanResult.value?.error), candidateCount: total })
    } else {
      pushMessage('assistant', message, { isError: Boolean(unknownNextScanResult.value?.error), candidateCount: total })
    }
    await refreshSmartSearchContext()
    return true
  }

  function keepCandidate(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    keptCandidateAddresses.value = Array.from(new Set([...keptCandidateAddresses.value, normalized]))
    ignoredCandidateAddresses.value = ignoredCandidateAddresses.value.filter((item) => item !== normalized)
    candidateFilter.value = normalized
    candidatePageIndex.value = 0
    void refreshCandidates()
    addActionLog('candidate', t('assistantSmartSearchStore.candidate.keptTitle', { address: normalized }), t('assistantSmartSearchStore.candidate.filterApplied'), 'success')
  }

  function ignoreCandidate(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    ignoredCandidateAddresses.value = Array.from(new Set([...ignoredCandidateAddresses.value, normalized]))
    keptCandidateAddresses.value = keptCandidateAddresses.value.filter((item) => item !== normalized)
    addActionLog('candidate', t('assistantSmartSearchStore.candidate.ignoredTitle', { address: normalized }), t('assistantSmartSearchStore.candidate.hiddenLocal'), 'warning')
  }

  // Slug stable (non traduit) utilisé comme classe CSS par CandidatePanel.vue
  // (.candidate-kept/.candidate-ignored/...) -- séparé du libellé affiché pour
  // que le style ne dépende pas de la langue active.
  function candidateVisualState(candidate: Record<string, unknown>): string {
    const address = String(candidate.address ?? '').replace(/^0x/i, '')
    if (keptCandidateAddresses.value.includes(address)) return 'kept'
    if (ignoredCandidateAddresses.value.includes(address)) return 'ignored'
    const confidence = Number(candidate.confidence ?? Number.NaN)
    if (Number.isFinite(confidence)) {
      if (confidence >= 0.8) return 'very-likely'
      if (confidence >= 0.5) return 'to-verify'
      return 'weak'
    }
    return 'standard'
  }

  function candidateVisualStateLabel(candidate: Record<string, unknown>): string {
    const state = candidateVisualState(candidate)
    if (state === 'kept') return t('assistantSmartSearchStore.candidate.stateKept')
    if (state === 'ignored') return t('assistantSmartSearchStore.candidate.stateIgnored')
    if (state === 'very-likely') return t('assistantSmartSearchStore.candidate.stateVeryLikely')
    if (state === 'to-verify') return t('assistantSmartSearchStore.candidate.stateToVerify')
    if (state === 'weak') return t('assistantSmartSearchStore.candidate.stateWeak')
    return t('assistantSmartSearchStore.candidate.stateStandard')
  }

  return {
    pendingAssistantAction,
    workflowPresets,
    lastWorkflowPresetId,
    activeChatMemoryTargets,
    smartSearchContext,
    investigationReport,
    searchQuery,
    searchResult,
    autoUiStringScanResult,
    autoUiStringSourceResult,
    autoUiStringSources,
    finalCandidateTargets,
    ignoredCandidateAddresses,
    keptCandidateAddresses,
    messages,
    workflowStatus,
    targetValueGuided,
    isSearching,
    configureAssistantSmartSearchContext,
    pushMessage,
    updateMessage,
    resetWorkflow,
    tellNewValue,
    doGuidedChange,
    applyWorkflowPreset,
    clearAutoResolveMemory,
    logAiAudit,
    confirmChatMemoryWrite,
    confirmChatMemoryFreeze,
    confirmRewriteLastAutoWrite,
    refreshActiveChatMemoryTargets,
    refreshSmartSearchContext,
    acknowledgePendingSmartSearchRecovery,
    clearActiveChatMemoryTargets,
    setInvestigationReport,
    doSearch,
    doAutoResolve,
    startNewSearchContext,
    useSuggestedAddresses,
    searchValueElsewhere,
    searchValueAsType,
    testSingleSuggestedAddress,
    runAutoEncryptedScan,
    runAutoTraceUiString,
    runAutoUnknownObservation,
    keepCandidate,
    ignoreCandidate,
    candidateVisualState,
    candidateVisualStateLabel,
  }
})
