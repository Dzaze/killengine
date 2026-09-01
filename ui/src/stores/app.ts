import { defineStore, storeToRefs } from 'pinia'
import { ref, computed } from 'vue'
import {
  backend,
  type AutoResolveReportResult,
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

export type AppView = 'assistant' | 'investigation' | 'trainer' | 'process' | 'memory' | 'clr' | 'webview2' | 'scripting' | 'speedhack' | 'network' | 'profiles' | 'expert' | 'lexicon' | 'settings'

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

export const useAppStore = defineStore('app', () => {
  // State
  const activeView = ref<AppView>('assistant')
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
    if (!isConnected.value) return 'Déconnecté'
    if (!isAttached.value) return 'Prêt'
    return `Attaché: ${processName.value}`
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
        result.success === true ? 'Session freeze désactivé' : 'Session freeze échoué',
        `0x${entry.address}. ${String(result.error ?? '')}`.trim(),
        result.success === true ? 'success' : 'error',
      )
    } catch (e) {
      addActionLog('freeze', 'Session freeze échoué', String(e), 'error')
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
              label: `AOB unique (${score}/100, ${fixedBytes} octets fixes)`,
              warning: '',
            }
          }
        }
      } catch (e) {
        addActionLog('trainer', 'Promotion Session : AOB ignoré', String(e), 'warning')
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
            label: `Pointer chain ${bestChain.module}+${bestChain.baseOffset}`,
            warning: '',
          }
        }
      } catch (e) {
        addActionLog('trainer', 'Promotion Session : pointer chain ignorée', String(e), 'warning')
      }
    }

    return {
      featureInput: { locatorKind: 'absolute' },
      label: 'Adresse absolue',
      warning: 'Aucun locator AOB unique ni pointer chain stable trouvé : cette feature ne survivra probablement pas à un relaunch ou à un changement de scène.',
    }
  }

  async function promoteSessionEntryToTrainer(entryId: string, name?: string): Promise<SessionPromotionResult> {
    const entry = sessionEntries.value.find((candidate) => candidate.id === entryId)
    if (!entry) return { success: false, featureIds: [], message: 'Entrée session introuvable.', warnings: [] }
    setSessionPromotionBusy(entryId, true)
    try {
      const value = await sessionTrainerValue(entry)
      if (!value) {
        const message = `Valeur live illisible pour 0x${entry.address}.`
        addActionLog('trainer', 'Promotion Session refusée', message, 'warning')
        return { success: false, featureIds: [], message, warnings: [] }
      }
      const locator = await resolveSessionTrainerLocator(entry)
      const feature = createTrainerFeature({
        name: name?.trim() || entry.label.trim() || `Session 0x${entry.address}`,
        action: sessionTrainerAction(entry),
        address: entry.address,
        valueType: entry.valueType,
        value,
        ...locator.featureInput,
      })
      if (!feature) {
        return { success: false, featureIds: [], message: 'Création Trainer refusée.', warnings: locator.warning ? [locator.warning] : [] }
      }
      const detail = locator.warning
        ? `${locator.label}. ${locator.warning}`
        : `${locator.label}.`
      addActionLog('trainer', `Session promue: ${feature.name}`, detail, locator.warning ? 'warning' : 'success')
      return {
        success: true,
        featureIds: [feature.id],
        message: `${feature.name} créée (${locator.label}).`,
        warnings: locator.warning ? [locator.warning] : [],
      }
    } catch (e) {
      const message = String(e)
      addActionLog('trainer', 'Promotion Session échouée', message, 'error')
      return { success: false, featureIds: [], message, warnings: [] }
    } finally {
      setSessionPromotionBusy(entryId, false)
    }
  }

  async function promoteSessionGroupToTrainer(groupId: string, name?: string): Promise<SessionPromotionResult> {
    const group = sessionGroups.value.find((candidate) => candidate.id === groupId)
    if (!group) return { success: false, featureIds: [], message: 'Groupe session introuvable.', warnings: [] }
    setSessionPromotionBusy(groupId, true)
    const featureIds: number[] = []
    const warnings: string[] = []
    try {
      for (const memberId of group.memberIds) {
        const entry = sessionEntries.value.find((candidate) => candidate.id === memberId)
        if (!entry) continue
        const entryName = `${name?.trim() || group.name || 'Groupe session'} · ${entry.label.trim() || `0x${entry.address}`}`
        const result = await promoteSessionEntryToTrainer(memberId, entryName)
        featureIds.push(...result.featureIds)
        warnings.push(...result.warnings)
      }
      const success = featureIds.length > 0
      const message = success
        ? `${featureIds.length} feature(s) Trainer créée(s) depuis ${group.name}.`
        : `Aucune feature Trainer créée depuis ${group.name}.`
      addActionLog('trainer', success ? 'Groupe Session promu' : 'Promotion groupe échouée', message, success ? (warnings.length ? 'warning' : 'success') : 'error')
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
      addActionLog('investigation', 'Archive restaurée', activeInvestigation.value.objective, 'success')
    }
    return restored
  }

  function exportInvestigationJson(): string {
    return investigationStore.exportInvestigationJson()
  }

  function exportInvestigationMarkdown(): string {
    const run = activeInvestigation.value
    if (!run) return '# Investigation\n\nAucune investigation active.\n'
    const report = autoResolveReport.value
    const nextBestAction = report?.nextBestAction && typeof report.nextBestAction === 'object'
      ? report.nextBestAction as Record<string, unknown>
      : null
    const topCheckpoints = [...run.checkpoints]
      .sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0))
      .slice(0, 5)
    const topActionPlans = topCheckpoints.map((checkpoint) => buildCheckpointActionPlan(checkpoint))
    const guardrails = Array.isArray(report?.guardrails) ? report.guardrails.slice(0, 8) : []
    const lines = [
      `# ${run.title}`,
      '',
      `Objectif: ${run.objective}`,
      `Processus: ${run.processName || 'non attache'}`,
      `Statut: ${run.status}`,
      '',
      '## Strategie',
      String(run.preferredStrategy?.label ?? 'Non determinee'),
      '',
      '## Meilleure Prochaine Action',
      nextBestAction
        ? `${String(nextBestAction.label ?? nextBestAction.id ?? 'Action')} (${String(nextBestAction.risk ?? 'safe')}${nextBestAction.confidence !== undefined ? `, confiance ${String(nextBestAction.confidence)}/100` : ''})`
        : String(run.checkpoints[0]?.label ?? run.hypotheses[0]?.label ?? 'Non determinee'),
      nextBestAction ? String(nextBestAction.reason ?? '') : String(run.checkpoints[0]?.reason ?? run.summary ?? ''),
      '',
      '## Etapes',
      ...run.steps.slice().reverse().map((step) => `- [${step.status}] ${step.title} - ${step.detail}`),
      '',
      '## Checkpoints',
      ...run.checkpoints.map((checkpoint) => `- ${String(checkpoint.label ?? checkpoint.address ?? checkpoint.id ?? 'checkpoint')} (${String(checkpoint.kind ?? 'checkpoint')}${checkpoint.confidenceLabel ? `, ${String(checkpoint.confidenceLabel)}` : ''})`),
      '',
      '## Top 5 Checkpoints Scores',
      ...(topCheckpoints.length > 0
        ? topCheckpoints.map((checkpoint, index) => `${index + 1}. ${String(checkpoint.label ?? checkpoint.address ?? 'checkpoint')} - ${String(checkpoint.kind ?? 'checkpoint')} - ${String(checkpoint.confidenceLabel ?? `score ${Number(checkpoint.confidenceScore ?? 0)}/100`)} - ${checkpoint.requiresConfirmation === true ? 'confirmation requise' : 'safe'}`)
        : ['Aucun checkpoint score.']),
      '',
      '## Plans D Action Checkpoints',
      ...(topActionPlans.length > 0
        ? topActionPlans.map((plan, index) => `${index + 1}. ${plan.label} - ${plan.safeCount} safe / ${plan.riskyCount} confirmation - ${plan.actions.filter((action) => action.enabled).map((action) => `${action.label}(${action.risk})`).join(', ') || 'aucune action active'}`)
        : ['Aucun plan d action.']),
      '',
      '## Garde-fous Actifs',
      ...(guardrails.length > 0
        ? guardrails.map((guardrail) => `- ${String(guardrail.label ?? guardrail.id ?? 'guardrail')} (${String(guardrail.risk ?? 'risk')})`)
        : ['Aucun garde-fou remonte.']),
      '',
      '## Bilan Auto',
      `Etapes safe: ${run.steps.filter((step) => step.risk === 'safe' && step.status === 'success').length}`,
      `Checkpoints actionnables: ${run.checkpoints.length}`,
      `Meilleure piste: ${String(run.checkpoints[0]?.label ?? run.hypotheses[0]?.label ?? 'non determinee')}`,
      `Prochaine etape: ${String(run.hypotheses[0]?.nextAction ?? run.checkpoints[0]?.reason ?? run.summary ?? 'continuer la reduction ou valider un checkpoint')}`,
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
      addActionLog('workspace', 'Bookmark inutilisable', 'Adresse manquante pour remplir Write / Freeze.', 'warning')
      return false
    }
    selectedCandidateAddress.value = bookmark.address
    if (bookmark.type) exactScanType.value = bookmark.type
    if (bookmark.value !== undefined) writeValue.value = bookmark.value
    addActionLog('workspace', `Bookmark chargé: ${bookmark.label}`, `Write / Freeze préparé sur 0x${bookmark.address}.`, 'success')
    addInvestigationStep({
      title: 'Bookmark chargé dans Write / Freeze',
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
    const label = String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? checkpoint.id ?? 'Checkpoint').trim() || 'Checkpoint'
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
        score > 0 ? `score ${score}/100` : '',
        checkpoint.requiresConfirmation === true ? 'confirmation requise' : 'safe',
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
      title: 'Bookmark créé depuis checkpoint',
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
      'Activer le mode Automation',
      "Autorise le pipe d'automatisation local (utilisé par le scripting Lua ke.call(...) et par tout agent/outil externe sur cette machine) à exécuter des lectures/écritures mémoire SANS confirmation par action, tant que le mode reste actif.",
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
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: 'Réponse backend absente.' }
    } finally {
      webView2CdpDebugFlagBusy.value = false
    }
  }

  async function enableWebView2CdpDebugFlag() {
    const accepted = await confirmRiskAction(
      'debug',
      'Activer le débogage CDP WebView2',
      "Force TOUS les hôtes WebView2 du user Windows courant (pas seulement une cible précise) à exposer un port de débogage CDP (--remote-debugging-port=9333) à leur PROCHAIN lancement — nécessaire pour inspecter l'état JavaScript d'une app WebView2/Electron/CEF non packagée (les apps Store/UWP passent par une autre voie, voir le diagnostic ci-dessous). Désactivable à tout moment.",
    )
    if (!accepted) return null
    webView2CdpDebugFlagBusy.value = true
    try {
      const result = await backend.getController().enableWebView2CdpDebugFlag?.()
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: 'Réponse backend absente.' }
      return result ?? null
    } finally {
      webView2CdpDebugFlagBusy.value = false
    }
  }

  async function disableWebView2CdpDebugFlag() {
    webView2CdpDebugFlagBusy.value = true
    try {
      const result = await backend.getController().disableWebView2CdpDebugFlag?.()
      webView2CdpDebugFlagStatus.value = result ?? { success: false, error: 'Réponse backend absente.' }
      return result ?? null
    } finally {
      webView2CdpDebugFlagBusy.value = false
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
      webView2SystemPrepStatus.value = result ?? { success: false, error: 'Réponse backend absente.' }
    } finally {
      webView2SystemPrepBusy.value = false
    }
  }

  async function installWebView2DeveloperModeCapability() {
    const accepted = await confirmRiskAction(
      'debug',
      "Installer la capacité Windows « Mode développeur »",
      "Lance Add-WindowsCapability avec une invite UAC visible pour installer Tools.DeveloperMode.Core — prérequis pour activer le Portail d'appareil Windows, nécessaire à l'inspection CDP des apps UWP/Store (ex: apps du Microsoft Store). Ne débloque PAS le port CDP direct des apps Store (restriction AppContainer séparée, toujours présente). Peut prendre plusieurs minutes et rester silencieux : suivre l'état dans Paramètres Windows ou relancer ce diagnostic ensuite.",
    )
    if (!accepted) return null
    webView2SystemPrepBusy.value = true
    webView2CapabilityInstallResult.value = null
    try {
      const result = await backend.getController().installWebView2DeveloperModeCapability?.()
      webView2CapabilityInstallResult.value = result ?? { success: false, error: 'Réponse backend absente.' }
      return result ?? null
    } finally {
      webView2SystemPrepBusy.value = false
    }
  }

  async function executeCheckpointFindWhatWrites(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    if (!address) {
      addActionLog('checkpoint', 'Debugger impossible', 'Adresse manquante.', 'warning')
      return null
    }
    const sizeByType: Record<string, number> = {
      Int8: 1, UInt8: 1, Int16: 2, UInt16: 2, Int32: 4, UInt32: 4, Float32: 4, Int64: 8, UInt64: 8, Float64: 8,
    }
    const size = sizeByType[type] ?? 4
    if (!await confirmRiskAction('debug', 'Checkpoint Find What Writes', `0x${address}, taille ${size}, fenêtre 5000 ms.`)) return null
    const controller = backend.getController()
    if (!controller.findWhatWrites) {
      addActionLog('checkpoint', 'Find What Writes indisponible', 'Backend non exposé.', 'warning')
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
        title: hits.length > 0 ? 'Find What Writes capturé' : 'Find What Writes sans hit',
        detail: `${hits.length} hit(s) pour 0x${address}.`,
        status: hits.length > 0 ? 'checkpoint' : 'warning',
        tool: 'findWhatWrites',
        risk: 'debug',
        payload: result,
      })
      logAiAudit('checkpoint_find_writes_executed', { address, type, size, hitCount: hits.length, success: result.success === true })
      return result
    } catch (e) {
      addActionLog('checkpoint', 'Find What Writes échoué', String(e), 'error')
      return null
    }
  }

  async function executeCheckpointDisassembleBackward(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    if (!address) {
      addActionLog('checkpoint', 'Désassemblage impossible', 'Adresse RIP manquante.', 'warning')
      return null
    }
    if (!await confirmRiskAction('patch', 'Désassembler en amont', `Lire les octets avant l'instruction 0x${address} et reconstruire les instructions précédentes (lecture seule).`)) return null
    const controller = backend.getController()
    if (!controller.disassembleBackward) {
      addActionLog('checkpoint', 'Désassemblage indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const result = await controller.disassembleBackward(address, {})
      const candidates = Array.isArray(result.candidateFields) ? result.candidateFields : []
      if (candidates.length > 0 && activeInvestigation.value) {
        activeInvestigation.value.checkpoints = [
          ...candidates.slice(0, 6).map((field) => ({
            kind: 'candidate_field',
            label: `Champ candidat [${field.memBaseRegister}+0x${(field.memDisplacement ?? 0).toString(16)}]`,
            address: field.address,
            sourceAddress: address,
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: candidates.length > 0 ? 'Désassemblage en amont : champs candidats trouvés' : 'Désassemblage en amont sans champ candidat',
        detail: `${candidates.length} champ(s) candidat(s) pour RIP 0x${address}.`,
        status: candidates.length > 0 ? 'checkpoint' : 'warning',
        tool: 'disassembleBackward',
        risk: 'patch',
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit('checkpoint_disassemble_backward_executed', { address, candidateCount: candidates.length, success: result.success === true })
      return result
    } catch (e) {
      addActionLog('checkpoint', 'Désassemblage en amont échoué', String(e), 'error')
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
      addActionLog('checkpoint', 'Test de champs candidats impossible', 'Adresse RIP ou adresse écrite manquante.', 'warning')
      return null
    }
    if (!await confirmRiskAction(
      'write',
      'Tester les champs candidats',
      `Écrit une valeur test transitoire sur chaque champ candidat trouvé avant 0x${writeInstructionAddressHex}, `
      + 'attend quelques secondes, puis restaure systématiquement la valeur d\'origine.',
    )) return null

    const controller = backend.getController()
    const testCandidateFieldsAsync = controller.testCandidateFieldsAsync
    const candidateFieldTestFinished = controller.candidateFieldTestFinished
    if (!testCandidateFieldsAsync || !candidateFieldTestFinished) {
      addActionLog('checkpoint', 'Test de champs candidats indisponible', 'Backend non exposé.', 'warning')
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
          resolve({ success: false, error: 'Timeout du test de champs candidats.' })
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
            resolve({ success: false, error: String(start.error ?? 'Impossible de démarrer le test de champs candidats.') })
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
            label: `Champ testé [${field.memBaseRegister}+0x${(field.memDisplacement ?? 0).toString(16)}] : tient`,
            address: field.address,
            sourceAddress: writeInstructionAddressHex,
            valueType: field.valueType,
            confidenceScore: 95,
            confidenceLabel: `Testé empiriquement : tient ${field.ticksSurvived ?? 0} sondage(s)`,
            requiresConfirmation: true,
          })),
          ...activeInvestigation.value.checkpoints,
        ].slice(0, 12)
        saveInvestigations()
      }
      addInvestigationStep({
        title: holding.length > 0 ? 'Test de champs candidats : source trouvée' : 'Test de champs candidats : rien ne tient',
        detail: `${outcomes.length} champ(s) testé(s), ${holding.length} tien(nen)t.`,
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
      addActionLog('checkpoint', 'Test de champs candidats échoué', String(e), 'error')
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
    if (!await confirmRiskAction('injection', 'Intercepter '+apiHookModuleName.value+'!'+apiHookFunctionName.value, `Injecte un composant MinHook dans le processus cible pour intercepter les appels à ${apiHookModuleName.value}!${apiHookFunctionName.value}.`)) return null
    return speedhackStore.startApiHook()
  }

  async function stopApiHook() {
    return speedhackStore.stopApiHook()
  }

  async function refreshApiHookStatus() {
    return speedhackStore.refreshApiHookStatus()
  }

  async function startSpeedhack(factor: number) {
    if (!await confirmRiskAction('injection', 'Activer le speedhack', `Injecte un composant dans le processus cible pour modifier la vitesse perçue du temps (facteur ${factor}x).`)) return null
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
    if (!await confirmRiskAction('injection', 'Couper le réseau du processus', `Ajoute une règle pare-feu Windows bloquant tout le trafic entrant/sortant de ${processName.value || 'ce processus'} (invite UAC requise).`)) return null
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
      addActionLog('checkpoint', 'AOB impossible', 'Adresse instruction manquante.', 'warning')
      return null
    }
    if (!await confirmRiskAction('patch', 'Checkpoint AOB/patch', `Lire l'instruction 0x${address}, générer une signature et proposer des patchs sans application.`)) return null
    const controller = backend.getController()
    if (!controller.generateAobSignature || !controller.suggestCodePatches) {
      addActionLog('checkpoint', 'AOB indisponible', 'Backend non exposé.', 'warning')
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
              label: String(patchRecord.label ?? `Patch 0x${address}`),
              address,
              patchBytes: String(patchRecord.patchBytes ?? patchRecord.bytesText ?? ''),
              risk: String(patchRecord.risk ?? patchRecord.riskLevel ?? 'medium'),
              aobPattern: signature.pattern,
              requiresConfirmation: true,
            }
          }),
          {
            kind: 'aob_signature',
            label: `Signature AOB 0x${address}`,
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
        title: 'AOB/patch préparé',
        detail: `${patchSuggestions.length} suggestion(s), signature ${String(signature.pattern ?? '').slice(0, 80)}.`,
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
      addActionLog('checkpoint', 'AOB échoué', String(e), 'error')
      return null
    }
  }

  async function executeCheckpointForceValue(checkpoint: Record<string, unknown>, value: string) {
    const address = checkpointAddress(checkpoint)
    const trimmedValue = value.trim()
    if (!address || !trimmedValue) {
      addActionLog('checkpoint', 'Forcer valeur impossible', 'RIP ou valeur manquante.', 'warning')
      return null
    }
    const controller = backend.getController()
    if (!controller.suggestCodePatches || !controller.forceWriteInstructionValue) {
      addActionLog('checkpoint', 'Forcer valeur indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const suggestions = await controller.suggestCodePatches(address, { maxBytes: 16 })
      const memBaseRegister = String(suggestions.memBaseRegister ?? '').trim()
      if (!memBaseRegister) {
        addActionLog('checkpoint', 'Forcer valeur impossible', 'Instruction sans destination mémoire exploitable (adressage indexé ou RIP-relatif).', 'warning')
        return null
      }
      const type = checkpointType(checkpoint)
      if (!await confirmRiskAction('patch', 'Checkpoint forcer valeur (hook)', `Installer un trampoline sur RIP 0x${address} pour forcer ${type} = ${trimmedValue}.`)) return null
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
        result.success === true ? 'Forcer valeur (hook) OK' : 'Forcer valeur (hook) échoué',
        String(result.error || `0x${address} ${type} = ${trimmedValue}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? 'Checkpoint forcer valeur exécuté' : 'Checkpoint forcer valeur échoué',
        detail: String(result.error || `0x${address} ${type} = ${trimmedValue} (trampoline)`),
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
      addActionLog('checkpoint', 'Forcer valeur (hook) échoué', String(e), 'error')
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
            String(info.message ?? `Le freeze sur 0x${address} ne tient pas.`) + ' ' + String(info.suggestion ?? ''),
            {
              recoveryActions: [
                {
                  id: 'escalate_freeze_bp',
                  label: 'Passer en Freeze BP',
                  address,
                  requiresConfirmation: true,
                },
                { id: 'open_expert', label: 'Ouvrir Expert' },
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
            String(info.message ?? `La valeur écrite à 0x${address} a changé toute seule.`) + ' ' + String(info.suggestion ?? ''),
            {
              recoveryActions: [
                {
                  id: 'find_what_writes_targets',
                  label: 'Capturer qui écrit dessus',
                  address,
                  type,
                },
                { id: 'open_expert', label: 'Ouvrir Expert' },
              ],
            },
          )
        })
        backendWriteWatchSignalConnected = true
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

  async function requestWindowsDefenderExclusion() {
    defenderExclusionBusy.value = true
    defenderExclusionResult.value = null
    try {
      const result = await backend.getController().requestWindowsDefenderExclusion?.()
      defenderExclusionResult.value = result ?? { success: false, error: 'Réponse backend absente.' }
    } finally {
      defenderExclusionBusy.value = false
    }
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
    if (!await confirmRiskAction('injection', 'Écriture mémoire via driver noyau', `0x${addressHex} = ${hexBytes.trim()} (contourne les protections mémoire usermode).`)) return
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
      mode === 'kernel' ? 'Mode mémoire kernel' : 'Mode mémoire standard',
      mode === 'kernel'
        ? 'Les lectures/écritures interactives utiliseront le driver quand il est disponible.'
        : 'Les lectures/écritures interactives utiliseront les API usermode.',
      'info',
    )
  }

  function kernelUnavailableResult(): MemoryWriteResult {
    return {
      success: false,
      verified: false,
      bytesWritten: 0,
      error: 'Mode Kernel actif, mais le driver ne fournit pas Accès mémoire kernel. Repasse en Standard ou teste le driver dans Paramètres.',
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
          error: 'Mode Kernel actif, mais la lecture kernel est indisponible. Repasse en Standard ou teste le driver dans Paramètres.',
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
        return { success: false, error: 'Mode Kernel actif, mais écriture kernel indisponible.' }
      }
      const result = await controller.writeMemoryKernel(addressHex, hexString)
      kernelMemoryWriteResult.value = result
      return result as unknown as Record<string, unknown>
    }
    if (!controller.writeMemoryHex) {
      return { success: false, error: 'writeMemoryHex non disponible dans ce backend.' }
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
        message: 'Scripting Lua non exposé par ce backend.',
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
    const label = ok ? 'Script Lua exécuté' : (payload.cancelled ? 'Script Lua annulé' : 'Script Lua échoué')
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
      'Exécution script Lua',
      'Le script peut appeler le pipe d’automatisation KillEngine et déclencher les actions exposées par le backend.',
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
          luaScriptResult.value = { success: false, error: 'Timeout client en attente du script Lua.' }
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
            luaScriptResult.value = { success: false, error: String(start.error ?? 'Impossible de démarrer le script Lua.') }
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
      luaScriptResult.value = { success: false, error: 'Exécution Lua non exposée par ce backend.' }
      return
    }

    luaScriptBusy.value = true
    try {
      luaScriptResult.value = await controller.executeLuaScript(script, options)
      logLuaScriptOutcome(luaScriptResult.value)
    } catch (e) {
      luaScriptResult.value = { success: false, error: String(e) }
      addActionLog('lua_script', 'Script Lua échoué', String(e), 'error')
    } finally {
      luaScriptBusy.value = false
    }
  }

  async function cancelLuaScriptExecution() {
    const controller = backend.getController()
    if (!controller.cancelLuaScriptExecution) {
      addActionLog('lua_script', 'Annulation indisponible', 'cancelLuaScriptExecution absent du backend.', 'warning')
      return
    }
    try {
      const result = await controller.cancelLuaScriptExecution()
      if (result.success !== true) {
        addActionLog('lua_script', 'Annulation impossible', String(result.error ?? ''), 'warning')
      }
    } catch (e) {
      addActionLog('lua_script', 'Annulation impossible', String(e), 'warning')
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
      luaScriptSaveResult.value = { success: false, error: 'Sauvegarde Lua non exposée par ce backend.' }
      return
    }
    try {
      luaScriptSaveResult.value = await controller.saveProfileLuaScript(luaProfileName(), name, script, {})
      const ok = luaScriptSaveResult.value.success === true
      addActionLog('lua_script', ok ? 'Script Lua sauvegardé' : 'Sauvegarde script Lua échouée', String(luaScriptSaveResult.value.error ?? name), ok ? 'success' : 'error')
      if (ok) await refreshSavedLuaScripts()
    } catch (e) {
      luaScriptSaveResult.value = { success: false, error: String(e) }
      addActionLog('lua_script', 'Sauvegarde script Lua échouée', String(e), 'error')
    }
  }

  function loadSavedLuaScript(name: string) {
    const entry = luaSavedScripts.value.find((s) => s.name === name)
    if (!entry) return
    luaScriptText.value = String(entry.scriptText ?? '')
    luaScriptSaveName.value = name
    addActionLog('lua_script', `Script "${name}" chargé`, '', 'success')
  }

  async function deleteSavedLuaScript(name: string) {
    const controller = backend.getController()
    if (!controller.deleteProfileLuaScript) return
    try {
      const result = await controller.deleteProfileLuaScript(luaProfileName(), name)
      const ok = result.success === true
      addActionLog('lua_script', ok ? `Script "${name}" supprimé` : `Suppression "${name}" échouée`, String(result.error ?? ''), ok ? 'success' : 'error')
      if (ok) await refreshSavedLuaScripts()
    } catch (e) {
      addActionLog('lua_script', `Suppression "${name}" échouée`, String(e), 'error')
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
          `Attach ${processName.value}`,
          mode === 'kernel'
            ? 'Process attaché avec mode mémoire Kernel actif pour les lectures/écritures interactives.'
            : 'Process attaché avec mode mémoire Standard.',
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
    if (!await confirmRiskAction('write', 'Écriture champ CLR', `${address}.${field} = ${text}. Champ primitif managé dans le processus attaché.`)) return
    return clrInspectorStore.writeClrPrimitiveField(address, field, text)
  }

  async function writeClrPrimitivePath(objectAddressHex: string, path: string, value: string) {
    const address = objectAddressHex.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!address || !pathText || !text) return
    if (!await confirmRiskAction('write', 'Écriture chemin CLR', `${address}.${pathText} = ${text}. Chemin symbolique managé dans le processus attaché.`)) return
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
    if (!await confirmRiskAction('write', 'Transaction CLR', `${address}: ${preview}. Rollback tenté si une écriture échoue.`)) return
    return clrInspectorStore.writeClrPrimitivePathBatch(address, sanitized)
  }

  async function writeClrPrimitivePathByLocator(typeSubstring: string, identityField: string, identityValue: string, path: string, value: string) {
    const type = typeSubstring.trim()
    const idField = identityField.trim()
    const idValue = identityValue.trim()
    const pathText = path.trim()
    const text = value.trim()
    if (!type || !idField || !idValue || !pathText || !text) return
    if (!await confirmRiskAction('write', 'Écriture chemin CLR par locator', `${type} (${idField}=${idValue}).${pathText} = ${text}. Objet relocalisé juste avant l'écriture (résistant à un déplacement GC).`)) return
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
    if (!await confirmRiskAction('write', 'Transaction CLR par locator', `${type} (${idField}=${idValue}): ${preview}. Objet relocalisé juste avant l'écriture, rollback tenté si une opération échoue.`)) return
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
    if (!await confirmRiskAction('write', 'Transaction CLR atomique (process suspendu)', `${address}: ${preview}. Suspend TOUTES les threads du processus attaché pendant l'écriture -- best-effort, pas une garantie absolue d'absence de deadlock.`)) return
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
    if (!await confirmRiskAction('injection', 'Appeler un setter CLR', `${address}.${method}(${text || '<0 argument>'}). Injecte et exécute réellement le setter dans le processus attaché.`)) return
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
        result.success ? 'Fichiers de sauvegarde découverts' : 'Découverte sauvegardes échouée',
        result.success
          ? `${discoveredSaveFiles.value.length} fichier(s)${discoveredSaveFilesFamilyName.value ? ` · ${discoveredSaveFilesFamilyName.value}` : ''}.`
          : (result.error ?? 'Erreur inconnue.'),
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
      addActionLog('save_files', 'Découverte sauvegardes échouée', String(e), 'error')
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
        'Snapshot "avant" pris',
        `${saveFileSnapshotBefore.value.length} fichier(s)${result.familyName ? ` · ${result.familyName}` : ''}.`,
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
        'Snapshot "après" pris',
        `${saveFileSnapshotAfter.value.length} fichier(s)${result.familyName ? ` · ${result.familyName}` : ''}.`,
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
      const finalResult: SaveFileSnapshotDiffResult = result ?? { success: false, error: 'Méthode non disponible' }
      saveFileSnapshotDiff.value = finalResult
      addActionLog(
        'save_files',
        finalResult.success ? 'Comparaison des snapshots terminée' : 'Comparaison échouée',
        finalResult.success
          ? `${finalResult.addedCount ?? 0} ajouté(s), ${finalResult.removedCount ?? 0} supprimé(s), ${finalResult.modifiedCount ?? 0} modifié(s).`
          : (finalResult.error ?? 'Erreur inconnue.'),
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
      const result = { success: false, path, text: '', truncated: false, error: 'Chemin vide.' }
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
        result.success ? 'Fichier de sauvegarde lu' : 'Lecture sauvegarde échouée',
        result.success ? `${trimmedPath}${result.truncated ? ' · tronqué' : ''}` : (result.error ?? 'Erreur inconnue.'),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      const result = { success: false, path: trimmedPath, text: '', truncated: false, error: String(e) }
      selectedSaveFileText.value = result
      console.error('[KillEngine] Failed to read process save file text:', e)
      addActionLog('save_files', 'Lecture sauvegarde échouée', String(e), 'error')
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
        result.success ? 'LocalSettings inspecté' : 'Inspection LocalSettings échouée',
        result.success
          ? `${result.count ?? result.values.length} valeur(s)${result.familyName ? ` · ${result.familyName}` : ''}.`
          : (result.error ?? 'Erreur inconnue.'),
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      const result = { success: false, values: [], error: String(e) }
      localSettingsResult.value = result
      console.error('[KillEngine] Failed to inspect process LocalSettings:', e)
      addActionLog('save_files', 'Inspection LocalSettings échouée', String(e), 'error')
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
      addActionLog('save_files', 'Surveillance fichier indisponible', 'startSaveFileWatchAsync absent du backend.', 'warning')
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
          ok ? 'Changement détecté' : (payload.cancelled ? 'Surveillance annulée' : 'Aucun changement avant timeout'),
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
          saveFileWatchResult.value = { success: false, error: String(start.error ?? 'Impossible de démarrer la surveillance.') }
          addActionLog('save_files', 'Surveillance fichier échouée', String(start.error ?? ''), 'error')
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
        addActionLog('save_files', 'Surveillance fichier échouée', String(e), 'error')
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
        addActionLog('save_files', 'Annulation surveillance impossible', String(result.error ?? ''), 'warning')
      }
    } catch (e) {
      addActionLog('save_files', 'Annulation surveillance impossible', String(e), 'warning')
    }
  }

  async function patchSelectedSaveFileBytes(path: string, findHex: string, replaceHex: string) {
    const trimmedPath = path.trim()
    if (!trimmedPath || !findHex.trim() || !replaceHex.trim()) return
    if (!await confirmRiskAction(
      'patch',
      'Édition d\'octets dans un fichier de sauvegarde',
      `Remplace la séquence "${findHex.trim()}" par "${replaceHex.trim()}" dans ${trimmedPath}. Refusé si la séquence n'apparaît pas exactement une fois ou si la longueur diffère.`,
    )) return

    const controller = backend.getController()
    if (!controller.patchProcessSaveFileBytes) {
      saveFilePatchResult.value = { success: false, error: 'Édition de fichier non exposée par ce backend.' }
      return
    }
    saveFilePatchBusy.value = true
    try {
      const result = await controller.patchProcessSaveFileBytes(trimmedPath, findHex.trim(), replaceHex.trim())
      saveFilePatchResult.value = result
      addActionLog(
        'save_files',
        result.success ? 'Fichier patché' : 'Patch fichier échoué',
        result.success ? `${trimmedPath} (${result.occurrencesFound ?? 1} occurrence)` : (result.error ?? 'Erreur inconnue.'),
        result.success ? 'success' : 'error',
      )
      return result
    } catch (e) {
      const result = { success: false, error: String(e) }
      saveFilePatchResult.value = result
      addActionLog('save_files', 'Patch fichier échoué', String(e), 'error')
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
        error: 'readMemoryBlock non disponible dans ce backend.',
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
    addActionLog('watch', `${boundedTargets.length} adresse(s) envoyée(s) au live`, `${added} nouvelle(s), limite ${limit}.`, 'info')
  }

  function removeAddressFromWatch(address: string) {
    const normalized = address.trim().replace(/^0x/i, '')
    watchedAddresses.value = watchedAddresses.value.filter((item) => item.address !== normalized)
  }

  function clearWatchedAddresses() {
    watchedAddresses.value = []
    addActionLog('watch', 'Watch live vidé', 'Toutes les adresses surveillées ont été retirées.', 'info')
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
    addActionLog('watch', enabled ? 'Watch live activé' : 'Watch live arrêté', `${watchedAddresses.value.length} adresse(s).`, enabled ? 'success' : 'info')
  }

  async function injectDll() {
    const path = injectDllPath.value.trim()
    if (!path) return
    if (!await confirmRiskAction('injection', 'Injection DLL', `Injecter "${path}" dans le processus attaché via CreateRemoteThread + LoadLibraryW.`)) return

    const controller = backend.getController()
    if (!controller.injectDllIntoProcess) {
      injectionResult.value = { success: false, error: 'Injection DLL non exposée par ce backend.' }
      addActionLog('injection', 'Injection DLL indisponible', injectionResult.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      injectionResult.value = await controller.injectDllIntoProcess(path)
      const ok = injectionResult.value.success === true
      addActionLog('injection', ok ? 'DLL injectée' : 'Injection DLL échouée', `${path}. ${String(injectionResult.value.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      injectionResult.value = { success: false, error: String(e) }
      addActionLog('injection', 'Injection DLL échouée', String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function installHook() {
    const target = hookTargetAddress.value.trim()
    const hook = hookFunctionAddress.value.trim()
    if (!target || !hook) return
    if (!await confirmRiskAction('injection', 'Installation hook', `Installer un inline hook sur 0x${target} -> 0x${hook}. Intercepte tous les appels à cette fonction.`)) return

    const controller = backend.getController()
    if (!controller.installFunctionHook) {
      activeFunctionHook.value = { success: false, error: 'Hooking non exposé par ce backend.' }
      addActionLog('injection', 'Hook indisponible', activeFunctionHook.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      activeFunctionHook.value = await controller.installFunctionHook(target, hook)
      const ok = activeFunctionHook.value.success === true
      addActionLog('injection', ok ? 'Hook installé' : 'Installation hook échouée', `0x${target} -> 0x${hook}. ${String(activeFunctionHook.value.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      activeFunctionHook.value = { success: false, error: String(e) }
      addActionLog('injection', 'Installation hook échouée', String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function removeHook() {
    const target = hookTargetAddress.value.trim()
    if (!target) return

    const controller = backend.getController()
    if (!controller.removeFunctionHook) {
      addActionLog('injection', 'Retrait hook indisponible', 'removeFunctionHook absent du backend.', 'warning')
      return
    }

    injectionBusy.value = true
    try {
      const result = await controller.removeFunctionHook(target)
      const ok = result.success === true
      if (ok) activeFunctionHook.value = null
      addActionLog('injection', ok ? 'Hook retiré' : 'Retrait hook échoué', `0x${target}. ${String(result.error ?? '')}`.trim(), ok ? 'success' : 'error')
    } catch (e) {
      addActionLog('injection', 'Retrait hook échoué', String(e), 'error')
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
      symbolResolveResult.value = { success: false, error: 'Résolution de symbole non exposée par ce backend.' }
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
        ok ? 'Symbole résolu' : 'Résolution de symbole échouée',
        `${moduleName}!${functionName} ${detail}`.trim(),
        ok ? 'success' : 'warning',
      )
    } catch (e) {
      symbolResolveResult.value = { success: false, error: String(e) }
      addActionLog('injection', 'Résolution de symbole échouée', String(e), 'error')
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
      autoAsmPreview.value = { success: false, parseError: 'Aperçu auto-assembler non exposé par ce backend.' }
      return
    }
    autoAsmPreview.value = await controller.parseAutoAssemblerScript(script)
  }

  async function executeAutoAsmScript() {
    const script = autoAsmScriptText.value
    if (!script.trim()) return
    if (!await confirmRiskAction('injection', 'Exécution script auto-assembler', 'Alloue de la mémoire et patche le processus attaché avec le code compilé du script. Vérifie l\'aperçu avant de confirmer.')) return

    const controller = backend.getController()
    if (!controller.executeAutoAssemblerScript) {
      autoAsmResult.value = { success: false, error: 'Exécution auto-assembler non exposée par ce backend.' }
      addActionLog('injection', 'Auto-assembler indisponible', autoAsmResult.value.error as string, 'warning')
      return
    }

    injectionBusy.value = true
    try {
      autoAsmResult.value = await controller.executeAutoAssemblerScript(script)
      const ok = autoAsmResult.value.success === true
      addActionLog('injection', ok ? 'Script auto-assembler exécuté' : 'Exécution script échouée', String(autoAsmResult.value.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      autoAsmResult.value = { success: false, error: String(e) }
      addActionLog('injection', 'Exécution script échouée', String(e), 'error')
    } finally {
      injectionBusy.value = false
    }
  }

  async function restoreAutoAsmScript() {
    const controller = backend.getController()
    if (!controller.restoreAutoAssemblerScript) {
      addActionLog('injection', 'Restauration auto-assembler indisponible', 'restoreAutoAssemblerScript absent du backend.', 'warning')
      return
    }

    injectionBusy.value = true
    try {
      const result = await controller.restoreAutoAssemblerScript()
      const ok = result.success === true
      if (ok) autoAsmResult.value = null
      addActionLog('injection', ok ? 'Script auto-assembler restauré' : 'Restauration script échouée', String(result.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      addActionLog('injection', 'Restauration script échouée', String(e), 'error')
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
      autoAsmSaveResult.value = { success: false, error: 'Sauvegarde auto-assembler non exposée par ce backend.' }
      return
    }
    try {
      autoAsmSaveResult.value = await controller.saveProfileAutoAsmScript(autoAsmProfileName(), name, script, {})
      const ok = autoAsmSaveResult.value.success === true
      addActionLog('injection', ok ? 'Script auto-assembler sauvegardé' : 'Sauvegarde script échouée', String(autoAsmSaveResult.value.error ?? name), ok ? 'success' : 'error')
      if (ok) await refreshSavedAutoAsmScripts()
    } catch (e) {
      autoAsmSaveResult.value = { success: false, error: String(e) }
      addActionLog('injection', 'Sauvegarde script échouée', String(e), 'error')
    }
  }

  async function applySavedAutoAsmScript(name: string) {
    if (!await confirmRiskAction('injection', 'Exécution script sauvegardé', `Alloue de la mémoire et patche le processus attaché avec le script "${name}".`)) return
    const controller = backend.getController()
    if (!controller.applyProfileAutoAsmScript) {
      autoAsmResult.value = { success: false, error: 'Exécution auto-assembler non exposée par ce backend.' }
      return
    }
    injectionBusy.value = true
    try {
      autoAsmResult.value = await controller.applyProfileAutoAsmScript(autoAsmProfileName(), name)
      const ok = autoAsmResult.value.success === true
      addActionLog('injection', ok ? `Script "${name}" exécuté` : `Exécution "${name}" échouée`, String(autoAsmResult.value.error ?? ''), ok ? 'success' : 'error')
    } catch (e) {
      autoAsmResult.value = { success: false, error: String(e) }
      addActionLog('injection', `Exécution "${name}" échouée`, String(e), 'error')
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
      addActionLog('injection', ok ? `Script "${name}" supprimé` : `Suppression "${name}" échouée`, String(result.error ?? ''), ok ? 'success' : 'error')
      if (ok) await refreshSavedAutoAsmScripts()
    } catch (e) {
      addActionLog('injection', `Suppression "${name}" échouée`, String(e), 'error')
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
          throw new Error(`Action non autorisée (bridge JS) : ${action}`)
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
