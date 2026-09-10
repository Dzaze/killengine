import { defineStore, storeToRefs } from 'pinia'
import { ref, computed } from 'vue'
import { i18n } from '@/i18n'
import {
  backend,
  type AutoResolveReportResult,
  type ModuleCatalogItem,
  type CandidateFieldTestResult,
  type ClrPathWriteOperation,
  type LuaScriptRunResult,
  type LuaScriptingStatus,
  type LogTailResult,
  type MemoryMapResult,
  type MemoryReadPreview,
  type MemoryWriteResult,
  type ProcessInfo,
  type ProcessModuleInfo,
  type ProcessLocalSettingsResult,
  type ProcessSaveFileDiscoveryResult,
  type ProcessSaveFileInfo,
  type SaveFileSnapshotDiffResult,
  type ProcessSaveFileTextResult,
  type SaveFilePatchResult,
  type SaveFileWatchResult,
  type SmartSearchDebugEventsResult,
  type TemporaryStorageStatus,
} from '@/services/backend'
import { useInvestigationStore, type InvestigationRun, type InvestigationStep } from './investigation'
import { useActionLogStore, type UserActionLogEntry } from './actionLog'
import { useClrInspectorStore } from './clrInspector'
import { useSpeedhackStore } from './speedhack'
import { useNetworkStore } from './network'
import { useAutomationPipeStore } from './automationPipe'
import { useKernelDriverStore } from './kernelDriver'
import { useRiskGateStore, type RiskDialogState } from './riskGate'
import { useSettingsStore } from './settings'
import { useScanningStore } from './scanning'
import { useWriteFreezeStore, type RuntimeActionPlan, type RuntimeActionPlanItem } from './writeFreeze'
import { useTrainerStore, type TrainerFeature } from './trainer'
import { useWorkspaceSessionStore, type WorkspaceProject, type WatchedPointerChain } from './workspaceSession'
import { useAssistantSmartSearchStore, type ChatMessage, type InvestigationReport, type WorkflowPreset } from './assistantSmartSearch'
import {
  useWorkspaceItemsStore,
  type StructureTemplateField,
  type StructureTemplate,
  type WorkspaceBookmark,
} from './workspaceItems'

export type { StructureTemplateField, StructureTemplate, WorkspaceBookmark }

export type { RiskDialogState }
export type { ChatMessage, InvestigationReport, WorkflowPreset }

export type { InvestigationRun, InvestigationStep }
export type { UserActionLogEntry }
export type { RuntimeActionPlan, RuntimeActionPlanItem }
export type { TrainerFeature }
export type { WorkspaceProject, WatchedPointerChain }

export interface SessionEntry {
  id: string
  address: string
  valueType: string
  label: string
  kind: 'freeze_polling' | 'freeze_breakpoint' | 'write'
  enabled: boolean
  createdAt: string
}

export interface SessionGroup {
  id: string
  name: string
  memberIds: string[]
}

export interface SessionPromotionResult {
  success: boolean
  featureIds: number[]
  message: string
  warnings: string[]
}

export type AppView = 'assistant' | 'investigation' | 'trainer' | 'process' | 'memory' | 'memory-timeline' | 'memory-heatmap' | 'pattern-learning' | 'clr' | 'webview2' | 'scripting' | 'speedhack' | 'network' | 'profiles' | 'expert' | 'lexicon' | 'modules' | 'settings'

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

const { t } = i18n.global

export const useAppStore = defineStore('app', () => {
  // State
  const activeView = ref<AppView>('process')
  // PHASE 120-B : etape Expert demandee par un bouton de recommandation
  // chat (recoveryActions), consommee une seule fois par ExpertView au
  // montage puis remise a null -- ne change jamais activeStep pour une
  // visite normale/manuelle d'Expert (design existant delibere : un
  // changement d'onglet subi est pire qu'un onglet a cliquer).
  const pendingExpertStep = ref<'find' | 'inspect' | 'act' | 'persist' | null>(null)
  // PHASE 120-B : id DOM optionnel a scroller au montage d'Expert, en plus du
  // filtre d'etape ci-dessus -- necessaire quand la bonne section est loin
  // dans une longue etape (ex. "Ecrit par" est au milieu d'un long panneau
  // "find"), meme regle de consommation unique que pendingExpertStep.
  const pendingExpertAnchor = ref<string | null>(null)
  const version = ref('...')
  const isConnected = ref(false)
  const showOnboarding = ref(false)
  const isAttached = ref(false)
  const processName = ref('')
  const processes = ref<ProcessInfo[]>([])
  const processModules = ref<ProcessModuleInfo[]>([])
  const discoveredSaveFiles = ref<ProcessSaveFileInfo[]>([])
  const discoveredSaveFilesFamilyName = ref('')
  const saveFileDiscoveryResult = ref<ProcessSaveFileDiscoveryResult | null>(null)
  const selectedSaveFileText = ref<ProcessSaveFileTextResult | null>(null)
  const selectedSaveFilePath = ref('')
  const saveFilesBusy = ref(false)
  const saveFileTextBusy = ref(false)
  // UWP-STATE-1 : snapshot avant/après pour isoler quel fichier change quand
  // une valeur affichée change (ex: gagner de l'XP), cf.
  // docs/UWP_STATE_INSPECTOR_SPEC.md. Réutilise discoverProcessSaveFiles
  // (déjà existant, PHASE 90/91) deux fois plutôt que de le refaire.
  const saveFileSnapshotBefore = ref<ProcessSaveFileInfo[]>([])
  const saveFileSnapshotAfter = ref<ProcessSaveFileInfo[]>([])
  const saveFileSnapshotDiff = ref<SaveFileSnapshotDiffResult | null>(null)
  const saveFileSnapshotBusy = ref(false)
  const localSettingsResult = ref<ProcessLocalSettingsResult | null>(null)
  const localSettingsBusy = ref(false)
  const saveFileWatchResult = ref<SaveFileWatchResult | null>(null)
  const saveFileWatchBusy = ref(false)
  const saveFileWatchPath = ref('')
  const saveFilePatchResult = ref<SaveFilePatchResult | null>(null)
  const saveFilePatchBusy = ref(false)
  const saveFilePatchFindHex = ref('')
  const saveFilePatchReplaceHex = ref('')
  const memoryMap = ref<MemoryMapResult | null>(null)
  const memoryPreview = ref<MemoryReadPreview | null>(null)
  const memoryPreviewAddress = ref('')
  const memoryPreviewLoading = ref(false)
  const memoryAccessMode = ref<'standard' | 'kernel'>('standard')
  const memoryPreviewAscii = computed(() => bytesToAscii(hexToBytes(memoryPreview.value?.hex ?? '')))
  const memoryPreviewDecoded = computed(() => decodePreviewValues(hexToBytes(memoryPreview.value?.hex ?? '')))
  const selectedMemoryRegion = ref<Record<string, unknown> | null>(null)
  // Visualiseur hexadécimal navigable (pagination sur une plage large, distinct de l'aperçu compact 64 o).
  const hexViewerOpen = ref(false)
  const hexViewerRootAddress = ref('')
  const hexViewerAddress = ref('')
  const hexViewerPageSize = ref(512)
  const hexViewerData = ref<MemoryReadPreview | null>(null)
  const hexViewerLoading = ref(false)
  const hexViewerRows = computed(() => {
    const bytes = hexToBytes(hexViewerData.value?.hex ?? '')
    const baseAddress = Number.parseInt(hexViewerAddress.value || '0', 16)
    const rows: Array<{ address: string; bytes: string[]; ascii: string }> = []
    for (let offset = 0; offset < bytes.length; offset += 16) {
      const rowBytes = bytes.slice(offset, offset + 16)
      rows.push({
        address: (baseAddress + offset).toString(16).toUpperCase().padStart(12, '0'),
        bytes: rowBytes.map((byte) => byte.toString(16).toUpperCase().padStart(2, '0')),
        ascii: bytesToAscii(rowBytes),
      })
    }
    return rows
  })
  const pingResult = ref('')
  const logFilePath = ref('')
  const logLines = ref<string[]>([])
  const logError = ref('')
  const diagnosticExportPath = ref('')
  const diagnosticExportError = ref('')
  const diagnosticFolderOpened = ref(false)
  const diagnosticOpenFolderError = ref('')
  const temporaryStorageStatus = ref<TemporaryStorageStatus | null>(null)
  const temporaryStorageCleanupResult = ref<Record<string, unknown> | null>(null)
  const temporaryStorageError = ref('')
  const smartSearchDebugFilePath = ref('')
  const scanTelemetryFilePath = ref('')
  const smartSearchDebugEvents = ref<Array<Record<string, unknown>>>([])
  const smartSearchDebugError = ref('')
  const autoResolveReport = ref<AutoResolveReportResult | null>(null)
  // Memoire de pattern structuree par jeu (module+offset relatif, roadmap H.2) --
  // distincte de learnedProfile ci-dessus qui n'a qu'un seul "dernier succes" ecrase.
  const rememberedPatterns = ref<Array<Record<string, unknown>>>([])
  const settingsStore = useSettingsStore()
  const {
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
    settingModelEnabled,
    settingModelThreads,
    aiModelStatus,
    aiModelStatusLoading,
    aiModelStatusError,
  } = storeToRefs(settingsStore)
  // Store Driver kernel extrait (candidat S4, docs/REFACTOR_ROADMAP.md, 29/08/2026).
  const kernelDriverStore = useKernelDriverStore()
  const {
    kernelDriverStatus,
    kernelDriverStatusLoading,
    kernelDriverStartLoading,
    kernelDriverStatusError,
    kernelMemoryReadResult,
    kernelMemoryReadBusy,
    kernelMemoryWriteResult,
    kernelMemoryWriteBusy,
  } = storeToRefs(kernelDriverStore)
  const kernelMemoryReady = computed(() => kernelDriverStatus.value?.capabilities.processMemoryAccess === true)
  const kernelMemoryModeActive = computed(() => memoryAccessMode.value === 'kernel')
  // Store CLR Inspector extrait (candidat S1, docs/REFACTOR_ROADMAP.md, 29/08/2026) --
  // meme patron que investigationStore/actionLogStore : refs directement
  // mutables, fonctions ci-dessous en wrappers minces. Les 7 fonctions a
  // risque (write..., callClrInstanceMethod) gardent leur confirmRiskAction ICI
  // (pas encore extrait, le store n'y a pas acces) avant de deleguer.
  const clrInspectorStore = useClrInspectorStore()
  const {
    clrInspectorStatus,
    clrInspectorBusy,
    clrInspectorError,
    clrTypeFilter,
    clrObjects,
    clrSelectedObject,
    clrRoots,
    clrLastResult,
    clrFieldLocatorResult,
    clrCallMethodResult,
    clrGcRootPathResult,
    clrDisassembleResult,
    clrObjectReportResult,
  } = storeToRefs(clrInspectorStore)
  // Store Speedhack/API-hook/blocage réseau extrait (candidat S2,
  // docs/REFACTOR_ROADMAP.md, 29/08/2026) -- même patron que
  // clrInspectorStore : `speedhackFactor` reste l'état local du slider
  // (curseur en cours de manipulation) tandis que `speedhackStatus.factor`
  // reflète la dernière valeur confirmée côté backend.
  const speedhackStore = useSpeedhackStore()
  const {
    speedhackStatus,
    speedhackBusy,
    speedhackFactor,
    networkBlockStatus,
    networkBlockBusy,
    apiHookStatus,
    apiHookBusy,
    apiHookModuleName,
    apiHookFunctionName,
    apiHookMode,
    apiHookForcedReturn,
  } = storeToRefs(speedhackStore)

  // Store Réseau (connexions, DLL, proxy HTTP, spoof DNS, lag switch)
  const networkStore = useNetworkStore()
  const networkConnections = networkStore.networkConnections
  const networkModules = networkStore.networkModules
  const networkConnectionsBusy = networkStore.networkConnectionsBusy
  const networkModulesBusy = networkStore.networkModulesBusy
  const networkLastRefresh = networkStore.networkLastRefresh
  const liveRefreshEnabled = networkStore.liveRefreshEnabled
  const httpProxyActive = networkStore.httpProxyActive
  const httpProxyBusy = networkStore.httpProxyBusy
  const httpProxyPort = networkStore.httpProxyPort
  const httpProxyInterceptHttps = networkStore.httpProxyInterceptHttps
  const httpProxyRequests = networkStore.httpProxyRequests
  const selectedHttpRequest = networkStore.selectedHttpRequest
  const httpRequestBodyEditor = networkStore.httpRequestBodyEditor
  const dnsSpoofEntries = networkStore.dnsSpoofEntries
  const dnsSpoofBusy = networkStore.dnsSpoofBusy
  const dnsSpoofDomain = networkStore.dnsSpoofDomain
  const dnsSpoofTargetIp = networkStore.dnsSpoofTargetIp
  const lagSwitchActive = networkStore.lagSwitchActive
  const lagSwitchBusy = networkStore.lagSwitchBusy
  const lagSwitchDelayMs = networkStore.lagSwitchDelayMs
  const _networkFilterProtocol = networkStore._networkFilterProtocol
  const _networkFilterState = networkStore._networkFilterState
  const _networkFilterIp = networkStore._networkFilterIp

  const luaScriptingStatus = ref<LuaScriptingStatus | null>(null)
  const luaScriptText = ref([
    'local ke = require("killengine")',
    '',
    'print("KillEngine Lua ready")',
    'print(ke.call("ping", { "hello from lua" }))',
  ].join('\n'))
  const luaScriptResult = ref<LuaScriptRunResult | null>(null)
  const luaScriptBusy = ref(false)
  const luaScriptTimeoutMs = ref(10000)
  const luaScriptRequestId = ref<number | null>(null)
  const luaSavedScripts = ref<Array<Record<string, unknown>>>([])
  const luaSavedScriptsBusy = ref(false)
  const luaScriptSaveName = ref('')
  const luaScriptSaveResult = ref<Record<string, unknown> | null>(null)
  // PROPOSITIONS-1 #4 — Live Lua REPL : process lua.exe persistant (contrairement
  // à luaScriptText ci-dessus, un script complet relancé à chaque exécution).
  // Une seule confirmation RiskGate au démarrage de la session (pas par ligne
  // envoyée ensuite) — même logique que chatOrigin/chat_memory_write (une
  // confirmation d'entrée dans un canal, pas une par action à l'intérieur).
  interface LuaReplHistoryEntry {
    requestId: number
    line: string
    output: string
    error: string
    finished: boolean
    elapsedMs?: number
  }
  const luaReplActive = ref(false)
  const luaReplBusy = ref(false)
  const luaReplInput = ref('')
  const luaReplHistory = ref<LuaReplHistoryEntry[]>([])
  const luaReplCompletions = ref<string[]>([])
  const luaReplRecall = ref<string[]>([]) // lignes tapees, pour navigation haut/bas (distinct de l'historique execute)
  const luaReplRecallIndex = ref<number | null>(null)
  const luaReplStartResult = ref<Record<string, unknown> | null>(null)
  // Store Investigation extrait (candidat S6, docs/REFACTOR_ROADMAP.md, 29/08/2026) --
  // activeInvestigation/investigationArchive restent des refs directement mutables
  // (storeToRefs), les fonctions sont ci-dessous des wrappers minces qui gardent les
  // memes noms/signatures qu'avant partout ou c'est appele (35+ sites internes +
  // InvestigationView/SettingsView/TrainerView.vue).
  const investigationStore = useInvestigationStore()
  const { activeInvestigation, investigationArchive } = storeToRefs(investigationStore)
  // Store Trainer Features extrait (candidat S8, docs/REFACTOR_ROADMAP.md,
  // PHASE 230, 30/08/2026) -- meme patron que writeFreeze.ts (S7, PHASE 229) :
  // refs mutables via storeToRefs, wrappers minces, dependances transversales
  // (Process/RiskGate/mode kernel) injectees une seule fois. Voir l'en-tete
  // de trainer.ts pour le detail des imports directs vers clrInspector.ts/
  // scanning.ts/writeFreeze.ts/workspaceItems.ts (feuilles independantes).
  const trainerStore = useTrainerStore()
  const {
    trainerFeatures,
    trainerBusy,
    trainerHotkeyStatus,
    trainerOverlayVisible,
    trainerOverlayStatus,
    trainerOverlayHotkey,
    trainerOverlayHotkeyId,
  } = storeToRefs(trainerStore)
  const {
    loadTrainerFeatures,
    loadOverlayHotkey,
    createTrainerFeature,
    createTrainerClrFieldFeature,
    createTrainerFeatureFromCheckpoint,
    createTrainerFeatureFromBookmark,
    applyTrainerFeature,
    restoreTrainerFeature,
    generateTrainerFeaturePointerChain,
    saveTrainerFeatureToProfile,
    applyAllTrainerFeatures,
    restoreAllTrainerFeatures,
    getTrainerFeaturesSnapshot,
    deleteTrainerFeature,
    updateTrainerFeatureDependencies,
    clearTrainerFeatures,
    exportTrainerFeaturesJson,
    exportTrainerFeaturesMarkdown,
    registerTrainerFeatureHotkey,
    unregisterTrainerFeatureHotkey,
    registerOverlayHotkey,
    unregisterOverlayHotkey,
    reregisterPersistedHotkeys,
    refreshTrainerOverlay,
    setTrainerOverlay,
    handleGlobalHotkey,
  } = trainerStore
  trainerStore.configureTrainerContext({
    processName,
    confirmRiskAction,
    kernelMemoryModeActive,
    writeMemoryValueByMode,
  })
  const workspaceItemsStore = useWorkspaceItemsStore()
  const {
    structureTemplates,
    workspaceBookmarks,
  } = storeToRefs(workspaceItemsStore)
  // Store RiskGate extrait (dernière fondation partagée, docs/REFACTOR_ROADMAP.md,
  // 29/08/2026) -- confirmRiskAction() ci-dessous délègue au store en
  // injectant logAiAudit en callback (le store ne connaît pas
  // activeInvestigation/searchQuery directement).
  const riskGateStore = useRiskGateStore()
  const { riskDialog } = storeToRefs(riskGateStore)
  // Store Scanning/Candidates extrait (candidat S11a, docs/REFACTOR_ROADMAP.md,
  // PHASE 223, 30/08/2026) -- refs directement mutables via storeToRefs, memes
  // noms qu'avant. Les fonctions qui traversent vers d'autres domaines
  // (write/freeze, chat/IA) restent ci-dessous et continuent de lire/ecrire
  // ces memes refs partagees sans changement.
  const scanningStore = useScanningStore()
  const {
    exactScanValue,
    exactScanType,
    exactScanResult,
    encryptedScanResult,
    encryptedScanMode,
    encryptedScanKey,
    encryptedScanKeySearchBits,
    groupScanEntries,
    groupScanResult,
    groupScanBusy,
    groupScanMaxDistance,
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
    autoUnknownAwaitingObservation,
    selectedCandidateAddress,
    candidateHistory,
    scanBusy,
    scanStatusText,
    scanProgressPercent,
  } = storeToRefs(scanningStore)
  // addAddressToWatch (function declaration, donc hissee) est definie plus
  // bas dans ce meme setup() -- l'appeler ici fonctionne des l'initialisation
  // du store, avant tout premier scan.
  scanningStore.configureCandidateWatchNotifier(addAddressToWatch)

  // Store Write/Freeze/Checkpoint extrait (candidat S7, docs/REFACTOR_ROADMAP.md,
  // PHASE 229, 30/08/2026) -- refs directement mutables via storeToRefs, memes
  // noms qu'avant. Les dependances transversales pas encore extraites
  // (Session/Trainer, Watch, Chat/SmartSearch, mode kernel, RiskGate/audit)
  // sont injectees UNE SEULE FOIS ci-dessous, memes noms de fonctions locales
  // qu'avant (function declarations hissees, cf. addAddressToWatch plus haut) --
  // voir l'en-tete de writeFreeze.ts pour le detail du couplage.
  const writeFreezeStore = useWriteFreezeStore()
  const {
    writeValue,
    writeResult,
    writeSafetyWarning,
    writeSafetyAcknowledged,
    freezeEnabled,
    breakpointFreezeEnabled,
    freezeIntervalMs,
    freezeIntervalResult,
    writeHistorySequence,
    canWriteSelectedValue,
  } = storeToRefs(writeFreezeStore)
  const {
    checkpointAddress,
    checkpointType,
    buildCheckpointActionPlan,
    updateWriteSafetyWarning,
    executeCheckpointWrite,
    executeCheckpointKernelWrite,
    writeSelectedAddresses,
    writeSelectedTargets,
    writeSelectedAtomic,
    writeSelectedValue,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    freezeCandidateCurrent,
    toggleFreeze,
    startBreakpointFreeze,
    escalateFreezeToBreakpoint,
    stopBreakpointFreeze,
    setFreezeInterval,
    refreshWriteHistorySequence,
    replayWriteHistorySequence,
    clearWriteHistorySequence,
  } = writeFreezeStore
  // Store Assistant / Smart Search / Candidate Actions extrait (S10 + S11b,
  // docs/REFACTOR_ROADMAP.md, PHASE 236, 30/08/2026) -- il porte les refs
  // du chat, du contexte Smart Search, de l'Auto Resolve et des actions
  // candidat -> écriture. Les dépendances encore possédées par app.ts sont
  // injectées une seule fois pour éviter une dépendance circulaire.
  const assistantSmartSearchStore = useAssistantSmartSearchStore()
  const {
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
  } = storeToRefs(assistantSmartSearchStore)
  const {
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
  } = assistantSmartSearchStore
  configureAssistantSmartSearchContext({
    activeView,
    processName,
    writeMemoryValueByMode,
    kernelMemoryModeActive,
    addAddressToWatch,
  })
  writeFreezeStore.configureWriteFreezeContext({
    confirmRiskAction,
    logAiAudit,
    addAddressToWatch,
    upsertSessionEntry,
    markSessionEntryEnabled,
    writeMemoryValueByMode,
    kernelMemoryModeActive,
    pushMessage,
    refreshWatchedAddress,
    refreshActiveChatMemoryTargets,
    refreshSmartSearchContext,
    regionForAddress,
  })
  const watchLiveEnabled = ref(false)
  const watchedAddresses = ref<WatchedAddress[]>([])
  const watchLiveReadLimit = 200
  let watchLiveTimer: ReturnType<typeof setInterval> | null = null

  // Phase 20 — outils Expert manuels d'injection/hooking/auto-assembler,
  // gardés par confirmRiskAction('injection', ...) (mode Auto = Trainer requis,
  // cf. logique existante de confirmRiskAction). État panneau uniquement,
  // rien n'est persisté en profil pour l'instant (pas de feature Trainer 'hook').
  const injectDllPath = ref('')
  const injectionResult = ref<Record<string, unknown> | null>(null)
  const hookTargetAddress = ref('')
  const hookFunctionAddress = ref('')
  const activeFunctionHook = ref<Record<string, unknown> | null>(null)
  // Roadmap section I — résolution "module!fonction" -> adresse (table d'export PE distante).
  const symbolModuleName = ref('')
  const symbolFunctionName = ref('')
  const symbolResolveResult = ref<Record<string, unknown> | null>(null)
  const autoAsmScriptText = ref('')
  const autoAsmPreview = ref<Record<string, unknown> | null>(null)
  const autoAsmResult = ref<Record<string, unknown> | null>(null)
  // Persistance des scripts auto-assembleur (roadmap E.4) : rejouables sans retaper le texte.
  const autoAsmScriptName = ref('')
  const autoAsmSavedScripts = ref<Array<Record<string, unknown>>>([])
  const autoAsmSaveResult = ref<Record<string, unknown> | null>(null)
  const autoAsmSavedScriptsBusy = ref(false)
  const injectionBusy = ref(false)

  // Store Action Log extrait (candidat S5, docs/REFACTOR_ROADMAP.md, 29/08/2026) --
  // meme patron que investigationStore : refs directement mutables, fonctions
  // ci-dessous en wrappers minces qui gardent les memes noms/signatures.
  const actionLogStore = useActionLogStore()
  const { actionLog } = storeToRefs(actionLogStore)
  const sessionEntries = ref<SessionEntry[]>([])
  const sessionGroups = ref<SessionGroup[]>([])
  const sessionGroupIdCounter = ref(0)
  const sessionPromotionBusyIds = ref<Set<string>>(new Set())
  let backendScanSignalsConnected = false
  let backendHotkeySignalConnected = false
  let backendFreezeInstabilitySignalConnected = false
  let backendWriteWatchSignalConnected = false
  let backendEdrKillSignalConnected = false
  let backendDnsSpoofSignalConnected = false
  let backendNetworkInjectionSignalConnected = false
  let backendSpeedhackApiHookSignalConnected = false
  let backendElevatedNetworkActionsSignalConnected = false
  let backendNetworkConnectionsSignalConnected = false
  let backendModuleInstallSignalConnected = false
  let backendClaudePendingActionSignalConnected = false
  // Defense-in-depth cote frontend : le backend ne notifie deja qu'une fois
  // par adresse (FreezeEntry::flaggedUnstable), ce Set couvre juste le cas
  // d'une reconnexion du signal (ex: rechargement dev).
  const freezeInstabilityNotified = new Set<string>()
  const freezeInstabilityVersion = ref(0)

  // Store Workspace Session extrait (candidat S9b, docs/REFACTOR_ROADMAP.md,
  // PHASE 234, 30/08/2026) -- meme patron storeToRefs/configureXxxContext que
  // writeFreeze.ts/trainer.ts. Voir l'en-tete de workspaceSession.ts pour le
  // detail du couplage (projets workspace, export/import JSON+Markdown,
  // Pointer Chain Watch).
  const workspaceSessionStore = useWorkspaceSessionStore()
  const {
    workspaceProjects,
    watchedPointerChains,
    watchedPointerChainsLiveEnabled,
  } = storeToRefs(workspaceSessionStore)
  const {
    loadWorkspaceProjects,
    exportWorkspaceJson,
    exportWorkspaceMarkdown,
    previewWorkspaceImport,
    importWorkspaceJson,
    saveCurrentWorkspaceProject,
    loadWorkspaceProject,
    deleteWorkspaceProject,
    clearWorkspaceProjects,
    addWatchedPointerChain,
    refreshWatchedPointerChain,
    refreshWatchedPointerChains,
    removeWatchedPointerChain,
    clearWatchedPointerChains,
    setWatchedPointerChainsLiveEnabled,
  } = workspaceSessionStore
  workspaceSessionStore.configureWorkspaceSessionContext({
    version,
    processName,
    isAttached,
    workflowStatus,
    lastWorkflowPresetId,
    workflowPresets,
    autoResolveReport,
    logFilePath,
    smartSearchDebugFilePath,
    scanTelemetryFilePath,
    readMemoryPreviewByMode,
    valueTypeReadSize,
    decodeTypedPreviewValue,
  })

  // Getters
  const statusText = computed(() => {
    if (!isConnected.value) return t('common.status.disconnected')
    if (!isAttached.value) return t('common.status.ready')
    return t('common.status.attached', { name: processName.value })
  })

  function nowTime(): string {
    return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
  }

  function normalizeSessionAddress(address: string): string {
    return address.trim().replace(/^0x/i, '').toUpperCase()
  }

  function sessionEntryId(address: string, valueType: string, kind: SessionEntry['kind']): string {
    return `${kind}:${normalizeSessionAddress(address).toLowerCase()}:${valueType}`
  }

  function upsertSessionEntry(address: string, valueType: string, kind: SessionEntry['kind'], enabled = true) {
    const normalized = normalizeSessionAddress(address)
    if (!normalized) return
    const id = sessionEntryId(normalized, valueType, kind)
    const existing = sessionEntries.value.find((entry) => entry.id === id)
    if (existing) {
      existing.enabled = enabled
      return
    }
    sessionEntries.value.unshift({
      id,
      address: normalized,
      valueType,
      label: '',
      kind,
      enabled,
      createdAt: new Date().toISOString(),
    })
  }

  function updateSessionEntryLabel(id: string, label: string) {
    const entry = sessionEntries.value.find((candidate) => candidate.id === id)
    if (entry) entry.label = label
  }

  function markSessionEntryEnabled(address: string, valueType: string, kind: SessionEntry['kind'], enabled: boolean) {
    const entry = sessionEntries.value.find((candidate) => candidate.id === sessionEntryId(address, valueType, kind))
    if (entry) entry.enabled = enabled
  }

  function clearSessionEntries() {
    sessionEntries.value = []
    sessionGroups.value = []
    freezeInstabilityNotified.clear()
    freezeInstabilityVersion.value += 1
  }

  function hasFreezeInstability(address: string): boolean {
    freezeInstabilityVersion.value
    return freezeInstabilityNotified.has(normalizeSessionAddress(address).toLowerCase())
  }

  async function disableSessionEntry(id: string) {
    const entry = sessionEntries.value.find((candidate) => candidate.id === id)
    if (!entry || !entry.enabled) return
    if (entry.kind === 'write') {
      entry.enabled = false
      return
    }
    try {
      const result = await backend.getController().setFreezeValue(entry.address, entry.valueType, '', false)
      if (result.success === true) {
        entry.enabled = false
        if (selectedCandidateAddress.value.replace(/^0x/i, '').toLowerCase() === entry.address.toLowerCase()) {
          freezeEnabled.value = false
          if (entry.kind === 'freeze_breakpoint') breakpointFreezeEnabled.value = false
        }
      }
      addActionLog(
        'freeze',
        result.success === true ? t('appStore.session.freezeDisabled') : t('appStore.session.freezeFailed'),
        `0x${entry.address}. ${String(result.error ?? '')}`.trim(),
        result.success === true ? 'success' : 'error',
      )
    } catch (e) {
      addActionLog('freeze', t('appStore.session.freezeFailed'), String(e), 'error')
    }
  }

  function createSessionGroup(memberIds: string[], name?: string): string | null {
    const validIds = new Set(sessionEntries.value.map((entry) => entry.id))
    const uniqueMemberIds = Array.from(new Set(memberIds)).filter((id) => validIds.has(id))
    if (uniqueMemberIds.length < 2) return null
    sessionGroupIdCounter.value += 1
    const id = `session-group-${sessionGroupIdCounter.value}`
    sessionGroups.value.unshift({
      id,
      name: name?.trim() || `Groupe (${uniqueMemberIds.length} adresses)`,
      memberIds: uniqueMemberIds,
    })
    return id
  }

  function updateSessionGroupName(id: string, name: string) {
    const group = sessionGroups.value.find((candidate) => candidate.id === id)
    if (group) group.name = name
  }

  function removeSessionGroup(id: string) {
    sessionGroups.value = sessionGroups.value.filter((group) => group.id !== id)
  }

  function removeSessionEntriesFromGroup(groupId: string, memberIds: string[]) {
    const group = sessionGroups.value.find((candidate) => candidate.id === groupId)
    if (!group) return
    const removed = new Set(memberIds)
    group.memberIds = group.memberIds.filter((id) => !removed.has(id))
    if (group.memberIds.length < 2) removeSessionGroup(groupId)
  }

  // PHASE 166 : createSessionGroup() ne peut que creer un nouveau groupe --
  // aucun chemin n'existait pour ajouter/deplacer une entree vers un groupe
  // deja existant (constat 27/08/2026), pourtant c'est exactement le geste de
  // bissection qui a motive PHASE 165 (sortir une adresse du groupe "bruit"
  // vers "confirme"). Reutilise removeSessionEntriesFromGroup() (donc son
  // auto-dissolution sous 2 membres) plutot que de dupliquer cette logique.
  function moveSessionEntryToGroup(entryId: string, targetGroupId: string) {
    const targetGroup = sessionGroups.value.find((group) => group.id === targetGroupId)
    if (!targetGroup) return
    const validIds = new Set(sessionEntries.value.map((entry) => entry.id))
    if (!validIds.has(entryId)) return

    const sourceGroupIds = sessionGroups.value
      .filter((group) => group.id !== targetGroupId && group.memberIds.includes(entryId))
      .map((group) => group.id)
    for (const sourceGroupId of sourceGroupIds) {
      removeSessionEntriesFromGroup(sourceGroupId, [entryId])
    }

    if (!targetGroup.memberIds.includes(entryId)) {
      targetGroup.memberIds = [...targetGroup.memberIds, entryId]
    }
  }

  async function disableSessionGroup(id: string) {
    const group = sessionGroups.value.find((candidate) => candidate.id === id)
    if (!group) return
    for (const memberId of group.memberIds) {
      await disableSessionEntry(memberId)
    }
  }

  function setSessionPromotionBusy(id: string, busy: boolean) {
    const next = new Set(sessionPromotionBusyIds.value)
    if (busy) next.add(id)
    else next.delete(id)
    sessionPromotionBusyIds.value = next
  }

  function isSessionPromotionBusy(id: string): boolean {
    return sessionPromotionBusyIds.value.has(id)
  }

  function sessionTrainerAction(entry: SessionEntry): TrainerFeature['action'] {
    if (entry.kind === 'freeze_breakpoint') return 'freeze_breakpoint'
    if (entry.kind === 'freeze_polling') return 'freeze_polling'
    return 'write'
  }

  async function sessionTrainerValue(entry: SessionEntry): Promise<string> {
    const normalized = normalizeSessionAddress(entry.address)
    let watched = watchedAddresses.value.find((item) => item.address.toLowerCase() === normalized.toLowerCase())
    if (!watched) {
      addAddressToWatch(normalized, entry.valueType)
      watched = await refreshWatchedAddress(normalized) ?? undefined
    } else if (!watched.value || watched.value === '-') {
      watched = await refreshWatchedAddress(normalized) ?? watched
    }
    const value = String(watched?.value ?? '').trim()
    return value && value !== '-' ? value : ''
  }

  async function resolveSessionTrainerLocator(entry: SessionEntry): Promise<{
    featureInput: Partial<TrainerFeature>
    label: string
    warning: string
  }> {
    const controller = backend.getController()
    const address = normalizeSessionAddress(entry.address)
    if (controller.generateAobSignature && controller.scanAobPattern) {
      try {
        const signature = await controller.generateAobSignature(address, { beforeBytes: 0, length: 20 })
        const quality = signature.signatureQuality
        const pattern = String(signature.pattern ?? '').trim()
        const fixedBytes = Number(quality?.fixedBytes ?? 0)
        const score = Number(quality?.score ?? 0)
        if (signature.success === true && pattern && fixedBytes >= 3 && score >= 35) {
          const scan = await controller.scanAobPattern(pattern, {
            executableOnly: false,
            imageOnly: true,
            maxResults: 2,
          })
          const matchCount = Array.isArray(scan.matches) ? scan.matches.length : Number(scan.matchesFound ?? 0)
          if (scan.success === true && matchCount === 1) {
            return {
              featureInput: {
                locatorKind: 'aob',
                aobPattern: pattern,
                signatureQuality: quality,
                signatureScore: score,
                signatureLevel: quality?.level,
                signatureWarning: quality?.warning,
                signatureFixedBytes: fixedBytes,
                signatureWildcardBytes: quality?.wildcardBytes,
                signatureUniqueFixedBytes: quality?.uniqueFixedBytes,
                signatureFixedRatio: quality?.fixedRatio,
                trainerSafe: quality?.trainerSafe,
                signatureMatches: 1,
              },
              label: t('appStore.session.locator.aobUnique', { score, fixedBytes }),
              warning: '',
            }
          }
        }
      } catch (e) {
        addActionLog('trainer', t('appStore.session.locator.aobIgnored'), String(e), 'warning')
      }
    }

    if (controller.scanPointerChains) {
      try {
        const pointerScan = await controller.scanPointerChains(address, {
          maxDepth: 3,
          maxResults: 5,
          onlyModuleBase: true,
        })
        const bestChain = pointerScan.chains?.[0]
        if (pointerScan.success === true && bestChain) {
          return {
            featureInput: {
              locatorKind: 'pointer_chain',
              pointerChain: bestChain,
            },
            label: t('appStore.session.locator.pointerChain', { module: bestChain.module, offset: bestChain.baseOffset }),
            warning: '',
          }
        }
      } catch (e) {
        addActionLog('trainer', t('appStore.session.locator.pointerChainIgnored'), String(e), 'warning')
      }
    }

    return {
      featureInput: { locatorKind: 'absolute' },
      label: t('appStore.session.locator.absoluteAddress'),
      warning: t('appStore.session.locator.noStableLocatorWarning'),
    }
  }

  async function promoteSessionEntryToTrainer(entryId: string, name?: string): Promise<SessionPromotionResult> {
    const entry = sessionEntries.value.find((candidate) => candidate.id === entryId)
    if (!entry) return { success: false, featureIds: [], message: t('appStore.session.entryNotFound'), warnings: [] }
    setSessionPromotionBusy(entryId, true)
    try {
      const value = await sessionTrainerValue(entry)
      if (!value) {
        const message = t('appStore.session.liveValueUnreadable', { address: entry.address })
        addActionLog('trainer', t('appStore.session.promotionRefused'), message, 'warning')
        return { success: false, featureIds: [], message, warnings: [] }
      }
      const locator = await resolveSessionTrainerLocator(entry)
      const feature = createTrainerFeature({
        name: name?.trim() || entry.label.trim() || t('appStore.session.defaultName', { address: entry.address }),
        action: sessionTrainerAction(entry),
        address: entry.address,
        valueType: entry.valueType,
        value,
        ...locator.featureInput,
      })
      if (!feature) {
        return { success: false, featureIds: [], message: t('appStore.session.trainerCreationRefused'), warnings: locator.warning ? [locator.warning] : [] }
      }
      const detail = locator.warning
        ? `${locator.label}. ${locator.warning}`
        : `${locator.label}.`
      addActionLog('trainer', t('appStore.session.entryPromoted', { name: feature.name }), detail, locator.warning ? 'warning' : 'success')
      return {
        success: true,
        featureIds: [feature.id],
        message: t('appStore.session.entryCreatedMessage', { name: feature.name, label: locator.label }),
        warnings: locator.warning ? [locator.warning] : [],
      }
    } catch (e) {
      const message = String(e)
      addActionLog('trainer', t('appStore.session.promotionFailed'), message, 'error')
      return { success: false, featureIds: [], message, warnings: [] }
    } finally {
      setSessionPromotionBusy(entryId, false)
    }
  }

  async function promoteSessionGroupToTrainer(groupId: string, name?: string): Promise<SessionPromotionResult> {
    const group = sessionGroups.value.find((candidate) => candidate.id === groupId)
    if (!group) return { success: false, featureIds: [], message: t('appStore.session.groupNotFound'), warnings: [] }
    setSessionPromotionBusy(groupId, true)
    const featureIds: number[] = []
    const warnings: string[] = []
    try {
      for (const memberId of group.memberIds) {
        const entry = sessionEntries.value.find((candidate) => candidate.id === memberId)
        if (!entry) continue
        const entryName = `${name?.trim() || group.name || t('appStore.session.defaultGroupName')} · ${entry.label.trim() || `0x${entry.address}`}`
        const result = await promoteSessionEntryToTrainer(memberId, entryName)
        featureIds.push(...result.featureIds)
        warnings.push(...result.warnings)
      }
      const success = featureIds.length > 0
      const message = success
        ? t('appStore.session.groupFeaturesCreated', { count: featureIds.length, group: group.name })
        : t('appStore.session.groupNoFeaturesCreated', { group: group.name })
      addActionLog('trainer', success ? t('appStore.session.groupPromoted') : t('appStore.session.groupPromotionFailed'), message, success ? (warnings.length ? 'warning' : 'success') : 'error')
      return { success, featureIds, message, warnings }
    } finally {
      setSessionPromotionBusy(groupId, false)
    }
  }

  function addActionLog(
    kind: string,
    title: string,
    detail = '',
    status: UserActionLogEntry['status'] = 'info',
  ) {
    actionLogStore.addActionLog(kind, title, detail, status)
  }

  function loadActionLog() {
    actionLogStore.loadActionLog()
  }

  function clearActionLog() {
    actionLogStore.clearActionLog()
  }

  function exportActionLogJson(): string {
    return actionLogStore.exportActionLogJson(processName.value)
  }

  function exportActionLogMarkdown(): string {
    return actionLogStore.exportActionLogMarkdown(processName.value)
  }

  function saveInvestigations() {
    investigationStore.saveInvestigations()
  }

  function loadInvestigations() {
    investigationStore.loadInvestigations()
  }

  function startInvestigation(objective: string, title?: string) {
    investigationStore.startInvestigation(objective, title, processName.value)
  }

  function addInvestigationStep(step: Omit<InvestigationStep, 'id' | 'time'>) {
    investigationStore.addInvestigationStep(step, searchQuery.value || 'Investigation manuelle', processName.value)
  }

  function updateInvestigationFromAutoResult(result: Record<string, unknown>) {
    investigationStore.updateInvestigationFromAutoResult(result)
  }

  function finishInvestigation(status?: InvestigationRun['status']) {
    investigationStore.finishInvestigation(status)
  }

  function clearInvestigation() {
    investigationStore.clearInvestigation()
  }

  function clearInvestigationArchive() {
    investigationStore.clearInvestigationArchive()
  }

  function restoreInvestigationFromArchive(id: number) {
    const restored = investigationStore.restoreInvestigationFromArchive(id)
    if (restored && activeInvestigation.value) {
      addActionLog('investigation', t('appStore.investigation.archiveRestored'), activeInvestigation.value.objective, 'success')
    }
    return restored
  }

  function exportInvestigationJson(): string {
    return investigationStore.exportInvestigationJson()
  }

  function exportInvestigationMarkdown(): string {
    const run = activeInvestigation.value
    if (!run) return t('appStore.investigation.markdown.noActiveInvestigation')
    const report = autoResolveReport.value
    const nextBestAction = report?.nextBestAction && typeof report.nextBestAction === 'object'
      ? report.nextBestAction as Record<string, unknown>
      : null
    const topCheckpoints = [...run.checkpoints]
      .sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0))
      .slice(0, 5)
    const topActionPlans = topCheckpoints.map((checkpoint) => buildCheckpointActionPlan(checkpoint))
    const guardrails = Array.isArray(report?.guardrails) ? report.guardrails.slice(0, 8) : []
    const notDetermined = t('appStore.investigation.markdown.notDetermined')
    const lines = [
      `# ${run.title}`,
      '',
      t('appStore.investigation.markdown.objective', { value: run.objective }),
      t('appStore.investigation.markdown.process', { value: run.processName || t('appStore.investigation.markdown.notAttached') }),
      t('appStore.investigation.markdown.status', { value: run.status }),
      '',
      `## ${t('appStore.investigation.markdown.strategyTitle')}`,
      String(run.preferredStrategy?.label ?? notDetermined),
      '',
      `## ${t('appStore.investigation.markdown.bestNextActionTitle')}`,
      nextBestAction
        ? `${String(nextBestAction.label ?? nextBestAction.id ?? t('appStore.investigation.markdown.actionFallback'))} (${String(nextBestAction.risk ?? 'safe')}${nextBestAction.confidence !== undefined ? `, ${t('appStore.investigation.markdown.confidence', { value: String(nextBestAction.confidence) })}` : ''})`
        : String(run.checkpoints[0]?.label ?? run.hypotheses[0]?.label ?? notDetermined),
      nextBestAction ? String(nextBestAction.reason ?? '') : String(run.checkpoints[0]?.reason ?? run.summary ?? ''),
      '',
      `## ${t('appStore.investigation.markdown.stepsTitle')}`,
      ...run.steps.slice().reverse().map((step) => `- [${step.status}] ${step.title} - ${step.detail}`),
      '',
      `## ${t('appStore.investigation.markdown.checkpointsTitle')}`,
      ...run.checkpoints.map((checkpoint) => `- ${String(checkpoint.label ?? checkpoint.address ?? checkpoint.id ?? 'checkpoint')} (${String(checkpoint.kind ?? 'checkpoint')}${checkpoint.confidenceLabel ? `, ${String(checkpoint.confidenceLabel)}` : ''})`),
      '',
      `## ${t('appStore.investigation.markdown.top5Title')}`,
      ...(topCheckpoints.length > 0
        ? topCheckpoints.map((checkpoint, index) => `${index + 1}. ${String(checkpoint.label ?? checkpoint.address ?? 'checkpoint')} - ${String(checkpoint.kind ?? 'checkpoint')} - ${String(checkpoint.confidenceLabel ?? t('appStore.investigation.markdown.scoreOutOf100', { value: Number(checkpoint.confidenceScore ?? 0) }))} - ${checkpoint.requiresConfirmation === true ? t('appStore.investigation.markdown.confirmationRequired') : 'safe'}`)
        : [t('appStore.investigation.markdown.noScoredCheckpoint')]),
      '',
      `## ${t('appStore.investigation.markdown.actionPlansTitle')}`,
      ...(topActionPlans.length > 0
        ? topActionPlans.map((plan, index) => `${index + 1}. ${plan.label} - ${plan.safeCount} safe / ${plan.riskyCount} confirmation - ${plan.actions.filter((action) => action.enabled).map((action) => `${action.label}(${action.risk})`).join(', ') || t('appStore.investigation.markdown.noActiveAction')}`)
        : [t('appStore.investigation.markdown.noActionPlan')]),
      '',
      `## ${t('appStore.investigation.markdown.guardrailsTitle')}`,
      ...(guardrails.length > 0
        ? guardrails.map((guardrail) => `- ${String(guardrail.label ?? guardrail.id ?? 'guardrail')} (${String(guardrail.risk ?? 'risk')})`)
        : [t('appStore.investigation.markdown.noGuardrail')]),
      '',
      `## ${t('appStore.investigation.markdown.autoSummaryTitle')}`,
      t('appStore.investigation.markdown.safeSteps', { value: run.steps.filter((step) => step.risk === 'safe' && step.status === 'success').length }),
      t('appStore.investigation.markdown.actionableCheckpoints', { value: run.checkpoints.length }),
      t('appStore.investigation.markdown.bestLead', { value: String(run.checkpoints[0]?.label ?? run.hypotheses[0]?.label ?? notDetermined) }),
      t('appStore.investigation.markdown.nextStep', { value: String(run.hypotheses[0]?.nextAction ?? run.checkpoints[0]?.reason ?? run.summary ?? t('appStore.investigation.markdown.continueOrValidate')) }),
      '',
    ]
    return lines.join('\n')
  }

  function loadStructureTemplates() {
    workspaceItemsStore.loadStructureTemplates()
  }

  function saveStructureTemplate(input: {
    name?: string
    baseAddress: string
    size: number
    fields: StructureTemplateField[]
  }) {
    return workspaceItemsStore.saveStructureTemplate(input, processName.value)
  }

  function deleteStructureTemplate(id: number) {
    workspaceItemsStore.deleteStructureTemplate(id)
  }

  function clearStructureTemplates() {
    workspaceItemsStore.clearStructureTemplates()
  }

  function loadWorkspaceBookmarks() {
    workspaceItemsStore.loadWorkspaceBookmarks()
  }

  function addWorkspaceBookmark(input: Partial<WorkspaceBookmark>) {
    return workspaceItemsStore.addWorkspaceBookmark(input, processName.value)
  }

  function updateWorkspaceBookmark(id: number, input: Partial<WorkspaceBookmark>) {
    return workspaceItemsStore.updateWorkspaceBookmark(id, input)
  }

  function useWorkspaceBookmarkAsWriteTarget(id: number) {
    const bookmark = workspaceBookmarks.value.find((item) => item.id === id)
    if (!bookmark?.address) {
      addActionLog('workspace', t('appStore.bookmark.unusable'), t('appStore.bookmark.missingAddress'), 'warning')
      return false
    }
    selectedCandidateAddress.value = bookmark.address
    if (bookmark.type) exactScanType.value = bookmark.type
    if (bookmark.value !== undefined) writeValue.value = bookmark.value
    addActionLog('workspace', t('appStore.bookmark.loaded', { label: bookmark.label }), t('appStore.bookmark.writeFreezePrepared', { address: bookmark.address }), 'success')
    addInvestigationStep({
      title: t('appStore.bookmark.loadedIntoWriteFreeze'),
      detail: `${bookmark.label} · 0x${bookmark.address} · ${bookmark.type || exactScanType.value}`,
      status: 'success',
      tool: 'useWorkspaceBookmarkAsWriteTarget',
      risk: 'safe',
      payload: bookmark.payload ?? { bookmarkId: bookmark.id },
    })
    return true
  }

  function createWorkspaceBookmarkFromCheckpoint(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    const payload = (checkpoint.payload && typeof checkpoint.payload === 'object')
      ? checkpoint.payload as Record<string, unknown>
      : {}
    const aobPattern = String(checkpoint.aobPattern ?? payload.aobPattern ?? '').trim()
    const pointerChain = checkpoint.pointerChain ?? payload.pointerChain
    const rawKind = String(checkpoint.kind ?? '').toLowerCase()
    const kind: WorkspaceBookmark['kind'] = aobPattern || rawKind.includes('aob') || rawKind.includes('code')
      ? 'aob'
      : pointerChain || rawKind.includes('pointer')
        ? 'pointer'
        : address
          ? 'address'
          : 'note'
    const label = String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? checkpoint.id ?? t('appStore.bookmark.defaultCheckpointLabel')).trim() || t('appStore.bookmark.defaultCheckpointLabel')
    const score = Number(checkpoint.confidenceScore ?? payload.confidenceScore ?? 0)
    const reason = String(checkpoint.reason ?? payload.reason ?? '').trim()
    const value = String(checkpoint.value ?? checkpoint.targetValue ?? payload.value ?? '').trim()
    const bookmark = addWorkspaceBookmark({
      kind,
      label,
      processName: String(checkpoint.processName ?? processName.value),
      address: address || undefined,
      type: String(checkpoint.type ?? checkpoint.valueType ?? exactScanType.value),
      value: value || undefined,
      note: [
        reason,
        score > 0 ? t('appStore.bookmark.scoreNote', { score }) : '',
        checkpoint.requiresConfirmation === true ? t('appStore.bookmark.confirmationRequiredNote') : t('appStore.bookmark.safeNote'),
      ].filter(Boolean).join(' · '),
      payload: {
        ...payload,
        checkpointKind: checkpoint.kind,
        confidenceScore: checkpoint.confidenceScore,
        confidenceLabel: checkpoint.confidenceLabel,
        requiresConfirmation: checkpoint.requiresConfirmation,
        aobPattern: aobPattern || undefined,
        patchBytes: checkpoint.patchBytes ?? payload.patchBytes,
        signatureQuality: checkpoint.signatureQuality ?? payload.signatureQuality,
        signatureScore: checkpoint.signatureScore ?? payload.signatureScore,
        signatureLevel: checkpoint.signatureLevel ?? payload.signatureLevel,
        signatureWarning: checkpoint.signatureWarning ?? payload.signatureWarning,
        signatureMatches: checkpoint.signatureMatches ?? payload.signatureMatches,
      },
    })
    addInvestigationStep({
      title: t('appStore.bookmark.createdFromCheckpoint'),
      detail: `${bookmark.label} · ${bookmark.address ? `0x${bookmark.address}` : bookmark.kind} · ${bookmark.type || '-'}`,
      status: 'success',
      tool: 'createWorkspaceBookmarkFromCheckpoint',
      risk: 'safe',
      payload: { bookmarkId: bookmark.id, checkpointLabel: label, kind },
    })
    return bookmark
  }

  function deleteWorkspaceBookmark(id: number) {
    workspaceItemsStore.deleteWorkspaceBookmark(id)
  }

  function clearWorkspaceBookmarks() {
    workspaceItemsStore.clearWorkspaceBookmarks()
  }

  async function confirmRiskAction(
    risk: NonNullable<InvestigationStep['risk']>,
    title: string,
    detail: string,
  ): Promise<boolean> {
    return riskGateStore.confirmRiskAction(risk, title, detail, logAiAudit)
  }

  function resolveRiskDialog(accepted: boolean) {
    return riskGateStore.resolveRiskDialog(accepted)
  }

  // Store Automation Pipe status extrait (candidat S3, docs/REFACTOR_ROADMAP.md,
  // 29/08/2026). Mode Automation (29/08/2026) : contournement RiskGate
  // volontaire pour un pilotage scripté/agent externe (pipe local + Lua
  // ke.call), pensé pour un utilisateur avancé qui sait ce qu'il active. Un
  // seul accord explicite via confirmRiskAction à l'activation (gate gardé
  // ICI, pas encore extrait) ; désactiver ne nécessite aucune confirmation.
  const automationPipeStore = useAutomationPipeStore()
  const { automationPipeStatus } = storeToRefs(automationPipeStore)

  async function refreshAutomationPipeStatus() {
    return automationPipeStore.refreshAutomationPipeStatus()
  }

  async function enableAutomationMode() {
    const accepted = await confirmRiskAction(
      'injection',
      t('appStore.automation.enableTitle'),
      t('appStore.automation.enableDesc'),
    )
    if (!accepted) return null
    return automationPipeStore.enableAutomationMode()
  }

  async function disableAutomationMode() {
    return automationPipeStore.disableAutomationMode()
  }

  // Exigence produit du 31/08/2026 (docs/PHASE_TRACKER.md, chantier WebView2/CDP) :
  // la variable d'env forçant le port de debug CDP WebView2 doit passer par un
  // vrai RiskGate en Paramètres, jamais posée silencieusement. Activer prévient
  // explicitement de la portée large (tous les hôtes WebView2 du user courant,
  // pas juste la cible visée) ; désactiver ne nécessite aucune confirmation
  // (même asymétrie que enableAutomationMode/disableAutomationMode ci-dessus).
  const webView2CdpDebugFlagStatus = ref<{ success: boolean; enabled?: boolean; value?: string; error?: string } | null>(null)
  const webView2CdpDebugFlagBusy = ref(false)

  async function refreshWebView2CdpDebugFlagStatus() {
    webView2CdpDebugFlagBusy.value = true
    try {
      const result = await backend.getController().getWebView2CdpDebugFlagStatus?.()
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
    } finally {
      webView2CdpDebugFlagBusy.value = false
    }
  }

  async function enableWebView2CdpDebugFlag() {
    const accepted = await confirmRiskAction(
      'debug',
      t('appStore.webview2Cdp.enableTitle'),
      t('appStore.webview2Cdp.enableDesc'),
    )
    if (!accepted) return null
    webView2CdpDebugFlagBusy.value = true
    try {
      const result = await backend.getController().enableWebView2CdpDebugFlag?.()
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
      return result ?? null
    } finally {
      webView2CdpDebugFlagBusy.value = false
    }
  }

  async function disableWebView2CdpDebugFlag() {
    webView2CdpDebugFlagBusy.value = true
    try {
      const result = await backend.getController().disableWebView2CdpDebugFlag?.()
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
      return result ?? null
    } finally {
      webView2CdpDebugFlagBusy.value = false
    }
  }

  // Backend IA externe (T5, docs/EXTERNAL_AI_BACKEND_ROADMAP.md, 07/09/2026) :
  // bascule manuelle globale vers un backend Claude (clé API personnelle),
  // à côté du modèle local qui reste le défaut. Activer envoie le contexte
  // des appels d'outils (adresses mémoire, nom du process, éventuellement du
  // code désassemblé) à un tiers -> RiskGate 'injection' explicite, même
  // asymétrie que WebView2 CDP/Stealth ci-dessus (repasser en local ne
  // nécessite aucune confirmation). hasExternalAiApiKey() ne retourne jamais
  // la clé elle-même, juste un booléen.
  const externalAiActiveBackend = ref<'local' | 'claude'>('local')
  const externalAiHasApiKey = ref(false)
  const externalAiRequestCount = ref(0)
  const externalAiBusy = ref(false)
  const externalAiError = ref('')

  async function refreshExternalAiStatus() {
    const controller = backend.getController()
    const [activeBackend, hasKey, requestCount] = await Promise.all([
      controller.getActiveAiBackend?.() ?? Promise.resolve('local'),
      controller.hasExternalAiApiKey?.() ?? Promise.resolve(false),
      controller.getExternalAiRequestCount?.() ?? Promise.resolve(0),
    ])
    externalAiActiveBackend.value = activeBackend === 'claude' ? 'claude' : 'local'
    externalAiHasApiKey.value = !!hasKey
    externalAiRequestCount.value = Number(requestCount) || 0
  }

  async function setExternalAiApiKey(apiKey: string) {
    externalAiBusy.value = true
    externalAiError.value = ''
    try {
      const result = await backend.getController().setExternalAiApiKey?.(apiKey)
      if (!result?.success) {
        externalAiError.value = String(result?.error ?? t('appStore.externalAi.saveKeyFailed'))
      }
      await refreshExternalAiStatus()
      return result ?? null
    } finally {
      externalAiBusy.value = false
    }
  }

  async function clearExternalAiApiKey() {
    externalAiBusy.value = true
    try {
      const result = await backend.getController().clearExternalAiApiKey?.()
      await refreshExternalAiStatus()
      return result ?? null
    } finally {
      externalAiBusy.value = false
    }
  }

  async function setActiveAiBackend(target: 'local' | 'claude') {
    if (target === 'claude') {
      const accepted = await confirmRiskAction(
        'injection',
        t('appStore.externalAi.enableTitle'),
        t('appStore.externalAi.enableDesc'),
      )
      if (!accepted) return null
    }
    externalAiBusy.value = true
    externalAiError.value = ''
    try {
      const result = await backend.getController().setActiveAiBackend?.(target)
      if (!result?.success) {
        externalAiError.value = String(result?.error ?? t('appStore.externalAi.switchFailed'))
      }
      await refreshExternalAiStatus()
      return result ?? null
    } finally {
      externalAiBusy.value = false
    }
  }

  // Stealth Profiler (03/09/2026) : mode de protection unifié (antiDebug/
  // processMask/dllMask) piloté jusqu'ici uniquement via le pipe/Lua, aucune
  // UI. Activer un profil modifie le process attaché (hooks + masquage) ->
  // RiskGate 'debug', même asymétrie que WebView2 CDP ci-dessus (désactiver
  // ne demande aucune confirmation). analyzeStealthRisk est une analyse pure
  // lecture seule, jamais gardée par RiskGate.
  const stealthStatus = ref<{
    active: boolean
    profile: string
    modules?: { antiDebug: boolean; processMask: boolean; dllMask: boolean }
  } | null>(null)
  const stealthRiskAnalysis = ref<Record<string, unknown> | null>(null)
  const stealthBusy = ref(false)

  async function refreshStealthStatus() {
    stealthBusy.value = true
    try {
      const result = await backend.getController().getStealthStatus?.()
      stealthStatus.value = result ?? null
    } finally {
      stealthBusy.value = false
    }
  }

  async function applyStealthMode(profile: string) {
    const accepted = await confirmRiskAction(
      'debug',
      t('appStore.stealth.enableTitle', { profile }),
      t('appStore.stealth.enableDesc'),
    )
    if (!accepted) return null
    stealthBusy.value = true
    try {
      const result = await backend.getController().applyStealthMode?.(profile)
      await refreshStealthStatus()
      if (result && !result.success) {
        const details = Array.isArray(result.warnings) && result.warnings.length > 0
          ? result.warnings.join('\n')
          : (result.error ?? t('appStore.stealth.unknownError'))
        pushMessage('assistant', t('appStore.stealth.failedMessage', { profile, details }))
      } else if (result && result.success && Array.isArray(result.warnings) && result.warnings.length > 0) {
        pushMessage('assistant', t('appStore.stealth.activeWithWarningsMessage', { profile, warnings: result.warnings.join('\n') }))
      }
      return result ?? null
    } finally {
      stealthBusy.value = false
    }
  }

  async function restoreStealthMode() {
    stealthBusy.value = true
    try {
      const result = await backend.getController().restoreStealthMode?.()
      await refreshStealthStatus()
      return result ?? null
    } finally {
      stealthBusy.value = false
    }
  }

  async function analyzeStealthRisk() {
    stealthBusy.value = true
    try {
      const result = await backend.getController().analyzeStealthRisk?.()
      stealthRiskAnalysis.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
      return result ?? null
    } finally {
      stealthBusy.value = false
    }
  }

  // Exigence produit du 01/09/2026 : diagnostic guidé "Préparer l'inspection
  // WebView2" pour les cibles UWP/Store (chaîne Windows Device Portal). Lecture
  // seule pour le statut ; l'installation de la capability reste derrière un
  // RiskGate même si Windows affiche déjà sa propre invite UAC, pour expliquer
  // POURQUOI avant de déclencher l'invite système.
  const webView2SystemPrepStatus = ref<{
    success: boolean
    developerModeEnabled?: boolean
    allowAllTrustedApps?: boolean
    capabilityQueried?: boolean
    capabilityState?: string
    capabilityInstalled?: boolean
    error?: string
  } | null>(null)
  const webView2SystemPrepBusy = ref(false)
  const webView2CapabilityInstallResult = ref<{ success: boolean; message?: string; cancelled?: boolean; error?: string } | null>(null)

  async function refreshWebView2SystemPrepStatus() {
    webView2SystemPrepBusy.value = true
    try {
      const result = await backend.getController().getWebView2SystemPrepStatus?.()
      webView2SystemPrepStatus.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
    } finally {
      webView2SystemPrepBusy.value = false
    }
  }

  async function installWebView2DeveloperModeCapability() {
    const accepted = await confirmRiskAction(
      'debug',
      t('appStore.webview2Prep.installCapabilityTitle'),
      t('appStore.webview2Prep.installCapabilityDesc'),
    )
    if (!accepted) return null
    webView2SystemPrepBusy.value = true
    webView2CapabilityInstallResult.value = null
    try {
      const result = await backend.getController().installWebView2DeveloperModeCapability?.()
      webView2CapabilityInstallResult.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
      return result ?? null
    } finally {
      webView2SystemPrepBusy.value = false
    }
  }

  // Vue "Modules" (menu gauche) : catalogue des modules complémentaires
  // optionnels (runtime Lua, modèle IA GGUF, inspecteur CLR, driver noyau)
  // avec statut installé/manquant et installation depuis l'UI — finalité
  // d'exportabilité de KillEngine sur d'autres machines.
  const moduleCatalog = ref<ModuleCatalogItem[]>([])
  const moduleCatalogBusy = ref(false)
  const moduleInstallBusy = ref(false)
  const moduleInstallModuleId = ref<string | null>(null)
  const moduleInstallProgress = ref<string>('')
  const moduleInstallResult = ref<Record<string, unknown> | null>(null)

  async function refreshModuleCatalog() {
    moduleCatalogBusy.value = true
    try {
      const result = await backend.getController().getModuleCatalog?.()
      moduleCatalog.value = result?.modules ?? []
    } catch (e) {
      moduleCatalog.value = []
      addActionLog('modules', t('appStore.modules.catalogTitle'), String(e), 'error')
    } finally {
      moduleCatalogBusy.value = false
    }
  }

  async function installModule(moduleId: string) {
    if (moduleInstallBusy.value) return null
    const labels: Record<string, string> = {
      lua_runtime: t('appStore.modules.installLuaRuntime'),
      ai_model: t('appStore.modules.installAiModel'),
      clr_inspector: t('appStore.modules.installClrInspector'),
      kernel_driver: t('appStore.modules.installKernelDriver'),
    }
    const accepted = await confirmRiskAction(
      'debug',
      labels[moduleId] ?? t('appStore.modules.installGeneric', { moduleId }),
      t('appStore.modules.installDesc'),
    )
    if (!accepted) return null
    moduleInstallBusy.value = true
    moduleInstallModuleId.value = moduleId
    moduleInstallProgress.value = ''
    moduleInstallResult.value = null
    try {
      const controller = backend.getController()
      const started = await controller.installModule?.(moduleId, {})
      if (!started?.started) {
        moduleInstallResult.value = started ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
        return moduleInstallResult.value
      }
      // kernel_driver : lancé élevé et détaché, pas de signal de fin attendu.
      if (moduleId === 'kernel_driver') {
        moduleInstallResult.value = { success: true, message: String(started.message ?? '') }
        return moduleInstallResult.value
      }
      const finishedSignal = controller.moduleInstallFinished
      if (!finishedSignal) {
        moduleInstallResult.value = { success: false, error: t('appStore.modules.finishedSignalMissing') }
        return moduleInstallResult.value
      }
      const requestId = Number(started.requestId ?? 0)
      await new Promise<void>((resolve) => {
        let settled = false
        const watchdog = window.setTimeout(() => {
          if (settled) return
          settled = true
          finishedSignal.disconnect?.(handler)
          moduleInstallBusy.value = false
          moduleInstallResult.value = { success: false, error: t('appStore.modules.installTimeout') }
          resolve()
        }, 30 * 60 * 1000)
        const handler = (payload: Record<string, unknown>) => {
          if (Number(payload.requestId) !== requestId) return
          settled = true
          window.clearTimeout(watchdog)
          finishedSignal.disconnect?.(handler)
          moduleInstallBusy.value = false
          moduleInstallResult.value = payload
          resolve()
        }
        finishedSignal.connect(handler)
      })
      return moduleInstallResult.value
    } finally {
      moduleInstallBusy.value = false
      moduleInstallModuleId.value = null
    }
  }

  async function cancelModuleInstall() {
    const result = await backend.getController().cancelModuleInstall?.()
    return result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
  }

  async function executeCheckpointFindWhatWrites(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    if (!address) {
      addActionLog('checkpoint', t('appStore.checkpoint.debuggerImpossible'), t('appStore.checkpoint.missingAddress'), 'warning')
      return null
    }
    const sizeByType: Record<string, number> = {
      Int8: 1, UInt8: 1, Int16: 2, UInt16: 2, Int32: 4, UInt32: 4, Float32: 4, Int64: 8, UInt64: 8, Float64: 8,
    }
    const size = sizeByType[type] ?? 4
    if (!await confirmRiskAction('debug', t('appStore.checkpoint.findWhatWritesTitle'), t('appStore.checkpoint.findWhatWritesDesc', { address, size }))) return null
    const controller = backend.getController()
    if (!controller.findWhatWrites) {
      addActionLog('checkpoint', t('appStore.checkpoint.findWhatWritesUnavailable'), t('appStore.checkpoint.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.findWhatWrites(address, { size, timeoutMs: 5000, maxHits: 8 })
      const hits = Array.isArray(result.hits) ? result.hits as Array<Record<string, unknown>> : []
      if (hits.length > 0 && activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...hits.slice(0, 6).map((hit) => ({
            kind: 'code_writer',
            label: `RIP 0x${String(hit.instructionPointer ?? '').replace(/^0x/i, '')}`,
            address: String(hit.instructionPointer ?? '').replace(/^0x/i, ''),
            sourceAddress: address,
            module: hit.module,
            moduleOffset: hit.moduleOffset,
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: hits.length > 0 ? t('appStore.checkpoint.findWhatWritesCapturedTitle') : t('appStore.checkpoint.findWhatWritesEmptyTitle'),
        detail: t('appStore.checkpoint.findWhatWritesDetail', { count: hits.length, address }),
        status: hits.length > 0 ? 'checkpoint' : 'warning',
        tool: 'findWhatWrites',
        risk: 'debug',
        payload: result,
      })
      logAiAudit('checkpoint_find_writes_executed', { address, type, size, hitCount: hits.length, success: result.success === true })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('appStore.checkpoint.findWhatWritesFailed'), String(e), 'error')
      return null
    }
  }

  async function executeCheckpointDisassembleBackward(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    if (!address) {
      addActionLog('checkpoint', t('appStore.checkpoint.disassembleImpossible'), t('appStore.checkpoint.missingRipAddress'), 'warning')
      return null
    }
    if (!await confirmRiskAction('patch', t('appStore.checkpoint.disassembleBackwardTitle'), t('appStore.checkpoint.disassembleBackwardDesc', { address }))) return null
    const controller = backend.getController()
    if (!controller.disassembleBackward) {
      addActionLog('checkpoint', t('appStore.checkpoint.disassembleUnavailable'), t('appStore.checkpoint.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.disassembleBackward(address, {})
      const candidates = Array.isArray(result.candidateFields) ? result.candidateFields : []
      if (candidates.length > 0 && activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...candidates.slice(0, 6).map((field) => ({
            kind: 'candidate_field',
            label: t('appStore.checkpoint.candidateFieldLabel', { register: field.memBaseRegister, offset: (field.memDisplacement ?? 0).toString(16) }),
            address: field.address,
            sourceAddress: address,
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: candidates.length > 0 ? t('appStore.checkpoint.disassembleBackwardFoundTitle') : t('appStore.checkpoint.disassembleBackwardEmptyTitle'),
        detail: t('appStore.checkpoint.candidateCountForRip', { count: candidates.length, address }),
        status: candidates.length > 0 ? 'checkpoint' : 'warning',
        tool: 'disassembleBackward',
        risk: 'patch',
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit('checkpoint_disassemble_backward_executed', { address, candidateCount: candidates.length, success: result.success === true })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('appStore.checkpoint.disassembleBackwardFailed'), String(e), 'error')
      return null
    }
  }

  // Teste automatiquement lequel des champs candidats de disassembleBackward
  // est la vraie source d'un compteur animé : écrit une valeur test sur
  // chaque champ résolu, attend, relit, classe holds/reverts, puis restaure
  // — remplace la lecture manuelle d'assembleur par une preuve empirique.
  // writeInstructionAddressHex = RIP de l'écriture capturée (comme
  // disassembleBackward), knownWriteTargetAddressHex = hit.address (adresse
  // mémoire réellement écrite, connue depuis findWhatWrites).
  async function executeCandidateFieldTest(writeInstructionAddressHex: string, knownWriteTargetAddressHex: string) {
    if (!writeInstructionAddressHex || !knownWriteTargetAddressHex) {
      addActionLog('checkpoint', t('appStore.checkpoint.candidateTestImpossible'), t('appStore.checkpoint.missingRipOrWriteAddress'), 'warning')
      return null
    }
    if (!await confirmRiskAction(
      'write',
      t('appStore.checkpoint.candidateTestTitle'),
      t('appStore.checkpoint.candidateTestDesc', { address: writeInstructionAddressHex }),
    )) return null

    const controller = backend.getController()
    const testCandidateFieldsAsync = controller.testCandidateFieldsAsync
    const candidateFieldTestFinished = controller.candidateFieldTestFinished
    if (!testCandidateFieldsAsync || !candidateFieldTestFinished) {
      addActionLog('checkpoint', t('appStore.checkpoint.candidateTestUnavailable'), t('appStore.checkpoint.backendNotExposed'), 'warning')
      return null
    }

    try {
      const result = await new Promise<CandidateFieldTestResult>((resolve) => {
        let requestId: number | null = null
        let settled = false
        const earlyPayloads: CandidateFieldTestResult[] = []
        const timeout = window.setTimeout(() => {
          settled = true
          candidateFieldTestFinished.disconnect?.(handler)
          resolve({ success: false, error: t('appStore.checkpoint.candidateTestTimeout') })
        }, 75000)

        const handler = (payload: CandidateFieldTestResult) => {
          if (requestId === null) {
            earlyPayloads.push(payload)
            return
          }
          if (Number(payload.requestId) !== requestId) return
          settled = true
          window.clearTimeout(timeout)
          candidateFieldTestFinished.disconnect?.(handler)
          resolve(payload)
        }
        candidateFieldTestFinished.connect(handler)

        void testCandidateFieldsAsync(writeInstructionAddressHex, knownWriteTargetAddressHex, {}).then((start) => {
          if (settled) return
          if (start.success !== true || start.started !== true) {
            settled = true
            window.clearTimeout(timeout)
            candidateFieldTestFinished.disconnect?.(handler)
            resolve({ success: false, error: String(start.error ?? t('appStore.checkpoint.candidateTestStartFailed')) })
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
          candidateFieldTestFinished.disconnect?.(handler)
          resolve({ success: false, error: String(error) })
        })
      })

      const outcomes = Array.isArray(result.results) ? result.results : []
      const holding = outcomes.filter((o) => o.verdict === 'holds')
      if (holding.length > 0 && activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...holding.map((field) => ({
            kind: 'candidate_field_verdict',
            label: t('appStore.checkpoint.candidateFieldHoldsLabel', { register: field.memBaseRegister, offset: (field.memDisplacement ?? 0).toString(16) }),
            address: field.address,
            sourceAddress: writeInstructionAddressHex,
            valueType: field.valueType,
            confidenceScore: 95,
            confidenceLabel: t('appStore.checkpoint.empiricallyTestedLabel', { count: field.ticksSurvived ?? 0 }),
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: holding.length > 0 ? t('appStore.checkpoint.candidateTestFoundTitle') : t('appStore.checkpoint.candidateTestEmptyTitle'),
        detail: t('appStore.checkpoint.candidateTestDetail', { tested: outcomes.length, holding: holding.length }),
        status: holding.length > 0 ? 'checkpoint' : 'warning',
        tool: 'testCandidateFields',
        risk: 'write',
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit('checkpoint_test_candidate_fields_executed', {
        writeInstructionAddressHex,
        knownWriteTargetAddressHex,
        candidateCount: outcomes.length,
        holdingCount: holding.length,
        success: result.success === true,
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('appStore.checkpoint.candidateTestFailed'), String(e), 'error')
      return null
    }
  }

  // Roadmap section J — Speedhack. start() est le seul moment gardé par
  // confirmRiskAction (c'est l'instant de l'injection) ; setFactor() ne
  // re-confirme jamais — ajuster un slider déjà consenti ne doit pas ouvrir
  // un dialogue à chaque cran, même logique que le preset d'intervalle
  // freeze existant (setFreezeInterval).
  async function refreshSpeedhackStatus() {
    return speedhackStore.refreshSpeedhackStatus()
  }

  // Roadmap section B - interception de fonctions. Gate confirmRiskAction
  // ICI (pas encore extrait de app.ts) avant de déléguer à speedhackStore.
  async function startApiHook() {
    if (!await confirmRiskAction('injection', t('appStore.speedhack.interceptTitle', { module: apiHookModuleName.value, fn: apiHookFunctionName.value }), t('appStore.speedhack.interceptDesc', { module: apiHookModuleName.value, fn: apiHookFunctionName.value }))) return null
    return speedhackStore.startApiHook()
  }

  async function stopApiHook() {
    return speedhackStore.stopApiHook()
  }

  async function refreshApiHookStatus() {
    return speedhackStore.refreshApiHookStatus()
  }

  async function startSpeedhack(factor: number) {
    if (!await confirmRiskAction('injection', t('appStore.speedhack.enableTitle'), t('appStore.speedhack.enableDesc', { factor }))) return null
    const result = await speedhackStore.startSpeedhack(factor)
    logAiAudit('speedhack_start_executed', { factor, success: result?.success === true })
    return result
  }

  async function setSpeedhackFactor(factor: number) {
    return speedhackStore.setSpeedhackFactor(factor)
  }

  async function stopSpeedhack() {
    const result = await speedhackStore.stopSpeedhack()
    logAiAudit('speedhack_stop_executed', {})
    return result
  }

  // Coupe/rétablit le réseau du processus attaché (règle pare-feu Windows
  // dédiée à son exécutable, invite UAC). Même palier de risque que le
  // speedhack ('injection') : c'est une modification système, pas une
  // simple lecture/écriture mémoire. Gate confirmRiskAction ICI avant de
  // déléguer à speedhackStore.
  async function blockProcessNetwork() {
    if (!await confirmRiskAction('injection', t('appStore.network.blockProcessTitle'), t('appStore.network.blockProcessDesc', { name: processName.value || t('appStore.network.thisProcessFallback') }))) return null
    const result = await speedhackStore.blockProcessNetwork(processName.value)
    logAiAudit('network_block_executed', { success: result?.success === true })
    return result
  }

  async function unblockProcessNetwork() {
    const result = await speedhackStore.unblockProcessNetwork()
    logAiAudit('network_unblock_executed', { success: result?.success === true })
    return result
  }

  async function refreshProcessNetworkBlockStatus() {
    return speedhackStore.refreshProcessNetworkBlockStatus()
  }

  async function prepareCheckpointAob(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    if (!address) {
      addActionLog('checkpoint', t('appStore.checkpoint.aobImpossible'), t('appStore.checkpoint.missingInstructionAddress'), 'warning')
      return null
    }
    if (!await confirmRiskAction('patch', t('appStore.checkpoint.aobPatchTitle'), t('appStore.checkpoint.aobPatchDesc', { address }))) return null
    const controller = backend.getController()
    if (!controller.generateAobSignature || !controller.suggestCodePatches) {
      addActionLog('checkpoint', t('appStore.checkpoint.aobUnavailable'), t('appStore.checkpoint.backendNotExposed'), 'warning')
      return null
    }
    try {
      const signature = await controller.generateAobSignature(address, { beforeBytes: 0, length: 32 })
      const suggestions = await controller.suggestCodePatches(address, { maxBytes: 16 })
      const patchSuggestions = Array.isArray(suggestions.suggestions) ? suggestions.suggestions : []
      if (activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...patchSuggestions.slice(0, 4).map((patch) => {
            const patchRecord = patch as unknown as Record<string, unknown>
            return {
              kind: 'code_patch_suggestion',
              label: String(patchRecord.label ?? t('appStore.checkpoint.patchLabel', { address })),
              address,
              patchBytes: String(patchRecord.patchBytes ?? patchRecord.bytesText ?? ''),
              risk: String(patchRecord.risk ?? patchRecord.riskLevel ?? 'medium'),
              aobPattern: signature.pattern,
              requiresConfirmation: true,
            }
          }),
          {
            kind: 'aob_signature',
            label: t('appStore.checkpoint.aobSignatureLabel', { address }),
            address,
            aobPattern: signature.pattern,
            module: signature.module,
            moduleOffset: signature.moduleOffset,
            requiresConfirmation: true,
          },
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: t('appStore.checkpoint.aobPreparedTitle'),
        detail: t('appStore.checkpoint.aobPreparedDetail', { count: patchSuggestions.length, pattern: String(signature.pattern ?? '').slice(0, 80) }),
        status: patchSuggestions.length > 0 ? 'checkpoint' : 'warning',
        tool: 'generateAobSignature/suggestCodePatches',
        risk: 'patch',
        payload: { signature, suggestions },
      })
      logAiAudit('checkpoint_aob_prepared', {
        address,
        patchSuggestionCount: patchSuggestions.length,
        success: signature.success === true,
        aobPattern: String(signature.pattern ?? ''),
        module: String(signature.module ?? ''),
        moduleOffset: String(signature.moduleOffset ?? ''),
      })
      return { signature, suggestions }
    } catch (e) {
      addActionLog('checkpoint', t('appStore.checkpoint.aobFailed'), String(e), 'error')
      return null
    }
  }

  async function executeCheckpointForceValue(checkpoint: Record<string, unknown>, value: string) {
    const address = checkpointAddress(checkpoint)
    const trimmedValue = value.trim()
    if (!address || !trimmedValue) {
      addActionLog('checkpoint', t('appStore.checkpoint.forceValueImpossible'), t('appStore.checkpoint.missingRipOrValue'), 'warning')
      return null
    }
    const controller = backend.getController()
    if (!controller.suggestCodePatches || !controller.forceWriteInstructionValue) {
      addActionLog('checkpoint', t('appStore.checkpoint.forceValueUnavailable'), t('appStore.checkpoint.backendNotExposed'), 'warning')
      return null
    }
    try {
      const suggestions = await controller.suggestCodePatches(address, { maxBytes: 16 })
      const memBaseRegister = String(suggestions.memBaseRegister ?? '').trim()
      if (!memBaseRegister) {
        addActionLog('checkpoint', t('appStore.checkpoint.forceValueImpossible'), t('appStore.checkpoint.noExploitableMemoryTarget'), 'warning')
        return null
      }
      const type = checkpointType(checkpoint)
      if (!await confirmRiskAction('patch', t('appStore.checkpoint.forceValueHookTitle'), t('appStore.checkpoint.forceValueHookDesc', { address, type, value: trimmedValue }))) return null
      const result = await controller.forceWriteInstructionValue(
        address,
        Number(suggestions.instructionLength ?? 0),
        memBaseRegister,
        Number(suggestions.memDisplacement ?? 0),
        type,
        trimmedValue,
      )
      addActionLog(
        'checkpoint',
        result.success === true ? t('appStore.checkpoint.forceValueHookOk') : t('appStore.checkpoint.forceValueHookFailed'),
        String(result.error || `0x${address} ${type} = ${trimmedValue}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? t('appStore.checkpoint.forceValueExecutedTitle') : t('appStore.checkpoint.forceValueFailedTitle'),
        detail: String(result.error || t('appStore.checkpoint.forceValueTrampolineDetail', { address, type, value: trimmedValue })),
        status: result.success === true ? 'success' : 'error',
        tool: 'forceWriteInstructionValue',
        risk: 'patch',
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit('checkpoint_force_value_executed', {
        success: result.success === true,
        address,
        type,
        value: trimmedValue,
        error: result.error ?? '',
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', t('appStore.checkpoint.forceValueHookFailed'), String(e), 'error')
      return null
    }
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
    scanningStore.setScanProgress(percent)
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
      if (!backendHotkeySignalConnected) {
        controller.globalHotkeyTriggered?.connect((event) => {
          void handleGlobalHotkey(event)
        })
        backendHotkeySignalConnected = true
      }
      if (!backendFreezeInstabilitySignalConnected) {
        // Détection automatique côté C++ (applyFreezeTick) : un freeze par
        // polling qui ne tient pas se signale tout seul, sans que
        // l'utilisateur ait besoin de le remarquer et de le décrire.
        controller.freezeInstabilityDetected?.connect((info) => {
          const address = String(info.address ?? '')
          const key = normalizeSessionAddress(address).toLowerCase()
          if (freezeInstabilityNotified.has(key)) return
          freezeInstabilityNotified.add(key)
          freezeInstabilityVersion.value += 1
          pushMessage(
            'assistant',
            String(info.message ?? t('appStore.signals.freezeDoesNotHold', { address })) + ' ' + String(info.suggestion ?? ''),
            {
              recoveryActions: [
                {
                  id: 'escalate_freeze_bp',
                  label: t('appStore.signals.escalateToFreezeBp'),
                  address,
                  requiresConfirmation: true,
                },
                { id: 'open_expert', label: t('appStore.signals.openExpert') },
              ],
            },
          )
        })
        backendFreezeInstabilitySignalConnected = true
      }
      if (!backendWriteWatchSignalConnected) {
        // Détection automatique côté C++ (applyWriteWatchTick) : une valeur
        // écrite (manuellement ou par un auto-write du chat) qui repart
        // toute seule dans les secondes qui suivent déclenche directement le
        // chemin "Écrit par" déjà câblé (find_what_writes_targets), au lieu
        // d'attendre que l'utilisateur remarque que ça n'a pas tenu.
        controller.writeDidNotHold?.connect((info) => {
          const address = String(info.address ?? '')
          const type = String(info.type ?? 'Int32')
          pushMessage(
            'assistant',
            String(info.message ?? t('appStore.signals.valueChangedByItself', { address })) + ' ' + String(info.suggestion ?? ''),
            {
              recoveryActions: [
                {
                  id: 'find_what_writes_targets',
                  label: t('appStore.signals.captureWhoWrites'),
                  address,
                  type,
                },
                { id: 'open_expert', label: t('appStore.signals.openExpert') },
              ],
            },
          )
        })
        backendWriteWatchSignalConnected = true
      }
      if (!backendEdrKillSignalConnected) {
        // EDR test : le processus cible a été tué par l'EDR pendant le test
        controller.processKilledByEdr?.connect((info) => {
          const pid = Number(info.pid ?? 0)
          const name = String(info.processName ?? t('appStore.signals.unknownProcessName'))
          pushMessage(
            'assistant',
            t('appStore.signals.edrKilledProcess', { name, pid }),
            {
              recoveryActions: [
                { id: 'add_edr_exclusion', label: t('appStore.signals.addDefenderExclusion') },
                { id: 'open_expert', label: t('appStore.signals.openExpert') },
              ],
            },
          )
        })
        backendEdrKillSignalConnected = true
      }
      if (!backendDnsSpoofSignalConnected) {
        // Spoof DNS non bloquant (voir spoofDnsAsync/restoreDnsAsync) :
        // résultat différé relayé vers le store Réseau.
        controller.dnsSpoofFinished?.connect((result) => {
          networkStore.onDnsSpoofFinished(result)
        })
        controller.dnsRestoreFinished?.connect((result) => {
          networkStore.onDnsRestoreFinished(result)
        })
        backendDnsSpoofSignalConnected = true
      }
      if (!backendNetworkInjectionSignalConnected) {
        // Proxy HTTP / lag switch non bloquants (voir startHttpProxyAsync/
        // stopHttpProxyAsync/setLagSwitchAsync) : résultat différé relayé
        // vers le store Réseau.
        controller.httpProxyStartFinished?.connect((result) => {
          networkStore.onHttpProxyStartFinished(result)
        })
        controller.httpProxyStopFinished?.connect((result) => {
          networkStore.onHttpProxyStopFinished(result)
        })
        controller.lagSwitchFinished?.connect((result) => {
          networkStore.onLagSwitchFinished(result)
        })
        backendNetworkInjectionSignalConnected = true
      }
      if (!backendSpeedhackApiHookSignalConnected) {
        // Speedhack / API hook non bloquants (voir startSpeedhackAsync/
        // startApiHookAsync/stopApiHookAsync) : résultat différé relayé vers
        // le store Speedhack.
        controller.speedhackStartFinished?.connect((result) => {
          speedhackStore.onSpeedhackStartFinished(result)
        })
        controller.apiHookStartFinished?.connect((result) => {
          speedhackStore.onApiHookStartFinished(result)
        })
        controller.apiHookStopFinished?.connect((result) => {
          speedhackStore.onApiHookStopFinished(result)
        })
        backendSpeedhackApiHookSignalConnected = true
      }
      if (!backendElevatedNetworkActionsSignalConnected) {
        // Exclusion Defender / blocage réseau non bloquants (voir
        // requestWindowsDefenderExclusionAsync/blockProcessNetworkAsync/
        // unblockProcessNetworkAsync) : résultat différé.
        controller.windowsDefenderExclusionRequestFinished?.connect((result) => {
          onWindowsDefenderExclusionRequestFinished(result)
        })
        controller.processNetworkBlockFinished?.connect((result) => {
          speedhackStore.onBlockProcessNetworkFinished(result)
        })
        controller.processNetworkUnblockFinished?.connect((result) => {
          speedhackStore.onUnblockProcessNetworkFinished(result)
        })
        backendElevatedNetworkActionsSignalConnected = true
      }
      if (!backendNetworkConnectionsSignalConnected) {
        // Liste des connexions réseau non bloquante (voir
        // getProcessNetworkConnectionsAsync) : résultat différé relayé vers
        // le store Réseau.
        controller.processNetworkConnectionsFinished?.connect((result) => {
          networkStore.onNetworkConnectionsFinished(result)
        })
        backendNetworkConnectionsSignalConnected = true
      }
      if (!backendModuleInstallSignalConnected) {
        // Vue "Modules" : progression des installations de module en cours
        // (téléchargement GGUF, scripts PowerShell) — le résultat final passe
        // par moduleInstallFinished, attendu dans installModule().
        controller.moduleInstallProgress?.connect((progress) => {
          if (moduleInstallBusy.value) {
            moduleInstallProgress.value = String(progress.message ?? '')
          }
        })
        backendModuleInstallSignalConnected = true
      }
      if (!backendClaudePendingActionSignalConnected) {
        // Backend IA externe (T4, docs/EXTERNAL_AI_BACKEND_ROADMAP.md) : le
        // backend Claude met sa boucle agentique en pause (C++, bloquant)
        // en attendant une action frontend, puis reprend dès que
        // resolveClaudePendingAction() est appelé. Deux natures d'action :
        // - "confirm_and_execute_in_cpp" : confirmation RiskGate réelle,
        //   la vraie écriture/action a lieu ENSUITE côté C++ (pas ici) ;
        // - "trainer_*" : aucun Q_INVOKABLE équivalent n'existe pour le CRUD
        //   Trainer (purement côté Pinia) -- l'action réelle a lieu ICI.
        controller.claudePendingActionRequested?.connect(async (request) => {
          const pendingId = String(request.pendingId ?? '')
          if (!pendingId) return
          const kind = String(request.kind ?? '')
          const args = (request.args as Record<string, unknown>) ?? {}

          if (kind === 'confirm_and_execute_in_cpp') {
            const risk = (request.risk as Parameters<typeof confirmRiskAction>[0]) ?? 'injection'
            const description = String(request.description ?? t('appStore.claudeAction.defaultDescription'))
            const approved = await confirmRiskAction(risk, t('appStore.claudeAction.confirmTitle'), description)
            await controller.resolveClaudePendingAction?.(pendingId, { approved })
            return
          }

          if (kind === 'trainer_list_features') {
            await controller.resolveClaudePendingAction?.(pendingId, {
              success: true,
              features: trainerStore.trainerFeatures.map((f) => ({
                id: f.id,
                name: f.name,
                action: f.action,
                address: f.address,
                valueType: f.valueType,
                value: f.value,
                enabled: f.enabled,
                locatorKind: f.locatorKind,
              })),
            })
            return
          }

          if (kind === 'trainer_create_write') {
            const locator = (request.locator as Record<string, unknown>) ?? {}
            const feature = trainerStore.createTrainerFeature({
              name: t('appStore.claudeAction.trainerWriteFeatureName'),
              action: 'write',
              address: String(args.address ?? ''),
              valueType: String(args.valueType ?? 'Int32'),
              value: String(args.value ?? ''),
              locatorKind: locator.locatorKind as TrainerFeature['locatorKind'] | undefined,
              aobPattern: locator.aobPattern as string | undefined,
              pointerChain: locator.pointerChain as TrainerFeature['pointerChain'] | undefined,
            })
            await controller.resolveClaudePendingAction?.(pendingId, {
              success: !!feature,
              error: feature ? undefined : t('appStore.claudeAction.missingAddressOrRefused'),
              feature: feature ? { id: feature.id, name: feature.name, locatorKind: feature.locatorKind } : undefined,
            })
            return
          }

          if (kind === 'trainer_delete_feature') {
            const id = Number(args.id)
            trainerStore.deleteTrainerFeature(id)
            await controller.resolveClaudePendingAction?.(pendingId, { success: true })
            return
          }

          if (kind === 'trainer_apply_request' || kind === 'trainer_restore_request') {
            const id = Number(args.id)
            const all = args.all === true
            if (kind === 'trainer_apply_request') {
              if (all) await trainerStore.applyAllTrainerFeatures()
              else await trainerStore.applyTrainerFeature(id)
            } else {
              if (all) await trainerStore.restoreAllTrainerFeatures()
              else await trainerStore.restoreTrainerFeature(id)
            }
            const feature = !all ? trainerStore.trainerFeatures.find((f) => f.id === id) : undefined
            await controller.resolveClaudePendingAction?.(pendingId, {
              success: all || (kind === 'trainer_apply_request' ? feature?.enabled === true : feature?.enabled === false),
              error: !all ? feature?.lastError || undefined : undefined,
            })
            return
          }

          await controller.resolveClaudePendingAction?.(pendingId, {
            success: false,
            error: t('appStore.claudeAction.unknownActionType', { kind }),
          })
        })
        backendClaudePendingActionSignalConnected = true
      }
      version.value = await controller.getVersion()
      loadActionLog()
      loadInvestigations()
      loadTrainerFeatures()
      loadOverlayHotkey()
      await reregisterPersistedHotkeys()
      loadStructureTemplates()
      loadWorkspaceBookmarks()
      loadWorkspaceProjects()
      await loadSettings()
      await refreshKernelDriverStatus()
      await refreshExternalAiStatus()
      await refreshLuaScriptingStatus()
      await refreshActiveChatMemoryTargets()
      await refreshSmartSearchContext()
      showOnboarding.value = controller.hasSeenOnboarding ? !(await controller.hasSeenOnboarding()) : false
      console.log('[KillEngine] Version:', version.value)
    } catch (e) {
      console.error('[KillEngine] Backend connection failed:', e)
    }
  }

  async function dismissOnboarding() {
    showOnboarding.value = false
    const controller = backend.getController()
    await controller.setOnboardingSeen?.(true)
  }

  async function openUserGuide() {
    await backend.getController().openUserGuide?.()
  }

  const defenderExclusionResult = ref<{ success: boolean; cancelled?: boolean; error?: string } | null>(null)
  const defenderExclusionBusy = ref(false)

  // requestWindowsDefenderExclusionAsync ne bloque plus le thread GUI
  // (élévation UAC + Add-MpPreference jusqu'à 15s sur thread séparé côté
  // backend) : elle renvoie juste {started:true} immédiatement, le vrai
  // résultat arrive via le signal windowsDefenderExclusionRequestFinished
  // (branché plus bas dans init()) — busy reste true jusque-là.
  async function requestWindowsDefenderExclusion() {
    defenderExclusionBusy.value = true
    defenderExclusionResult.value = null
    try {
      const result = await backend.getController().requestWindowsDefenderExclusionAsync?.()
      if (!result?.started) {
        defenderExclusionBusy.value = false
        defenderExclusionResult.value = result ?? { success: false, error: t('appStore.errors.backendResponseMissing') }
      }
    } catch (e) {
      defenderExclusionBusy.value = false
      defenderExclusionResult.value = { success: false, error: String(e) }
    }
  }

  function onWindowsDefenderExclusionRequestFinished(result: { success: boolean; cancelled?: boolean; error?: string }) {
    defenderExclusionBusy.value = false
    defenderExclusionResult.value = result
  }

  async function refreshKernelDriverStatus() {
    return kernelDriverStore.refreshKernelDriverStatus()
  }

  async function startKernelDriver() {
    const result = await kernelDriverStore.startKernelDriver()
    if (result) {
      logAiAudit('kernel_driver_start', {
        success: result.success,
        status: result.status,
        started: result.started === true,
        alreadyRunning: result.alreadyRunning === true,
      })
    }
    return result
  }

  async function readMemoryKernel(addressHex: string, size: number) {
    const result = await kernelDriverStore.readMemoryKernel(addressHex, size)
    if (result) {
      logAiAudit('kernel_memory_read', { address: addressHex, size, success: result.success })
    }
    return result
  }

  // Contourne les protections mémoire usermode normales (VirtualProtect,
  // PAGE_GUARD) en écrivant directement depuis le ring 0 -- traité comme
  // une injection, le palier de risque le plus strict déjà utilisé dans
  // ce store (voir confirmRiskAction), pas comme un simple 'write'. Gate
  // gardé ICI avant de déléguer à kernelDriverStore.
  async function writeMemoryKernel(addressHex: string, hexBytes: string) {
    if (!await confirmRiskAction('injection', t('appStore.kernel.writeTitle'), t('appStore.kernel.writeDesc', { address: addressHex, bytes: hexBytes.trim() }))) return
    const result = await kernelDriverStore.writeMemoryKernel(addressHex, hexBytes)
    if (result) {
      logAiAudit('kernel_memory_write', { address: addressHex, bytes: hexBytes, success: result.success })
    }
    return result
  }

  function setMemoryAccessMode(mode: 'standard' | 'kernel') {
    memoryAccessMode.value = mode
    addActionLog(
      'memory_access_mode',
      mode === 'kernel' ? t('appStore.kernel.modeKernelTitle') : t('appStore.kernel.modeStandardTitle'),
      mode === 'kernel'
        ? t('appStore.kernel.modeKernelDetail')
        : t('appStore.kernel.modeStandardDetail'),
      'info',
    )
  }

  function kernelUnavailableResult(): MemoryWriteResult {
    return {
      success: false,
      verified: false,
      bytesWritten: 0,
      error: t('appStore.kernel.driverAccessUnavailable'),
    }
  }

  async function writeMemoryValueByMode(address: string, type: string, value: string): Promise<MemoryWriteResult> {
    const controller = backend.getController()
    if (kernelMemoryModeActive.value) {
      if (!kernelMemoryReady.value || !controller.writeMemoryValueKernel) return kernelUnavailableResult()
      const result = await controller.writeMemoryValueKernel(address, type, value)
      kernelMemoryWriteResult.value = result
      return {
        success: result.success,
        verified: result.success,
        bytesWritten: result.bytesWritten ?? 0,
        error: result.error ?? '',
      }
    }
    return controller.writeMemoryValue(address, type, value)
  }

  async function readMemoryPreviewByMode(addressHex: string, size: number): Promise<MemoryReadPreview> {
    const controller = backend.getController()
    if (kernelMemoryModeActive.value) {
      if (!kernelMemoryReady.value || !controller.readMemoryKernel) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          bytesRead: 0,
          requestedBytes: size,
          error: t('appStore.kernel.readUnavailable'),
          hex: '',
        }
      }
      const result = await controller.readMemoryKernel(addressHex, size)
      kernelMemoryReadResult.value = result
      return {
        success: result.success,
        partial: false,
        cancelled: false,
        bytesRead: result.bytesRead ?? 0,
        requestedBytes: size,
        error: result.error ?? '',
        hex: result.hex ?? '',
      }
    }
    return controller.readMemoryPreview(addressHex, size)
  }

  async function writeMemoryHexByMode(addressHex: string, hexString: string): Promise<Record<string, unknown>> {
    const controller = backend.getController()
    if (kernelMemoryModeActive.value) {
      if (!kernelMemoryReady.value || !controller.writeMemoryKernel) {
        return { success: false, error: t('appStore.kernel.hexWriteUnavailable') }
      }
      const result = await controller.writeMemoryKernel(addressHex, hexString)
      kernelMemoryWriteResult.value = result
      return result as unknown as Record<string, unknown>
    }
    if (!controller.writeMemoryHex) {
      return { success: false, error: t('appStore.kernel.writeMemoryHexUnavailable') }
    }
    return controller.writeMemoryHex(addressHex, hexString)
  }

  async function refreshProcesses() {
    try {
      processes.value = await backend.getController().getProcesses()
    } catch (e) {
      console.error('[KillEngine] Failed to get processes:', e)
    }
  }

  async function refreshLuaScriptingStatus() {
    const controller = backend.getController()
    if (!controller.getLuaScriptingStatus) {
      luaScriptingStatus.value = {
        success: false,
        available: false,
        helperAvailable: false,
        message: t('appStore.lua.scriptingNotExposed'),
      }
      return
    }
    try {
      luaScriptingStatus.value = await controller.getLuaScriptingStatus()
    } catch (e) {
      luaScriptingStatus.value = {
        success: false,
        available: false,
        helperAvailable: false,
        error: String(e),
      }
    }
  }

  function logLuaScriptOutcome(payload: LuaScriptRunResult) {
    const ok = payload.success === true
    const label = ok ? t('appStore.lua.scriptExecuted') : (payload.cancelled ? t('appStore.lua.scriptCancelled') : t('appStore.lua.scriptFailed'))
    addActionLog(
      'lua_script',
      label,
      String(payload.error ?? payload.stdout ?? '').trim(),
      ok ? 'success' : (payload.cancelled ? 'warning' : 'error'),
    )
  }

  async function executeLuaScript() {
    const script = luaScriptText.value
    if (!script.trim() || luaScriptBusy.value) return
    if (!await confirmRiskAction(
      'injection',
      t('appStore.lua.executeScriptTitle'),
      t('appStore.lua.executeScriptDesc'),
    )) return

    const controller = backend.getController()
    const options = {
      timeoutMs: luaScriptTimeoutMs.value,
      pipeName: luaScriptingStatus.value?.pipeName ?? 'KillEngineAutomationPipe',
    }

    const asyncFn = controller.executeLuaScriptAsync
    const finishedSignal = controller.luaScriptExecutionFinished
    if (asyncFn && finishedSignal) {
      luaScriptBusy.value = true
      luaScriptResult.value = null
      luaScriptRequestId.value = null
      await new Promise<void>((resolve) => {
        let requestId: number | null = null
        let settled = false
        const earlyPayloads: Array<Record<string, unknown>> = []
        const watchdog = window.setTimeout(() => {
          if (settled) return
          settled = true
          finishedSignal.disconnect?.(handler)
          luaScriptBusy.value = false
          luaScriptRequestId.value = null
          luaScriptResult.value = { success: false, error: t('appStore.lua.scriptTimeout') }
          logLuaScriptOutcome(luaScriptResult.value)
          resolve()
        }, luaScriptTimeoutMs.value + 5000)

        const handler = (payload: Record<string, unknown>) => {
          if (requestId === null) {
            earlyPayloads.push(payload)
            return
          }
          if (Number(payload.requestId) !== requestId) return
          settled = true
          window.clearTimeout(watchdog)
          finishedSignal.disconnect?.(handler)
          luaScriptBusy.value = false
          luaScriptRequestId.value = null
          luaScriptResult.value = payload as unknown as LuaScriptRunResult
          logLuaScriptOutcome(luaScriptResult.value)
          resolve()
        }
        finishedSignal.connect(handler)

        asyncFn(script, options).then((start) => {
          if (settled) return
          if (start.success !== true || start.started !== true) {
            settled = true
            window.clearTimeout(watchdog)
            finishedSignal.disconnect?.(handler)
            luaScriptBusy.value = false
            luaScriptResult.value = { success: false, error: String(start.error ?? t('appStore.lua.scriptStartFailed')) }
            logLuaScriptOutcome(luaScriptResult.value)
            resolve()
            return
          }
          requestId = Number(start.requestId)
          luaScriptRequestId.value = requestId
          for (const payload of earlyPayloads.splice(0)) {
            handler(payload)
            if (settled) break
          }
        }).catch((e) => {
          if (settled) return
          settled = true
          window.clearTimeout(watchdog)
          finishedSignal.disconnect?.(handler)
          luaScriptBusy.value = false
          luaScriptResult.value = { success: false, error: String(e) }
          logLuaScriptOutcome(luaScriptResult.value)
          resolve()
        })
      })
      return
    }

    if (!controller.executeLuaScript) {
      luaScriptResult.value = { success: false, error: t('appStore.lua.executeNotExposed') }
      return
    }

    luaScriptBusy.value = true
    try {
      luaScriptResult.value = await controller.executeLuaScript(script, options)
      logLuaScriptOutcome(luaScriptResult.value)
    } catch (e) {
      luaScriptResult.value = { success: false, error: String(e) }
      addActionLog('lua_script', t('appStore.lua.scriptFailed'), String(e), 'error')
    } finally {
      luaScriptBusy.value = false
    }
  }

  async function cancelLuaScriptExecution() {
    const controller = backend.getController()
    if (!controller.cancelLuaScriptExecution) {
      addActionLog('lua_script', t('appStore.lua.cancelUnavailable'), t('appStore.lua.cancelFunctionMissing'), 'warning')
      return
    }
    try {
      const result = await controller.cancelLuaScriptExecution()
      if (result.success !== true) {
        addActionLog('lua_script', t('appStore.lua.cancelImpossible'), String(result.error ?? ''), 'warning')
      }
    } catch (e) {
      addActionLog('lua_script', t('appStore.lua.cancelImpossible'), String(e), 'warning')
    }
  }

  async function startLuaReplSession() {
    if (luaReplActive.value) return
    if (!await confirmRiskAction(
      'injection',
      t('appStore.lua.startReplTitle'),
      t('appStore.lua.startReplDesc'),
    )) return

    const controller = backend.getController()
    if (!controller.startLuaRepl) {
      luaReplStartResult.value = { success: false, error: t('appStore.lua.replNotExposed') }
      return
    }
    try {
      const result = await controller.startLuaRepl({
        timeoutMs: luaScriptTimeoutMs.value,
        pipeName: luaScriptingStatus.value?.pipeName ?? 'KillEngineAutomationPipe',
      })
      luaReplStartResult.value = result
      luaReplActive.value = result.success === true
      luaReplHistory.value = []
      if (luaReplActive.value) {
        addActionLog('lua_repl', t('appStore.lua.replStarted'), String(result.luaPath ?? ''), 'success')
        void refreshLuaReplCompletions('')
      } else {
        addActionLog('lua_repl', t('appStore.lua.replNotStarted'), String(result.error ?? ''), 'error')
      }
    } catch (e) {
      luaReplStartResult.value = { success: false, error: String(e) }
      addActionLog('lua_repl', t('appStore.lua.replNotStarted'), String(e), 'error')
    }
  }

  async function stopLuaReplSession() {
    const controller = backend.getController()
    if (!controller.stopLuaRepl) return
    try {
      await controller.stopLuaRepl()
    } catch (e) {
      addActionLog('lua_repl', t('appStore.lua.replStopFailed'), String(e), 'warning')
    } finally {
      luaReplActive.value = false
      luaReplBusy.value = false
    }
  }

  async function refreshLuaReplCompletions(prefix: string) {
    const controller = backend.getController()
    if (!controller.getLuaReplCompletions) return
    try {
      const result = await controller.getLuaReplCompletions(prefix)
      luaReplCompletions.value = result.success === true
        ? ((result.completions as string[]) ?? [])
        : []
    } catch {
      luaReplCompletions.value = []
    }
  }

  async function sendLuaReplLine() {
    const line = luaReplInput.value
    if (!line.trim() || luaReplBusy.value || !luaReplActive.value) return
    luaReplRecall.value.push(line)
    luaReplRecallIndex.value = null
    luaReplInput.value = ''

    const controller = backend.getController()
    if (!controller.sendLuaReplLine) {
      addActionLog('lua_repl', t('appStore.lua.replUnavailable'), t('appStore.lua.replLineFunctionMissing'), 'error')
      return
    }

    luaReplBusy.value = true
    const finishedSignal = controller.luaReplLineFinished
    const pendingEntry: LuaReplHistoryEntry = { requestId: -1, line, output: '', error: '', finished: false }
    luaReplHistory.value.push(pendingEntry)

    await new Promise<void>((resolve) => {
      let requestId: number | null = null
      let settled = false
      const timeoutMs = luaScriptTimeoutMs.value + 5000
      const earlyPayloads: Array<Record<string, unknown>> = []

      const finish = (payload: Record<string, unknown>) => {
        if (settled) return
        settled = true
        window.clearTimeout(watchdog)
        finishedSignal?.disconnect?.(handler)
        luaReplBusy.value = false
        pendingEntry.requestId = Number(payload.requestId ?? requestId ?? 0)
        pendingEntry.output = String(payload.output ?? '')
        pendingEntry.error = String(payload.error ?? '')
        pendingEntry.finished = true
        pendingEntry.elapsedMs = Number(payload.elapsedMs ?? 0)
        resolve()
      }

      const watchdog = window.setTimeout(() => {
        if (settled) return
        pendingEntry.error = t('appStore.lua.replLineTimeout')
        finish({})
      }, timeoutMs)

      // Même payload que luaScriptExecutionFinished : "requestId" est une clé
      // du payload, pas un argument de signal séparé (QWebChannelSignal<T>
      // ne modélise qu'un seul payload).
      const handler = (payload: Record<string, unknown>) => {
        if (requestId === null) {
          earlyPayloads.push(payload)
          return
        }
        if (Number(payload.requestId) !== requestId) return
        finish(payload)
      }
      finishedSignal?.connect?.(handler)

      controller.sendLuaReplLine!(line).then(async (start) => {
        if (settled) return
        if (start.success !== true || start.started !== true) {
          pendingEntry.error = String(start.error ?? t('appStore.lua.replLineSendFailed'))
          finish({})
          return
        }
        requestId = Number(start.requestId)
        for (const payload of earlyPayloads.splice(0)) {
          handler(payload)
          if (settled) return
        }
        // Le pipe d'automatisation ne relaie pas les signaux Qt (voir doc backend) --
        // si aucun signal n'est disponible, on bascule sur le poll getLuaReplLineResult.
        if (!finishedSignal?.connect) {
          while (!settled) {
            await new Promise((r) => window.setTimeout(r, 200))
            if (settled) return
            const polled = await controller.getLuaReplLineResult!(requestId)
            if (polled.found === true && polled.finished === true) {
              finish(polled)
              return
            }
          }
        }
      }).catch((e) => {
        if (settled) return
        pendingEntry.error = String(e)
        finish({})
      })
    })
  }

  function recallLuaReplHistory(direction: -1 | 1) {
    if (luaReplRecall.value.length === 0) return
    const current = luaReplRecallIndex.value
    let next: number
    if (current === null) {
      next = direction === -1 ? luaReplRecall.value.length - 1 : luaReplRecall.value.length
    } else {
      next = current + direction
    }
    if (next < 0) next = 0
    if (next >= luaReplRecall.value.length) {
      luaReplRecallIndex.value = null
      luaReplInput.value = ''
      return
    }
    luaReplRecallIndex.value = next
    luaReplInput.value = luaReplRecall.value[next] ?? ''
  }

  // PROPOSITIONS-1 #3 — Memory Timeline (02/09/2026, Claude). Wrappers minces
  // au-dessus du backend, comme saveLuaScript/refreshLuaScriptingStatus —
  // MemoryTimelineView.vue garde son propre état local (watchedAddresses,
  // isCollecting, etc.), ces fonctions ne font que traduire la forme
  // QVariantMap du backend (success/error/...) vers ce que la vue attend.
  async function addTimelineAddress(addressHex: string, valueSize: number): Promise<boolean> {
    const controller = backend.getController()
    if (!controller.addTimelineAddress) return false
    try {
      const result = await controller.addTimelineAddress(addressHex, valueSize)
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.addressRefused'), String(result.error ?? addressHex), 'warning')
      }
      return result.success === true
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.addressRefused'), String(e), 'error')
      return false
    }
  }

  async function removeTimelineAddress(addressHex: string): Promise<void> {
    const controller = backend.getController()
    if (!controller.removeTimelineAddress) return
    try {
      await controller.removeTimelineAddress(addressHex)
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.addressRemoveFailed'), String(e), 'warning')
    }
  }

  function clearTimelineAddresses(): void {
    const controller = backend.getController()
    void controller.clearTimelineAddresses?.()
  }

  async function getTimelineWatchedAddresses(): Promise<string[]> {
    const controller = backend.getController()
    if (!controller.getTimelineWatchedAddresses) return []
    try {
      const result = await controller.getTimelineWatchedAddresses()
      return result.success === true ? ((result.addresses as string[]) ?? []) : []
    } catch {
      return []
    }
  }

  async function setTimelineConfig(config: Record<string, unknown>): Promise<void> {
    const controller = backend.getController()
    if (!controller.setTimelineConfig) return
    try {
      await controller.setTimelineConfig(config)
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.configFailed'), String(e), 'warning')
    }
  }

  async function startTimelineCollection(): Promise<boolean> {
    const controller = backend.getController()
    if (!controller.startTimelineCollection) return false
    try {
      const result = await controller.startTimelineCollection()
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.collectionNotStarted'), String(result.error ?? ''), 'warning')
      }
      return result.success === true
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.collectionNotStarted'), String(e), 'error')
      return false
    }
  }

  async function stopTimelineCollection(): Promise<void> {
    const controller = backend.getController()
    if (!controller.stopTimelineCollection) return
    try {
      await controller.stopTimelineCollection()
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.collectionStopFailed'), String(e), 'warning')
    }
  }

  async function getTimelineSeries(addressHex: string): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.getTimelineSeriesForAddress) return null
    try {
      const result = await controller.getTimelineSeriesForAddress(addressHex)
      return result.success === true ? (result.series as Record<string, unknown>) : null
    } catch {
      return null
    }
  }

  async function detectTimelinePatterns(addressHex: string): Promise<Array<Record<string, unknown>>> {
    const controller = backend.getController()
    if (!controller.detectTimelinePatterns) return []
    try {
      const result = await controller.detectTimelinePatterns(addressHex)
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.patternDetectionUnavailable'), String(result.error ?? ''), 'warning')
        return []
      }
      return (result.patterns as Array<Record<string, unknown>>) ?? []
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.patternDetectionUnavailable'), String(e), 'error')
      return []
    }
  }

  async function findVolatileTimelineAddresses(threshold: number): Promise<string[]> {
    const controller = backend.getController()
    if (!controller.findVolatileTimelineAddresses) return []
    try {
      const result = await controller.findVolatileTimelineAddresses(threshold)
      return result.success === true ? ((result.addresses as string[]) ?? []) : []
    } catch {
      return []
    }
  }

  async function findStableTimelineAddresses(minDurationMs: number): Promise<string[]> {
    const controller = backend.getController()
    if (!controller.findStableTimelineAddresses) return []
    try {
      const result = await controller.findStableTimelineAddresses(minDurationMs)
      return result.success === true ? ((result.addresses as string[]) ?? []) : []
    } catch {
      return []
    }
  }

  async function analyzeTimelineBehavior(addressHex: string): Promise<Record<string, unknown>> {
    const controller = backend.getController()
    const fallback = { changesPerSecond: 0, regularityScore: 0, hasBurstBehavior: false }
    if (!controller.analyzeTimelineBehavior) return fallback
    try {
      const result = await controller.analyzeTimelineBehavior(addressHex)
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.behaviorProfileUnavailable'), String(result.error ?? ''), 'warning')
        return fallback
      }
      return result
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.behaviorProfileUnavailable'), String(e), 'error')
      return fallback
    }
  }

  async function predictTimelineNextValue(addressHex: string): Promise<Record<string, unknown>> {
    const controller = backend.getController()
    const fallback = { changeProbability: 0, predictedValueHex: '0x0' }
    if (!controller.predictTimelineNextValue) return fallback
    try {
      const result = await controller.predictTimelineNextValue(addressHex)
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.predictionUnavailable'), String(result.error ?? ''), 'warning')
        return fallback
      }
      return result
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.predictionUnavailable'), String(e), 'error')
      return fallback
    }
  }

  async function findTimelineCorrelations(): Promise<Array<Record<string, unknown>>> {
    const controller = backend.getController()
    if (!controller.findTimelineCorrelations) return []
    try {
      const result = await controller.findTimelineCorrelations()
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.correlationsUnavailable'), String(result.error ?? ''), 'warning')
        return []
      }
      return (result.correlations as Array<Record<string, unknown>>) ?? []
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.correlationsUnavailable'), String(e), 'error')
      return []
    }
  }

  async function generateTimelineReport(): Promise<string | null> {
    const controller = backend.getController()
    if (!controller.generateTimelineReport) return null
    try {
      const result = await controller.generateTimelineReport()
      if (result.success !== true) {
        addActionLog('memory_timeline', t('appStore.timeline.reportGenerationUnavailable'), String(result.error ?? ''), 'warning')
        return null
      }
      return (result.report as string) ?? null
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.reportGenerationUnavailable'), String(e), 'error')
      return null
    }
  }

  async function exportTimelineToJson(): Promise<string | null> {
    const controller = backend.getController()
    if (!controller.exportTimelineToJson) return null
    try {
      const result = await controller.exportTimelineToJson()
      return result.success === true ? String(result.filepath ?? '') : null
    } catch (e) {
      addActionLog('memory_timeline', t('appStore.timeline.exportFailed'), String(e), 'error')
      return null
    }
  }

  // ANALYSE-CLINE-1 — Memory Heatmap (03/09/2026, Claude). Wrappers minces au
  // même patron que les fonctions Timeline ci-dessus : traduisent la forme
  // QVariantMap du backend, journalisent l'échec, ne dupliquent aucun état
  // (MemoryHeatmapView.vue garde son propre état local).
  async function startMemoryHeatmap(addressHex: string, options: Record<string, unknown>): Promise<boolean> {
    const controller = backend.getController()
    if (!controller.startMemoryHeatmap) return false
    try {
      const result = await controller.startMemoryHeatmap(addressHex, options)
      if (result.success !== true) {
        addActionLog('memory_heatmap', t('appStore.heatmap.notStarted'), String(result.error ?? ''), 'warning')
      }
      return result.success === true
    } catch (e) {
      addActionLog('memory_heatmap', t('appStore.heatmap.notStarted'), String(e), 'error')
      return false
    }
  }

  async function stopMemoryHeatmap(): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.stopMemoryHeatmap) return null
    try {
      return await controller.stopMemoryHeatmap()
    } catch (e) {
      addActionLog('memory_heatmap', t('appStore.heatmap.stopFailed'), String(e), 'warning')
      return null
    }
  }

  async function getMemoryHeatmapStatus(): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.getMemoryHeatmapStatus) return null
    try {
      return await controller.getMemoryHeatmapStatus()
    } catch {
      return null
    }
  }

  async function getMemoryHeatmapData(): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.getMemoryHeatmapData) return null
    try {
      return await controller.getMemoryHeatmapData()
    } catch {
      return null
    }
  }

  // ANALYSE-CLINE-1 — Pattern Learning (03/09/2026, Claude). Mêmes wrappers
  // minces. Le backend ne renvoie pas de champ `success` pour ces méthodes
  // (pass-through direct de PatternLearningManager) : un objet/tableau vide
  // signale un échec (moteur non initialisé, adresse invalide, profil
  // introuvable...), donc ces wrappers testent la présence de clés plutôt
  // que `result.success`.
  async function getPatternLearningStatistics(): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.getPatternLearningStatistics) return null
    try {
      const result = await controller.getPatternLearningStatistics()
      return Object.keys(result).length > 0 ? result : null
    } catch {
      return null
    }
  }

  async function detectGameEngine(moduleNames: string[]): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.detectGameEngine) return null
    try {
      const result = await controller.detectGameEngine(moduleNames, {})
      return Object.keys(result).length > 0 ? result : null
    } catch (e) {
      addActionLog('pattern_learning', t('appStore.patternLearning.engineDetectionFailed'), String(e), 'warning')
      return null
    }
  }

  async function classifyMemoryPattern(
    addressHex: string,
    valueHistory: number[],
    timestamps: number[],
  ): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.classifyMemoryPattern) return null
    try {
      const result = await controller.classifyMemoryPattern(addressHex, valueHistory, timestamps)
      return Object.keys(result).length > 0 ? result : null
    } catch (e) {
      addActionLog('pattern_learning', t('appStore.patternLearning.classificationFailed'), String(e), 'warning')
      return null
    }
  }

  async function loadGameProfile(gameName: string): Promise<Record<string, unknown> | null> {
    const controller = backend.getController()
    if (!controller.loadGameProfile) return null
    try {
      const result = await controller.loadGameProfile(gameName)
      return Object.keys(result).length > 0 ? result : null
    } catch (e) {
      addActionLog('pattern_learning', t('appStore.patternLearning.profileLoadFailed'), String(e), 'warning')
      return null
    }
  }

  async function saveGameProfile(profile: Record<string, unknown>): Promise<boolean> {
    const controller = backend.getController()
    if (!controller.saveGameProfile) return false
    try {
      const ok = await controller.saveGameProfile(profile)
      if (!ok) {
        addActionLog('pattern_learning', t('appStore.patternLearning.profileSaveFailed'), String(profile.gameName ?? ''), 'warning')
      }
      return ok
    } catch (e) {
      addActionLog('pattern_learning', t('appStore.patternLearning.profileSaveFailed'), String(e), 'error')
      return false
    }
  }

  async function listKnownGameProfiles(): Promise<string[]> {
    const controller = backend.getController()
    if (!controller.listKnownGameProfiles) return []
    try {
      return await controller.listKnownGameProfiles()
    } catch {
      return []
    }
  }

  async function deleteGameProfile(gameName: string): Promise<boolean> {
    const controller = backend.getController()
    if (!controller.deleteGameProfile) return false
    try {
      return await controller.deleteGameProfile(gameName)
    } catch (e) {
      addActionLog('pattern_learning', t('appStore.patternLearning.profileDeleteFailed'), String(e), 'warning')
      return false
    }
  }

  async function getTopPatternSuggestions(
    gameName: string,
    patternType: number,
    count: number,
  ): Promise<Array<Record<string, unknown>>> {
    const controller = backend.getController()
    if (!controller.getTopPatternSuggestions) return []
    try {
      return await controller.getTopPatternSuggestions(gameName, patternType, count)
    } catch (e) {
      addActionLog('pattern_learning', 'Suggestions indisponibles', String(e), 'warning')
      return []
    }
  }

  function luaProfileName(): string {
    return (processName.value || 'KillEngineTrainer')
      .replace(/\.[^.]+$/, '')
      .replace(/[^a-z0-9_.-]+/gi, '_')
      .slice(0, 80) || 'KillEngineTrainer'
  }

  async function refreshSavedLuaScripts() {
    const controller = backend.getController()
    luaSavedScriptsBusy.value = true
    try {
      const result = await controller.loadProfile(luaProfileName())
      luaSavedScripts.value = result.success
        ? ((result.luaScripts as Array<Record<string, unknown>>) ?? [])
        : []
    } catch (e) {
      luaSavedScripts.value = []
      console.error('[KillEngine] Failed to load saved Lua scripts:', e)
    } finally {
      luaSavedScriptsBusy.value = false
    }
  }

  async function saveLuaScript() {
    const script = luaScriptText.value
    const name = luaScriptSaveName.value.trim()
    if (!script.trim() || !name) return
    const controller = backend.getController()
    if (!controller.saveProfileLuaScript) {
      luaScriptSaveResult.value = { success: false, error: t('appStore.lua.saveNotExposed') }
      return
    }
    try {
      luaScriptSaveResult.value = await controller.saveProfileLuaScript(luaProfileName(), name, script, {})
      const ok = luaScriptSaveResult.value.success === true
      addActionLog('lua_script', ok ? t('appStore.lua.scriptSaved') : t('appStore.lua.scriptSaveFailed'), String(luaScriptSaveResult.value.error ?? name), ok ? 'success' : 'error')
      if (ok) await refreshSavedLuaScripts()
    } catch (e) {
      luaScriptSaveResult.value = { success: false, error: String(e) }
      addActionLog('lua_script', t('appStore.lua.scriptSaveFailed'), String(e), 'error')
    }
  }

  function loadSavedLuaScript(name: string) {
    const entry = luaSavedScripts.value.find((s) => s.name === name)
    if (!entry) return
    luaScriptText.value = String(entry.scriptText ?? '')
    luaScriptSaveName.value = name
    addActionLog('lua_script', t('appStore.lua.scriptLoaded', { name }), '', 'success')
  }

  async function deleteSavedLuaScript(name: string) {
    const controller = backend.getController()
    if (!controller.deleteProfileLuaScript) return
    try {
      const result = await controller.deleteProfileLuaScript(luaProfileName(), name)
      const ok = result.success === true
      addActionLog('lua_script', ok ? t('appStore.lua.scriptDeleted', { name }) : t('appStore.lua.scriptDeleteFailed', { name }), String(result.error ?? ''), ok ? 'success' : 'error')
      if (ok) await refreshSavedLuaScripts()
    } catch (e) {
      addActionLog('lua_script', t('appStore.lua.scriptDeleteFailed', { name }), String(e), 'error')
    }
  }

  async function attach(pid: number, mode: 'standard' | 'kernel' = memoryAccessMode.value) {
    try {
      clearSessionEntries()
      setMemoryAccessMode(mode)
      const ok = await backend.getController().attachProcess(pid)
      if (ok) {
        isAttached.value = true
        const proc = processes.value.find((p) => p.pid === pid)
        processName.value = proc?.name ?? `PID ${pid}`
        addActionLog(
          'attach',
          t('appStore.attach.title', { name: processName.value }),
          mode === 'kernel'
            ? t('appStore.attach.kernelModeDetail')
            : t('appStore.attach.standardModeDetail'),
          'success',
        )
      }
    } catch (e) {
      console.error('[KillEngine] Attach failed:', e)
    }
  }

  async function refreshClrInspectorStatus() {
    return clrInspectorStore.refreshClrInspectorStatus()
  }

  async function attachClrInspector() {
    return clrInspectorStore.attachClrInspector()
  }

  async function detachClrInspector() {
    return clrInspectorStore.detachClrInspector()
  }

  async function shutdownClrInspector() {
    return clrInspectorStore.shutdownClrInspector()
  }

  async function flushClrInspectorCache() {
    return clrInspectorStore.flushClrInspectorCache()
  }

  async function findClrObjects(typeSubstring = clrTypeFilter.value) {
    return clrInspectorStore.findClrObjects(typeSubstring)
  }

  async function findClrObjectsByFieldValue(typeSubstring: string, fieldName: string, expectedValue: string, maxResults = 20) {
    return clrInspectorStore.findClrObjectsByFieldValue(typeSubstring, fieldName, expectedValue, maxResults)
  }

  async function readClrObject(addressHex: string) {
    return clrInspectorStore.readClrObject(addressHex)
  }

  // Les 7 fonctions ci-dessous valident les entrées et appellent
  // confirmRiskAction ICI (pas encore extrait de app.ts) avant de déléguer
  // le travail réel (appel backend + addActionLog + éventuel readClrObject
  // de suivi) à clrInspectorStore -- même règle que app.ts vs
  // ui/src/stores/clrInspector.ts partout ailleurs dans ce fichier.
  async function writeClrPrimitiveField(objectAddressHex: string, fieldName: string, value: string) {
    const address = objectAddressHex.trim()
    const field = fieldName.trim()
    const text = value.trim()
    if (!address || !field || !text) return
    if (!await confirmRiskAction('write', t('appStore.clr.writeFieldTitle'), t('appStore.clr.writeFieldDesc', { address, field, value: text }))) return
    return clrInspectorStore.writeClrPrimitiveField(address, field, text)
  }

  async function writeClrPrimitivePath(objectAddressHex: string, path: string, value: string) {
    const address = objectAddressHex.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!address || !pathText || !text) return
    if (!await confirmRiskAction('write', t('appStore.clr.writePathTitle'), t('appStore.clr.writePathDesc', { address, path: pathText, value: text }))) return
    return clrInspectorStore.writeClrPrimitivePath(address, pathText, text)
  }

  async function writeClrPrimitivePathBatch(objectAddressHex: string, operations: ClrPathWriteOperation[]) {
    const address = objectAddressHex.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!address || sanitized.length === 0) return
    const preview = sanitized.map((operation) => `${operation.path} = ${operation.value}`).join(', ')
    if (!await confirmRiskAction('write', t('appStore.clr.transactionTitle'), t('appStore.clr.transactionDesc', { address, preview }))) return
    return clrInspectorStore.writeClrPrimitivePathBatch(address, sanitized)
  }

  async function writeClrPrimitivePathByLocator(typeSubstring: string, identityField: string, identityValue: string, path: string, value: string) {
    const type = typeSubstring.trim()
    const idField = identityField.trim()
    const idValue = identityValue.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!type || !idField || !idValue || !pathText || !text) return
    if (!await confirmRiskAction('write', t('appStore.clr.writePathByLocatorTitle'), t('appStore.clr.writePathByLocatorDesc', { type, idField, idValue, path: pathText, value: text }))) return
    return clrInspectorStore.writeClrPrimitivePathByLocator(type, idField, idValue, pathText, text)
  }

  async function writeClrPrimitivePathBatchByLocator(typeSubstring: string, identityField: string, identityValue: string, operations: ClrPathWriteOperation[]) {
    const type = typeSubstring.trim()
    const idField = identityField.trim()
    const idValue = identityValue.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!type || !idField || !idValue || sanitized.length === 0) return
    const preview = sanitized.map((operation) => `${operation.path} = ${operation.value}`).join(', ')
    if (!await confirmRiskAction('write', t('appStore.clr.transactionByLocatorTitle'), t('appStore.clr.transactionByLocatorDesc', { type, idField, idValue, preview }))) return
    return clrInspectorStore.writeClrPrimitivePathBatchByLocator(type, idField, idValue, sanitized)
  }

  async function writeClrPrimitivePathBatchAtomic(objectAddressHex: string, operations: ClrPathWriteOperation[]) {
    const address = objectAddressHex.trim()
    const sanitized = operations
      .map((operation) => ({ path: operation.path.trim(), value: operation.value.trim() }))
      .filter((operation) => operation.path && operation.value)
      .slice(0, 32)
    if (!address || sanitized.length === 0) return
    const preview = sanitized.map((operation) => `${operation.path} = ${operation.value}`).join(', ')
    if (!await confirmRiskAction('write', t('appStore.clr.atomicTransactionTitle'), t('appStore.clr.atomicTransactionDesc', { address, preview }))) return
    return clrInspectorStore.writeClrPrimitivePathBatchAtomic(address, sanitized)
  }

  async function callClrInstanceMethod(objectAddressHex: string, methodName: string, valueText: string, valueType: string) {
    const address = objectAddressHex.trim()
    const method = methodName.trim()
    const text = valueText.trim()
    if (!address || !method) return
    // Categoriquement plus a risque que writeClrPrimitive*/writeClrPrimitivePath :
    // injecte et EXECUTE du code dans le processus cible (shellcode + thread
    // distant), pas une ecriture memoire passive. Meme gate 'injection' que
    // injectDllIntoProcess/installFunctionHook/le speedhack.
    if (!await confirmRiskAction('injection', t('appStore.clr.callSetterTitle'), t('appStore.clr.callSetterDesc', { address, method, argument: text || t('appStore.clr.noArgumentFallback') }))) return
    return clrInspectorStore.callClrInstanceMethod(address, method, text, valueType.trim())
  }

  async function enumerateClrRoots(typeSubstring = clrTypeFilter.value) {
    return clrInspectorStore.enumerateClrRoots(typeSubstring)
  }

  async function findClrGcRootPath(targetObjectAddressHex: string, maxDepth = 8, maxRootsScanned = 4000) {
    return clrInspectorStore.findClrGcRootPath(targetObjectAddressHex, maxDepth, maxRootsScanned)
  }

  async function disassembleClrMethod(objectAddressHex: string, methodName: string, instructionCount = 24) {
    return clrInspectorStore.disassembleClrMethod(objectAddressHex, methodName, instructionCount)
  }

  async function generateClrObjectReport(objectAddressHex: string, maxDepth = 0, maxNodes = 0, includeGcRootChain = true) {
    return clrInspectorStore.generateClrObjectReport(objectAddressHex, maxDepth, maxNodes, includeGcRootChain)
  }

  async function refreshProcessModules(pid: number) {
    try {
      processModules.value = await backend.getController().getProcessModules(pid)
    } catch (e) {
      processModules.value = []
      console.error('[KillEngine] Failed to get process modules:', e)
    }
  }

  async function discoverSaveFiles(maxResults = 50) {
    saveFilesBusy.value = true
    try {
      const result = await backend.getController().discoverProcessSaveFiles(maxResults)
      saveFileDiscoveryResult.value = result
      discoveredSaveFiles.value = result.success ? (result.files ?? []) : []
      discoveredSaveFilesFamilyName.value = result.success ? (result.familyName ?? '') : ''
      selectedSaveFileText.value = null
      selectedSaveFilePath.value = ''
      addActionLog(
        'save_files',
        result.success ? t('appStore.saveFiles.discoveredTitle') : t('appStore.saveFiles.discoveryFailed'),
        result.success
          ? t('appStore.saveFiles.discoveredDetail', { count: discoveredSaveFiles.value.length, family: discoveredSaveFilesFamilyName.value ? ` · ${discoveredSaveFilesFamilyName.value}` : '' })
          : (result.error ?? t('appStore.errors.unknown')),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      const result = { success: false, files: [], error: String(e) }
      saveFileDiscoveryResult.value = result
      discoveredSaveFiles.value = []
      discoveredSaveFilesFamilyName.value = ''
      selectedSaveFileText.value = null
      selectedSaveFilePath.value = ''
      console.error('[KillEngine] Failed to discover process save files:', e)
      addActionLog('save_files', t('appStore.saveFiles.discoveryFailed'), String(e), 'error')
      return result
    } finally {
      saveFilesBusy.value = false
    }
  }

  async function takeSaveFileSnapshotBefore(maxResults = 200) {
    saveFileSnapshotBusy.value = true
    try {
      const result = await backend.getController().discoverProcessSaveFiles(maxResults)
      saveFileSnapshotBefore.value = result.success ? (result.files ?? []) : []
      saveFileSnapshotDiff.value = null
      addActionLog(
        'save_files',
        t('appStore.saveFiles.snapshotBeforeTitle'),
        t('appStore.saveFiles.discoveredDetail', { count: saveFileSnapshotBefore.value.length, family: result.familyName ? ` · ${result.familyName}` : '' }),
        result.success ? 'success' : 'warning',
      )
      return result
    } finally {
      saveFileSnapshotBusy.value = false
    }
  }

  async function takeSaveFileSnapshotAfter(maxResults = 200) {
    saveFileSnapshotBusy.value = true
    try {
      const result = await backend.getController().discoverProcessSaveFiles(maxResults)
      saveFileSnapshotAfter.value = result.success ? (result.files ?? []) : []
      saveFileSnapshotDiff.value = null
      addActionLog(
        'save_files',
        t('appStore.saveFiles.snapshotAfterTitle'),
        t('appStore.saveFiles.discoveredDetail', { count: saveFileSnapshotAfter.value.length, family: result.familyName ? ` · ${result.familyName}` : '' }),
        result.success ? 'success' : 'warning',
      )
      return result
    } finally {
      saveFileSnapshotBusy.value = false
    }
  }

  async function compareSaveFileSnapshots() {
    saveFileSnapshotBusy.value = true
    try {
      const controller = backend.getController()
      const result = await controller.compareProcessSaveFileSnapshots?.(
        saveFileSnapshotBefore.value,
        saveFileSnapshotAfter.value,
      )
      const finalResult: SaveFileSnapshotDiffResult = result ?? { success: false, error: t('appStore.saveFiles.methodUnavailable') }
      saveFileSnapshotDiff.value = finalResult
      addActionLog(
        'save_files',
        finalResult.success ? t('appStore.saveFiles.comparisonDoneTitle') : t('appStore.saveFiles.comparisonFailed'),
        finalResult.success
          ? t('appStore.saveFiles.comparisonDetail', { added: finalResult.addedCount ?? 0, removed: finalResult.removedCount ?? 0, modified: finalResult.modifiedCount ?? 0 })
          : (finalResult.error ?? t('appStore.errors.unknown')),
        finalResult.success ? 'success' : 'warning',
      )
      return finalResult
    } finally {
      saveFileSnapshotBusy.value = false
    }
  }

  async function readSaveFileText(path: string, maxBytes = 65536) {
    const trimmedPath = path.trim()
    if (!trimmedPath) {
      const result = { success: false, path, text: '', truncated: false, error: t('appStore.saveFiles.emptyPath') }
      selectedSaveFileText.value = result
      return result
    }
    selectedSaveFilePath.value = trimmedPath
    selectedSaveFileText.value = null
    saveFileTextBusy.value = true
    try {
      const result = await backend.getController().readProcessSaveFileText(trimmedPath, maxBytes)
      selectedSaveFileText.value = result
      addActionLog(
        'save_files',
        result.success ? t('appStore.saveFiles.fileReadTitle') : t('appStore.saveFiles.fileReadFailed'),
        result.success ? `${trimmedPath}${result.truncated ? ` · ${t('appStore.saveFiles.truncated')}` : ''}` : (result.error ?? t('appStore.errors.unknown')),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      const result = { success: false, path: trimmedPath, text: '', truncated: false, error: String(e) }
      selectedSaveFileText.value = result
      console.error('[KillEngine] Failed to read process save file text:', e)
      addActionLog('save_files', t('appStore.saveFiles.fileReadFailed'), String(e), 'error')
      return result
    } finally {
      saveFileTextBusy.value = false
    }
  }

  async function inspectLocalSettings(maxValues = 200) {
    localSettingsBusy.value = true
    try {
      const result = await backend.getController().inspectProcessLocalSettings(maxValues)
      localSettingsResult.value = result
      addActionLog(
        'save_files',
        result.success ? t('appStore.saveFiles.localSettingsInspectedTitle') : t('appStore.saveFiles.localSettingsInspectFailed'),
        result.success
          ? t('appStore.saveFiles.localSettingsDetail', { count: result.count ?? result.values.length, family: result.familyName ? ` · ${result.familyName}` : '' })
          : (result.error ?? t('appStore.errors.unknown')),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      const result = { success: false, values: [], error: String(e) }
      localSettingsResult.value = result
      console.error('[KillEngine] Failed to inspect process LocalSettings:', e)
      addActionLog('save_files', t('appStore.saveFiles.localSettingsInspectFailed'), String(e), 'error')
      return result
    } finally {
      localSettingsBusy.value = false
    }
  }

  async function watchSelectedSaveFile(path: string, timeoutMs = 8000) {
    const trimmedPath = path.trim()
    if (!trimmedPath || saveFileWatchBusy.value) return
    const controller = backend.getController()
    const asyncFn = controller.startSaveFileWatchAsync
    const finishedSignal = controller.saveFileWatchFinished
    const options = { timeoutMs }

    if (!asyncFn || !finishedSignal) {
      addActionLog('save_files', t('appStore.saveFiles.watchUnavailable'), t('appStore.saveFiles.watchFunctionMissing'), 'warning')
      return
    }

    saveFileWatchBusy.value = true
    saveFileWatchPath.value = trimmedPath
    saveFileWatchResult.value = null
    await new Promise<void>((resolve) => {
      let requestId: number | null = null
      let settled = false
      const earlyPayloads: Array<Record<string, unknown>> = []
      const finalize = (payload: Record<string, unknown>) => {
        if (settled) return
        settled = true
        finishedSignal.disconnect?.(handler)
        saveFileWatchBusy.value = false
        saveFileWatchResult.value = payload as unknown as SaveFileWatchResult
        const ok = payload.changed === true
        addActionLog(
          'save_files',
          ok ? t('appStore.saveFiles.changeDetected') : (payload.cancelled ? t('appStore.saveFiles.watchCancelled') : t('appStore.saveFiles.noChangeBeforeTimeout')),
          `${trimmedPath}${payload.changeType ? ` · ${String(payload.changeType)}` : ''}`,
          ok ? 'success' : 'warning',
        )
        resolve()
      }
      const handler = (payload: Record<string, unknown>) => {
        if (requestId === null) {
          earlyPayloads.push(payload)
          return
        }
        if (Number(payload.requestId) !== requestId) return
        finalize(payload)
      }
      finishedSignal.connect(handler)

      asyncFn(trimmedPath, options).then((start) => {
        if (settled) return
        if (start.success !== true || start.started !== true) {
          settled = true
          finishedSignal.disconnect?.(handler)
          saveFileWatchBusy.value = false
          saveFileWatchResult.value = { success: false, error: String(start.error ?? t('appStore.saveFiles.watchStartFailed')) }
          addActionLog('save_files', t('appStore.saveFiles.watchFailed'), String(start.error ?? ''), 'error')
          resolve()
          return
        }
        requestId = Number(start.requestId)
        for (const payload of earlyPayloads.splice(0)) {
          handler(payload)
          if (settled) break
        }
      }).catch((e) => {
        if (settled) return
        settled = true
        finishedSignal.disconnect?.(handler)
        saveFileWatchBusy.value = false
        saveFileWatchResult.value = { success: false, error: String(e) }
        addActionLog('save_files', t('appStore.saveFiles.watchFailed'), String(e), 'error')
        resolve()
      })
    })
  }

  async function cancelSaveFileWatchAction() {
    const controller = backend.getController()
    if (!controller.cancelSaveFileWatch) return
    try {
      const result = await controller.cancelSaveFileWatch()
      if (result.success !== true) {
        addActionLog('save_files', t('appStore.saveFiles.cancelWatchImpossible'), String(result.error ?? ''), 'warning')
      }
    } catch (e) {
      addActionLog('save_files', t('appStore.saveFiles.cancelWatchImpossible'), String(e), 'warning')
    }
  }

  async function patchSelectedSaveFileBytes(path: string, findHex: string, replaceHex: string) {
    const trimmedPath = path.trim()
    if (!trimmedPath || !findHex.trim() || !replaceHex.trim()) return
    if (!await confirmRiskAction(
      'patch',
      t('appStore.saveFiles.patchBytesTitle'),
      t('appStore.saveFiles.patchBytesDesc', { find: findHex.trim(), replace: replaceHex.trim(), path: trimmedPath }),
    )) return

    const controller = backend.getController()
    if (!controller.patchProcessSaveFileBytes) {
      saveFilePatchResult.value = { success: false, error: t('appStore.saveFiles.patchNotExposed') }
      return
    }
    saveFilePatchBusy.value = true
    try {
      const result = await controller.patchProcessSaveFileBytes(trimmedPath, findHex.trim(), replaceHex.trim())
      saveFilePatchResult.value = result
      addActionLog(
        'save_files',
        result.success ? t('appStore.saveFiles.patchedTitle') : t('appStore.saveFiles.patchFailed'),
        result.success ? t('appStore.saveFiles.patchedDetail', { path: trimmedPath, count: result.occurrencesFound ?? 1 }) : (result.error ?? t('appStore.errors.unknown')),
        result.success ? 'success' : 'error',
      )
      return result
    } catch (e) {
      const result = { success: false, error: String(e) }
      saveFilePatchResult.value = result
      addActionLog('save_files', t('appStore.saveFiles.patchFailed'), String(e), 'error')
      return result
    } finally {
      saveFilePatchBusy.value = false
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
      memoryPreview.value = await readMemoryPreviewByMode(addressHex, size)
      addActionLog(
        'memory_preview',
        t('appStore.memory.previewTitle', { address: normalizedAddress }),
        memoryPreview.value.success
          ? t('appStore.memory.previewBytesRead', { read: memoryPreview.value.bytesRead, requested: memoryPreview.value.requestedBytes })
          : memoryPreview.value.error || t('appStore.memory.previewNoData'),
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
      addActionLog('memory_preview', t('appStore.memory.previewTitle', { address: normalizedAddress }), String(e), 'error')
    } finally {
      memoryPreviewLoading.value = false
    }
  }

  async function loadHexViewerPage() {
    if (!hexViewerAddress.value) return
    const controller = backend.getController()
    if (!kernelMemoryModeActive.value && !controller.readMemoryBlock) {
      hexViewerData.value = {
        success: false,
        partial: false,
        cancelled: false,
        bytesRead: 0,
        requestedBytes: hexViewerPageSize.value,
        error: t('appStore.memory.readMemoryBlockUnavailable'),
        hex: '',
      }
      return
    }
    hexViewerLoading.value = true
    try {
      hexViewerData.value = kernelMemoryModeActive.value
        ? await readMemoryPreviewByMode(hexViewerAddress.value, hexViewerPageSize.value)
        : await controller.readMemoryBlock!(hexViewerAddress.value, hexViewerPageSize.value)
    } catch (e) {
      hexViewerData.value = {
        success: false,
        partial: false,
        cancelled: false,
        bytesRead: 0,
        requestedBytes: hexViewerPageSize.value,
        error: String(e),
        hex: '',
      }
    } finally {
      hexViewerLoading.value = false
    }
  }

  async function openHexViewer(addressHex: string) {
    const normalized = addressHex.trim().replace(/^0x/i, '')
    if (!normalized) return
    hexViewerOpen.value = true
    hexViewerRootAddress.value = normalized
    hexViewerAddress.value = normalized
    await loadHexViewerPage()
  }

  function closeHexViewer() {
    hexViewerOpen.value = false
    hexViewerData.value = null
  }

  async function hexViewerJumpTo(addressHex: string) {
    const normalized = addressHex.trim().replace(/^0x/i, '')
    if (!normalized) return
    hexViewerAddress.value = normalized
    await loadHexViewerPage()
  }

  async function hexViewerGoToOffset(deltaBytes: number) {
    const current = Number.parseInt(hexViewerAddress.value || '0', 16)
    const next = Math.max(0, current + deltaBytes)
    hexViewerAddress.value = next.toString(16)
    await loadHexViewerPage()
  }

  async function hexViewerSetPageSize(pageSize: number) {
    hexViewerPageSize.value = pageSize
    await loadHexViewerPage()
  }

  async function hexViewerWriteRow(rowAddressHex: string, hexString: string) {
    const result = await writeMemoryHexByMode(rowAddressHex, hexString)
    if (result.success) {
      await loadHexViewerPage()
    }
    return result
  }

  async function detach() {
    try {
      await backend.getController().detachProcess()
      isAttached.value = false
      clearSessionEntries()
      processName.value = ''
      processModules.value = []
      memoryMap.value = null
      memoryPreview.value = null
      memoryPreviewAddress.value = ''
      memoryPreviewLoading.value = false
      closeHexViewer()
      clrObjects.value = []
      clrSelectedObject.value = null
      clrRoots.value = []
      await refreshClrInspectorStatus()
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
      await refreshAutoResolveReport()
      await refreshRememberedPatterns()
      await refreshWriteHistorySequence()
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
      autoResolveReport.value = null
      console.error('[KillEngine] Failed to refresh diagnostics:', e)
    }
  }

  async function refreshAutoResolveReport() {
    const controller = backend.getController()
    autoResolveReport.value = controller.getAutoResolveReport
      ? await controller.getAutoResolveReport(settingSmartSearchDebugMaxEvents.value)
      : null
    return autoResolveReport.value
  }

  async function refreshRememberedPatterns() {
    const controller = backend.getController()
    if (!controller.getRememberedPatterns) {
      rememberedPatterns.value = []
      return
    }
    const result = await controller.getRememberedPatterns()
    rememberedPatterns.value = result.success
      ? ((result.patterns as Array<Record<string, unknown>>) ?? [])
      : []
  }

  async function previewRememberedPattern(pattern: Record<string, unknown>) {
    const address = String(pattern.liveAddress ?? '')
    if (!address) return
    activeView.value = 'memory'
    await readMemoryPreview(address, 64)
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
    diagnosticFolderOpened.value = false
    diagnosticOpenFolderError.value = ''
    try {
      const result = await backend.getController().exportDiagnostics()
      if (result.success === true) {
        diagnosticExportPath.value = String(result.path ?? '')
        diagnosticFolderOpened.value = Boolean(result.folderOpened ?? false)
        diagnosticOpenFolderError.value = String(result.openFolderError ?? '')
      } else {
        diagnosticExportError.value = String(result.error ?? t('appStore.diagnostics.exportImpossible'))
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
    if (scanBusy.value) return { success: false, error: t('appStore.cleanup.scanActive') }
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
      scanStatusText.value = String(temporaryStorageCleanupResult.value.message ?? t('appStore.cleanup.temporaryStorageCleaned'))
      addActionLog(
        'cleanup',
        t('appStore.cleanup.temporaryCleanedTitle'),
        t('appStore.cleanup.temporaryCleanedDetail', { files: String(temporaryStorageCleanupResult.value.removedFileCount ?? 0), bytes: String(temporaryStorageCleanupResult.value.removedBytes ?? 0) }),
        temporaryStorageCleanupResult.value.success === true ? 'success' : 'warning',
      )
      return temporaryStorageCleanupResult.value
    } catch (e) {
      temporaryStorageCleanupResult.value = { success: false, error: String(e) }
      temporaryStorageError.value = String(e)
      addActionLog('cleanup', t('appStore.cleanup.cleanupFailed'), String(e), 'error')
      return temporaryStorageCleanupResult.value
    }
  }

  function syncScanDefaultsFromSettings() {
    scanningStore.syncScanDefaultsFromSettings()
  }

  async function loadSettings() {
    return settingsStore.loadSettings(syncScanDefaultsFromSettings)
  }

  async function refreshAiModelStatus() {
    return settingsStore.refreshAiModelStatus()
  }

  async function browseForModel() {
    return settingsStore.browseForModel()
  }

  async function saveSettings() {
    return settingsStore.saveSettings(syncScanDefaultsFromSettings, refreshDiagnostics)
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

  async function doExactScan() {
    return scanningStore.doExactScan()
  }

  async function doGroupScan() {
    return scanningStore.doGroupScan()
  }

  function addGroupScanEntry() {
    scanningStore.addGroupScanEntry()
  }

  function removeGroupScanEntry(index: number) {
    scanningStore.removeGroupScanEntry(index)
  }

  function clearGroupScanEntries() {
    scanningStore.clearGroupScanEntries()
  }

  async function doEncryptedScan() {
    return scanningStore.doEncryptedScan()
  }

  async function refreshCandidates() {
    return scanningStore.refreshCandidates(addAddressToWatch)
  }

  async function doNextScan() {
    return scanningStore.doNextScan()
  }

  async function undoCandidateScan() {
    return scanningStore.undoCandidateScan()
  }

  async function cancelActiveScan() {
    return scanningStore.cancelActiveScan()
  }

  async function captureUnknownSnapshot() {
    return scanningStore.captureUnknownSnapshot()
  }

  async function doUnknownNextScan() {
    return scanningStore.doUnknownNextScan()
  }

  function selectCandidate(address: string, type: string) {
    scanningStore.selectCandidate(address, type)
  }

  async function runUnknownGuideStep(mode: 'increased' | 'decreased' | 'unchanged' | 'changed') {
    return scanningStore.runUnknownGuideStep(mode)
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
      t('appStore.navigation.regionOpenedInExpert', { address }),
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

  function inferredTypesForValue(value: string): Array<Record<string, unknown>> {
    const normalized = value.trim().replace(',', '.')
    const numberValue = Number(normalized)
    if (!Number.isFinite(numberValue)) return []
    const integer = Number.isInteger(numberValue)
    const results: Array<Record<string, unknown>> = []
    if (integer && numberValue >= 0 && numberValue <= 255) {
      results.push({ type: 'UInt8', confidence: t('appStore.typeInference.confidenceLow'), reason: t('appStore.typeInference.reasonSmallCompactValue') })
    }
    if (integer && numberValue >= -128 && numberValue <= 127) {
      results.push({ type: 'Int8', confidence: t('appStore.typeInference.confidenceLow'), reason: t('appStore.typeInference.reasonSmallSignedCompactValue') })
    }
    if (integer && numberValue >= 0 && numberValue <= 65535) {
      results.push({ type: 'UInt16', confidence: t('appStore.typeInference.confidenceMedium'), reason: t('appStore.typeInference.reasonCompactResource') })
    }
    if (integer && numberValue >= -32768 && numberValue <= 32767) {
      results.push({ type: 'Int16', confidence: t('appStore.typeInference.confidenceMedium'), reason: t('appStore.typeInference.reasonShortInteger') })
    }
    if (integer && numberValue >= -2147483648 && numberValue <= 2147483647) {
      results.push({ type: 'Int32', confidence: t('appStore.typeInference.confidenceHigh'), reason: t('appStore.typeInference.reasonCommonGameInteger') })
    }
    if (integer && numberValue >= 0 && numberValue <= 4294967295) {
      results.push({ type: 'UInt32', confidence: t('appStore.typeInference.confidenceHigh'), reason: t('appStore.typeInference.reasonCommonUnsignedResource') })
    }
    if (integer) results.push({ type: 'Int64', confidence: t('appStore.typeInference.confidenceMedium'), reason: t('appStore.typeInference.reasonLargeInteger') })
    if (integer && numberValue >= 0) results.push({ type: 'UInt64', confidence: t('appStore.typeInference.confidenceLow'), reason: t('appStore.typeInference.reasonLargeUnsignedInteger') })
    results.push({ type: 'Float32', confidence: integer ? t('appStore.typeInference.confidenceMedium') : t('appStore.typeInference.confidenceHigh'), reason: t('appStore.typeInference.reasonDisplayedValueOftenFloat') })
    results.push({ type: 'Float64', confidence: t('appStore.typeInference.confidenceLow'), reason: t('appStore.typeInference.reasonLessCommonSpecificTools') })
    for (const scale of [10, 100, 1000, 4096, 65536]) {
      const scaled = numberValue * scale
      if (integer && Math.abs(scaled) <= 2147483647) {
        results.push({ type: `Int32 x${scale}`, confidence: t('appStore.typeInference.confidenceMedium'), reason: t('appStore.typeInference.reasonScaledStorage', { value, scaled }) })
      }
    }
    return results
  }

  const inferredExactTypes = computed(() => inferredTypesForValue(exactScanValue.value))

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

  function addAddressesToWatch(targets: Array<{ address: string, type?: string }>, limit = watchLiveReadLimit) {
    let added = 0
    const boundedTargets = targets.slice(0, limit)
    for (const target of boundedTargets) {
      const before = watchedAddresses.value.length
      addAddressToWatch(target.address, target.type ?? exactScanType.value)
      if (watchedAddresses.value.length > before) added += 1
    }
    addActionLog('watch', t('appStore.watch.addressesSentToLive', { count: boundedTargets.length }), t('appStore.watch.addressesSentDetail', { added, limit }), 'info')
  }

  function removeAddressFromWatch(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    watchedAddresses.value = watchedAddresses.value.filter((item) => item.address !== normalized)
  }

  function clearWatchedAddresses() {
    watchedAddresses.value = []
    addActionLog('watch', t('appStore.watch.liveCleared'), t('appStore.watch.allAddressesRemoved'), 'info')
  }

  async function refreshWatchedAddress(address: string): Promise<WatchedAddress | null> {
    const normalized = address.trim().replace(/^0x/i, '')
    const watched = watchedAddresses.value.find((item) => item.address === normalized)
    if (!watched) return null

    let updated: WatchedAddress
    try {
      const preview = await readMemoryPreviewByMode(watched.address, valueTypeReadSize(watched.type))
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
    for (const watched of watchedAddresses.value.slice(0, watchLiveReadLimit)) {
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
    addActionLog('watch', enabled ? t('appStore.watch.liveEnabled') : t('appStore.watch.liveStopped'), t('appStore.watch.addressCount', { count: watchedAddresses.value.length }), enabled ? 'success' : 'info')
  }

  async function injectDll() {
    const path = injectDllPath.value.trim()
    if (!path) return
    if (!await confirmRiskAction('injection', t('appStore.injection.dllTitle'), t('appStore.injection.dllDesc', { path }))) return

    const controller = backend.getController()
    if (!controller.injectDllIntoProcess) {
      injectionResult.value = { success: false, error: t('appStore.injection.dllNotExposed') }
      addActionLog('injection', t('appStore.injection.dllUnavailable'), injectionResult.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      injectionResult.value = await controller.injectDllIntoProcess(path)
      const ok = injectionResult.value.success === true
      addActionLog('injection', ok ? t('appStore.injection.dllInjected') : t('appStore.injection.dllFailed'), `${path}. ${String(injectionResult.value.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      injectionResult.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.injection.dllFailed'), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function installHook() {
    const target = hookTargetAddress.value.trim()
    const hook = hookFunctionAddress.value.trim()
    if (!target || !hook) return
    if (!await confirmRiskAction('injection', t('appStore.injection.hookInstallTitle'), t('appStore.injection.hookInstallDesc', { target, hook }))) return

    const controller = backend.getController()
    if (!controller.installFunctionHook) {
      activeFunctionHook.value = { success: false, error: t('appStore.injection.hookingNotExposed') }
      addActionLog('injection', t('appStore.injection.hookUnavailable'), activeFunctionHook.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      activeFunctionHook.value = await controller.installFunctionHook(target, hook)
      const ok = activeFunctionHook.value.success === true
      addActionLog('injection', ok ? t('appStore.injection.hookInstalled') : t('appStore.injection.hookInstallFailed'), `0x${target} -> 0x${hook}. ${String(activeFunctionHook.value.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      activeFunctionHook.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.injection.hookInstallFailed'), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function removeHook() {
    const target = hookTargetAddress.value.trim()
    if (!target) return

    const controller = backend.getController()
    if (!controller.removeFunctionHook) {
      addActionLog('injection', t('appStore.injection.hookRemoveUnavailable'), t('appStore.injection.hookRemoveFunctionMissing'), 'warning')
      return
    }

    injectionBusy.value = true
    try {
      const result = await controller.removeFunctionHook(target)
      const ok = result.success === true
      if (ok) activeFunctionHook.value = null
      addActionLog('injection', ok ? t('appStore.injection.hookRemoved') : t('appStore.injection.hookRemoveFailed'), `0x${target}. ${String(result.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      addActionLog('injection', t('appStore.injection.hookRemoveFailed'), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function resolveSymbol() {
    const moduleName = symbolModuleName.value.trim()
    const functionName = symbolFunctionName.value.trim()
    if (!moduleName || !functionName) return

    const controller = backend.getController()
    if (!controller.resolveSymbolAddress) {
      symbolResolveResult.value = { success: false, error: t('appStore.injection.symbolResolveNotExposed') }
      return
    }

    try {
      symbolResolveResult.value = await controller.resolveSymbolAddress(moduleName, functionName)
      const ok = symbolResolveResult.value.success === true
      const detail = ok
        ? `-> 0x${String(symbolResolveResult.value.address ?? '')}`
        : String(symbolResolveResult.value.error ?? '')
      addActionLog(
        'injection',
        ok ? t('appStore.injection.symbolResolved') : t('appStore.injection.symbolResolveFailed'),
        `${moduleName}!${functionName} ${detail}`.trim(),
        ok ? 'success' : 'warning',
      )
    } catch (e) {
      symbolResolveResult.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.injection.symbolResolveFailed'), String(e), 'error')
    }
  }

  // Réutilise l'adresse résolue comme cible de hook sans re-taper l'hexadécimal.
  function applyResolvedSymbolToHookTarget() {
    if (symbolResolveResult.value?.success === true && symbolResolveResult.value.address) {
      hookTargetAddress.value = String(symbolResolveResult.value.address)
    }
  }

  async function previewAutoAsmScript() {
    const script = autoAsmScriptText.value
    if (!script.trim()) return
    const controller = backend.getController()
    if (!controller.parseAutoAssemblerScript) {
      autoAsmPreview.value = { success: false, parseError: t('appStore.autoAsm.previewNotExposed') }
      return
    }
    autoAsmPreview.value = await controller.parseAutoAssemblerScript(script)
  }

  async function executeAutoAsmScript() {
    const script = autoAsmScriptText.value
    if (!script.trim()) return
    if (!await confirmRiskAction('injection', t('appStore.autoAsm.executeTitle'), t('appStore.autoAsm.executeDesc'))) return

    const controller = backend.getController()
    if (!controller.executeAutoAssemblerScript) {
      autoAsmResult.value = { success: false, error: t('appStore.autoAsm.executeNotExposed') }
      addActionLog('injection', t('appStore.autoAsm.unavailable'), autoAsmResult.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      autoAsmResult.value = await controller.executeAutoAssemblerScript(script)
      const ok = autoAsmResult.value.success === true
      addActionLog('injection', ok ? t('appStore.autoAsm.executed') : t('appStore.autoAsm.executeFailed'), String(autoAsmResult.value.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      autoAsmResult.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.autoAsm.executeFailed'), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function restoreAutoAsmScript() {
    const controller = backend.getController()
    if (!controller.restoreAutoAssemblerScript) {
      addActionLog('injection', t('appStore.autoAsm.restoreUnavailable'), t('appStore.autoAsm.restoreFunctionMissing'), 'warning')
      return
    }

    injectionBusy.value = true
    try {
      const result = await controller.restoreAutoAssemblerScript()
      const ok = result.success === true
      if (ok) autoAsmResult.value = null
      addActionLog('injection', ok ? t('appStore.autoAsm.restored') : t('appStore.autoAsm.restoreFailed'), String(result.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      addActionLog('injection', t('appStore.autoAsm.restoreFailed'), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  function autoAsmProfileName(): string {
    return (processName.value || 'KillEngineTrainer')
      .replace(/\.[^.]+$/, '')
      .replace(/[^a-z0-9_.-]+/gi, '_')
      .slice(0, 80) || 'KillEngineTrainer'
  }

  async function refreshSavedAutoAsmScripts() {
    const controller = backend.getController()
    autoAsmSavedScriptsBusy.value = true
    try {
      const result = await controller.loadProfile(autoAsmProfileName())
      autoAsmSavedScripts.value = result.success
        ? ((result.autoAsmScripts as Array<Record<string, unknown>>) ?? [])
        : []
    } catch (e) {
      autoAsmSavedScripts.value = []
      console.error('[KillEngine] Failed to load saved auto-asm scripts:', e)
    } finally {
      autoAsmSavedScriptsBusy.value = false
    }
  }

  async function saveAutoAsmScript() {
    const script = autoAsmScriptText.value
    const name = autoAsmScriptName.value.trim()
    if (!script.trim() || !name) return
    const controller = backend.getController()
    if (!controller.saveProfileAutoAsmScript) {
      autoAsmSaveResult.value = { success: false, error: t('appStore.autoAsm.saveNotExposed') }
      return
    }
    try {
      autoAsmSaveResult.value = await controller.saveProfileAutoAsmScript(autoAsmProfileName(), name, script, {})
      const ok = autoAsmSaveResult.value.success === true
      addActionLog('injection', ok ? t('appStore.autoAsm.saved') : t('appStore.autoAsm.saveFailed'), String(autoAsmSaveResult.value.error ?? name), ok ? 'success' : 'error')
      if (ok) await refreshSavedAutoAsmScripts()
    } catch (e) {
      autoAsmSaveResult.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.autoAsm.saveFailed'), String(e), 'error')
    }
  }

  async function applySavedAutoAsmScript(name: string) {
    if (!await confirmRiskAction('injection', t('appStore.autoAsm.applySavedTitle'), t('appStore.autoAsm.applySavedDesc', { name }))) return
    const controller = backend.getController()
    if (!controller.applyProfileAutoAsmScript) {
      autoAsmResult.value = { success: false, error: t('appStore.autoAsm.executeNotExposed') }
      return
    }
    injectionBusy.value = true
    try {
      autoAsmResult.value = await controller.applyProfileAutoAsmScript(autoAsmProfileName(), name)
      const ok = autoAsmResult.value.success === true
      addActionLog('injection', ok ? t('appStore.autoAsm.namedExecuted', { name }) : t('appStore.autoAsm.namedExecuteFailed', { name }), String(autoAsmResult.value.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      autoAsmResult.value = { success: false, error: String(e) }
      addActionLog('injection', t('appStore.autoAsm.namedExecuteFailed', { name }), String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function deleteSavedAutoAsmScript(name: string) {
    const controller = backend.getController()
    if (!controller.deleteProfileAutoAsmScript) return
    try {
      const result = await controller.deleteProfileAutoAsmScript(autoAsmProfileName(), name)
      const ok = result.success === true
      addActionLog('injection', ok ? t('appStore.autoAsm.namedDeleted', { name }) : t('appStore.autoAsm.namedDeleteFailed', { name }), String(result.error ?? ''), ok ? 'success' : 'error')
      if (ok) await refreshSavedAutoAsmScripts()
    } catch (e) {
      addActionLog('injection', t('appStore.autoAsm.namedDeleteFailed', { name }), String(e), 'error')
    }
  }

  async function nextCandidatePage() {
    return scanningStore.nextCandidatePage()
  }

  async function previousCandidatePage() {
    return scanningStore.previousCandidatePage()
  }

  // PHASE 119 -- pont pipe d'automatisation -> couche Vue/Pinia (voir
  // docs/POWER_UP_ROADMAP.md section N). Expose UNIQUEMENT les actions
  // listees ici sur window, pour qu'ApplicationController::callVueStoreAction
  // (C++) puisse les invoquer via page()->runJavaScript() depuis le pipe.
  // Liste blanche cote JS ET cote C++ (allowedVueStoreActions() dans
  // application_controller.cpp) : les deux doivent matcher independamment
  // pour qu'une action s'execute -- jamais de JS arbitraire, uniquement ces
  // fonctions nommees avec leur propre signature figee.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const automationBridgeActions: Record<string, (...args: any[]) => unknown> = {
    keepCandidate,
    ignoreCandidate,
    addAddressToWatch,
    writeSelectedValue,
    writeSelectedAddresses,
    writeSelectedTargets,
    writeSelectedAtomic,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    freezeCandidateCurrent,
    toggleFreeze,
    startBreakpointFreeze,
    createTrainerFeature,
    deleteTrainerFeature,
    applyTrainerFeature,
    restoreTrainerFeature,
    applyAllTrainerFeatures,
    restoreAllTrainerFeatures,
    getTrainerFeaturesSnapshot,
    generateTrainerFeaturePointerChain,
  }
  if (typeof window !== 'undefined') {
    window.__killengineAutomationBridge = {
      dispatch(action: string, args: unknown[]) {
        const fn = automationBridgeActions[action]
        if (typeof fn !== 'function') {
          throw new Error(t('appStore.automationBridge.unauthorizedAction', { action }))
        }
        riskGateStore.automationPipeDispatchDepth += 1
        try {
          const result = fn(...(Array.isArray(args) ? args : []))
          if (result && typeof (result as Promise<unknown>).finally === 'function') {
            return (result as Promise<unknown>).finally(() => {
              riskGateStore.automationPipeDispatchDepth = Math.max(0, riskGateStore.automationPipeDispatchDepth - 1)
            })
          }
          riskGateStore.automationPipeDispatchDepth = Math.max(0, riskGateStore.automationPipeDispatchDepth - 1)
          return result
        } catch (e) {
          riskGateStore.automationPipeDispatchDepth = Math.max(0, riskGateStore.automationPipeDispatchDepth - 1)
          throw e
        }
      },
    }
  }

  return {
    version,
    activeView,
    pendingExpertStep,
    pendingExpertAnchor,
    isConnected,
    showOnboarding,
    dismissOnboarding,
    openUserGuide,
    defenderExclusionResult,
    defenderExclusionBusy,
    requestWindowsDefenderExclusion,
    kernelDriverStatus,
    kernelDriverStatusLoading,
    kernelDriverStartLoading,
    kernelDriverStatusError,
    refreshKernelDriverStatus,
    startKernelDriver,
    kernelMemoryReadResult,
    kernelMemoryReadBusy,
    readMemoryKernel,
    kernelMemoryWriteResult,
    kernelMemoryWriteBusy,
    writeMemoryKernel,
    setMemoryAccessMode,
    writeMemoryValueByMode,
    writeMemoryHexByMode,
    memoryAccessMode,
    kernelMemoryReady,
    kernelMemoryModeActive,
    clrInspectorStatus,
    clrInspectorBusy,
    clrInspectorError,
    clrTypeFilter,
    clrObjects,
    clrSelectedObject,
    clrRoots,
    clrLastResult,
    clrFieldLocatorResult,
    clrCallMethodResult,
    clrGcRootPathResult,
    clrDisassembleResult,
    clrObjectReportResult,
    findClrGcRootPath,
    disassembleClrMethod,
    generateClrObjectReport,
    refreshClrInspectorStatus,
    attachClrInspector,
    detachClrInspector,
    shutdownClrInspector,
    flushClrInspectorCache,
    findClrObjects,
    findClrObjectsByFieldValue,
    readClrObject,
    writeClrPrimitiveField,
    writeClrPrimitivePath,
    writeClrPrimitivePathBatch,
    writeClrPrimitivePathByLocator,
    writeClrPrimitivePathBatchByLocator,
    writeClrPrimitivePathBatchAtomic,
    callClrInstanceMethod,
    enumerateClrRoots,
    luaScriptingStatus,
    luaScriptText,
    luaScriptResult,
    luaScriptBusy,
    luaScriptTimeoutMs,
    luaScriptRequestId,
    luaSavedScripts,
    luaSavedScriptsBusy,
    luaScriptSaveName,
    luaScriptSaveResult,
    refreshLuaScriptingStatus,
    executeLuaScript,
    cancelLuaScriptExecution,
    luaReplActive,
    luaReplBusy,
    luaReplInput,
    luaReplHistory,
    luaReplCompletions,
    luaReplStartResult,
    startLuaReplSession,
    stopLuaReplSession,
    sendLuaReplLine,
    refreshLuaReplCompletions,
    recallLuaReplHistory,
    addTimelineAddress,
    removeTimelineAddress,
    clearTimelineAddresses,
    getTimelineWatchedAddresses,
    setTimelineConfig,
    startTimelineCollection,
    stopTimelineCollection,
    getTimelineSeries,
    detectTimelinePatterns,
    findVolatileTimelineAddresses,
    findStableTimelineAddresses,
    analyzeTimelineBehavior,
    predictTimelineNextValue,
    findTimelineCorrelations,
    generateTimelineReport,
    exportTimelineToJson,
    startMemoryHeatmap,
    stopMemoryHeatmap,
    getMemoryHeatmapStatus,
    getMemoryHeatmapData,
    getPatternLearningStatistics,
    detectGameEngine,
    classifyMemoryPattern,
    loadGameProfile,
    saveGameProfile,
    listKnownGameProfiles,
    deleteGameProfile,
    getTopPatternSuggestions,
    refreshSavedLuaScripts,
    saveLuaScript,
    loadSavedLuaScript,
    deleteSavedLuaScript,
    isAttached,
    processName,
    processes,
    processModules,
    discoveredSaveFiles,
    discoveredSaveFilesFamilyName,
    saveFileDiscoveryResult,
    selectedSaveFileText,
    selectedSaveFilePath,
    saveFilesBusy,
    saveFileTextBusy,
    saveFileSnapshotBefore,
    saveFileSnapshotAfter,
    saveFileSnapshotDiff,
    saveFileSnapshotBusy,
    localSettingsResult,
    localSettingsBusy,
    saveFileWatchResult,
    saveFileWatchBusy,
    saveFileWatchPath,
    saveFilePatchResult,
    saveFilePatchBusy,
    saveFilePatchFindHex,
    saveFilePatchReplaceHex,
    memoryMap,
    memoryPreview,
    memoryPreviewAddress,
    memoryPreviewLoading,
    memoryPreviewAscii,
    memoryPreviewDecoded,
    selectedMemoryRegion,
    hexViewerOpen,
    hexViewerRootAddress,
    hexViewerAddress,
    hexViewerPageSize,
    hexViewerData,
    hexViewerLoading,
    hexViewerRows,
    pingResult,
    logFilePath,
    logLines,
    logError,
    diagnosticExportPath,
    diagnosticExportError,
    diagnosticFolderOpened,
    diagnosticOpenFolderError,
    temporaryStorageStatus,
    temporaryStorageCleanupResult,
    temporaryStorageError,
    smartSearchDebugFilePath,
    scanTelemetryFilePath,
    smartSearchDebugEvents,
    smartSearchDebugError,
    autoResolveReport,
    rememberedPatterns,
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
    pendingAssistantAction,
    settingModelPath,
    settingModelEnabled,
    settingModelThreads,
    aiModelStatus,
    aiModelStatusLoading,
    aiModelStatusError,
    workflowPresets,
    lastWorkflowPresetId,
    activeChatMemoryTargets,
    smartSearchContext,
    investigationReport,
    activeInvestigation,
    investigationArchive,
    trainerFeatures,
    trainerBusy,
    trainerHotkeyStatus,
    trainerOverlayVisible,
    trainerOverlayStatus,
    trainerOverlayHotkey,
    trainerOverlayHotkeyId,
    structureTemplates,
    workspaceBookmarks,
    workspaceProjects,
    riskDialog,
    searchQuery,
    searchResult,
    exactScanValue,
    exactScanType,
    exactScanResult,
    encryptedScanResult,
    autoUiStringScanResult,
    autoUiStringSourceResult,
    autoUiStringSources,
    encryptedScanMode,
    encryptedScanKey,
    encryptedScanKeySearchBits,
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
    autoUnknownAwaitingObservation,
    selectedCandidateAddress,
    writeValue,
    writeResult,
    writeSafetyWarning,
    writeSafetyAcknowledged,
    canWriteSelectedValue,
    freezeEnabled,
    breakpointFreezeEnabled,
    freezeIntervalMs,
    freezeIntervalResult,
    finalCandidateTargets,
    ignoredCandidateAddresses,
    keptCandidateAddresses,
    watchLiveEnabled,
    watchedAddresses,
    watchLiveReadLimit,
    sessionEntries,
    sessionGroups,
    sessionPromotionBusyIds,
    messages,
    actionLog,
    addActionLog,
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
    discoverSaveFiles,
    takeSaveFileSnapshotBefore,
    takeSaveFileSnapshotAfter,
    compareSaveFileSnapshots,
    readSaveFileText,
    inspectLocalSettings,
    watchSelectedSaveFile,
    cancelSaveFileWatchAction,
    patchSelectedSaveFileBytes,
    refreshMemoryMap,
    readMemoryPreview,
    readMemoryPreviewByMode,
    openHexViewer,
    closeHexViewer,
    hexViewerJumpTo,
    hexViewerGoToOffset,
    hexViewerSetPageSize,
    hexViewerWriteRow,
    attach,
    detach,
    doPing,
    loadSettings,
    saveSettings,
    refreshAiModelStatus,
    browseForModel,
    refreshDiagnostics,
    refreshAutoResolveReport,
    refreshRememberedPatterns,
    previewRememberedPattern,
    writeHistorySequence,
    refreshWriteHistorySequence,
    replayWriteHistorySequence,
    clearWriteHistorySequence,
    refreshLogTail,
    exportDiagnostics,
    refreshTemporaryStorageStatus,
    clearTemporaryStorage,
    refreshActiveChatMemoryTargets,
    refreshSmartSearchContext,
    setInvestigationReport,
    startInvestigation,
    addInvestigationStep,
    applyWorkflowPreset,
    updateInvestigationFromAutoResult,
    finishInvestigation,
    clearInvestigation,
    clearInvestigationArchive,
    restoreInvestigationFromArchive,
    exportInvestigationJson,
    exportInvestigationMarkdown,
    clearActionLog,
    exportActionLogJson,
    exportActionLogMarkdown,
    confirmRiskAction,
    resolveRiskDialog,
    buildCheckpointActionPlan,
    executeCheckpointWrite,
    executeCheckpointFindWhatWrites,
    executeCheckpointDisassembleBackward,
    executeCandidateFieldTest,
    speedhackStatus,
    speedhackBusy,
    speedhackFactor,
    refreshSpeedhackStatus,
    startSpeedhack,
    setSpeedhackFactor,
    stopSpeedhack,
    networkBlockStatus,
    networkBlockBusy,
    blockProcessNetwork,
    unblockProcessNetwork,
    refreshProcessNetworkBlockStatus,
    // Réseau — lecture seule
    networkConnections,
    networkModules,
    networkConnectionsBusy,
    networkModulesBusy,
    networkLastRefresh,
    liveRefreshEnabled,
    refreshNetworkConnections: networkStore.refreshNetworkConnections,
    refreshNetworkModules: networkStore.refreshNetworkModules,
    refreshAllNetwork: networkStore.refreshAllNetwork,
    startLiveRefresh: networkStore.startLiveRefresh,
    stopLiveRefresh: networkStore.stopLiveRefresh,
    // Proxy HTTP
    httpProxyActive,
    httpProxyBusy,
    httpProxyPort,
    httpProxyInterceptHttps,
    httpProxyRequests,
    selectedHttpRequest,
    httpRequestBodyEditor,
    startHttpProxy: networkStore.startHttpProxy,
    stopHttpProxy: networkStore.stopHttpProxy,
    refreshHttpProxyRequests: networkStore.refreshHttpProxyRequests,
    modifySelectedHttpRequest: networkStore.modifySelectedHttpRequest,
    // Spoof DNS
    dnsSpoofEntries,
    dnsSpoofBusy,
    dnsSpoofDomain,
    dnsSpoofTargetIp,
    addDnsSpoofEntry: networkStore.addDnsSpoofEntry,
    removeDnsSpoofEntry: networkStore.removeDnsSpoofEntry,
    // Lag switch
    lagSwitchActive,
    lagSwitchBusy,
    lagSwitchDelayMs,
    toggleLagSwitch: networkStore.toggleLagSwitch,
    // Network filter state (refs)
    _networkFilterProtocol,
    _networkFilterState,
    _networkFilterIp,
    apiHookStatus,
    apiHookBusy,
    apiHookModuleName,
    apiHookFunctionName,
    apiHookMode,
    apiHookForcedReturn,
    startApiHook,
    stopApiHook,
    refreshApiHookStatus,
    executeCheckpointKernelWrite,
    confirmChatMemoryWrite,
    confirmChatMemoryFreeze,
    confirmRewriteLastAutoWrite,
    automationPipeStatus,
    refreshAutomationPipeStatus,
    enableAutomationMode,
    disableAutomationMode,
    stealthStatus,
    externalAiActiveBackend,
    externalAiHasApiKey,
    externalAiRequestCount,
    externalAiBusy,
    externalAiError,
    refreshExternalAiStatus,
    setExternalAiApiKey,
    clearExternalAiApiKey,
    setActiveAiBackend,
    stealthRiskAnalysis,
    stealthBusy,
    refreshStealthStatus,
    applyStealthMode,
    restoreStealthMode,
    analyzeStealthRisk,
    webView2CdpDebugFlagStatus,
    webView2CdpDebugFlagBusy,
    refreshWebView2CdpDebugFlagStatus,
    enableWebView2CdpDebugFlag,
    disableWebView2CdpDebugFlag,
    webView2SystemPrepStatus,
    webView2SystemPrepBusy,
    webView2CapabilityInstallResult,
    refreshWebView2SystemPrepStatus,
    installWebView2DeveloperModeCapability,
    moduleCatalog,
    moduleCatalogBusy,
    moduleInstallBusy,
    moduleInstallModuleId,
    moduleInstallProgress,
    moduleInstallResult,
    refreshModuleCatalog,
    installModule,
    cancelModuleInstall,
    prepareCheckpointAob,
    executeCheckpointForceValue,
    createTrainerFeature,
    createTrainerClrFieldFeature,
    createTrainerFeatureFromCheckpoint,
    generateTrainerFeaturePointerChain,
    applyTrainerFeature,
    restoreTrainerFeature,
    applyAllTrainerFeatures,
    restoreAllTrainerFeatures,
    deleteTrainerFeature,
    updateTrainerFeatureDependencies,
    saveTrainerFeatureToProfile,
    clearTrainerFeatures,
    exportTrainerFeaturesJson,
    exportTrainerFeaturesMarkdown,
    exportWorkspaceJson,
    exportWorkspaceMarkdown,
    previewWorkspaceImport,
    importWorkspaceJson,
    saveStructureTemplate,
    deleteStructureTemplate,
    clearStructureTemplates,
    addWorkspaceBookmark,
    updateWorkspaceBookmark,
    createWorkspaceBookmarkFromCheckpoint,
    useWorkspaceBookmarkAsWriteTarget,
    createTrainerFeatureFromBookmark,
    deleteWorkspaceBookmark,
    clearWorkspaceBookmarks,
    saveCurrentWorkspaceProject,
    loadWorkspaceProject,
    deleteWorkspaceProject,
    clearWorkspaceProjects,
    registerTrainerFeatureHotkey,
    unregisterTrainerFeatureHotkey,
    registerOverlayHotkey,
    unregisterOverlayHotkey,
    setTrainerOverlay,
    refreshTrainerOverlay,
    clearAutoResolveMemory,
    clearActiveChatMemoryTargets,
    acknowledgePendingSmartSearchRecovery,
    clearSmartSearchDebug,
    doSearch,
    doAutoResolve,
    updateMessage,
    startNewSearchContext,
    useSuggestedAddresses,
    searchValueElsewhere,
    searchValueAsType,
    testSingleSuggestedAddress,
    doExactScan,
    doEncryptedScan,
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
    setWatchedPointerChainsLiveEnabled,
    watchedPointerChainsLiveEnabled,
    runAutoEncryptedScan,
    runAutoTraceUiString,
    cancelActiveScan,
    refreshCandidates,
    nextCandidatePage,
    previousCandidatePage,
    doNextScan,
    undoCandidateScan,
    captureUnknownSnapshot,
    doUnknownNextScan,
    runUnknownGuideStep,
    runAutoUnknownObservation,
    selectCandidate,
    openExpertAtAddress,
    openExpertForRegion,
    scanAroundPreview,
    inferredExactTypes,
    useInferredType,
    updateWriteSafetyWarning,
    addAddressToWatch,
    addAddressesToWatch,
    removeAddressFromWatch,
    clearWatchedAddresses,
    refreshWatchedAddress,
    refreshWatchedAddresses,
    setWatchLiveEnabled,
    updateSessionEntryLabel,
    disableSessionEntry,
    hasFreezeInstability,
    createSessionGroup,
    updateSessionGroupName,
    removeSessionGroup,
    removeSessionEntriesFromGroup,
    moveSessionEntryToGroup,
    disableSessionGroup,
    isSessionPromotionBusy,
    promoteSessionEntryToTrainer,
    promoteSessionGroupToTrainer,
    keepCandidate,
    ignoreCandidate,
    candidateVisualState,
    writeSelectedValue,
    writeSelectedAddresses,
    writeSelectedTargets,
    writeSelectedAtomic,
    rollbackLastWrite,
    rollbackLastWriteBatch,
    freezeCandidateCurrent,
    toggleFreeze,
    startBreakpointFreeze,
    escalateFreezeToBreakpoint,
    injectDllPath,
    injectionResult,
    hookTargetAddress,
    hookFunctionAddress,
    activeFunctionHook,
    symbolModuleName,
    symbolFunctionName,
    symbolResolveResult,
    resolveSymbol,
    applyResolvedSymbolToHookTarget,
    autoAsmScriptText,
    autoAsmPreview,
    autoAsmResult,
    autoAsmScriptName,
    autoAsmSavedScripts,
    autoAsmSaveResult,
    autoAsmSavedScriptsBusy,
    injectionBusy,
    injectDll,
    installHook,
    removeHook,
    previewAutoAsmScript,
    executeAutoAsmScript,
    restoreAutoAsmScript,
    refreshSavedAutoAsmScripts,
    saveAutoAsmScript,
    applySavedAutoAsmScript,
    deleteSavedAutoAsmScript,
    stopBreakpointFreeze,
    setFreezeInterval,
    pushMessage,
    tellNewValue,
    doGuidedChange,
    resetWorkflow,
  }
})
