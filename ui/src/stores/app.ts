import { defineStore } from 'pinia'
import { ref, computed, nextTick, watch } from 'vue'
import {
  backend,
  type AppSettings,
  type AiModelStatus,
  type AtomicWriteTarget,
  type AutoResolveReportResult,
  type CandidateFieldTestResult,
  type CandidatePage,
  type ClrInspectorStatus,
  type ClrObjectReadResult,
  type ClrObjectSummary,
  type ClrRootInfo,
  type ClrRpcResult,
  type EncryptedScanResult,
  type ChatMemoryTargetsResult,
  type ExactScanResult,
  type KernelDriverStatus,
  type KernelMemoryReadResult,
  type KernelMemoryWriteResult,
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
  type UiStringCandidate,
  type UiStringSourceCandidate,
  type UiStringSourceResult,
  type UiStringScanResult,
  type UnknownNextScanResult,
  type UnknownSnapshotResult,
  type AobPatternQuality,
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

export interface UserActionLogEntry {
  id: number
  time: string
  kind: string
  title: string
  detail: string
  status: 'info' | 'success' | 'warning' | 'error'
}

export interface InvestigationStep {
  id: number
  time: string
  title: string
  detail: string
  status: 'planned' | 'running' | 'success' | 'warning' | 'error' | 'checkpoint'
  tool?: string
  risk?: 'safe' | 'write' | 'debug' | 'patch' | 'injection'
  confidence?: number
  payload?: Record<string, unknown>
}

export interface InvestigationRun {
  id: number
  title: string
  objective: string
  processName: string
  startedAt: string
  updatedAt: string
  status: 'active' | 'checkpoint' | 'done' | 'blocked'
  preferredStrategy?: Record<string, unknown>
  summary: string
  steps: InvestigationStep[]
  hypotheses: Array<Record<string, unknown>>
  checkpoints: Array<Record<string, unknown>>
}

export interface TrainerFeature {
  id: number
  name: string
  processName: string
  action: 'write' | 'freeze_polling' | 'freeze_breakpoint' | 'patch'
  locatorKind: 'absolute' | 'aob'
  address: string
  valueType: string
  value: string
  patchBytes?: string
  aobPattern?: string
  signatureQuality?: AobPatternQuality
  signatureScore?: number
  signatureLevel?: string
  signatureWarning?: string
  signatureFixedBytes?: number
  signatureWildcardBytes?: number
  signatureUniqueFixedBytes?: number
  signatureFixedRatio?: number
  trainerSafe?: boolean
  signatureMatches?: number
  hotkey?: string
  hotkeyId?: number
  enabled: boolean
  status: 'idle' | 'active' | 'error' | 'ambiguous'
  lastError: string
  history?: Array<{
    time: string
    action: string
    status: 'success' | 'warning' | 'error' | 'info'
    detail: string
  }>
  createdAt: string
  updatedAt: string
}

export interface StructureTemplateField {
  offset: number
  type: string
  label: string
  note: string
  sampleValue: string
  rawHex?: string
}

export interface StructureTemplate {
  id: number
  name: string
  processName: string
  baseAddress: string
  size: number
  fieldCount: number
  fields: StructureTemplateField[]
  createdAt: string
  updatedAt: string
}

export interface WorkspaceBookmark {
  id: number
  kind: 'address' | 'structure_field' | 'aob' | 'pointer' | 'note'
  label: string
  processName: string
  address?: string
  type?: string
  value?: string
  note: string
  payload?: Record<string, unknown>
  createdAt: string
  updatedAt: string
}

export interface WorkspaceProject {
  id: number
  name: string
  processName: string
  snapshotJson: string
  investigationCount: number
  trainerFeatureCount: number
  structureTemplateCount: number
  bookmarkCount: number
  auditCount?: number
  createdAt: string
  updatedAt: string
}

export interface RuntimeActionPlanItem {
  id: 'watch' | 'write' | 'freeze_polling' | 'find_writes' | 'aob_patch' | 'force_value' | 'bookmark' | 'trainer'
  label: string
  risk: 'safe' | 'write' | 'debug' | 'patch'
  enabled: boolean
  reason: string
}

export interface RuntimeActionPlan {
  label: string
  address: string
  type: string
  value: string
  kind: string
  isCode: boolean
  safeCount: number
  riskyCount: number
  actions: RuntimeActionPlanItem[]
}

export type AppView = 'assistant' | 'investigation' | 'trainer' | 'process' | 'memory' | 'clr' | 'profiles' | 'expert' | 'settings'

export interface WorkflowPreset {
  id: string
  title: string
  description: string
  prompt: string
  startView: AppView
  mode: 'auto' | 'manual'
  valueType?: string
  risk: 'safe' | 'write' | 'debug' | 'patch' | 'injection'
  nextStep: string
}

export interface RiskDialogState {
  open: boolean
  risk: NonNullable<InvestigationStep['risk']>
  title: string
  detail: string
  mode: AppSettings['autoRiskMode']
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
  const activeView = ref<AppView>('assistant')
  const uiMode = ref<'beginner' | 'expert'>('beginner')
  const version = ref('...')
  const isConnected = ref(false)
  const showOnboarding = ref(false)
  const isAttached = ref(false)
  const processName = ref('')
  const processes = ref<ProcessInfo[]>([])
  const processModules = ref<ProcessModuleInfo[]>([])
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
  // Memoire de pattern structuree par jeu (module+offset relatif, roadmap H.2) —
  // distincte de learnedProfile ci-dessus qui n'a qu'un seul "dernier succes" ecrase.
  const rememberedPatterns = ref<Array<Record<string, unknown>>>([])
  // Sequence ordonnee (ordre + doublons conserves) des dernieres ecritures
  // confirmees, persistee par executable — roadmap I, replay inter-session.
  const writeHistorySequence = ref<Array<Record<string, unknown>>>([])
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
  const settingAutoRiskMode = ref<AppSettings['autoRiskMode']>('Safe')
  // Raison du dernier blocage confirmRiskAction (mode Auto trop restrictif),
  // distincte d'une vraie annulation utilisateur. Sans ça, les appelants qui
  // batissent un message d'erreur apres un confirmRiskAction refuse ne
  // peuvent pas distinguer les deux cas et affichent a tort "annule par
  // l'utilisateur" alors que c'est le reglage Auto qui bloque.
  const lastRiskBlockReason = ref('')
  // Action de l'echelle d'escalade (Assistant) dont le backend attend la
  // reponse en texte libre, quand cette action ne vit que cote frontend
  // (ex: encrypted_scan, qui boucle sur plusieurs modes via runAutoEncryptedScan
  // et n'a pas d'equivalent backend unique a appeler directement).
  const pendingAssistantAction = ref('')
  const settingModelPath = ref('')
  const settingModelEnabled = ref(true)
  const settingModelThreads = ref(4)
  const aiModelStatus = ref<AiModelStatus | null>(null)
  const aiModelStatusLoading = ref(false)
  const aiModelStatusError = ref('')
  const kernelDriverStatus = ref<KernelDriverStatus | null>(null)
  const kernelDriverStatusLoading = ref(false)
  const kernelDriverStatusError = ref('')
  const kernelMemoryReadResult = ref<KernelMemoryReadResult | null>(null)
  const kernelMemoryReadBusy = ref(false)
  const kernelMemoryWriteResult = ref<KernelMemoryWriteResult | null>(null)
  const kernelMemoryWriteBusy = ref(false)
  const kernelMemoryReady = computed(() => kernelDriverStatus.value?.capabilities.processMemoryAccess === true)
  const kernelMemoryModeActive = computed(() => memoryAccessMode.value === 'kernel')
  const clrInspectorStatus = ref<ClrInspectorStatus | null>(null)
  const clrInspectorBusy = ref(false)
  const clrInspectorError = ref('')
  const clrTypeFilter = ref('KillEngine.ClrTestTarget')
  const clrObjects = ref<ClrObjectSummary[]>([])
  const clrSelectedObject = ref<ClrObjectReadResult | null>(null)
  const clrRoots = ref<ClrRootInfo[]>([])
  const clrLastResult = ref<ClrRpcResult | null>(null)
  const workflowPresets = ref<WorkflowPreset[]>([
    {
      id: 'exact-value',
      title: 'Valeur directe',
      description: 'Quand tu connais la valeur actuelle et la valeur cible.',
      prompt: 'Valeur actuelle 100, objectif 9999',
      startView: 'assistant',
      mode: 'auto',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Auto lance un scan exact puis prépare un checkpoint si peu de candidats restent.',
    },
    {
      id: 'unknown-change',
      title: 'Valeur inconnue',
      description: 'Quand tu sais seulement que la valeur augmente, diminue ou change.',
      prompt: 'Je ne connais pas la valeur exacte, aide-moi à la retrouver avec une recherche unknown',
      startView: 'assistant',
      mode: 'auto',
      valueType: 'Auto',
      risk: 'safe',
      nextStep: 'Auto prépare une capture unknown, puis attend ton observation suivante.',
    },
    {
      id: 'display-trace',
      title: 'Valeur affichée introuvable',
      description: 'Quand le scan numérique ne trouve rien mais le texte est visible à l’écran.',
      prompt: 'La valeur affichée existe mais le scan exact ne trouve rien, lance Trace UI string',
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Expert ouvre Trace UI string pour chercher texte, sources numériques et backrefs.',
    },
    {
      id: 'stable-trainer',
      title: 'Transformer en trainer',
      description: 'Quand une adresse ou signature semble fiable et doit devenir un toggle.',
      prompt: 'Transforme la trouvaille confirmée en feature Trainer réutilisable',
      startView: 'trainer',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'write',
      nextStep: 'Trainer prépare une feature locale avec confirmation avant écriture, freeze ou patch.',
    },
    {
      id: 'code-investigation',
      title: 'Qui écrit cette valeur',
      description: 'Quand il faut comprendre quelle instruction modifie une adresse confirmée.',
      prompt: 'Adresse confirmée : trouver ce qui écrit dessus puis proposer une signature AOB',
      startView: 'investigation',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'debug',
      nextStep: 'Investigation garde le checkpoint; Expert lance Find What Writes uniquement après confirmation.',
    },
    // Scénarios courants Expert/Trainer (finition commerciale) : mêmes champs
    // que les presets Assistant ci-dessus, réutilisent applyWorkflowPreset()
    // tel quel — juste des données, pas un nouveau mécanisme.
    {
      id: 'scenario-money',
      title: 'Argent / Or',
      description: 'Ressource principale du jeu (pièces, or, crédits...).',
      prompt: 'Je cherche l\'argent ou l\'or, valeur affichée à l\'écran',
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Lance un scan exact avec la valeur affichée dans le panneau Scan exact.',
    },
    {
      id: 'scenario-health',
      title: 'Vie / PV',
      description: 'Points de vie ou de santé du joueur.',
      prompt: 'Je cherche les points de vie (HP), valeur affichée à l\'écran',
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Lance un scan exact avec la valeur affichée, fais varier la vie en jeu puis réduis.',
    },
    {
      id: 'scenario-score',
      title: 'Score / Niveau',
      description: 'Score, expérience ou niveau du joueur.',
      prompt: 'Je cherche le score ou le niveau, valeur affichée à l\'écran',
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Lance un scan exact, ou passe en Unknown si la valeur change en continu.',
    },
    {
      id: 'scenario-ammo',
      title: 'Munitions',
      description: 'Compteur de munitions ou de ressources consommables.',
      prompt: 'Je cherche les munitions, valeur affichée à l\'écran',
      startView: 'expert',
      mode: 'manual',
      valueType: 'Int32',
      risk: 'safe',
      nextStep: 'Lance un scan exact, tire une fois en jeu puis réduis avec la nouvelle valeur.',
    },
  ])
  const lastWorkflowPresetId = ref('')
  const activeChatMemoryTargets = ref<Array<Record<string, unknown>>>([])
  const smartSearchContext = ref<SmartSearchContextResult | null>(null)
  const investigationReport = ref<InvestigationReport | null>(null)
  const activeInvestigation = ref<InvestigationRun | null>(null)
  const investigationArchive = ref<InvestigationRun[]>([])
  const investigationStepIdCounter = ref(0)
  const investigationRunIdCounter = ref(0)
  const trainerFeatures = ref<TrainerFeature[]>([])
  const trainerFeatureIdCounter = ref(0)
  const trainerBusy = ref(false)
  const trainerHotkeyStatus = ref('')
  const trainerOverlayVisible = ref(false)
  const trainerOverlayStatus = ref('')
  // Hotkey dediee pour afficher/masquer l'overlay lui-meme (roadmap G) — distincte
  // des hotkeys par feature deja existantes (freeze/patch/write toggle).
  const trainerOverlayHotkey = ref('')
  const trainerOverlayHotkeyId = ref<number | undefined>(undefined)
  const structureTemplates = ref<StructureTemplate[]>([])
  const structureTemplateIdCounter = ref(0)
  const workspaceBookmarks = ref<WorkspaceBookmark[]>([])
  const workspaceBookmarkIdCounter = ref(0)
  const workspaceProjects = ref<WorkspaceProject[]>([])
  const workspaceProjectIdCounter = ref(0)
  const riskDialog = ref<RiskDialogState | null>(null)
  let riskDialogResolver: ((accepted: boolean) => void) | null = null
  const searchQuery = ref('')
  const searchResult = ref('')
  const exactScanValue = ref('')
  const exactScanType = ref('Int32')
  const exactScanResult = ref<ExactScanResult | null>(null)
  const encryptedScanResult = ref<EncryptedScanResult | null>(null)
// ---- Scan groupe (P1) : N valeurs avec offsets fixes connus ----
interface GroupScanEntryInput {
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
interface WatchedPointerChain {
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
  const autoUiStringScanResult = ref<UiStringScanResult | null>(null)
  const autoUiStringSourceResult = ref<UiStringSourceResult | null>(null)
  const autoUiStringSources = ref<UiStringSourceCandidate[]>([])
  const encryptedScanMode = ref<'xor' | 'add' | 'sub' | 'not'>('xor')
  const encryptedScanKey = ref('0')
  const encryptedScanKeySearchBits = ref(0)

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
  const unknownScanType = ref('Auto')
  const unknownWritableOnly = ref(true)
  const unknownCopyOnWriteOnly = ref(false)
  const unknownSnapshotResult = ref<UnknownSnapshotResult | null>(null)
  const unknownNextScanResult = ref<UnknownNextScanResult | null>(null)
  const unknownGuideSteps = ref<UnknownGuideStep[]>([])
  const autoUnknownAwaitingObservation = ref(false)
  const unknownGuideStepIdCounter = ref(0)
  const selectedCandidateAddress = ref('')
  const writeValue = ref('')
  const writeResult = ref<MemoryWriteResult | null>(null)
  const writeSafetyWarning = ref('')
  const writeSafetyAcknowledged = ref(false)
  const freezeEnabled = ref(false)
  const breakpointFreezeEnabled = ref(false)
  const freezeIntervalMs = ref(100)
  const freezeIntervalResult = ref<Record<string, unknown> | null>(null)
  const finalCandidateTargets = ref<Array<Record<string, unknown>>>([])
  const ignoredCandidateAddresses = ref<string[]>([])
  const keptCandidateAddresses = ref<string[]>([])
  const watchLiveEnabled = ref(false)
  const watchedAddresses = ref<WatchedAddress[]>([])
  const watchLiveReadLimit = 200
  let watchLiveTimer: ReturnType<typeof setInterval> | null = null
  // Watch expressions (roadmap I) : re-evaluation live des chaines de pointeurs
  // watchees, distinct du timer watchLiveTimer ci-dessus (adresses fixes).
  const watchedPointerChainsLiveEnabled = ref(false)
  let watchedPointerChainsLiveTimer: ReturnType<typeof setInterval> | null = null

  // Phase 20 — outils Expert manuels d'injection/hooking/auto-assembler,
  // gardés par confirmRiskAction('injection', ...) (mode Auto = Trainer requis,
  // cf. logique existante de confirmRiskAction). État panneau uniquement,
  // rien n'est persisté en profil pour l'instant (pas de feature Trainer 'hook').
  const injectDllPath = ref('')
  const injectionResult = ref<Record<string, unknown> | null>(null)
  const hookTargetAddress = ref('')
  const hookFunctionAddress = ref('')
  const activeFunctionHook = ref<Record<string, unknown> | null>(null)
  const autoAsmScriptText = ref('')
  const autoAsmPreview = ref<Record<string, unknown> | null>(null)
  const autoAsmResult = ref<Record<string, unknown> | null>(null)
  // Persistance des scripts auto-assembleur (roadmap E.4) : rejouables sans retaper le texte.
  const autoAsmScriptName = ref('')
  const autoAsmSavedScripts = ref<Array<Record<string, unknown>>>([])
  const autoAsmSaveResult = ref<Record<string, unknown> | null>(null)
  const autoAsmSavedScriptsBusy = ref(false)
  const injectionBusy = ref(false)

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
  let backendHotkeySignalConnected = false
  let backendFreezeInstabilitySignalConnected = false
  let backendWriteWatchSignalConnected = false
  // Defense-in-depth cote frontend : le backend ne notifie deja qu'une fois
  // par adresse (FreezeEntry::flaggedUnstable), ce Set couvre juste le cas
  // d'une reconnexion du signal (ex: rechargement dev).
  const freezeInstabilityNotified = new Set<string>()

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
    saveActionLog()
  }

  const actionLogStorageKey = 'killengine.action_log.v1'
  const investigationStorageKey = 'killengine.investigation.v1'

  function saveActionLog() {
    try {
      window.localStorage.setItem(actionLogStorageKey, JSON.stringify({
        entries: actionLog.value.slice(0, 200),
        id: actionLogIdCounter.value,
      }))
    } catch {
      // Best-effort audit: runtime actions must continue even if local storage is full.
    }
  }

  function loadActionLog() {
    try {
      const raw = window.localStorage.getItem(actionLogStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { entries?: UserActionLogEntry[], id?: number }
      actionLog.value = Array.isArray(parsed.entries) ? parsed.entries.slice(0, 200) : []
      actionLogIdCounter.value = Number(parsed.id ?? actionLog.value.reduce((max, entry) => Math.max(max, Number(entry.id) || 0), 0))
    } catch {
      actionLog.value = []
      actionLogIdCounter.value = 0
    }
  }

  function clearActionLog() {
    actionLog.value = []
    actionLogIdCounter.value = 0
    saveActionLog()
  }

  function exportActionLogJson(): string {
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      processName: processName.value,
      entries: actionLog.value,
    }, null, 2)
  }

  function exportActionLogMarkdown(): string {
    const lines = [
      '# KillEngine Audit Log',
      '',
      `Export: ${new Date().toISOString()}`,
      `Processus: ${processName.value || 'non attache'}`,
      `Entrées: ${actionLog.value.length}`,
      '',
      ...actionLog.value.slice(0, 200).map((entry) =>
        `- ${entry.time} [${entry.status}] ${entry.kind} - ${entry.title}${entry.detail ? `: ${entry.detail}` : ''}`,
      ),
    ]
    return lines.join('\n')
  }

  function saveInvestigations() {
    try {
      window.localStorage.setItem(investigationStorageKey, JSON.stringify({
        active: activeInvestigation.value,
        archive: investigationArchive.value.slice(0, 20),
        stepId: investigationStepIdCounter.value,
        runId: investigationRunIdCounter.value,
      }))
    } catch {
      // Best-effort persistence: analysis must keep working even if storage is unavailable.
    }
  }

  function loadInvestigations() {
    try {
      const raw = window.localStorage.getItem(investigationStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as {
        active?: InvestigationRun | null
        archive?: InvestigationRun[]
        stepId?: number
        runId?: number
      }
      activeInvestigation.value = parsed.active ?? null
      investigationArchive.value = Array.isArray(parsed.archive) ? parsed.archive.slice(0, 20) : []
      investigationStepIdCounter.value = Number(parsed.stepId ?? 0)
      investigationRunIdCounter.value = Number(parsed.runId ?? 0)
    } catch {
      activeInvestigation.value = null
      investigationArchive.value = []
    }
  }

  function startInvestigation(objective: string, title = 'Investigation Auto') {
    if (activeInvestigation.value) {
      investigationArchive.value.unshift({
        ...activeInvestigation.value,
        status: activeInvestigation.value.status === 'active' ? 'blocked' : activeInvestigation.value.status,
      })
      investigationArchive.value = investigationArchive.value.slice(0, 20)
    }

    investigationRunIdCounter.value += 1
    const now = new Date().toISOString()
    activeInvestigation.value = {
      id: investigationRunIdCounter.value,
      title,
      objective,
      processName: processName.value,
      startedAt: now,
      updatedAt: now,
      status: 'active',
      summary: '',
      steps: [],
      hypotheses: [],
      checkpoints: [],
    }
    saveInvestigations()
  }

  function addInvestigationStep(step: Omit<InvestigationStep, 'id' | 'time'>) {
    if (!activeInvestigation.value) {
      startInvestigation(searchQuery.value || 'Investigation manuelle')
    }
    if (!activeInvestigation.value) return

    investigationStepIdCounter.value += 1
    activeInvestigation.value.steps.unshift({
      id: investigationStepIdCounter.value,
      time: nowTime(),
      ...step,
    })
    activeInvestigation.value.updatedAt = new Date().toISOString()
    if (step.status === 'checkpoint') activeInvestigation.value.status = 'checkpoint'
    if (step.status === 'error') activeInvestigation.value.status = 'blocked'
    saveInvestigations()
  }

  function applyWorkflowPreset(id: string) {
    const preset = workflowPresets.value.find((item) => item.id === id)
    if (!preset) {
      addActionLog('workflow', 'Preset introuvable', id, 'warning')
      return null
    }
    lastWorkflowPresetId.value = preset.id
    searchQuery.value = preset.prompt
    if (preset.valueType) {
      if (preset.valueType === 'Auto') unknownScanType.value = 'Auto'
      else exactScanType.value = preset.valueType
    }
    if (!activeInvestigation.value) {
      startInvestigation(preset.prompt, `Preset: ${preset.title}`)
    }
    addInvestigationStep({
      title: `Preset chargé: ${preset.title}`,
      detail: `${preset.description} Prochaine étape: ${preset.nextStep}`,
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
    pushMessage('assistant', `Preset chargé: ${preset.title}. ${preset.nextStep}`)
    activeView.value = preset.startView
    addActionLog('workflow', `Preset chargé: ${preset.title}`, preset.nextStep, 'success')
    return preset
  }

  function updateInvestigationFromAutoResult(result: Record<string, unknown>) {
    if (!activeInvestigation.value) return

    const contextReport = typeof result.contextReport === 'object' && result.contextReport !== null
      ? result.contextReport as Record<string, unknown>
      : {}
    const preferredStrategy = typeof contextReport.preferredStrategy === 'object' && contextReport.preferredStrategy !== null
      ? contextReport.preferredStrategy as Record<string, unknown>
      : undefined
    const recommendations = Array.isArray(contextReport.recommendations)
      ? contextReport.recommendations as Array<Record<string, unknown>>
      : []
    const nextBestAction = typeof contextReport.nextBestAction === 'object' && contextReport.nextBestAction !== null
      ? contextReport.nextBestAction as Record<string, unknown>
      : null
    const telemetryInsights = Array.isArray(contextReport.telemetryInsights)
      ? contextReport.telemetryInsights as Array<Record<string, unknown>>
      : []
    const displayValueReport = typeof contextReport.displayValueReport === 'object' && contextReport.displayValueReport !== null
      ? contextReport.displayValueReport as Record<string, unknown>
      : {}
    const displayValueHypothesis = displayValueReport.enabled === true
      ? [{
          id: 'display_value_report',
          label: 'Rapport valeurs affichees',
          reason: String(displayValueReport.recommendation ?? 'Trace UI string et sources numeriques avant debugger.'),
          safe: true,
          traceUiSourceCount: displayValueReport.traceUiSourceCount,
          globalValueHits: displayValueReport.globalValueHits,
        }]
      : []
    const suggestedWrites = Array.isArray(result.suggestedWrites)
      ? result.suggestedWrites as Array<Record<string, unknown>>
      : []
    const plan = Array.isArray(result.plan)
      ? result.plan as Array<Record<string, unknown>>
      : []

    activeInvestigation.value.preferredStrategy = preferredStrategy
    activeInvestigation.value.summary = String(result.message ?? result.error ?? '').trim()
    activeInvestigation.value.hypotheses = [
      ...(nextBestAction ? [{ ...nextBestAction, id: 'next_best_action', label: `Priorité: ${String(nextBestAction.label ?? nextBestAction.id ?? 'action')}` }] : []),
      ...displayValueHypothesis,
      ...telemetryInsights,
      ...recommendations,
    ].slice(0, 8)
    activeInvestigation.value.checkpoints = [
      ...suggestedWrites.map((item) => ({ ...item, kind: 'suggested_write', requiresConfirmation: true })),
      ...telemetryInsights.filter((item) => item.safe === false || item.requiresConfirmation === true),
      ...recommendations.filter((item) => item.safe === false || item.requiresConfirmation === true),
    ].slice(0, 12)

    if (plan.length > 0 && activeInvestigation.value.steps.length === 0) {
      for (const item of plan.slice().reverse()) {
        addInvestigationStep({
          title: String(item.description ?? item.type ?? 'Etape planifiee'),
          detail: String(item.type ?? 'planned'),
          status: 'planned',
          tool: String(item.type ?? ''),
          risk: 'safe',
          payload: item,
        })
      }
    }

    saveInvestigations()
  }

  function finishInvestigation(status: InvestigationRun['status'] = 'done') {
    if (!activeInvestigation.value) return
    activeInvestigation.value.status = status
    activeInvestigation.value.updatedAt = new Date().toISOString()
    investigationArchive.value.unshift(activeInvestigation.value)
    investigationArchive.value = investigationArchive.value.slice(0, 20)
    activeInvestigation.value = null
    saveInvestigations()
  }

  function clearInvestigation() {
    activeInvestigation.value = null
    saveInvestigations()
  }

  function clearInvestigationArchive() {
    investigationArchive.value = []
    saveInvestigations()
  }

  function restoreInvestigationFromArchive(id: number) {
    const index = investigationArchive.value.findIndex((item) => item.id === id)
    if (index < 0) return false

    if (activeInvestigation.value) {
      investigationArchive.value.unshift({
        ...activeInvestigation.value,
        status: activeInvestigation.value.status === 'active' ? 'blocked' : activeInvestigation.value.status,
        updatedAt: new Date().toISOString(),
      })
    }

    const [restored] = investigationArchive.value.splice(index + (activeInvestigation.value ? 1 : 0), 1)
    if (!restored) return false

    activeInvestigation.value = {
      ...restored,
      status: restored.status === 'done' ? 'checkpoint' : restored.status,
      updatedAt: new Date().toISOString(),
    }
    investigationArchive.value = investigationArchive.value.slice(0, 20)
    saveInvestigations()
    addActionLog('investigation', 'Archive restaurée', activeInvestigation.value.objective, 'success')
    return true
  }

  function exportInvestigationJson(): string {
    return JSON.stringify(activeInvestigation.value ?? {}, null, 2)
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

  const trainerStorageKey = 'killengine.trainer.features.v1'
  const overlayHotkeyStorageKey = 'killengine.trainer.overlayHotkey.v1'
  const structureTemplateStorageKey = 'killengine.structure.templates.v1'
  const workspaceBookmarkStorageKey = 'killengine.workspace.bookmarks.v1'
  const workspaceProjectStorageKey = 'killengine.workspace.projects.v1'

  function saveTrainerFeatures() {
    try {
      window.localStorage.setItem(trainerStorageKey, JSON.stringify({
        features: trainerFeatures.value,
        id: trainerFeatureIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadTrainerFeatures() {
    try {
      const raw = window.localStorage.getItem(trainerStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { features?: TrainerFeature[], id?: number }
      trainerFeatures.value = Array.isArray(parsed.features) ? parsed.features : []
      trainerFeatureIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      trainerFeatures.value = []
    }
  }

  function saveOverlayHotkey() {
    try {
      window.localStorage.setItem(overlayHotkeyStorageKey, trainerOverlayHotkey.value)
    } catch {
      // Best-effort persistence.
    }
  }

  function loadOverlayHotkey() {
    try {
      trainerOverlayHotkey.value = window.localStorage.getItem(overlayHotkeyStorageKey) ?? ''
    } catch {
      trainerOverlayHotkey.value = ''
    }
  }

  function saveStructureTemplates() {
    try {
      window.localStorage.setItem(structureTemplateStorageKey, JSON.stringify({
        templates: structureTemplates.value,
        id: structureTemplateIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadStructureTemplates() {
    try {
      const raw = window.localStorage.getItem(structureTemplateStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { templates?: StructureTemplate[], id?: number }
      structureTemplates.value = Array.isArray(parsed.templates) ? parsed.templates.slice(0, 100) : []
      structureTemplateIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      structureTemplates.value = []
      structureTemplateIdCounter.value = 0
    }
  }

  function saveStructureTemplate(input: {
    name?: string
    baseAddress: string
    size: number
    fields: StructureTemplateField[]
  }) {
    const fields = input.fields
      .filter((field) => Number.isFinite(field.offset) && field.type.trim())
      .slice(0, 256)
    if (fields.length === 0) {
      addActionLog('structure', 'Template refusé', 'Aucun champ typé exploitable.', 'warning')
      return null
    }

    structureTemplateIdCounter.value += 1
    const now = new Date().toISOString()
    const template: StructureTemplate = {
      id: structureTemplateIdCounter.value,
      name: String(input.name ?? `Structure 0x${input.baseAddress}`).trim() || `Structure 0x${input.baseAddress}`,
      processName: processName.value,
      baseAddress: input.baseAddress.replace(/^0x/i, '').toUpperCase(),
      size: Math.max(0, Math.round(input.size)),
      fieldCount: fields.length,
      fields,
      createdAt: now,
      updatedAt: now,
    }
    structureTemplates.value.unshift(template)
    structureTemplates.value = structureTemplates.value.slice(0, 100)
    saveStructureTemplates()
    addActionLog('structure', `Template sauvegardé: ${template.name}`, `${template.fieldCount} champ(s).`, 'success')
    return template
  }

  function deleteStructureTemplate(id: number) {
    const before = structureTemplates.value.length
    structureTemplates.value = structureTemplates.value.filter((item) => item.id !== id)
    if (structureTemplates.value.length !== before) {
      saveStructureTemplates()
      addActionLog('structure', 'Template supprimé', `id=${id}`, 'warning')
    }
  }

  function clearStructureTemplates() {
    structureTemplates.value = []
    saveStructureTemplates()
    addActionLog('structure', 'Templates vidés', 'Tous les templates locaux ont été supprimés.', 'warning')
  }

  function saveWorkspaceBookmarks() {
    try {
      window.localStorage.setItem(workspaceBookmarkStorageKey, JSON.stringify({
        bookmarks: workspaceBookmarks.value,
        id: workspaceBookmarkIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadWorkspaceBookmarks() {
    try {
      const raw = window.localStorage.getItem(workspaceBookmarkStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { bookmarks?: WorkspaceBookmark[], id?: number }
      workspaceBookmarks.value = Array.isArray(parsed.bookmarks) ? parsed.bookmarks.slice(0, 500) : []
      workspaceBookmarkIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      workspaceBookmarks.value = []
      workspaceBookmarkIdCounter.value = 0
    }
  }

  function addWorkspaceBookmark(input: Partial<WorkspaceBookmark>) {
    workspaceBookmarkIdCounter.value += 1
    const now = new Date().toISOString()
    const bookmark: WorkspaceBookmark = {
      id: workspaceBookmarkIdCounter.value,
      kind: input.kind ?? 'address',
      label: String(input.label ?? input.address ?? 'Bookmark').trim() || 'Bookmark',
      processName: String(input.processName ?? processName.value),
      address: input.address ? String(input.address).replace(/^0x/i, '').toUpperCase() : undefined,
      type: input.type ? String(input.type) : undefined,
      value: input.value ? String(input.value) : undefined,
      note: String(input.note ?? ''),
      payload: input.payload,
      createdAt: now,
      updatedAt: now,
    }
    workspaceBookmarks.value.unshift(bookmark)
    workspaceBookmarks.value = workspaceBookmarks.value.slice(0, 500)
    saveWorkspaceBookmarks()
    addActionLog('workspace', `Bookmark ajouté: ${bookmark.label}`, bookmark.address ? `0x${bookmark.address}` : bookmark.note, 'success')
    return bookmark
  }

  function updateWorkspaceBookmark(id: number, input: Partial<WorkspaceBookmark>) {
    const bookmark = workspaceBookmarks.value.find((item) => item.id === id)
    if (!bookmark) return null
    bookmark.kind = input.kind ?? bookmark.kind
    bookmark.label = input.label !== undefined ? String(input.label).trim() || bookmark.label : bookmark.label
    bookmark.processName = input.processName !== undefined ? String(input.processName) : bookmark.processName
    bookmark.address = input.address !== undefined
      ? String(input.address).replace(/^0x/i, '').trim().toUpperCase() || undefined
      : bookmark.address
    bookmark.type = input.type !== undefined ? String(input.type).trim() || undefined : bookmark.type
    bookmark.value = input.value !== undefined ? String(input.value).trim() || undefined : bookmark.value
    bookmark.note = input.note !== undefined ? String(input.note) : bookmark.note
    bookmark.payload = input.payload !== undefined ? input.payload : bookmark.payload
    bookmark.updatedAt = new Date().toISOString()
    saveWorkspaceBookmarks()
    addActionLog('workspace', `Bookmark modifié: ${bookmark.label}`, bookmark.address ? `0x${bookmark.address}` : bookmark.note, 'success')
    return bookmark
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

  function createTrainerFeatureFromBookmark(id: number, action: TrainerFeature['action'] = 'write') {
    const bookmark = workspaceBookmarks.value.find((item) => item.id === id)
    if (!bookmark?.address) {
      addActionLog('trainer', 'Feature refusée', 'Bookmark sans adresse.', 'warning')
      return null
    }
    const patchBytes = String(bookmark.payload?.patchBytes ?? '').trim()
    const aobPattern = String(bookmark.payload?.aobPattern ?? '').trim()
    const signatureQuality = bookmark.payload?.signatureQuality as AobPatternQuality | undefined
    const inferredAction = action === 'patch' || patchBytes ? 'patch' : action
    const feature = createTrainerFeature({
      name: bookmark.label,
      processName: bookmark.processName || processName.value,
      action: inferredAction,
      locatorKind: bookmark.kind === 'aob' || aobPattern ? 'aob' : 'absolute',
      address: bookmark.address,
      valueType: bookmark.type || exactScanType.value,
      value: bookmark.value ?? writeValue.value,
      patchBytes: patchBytes || undefined,
      aobPattern: aobPattern || undefined,
      signatureQuality,
      signatureScore: Number(bookmark.payload?.signatureScore ?? signatureQuality?.score ?? 0) || undefined,
      signatureLevel: String(bookmark.payload?.signatureLevel ?? signatureQuality?.level ?? ''),
      signatureWarning: String(bookmark.payload?.signatureWarning ?? signatureQuality?.warning ?? ''),
      signatureFixedBytes: Number(bookmark.payload?.signatureFixedBytes ?? signatureQuality?.fixedBytes ?? 0) || undefined,
      signatureWildcardBytes: Number(bookmark.payload?.signatureWildcardBytes ?? signatureQuality?.wildcardBytes ?? 0) || undefined,
      signatureUniqueFixedBytes: Number(bookmark.payload?.signatureUniqueFixedBytes ?? signatureQuality?.uniqueFixedBytes ?? 0) || undefined,
      signatureFixedRatio: Number(bookmark.payload?.signatureFixedRatio ?? signatureQuality?.fixedRatio ?? 0) || undefined,
      trainerSafe: Boolean(bookmark.payload?.trainerSafe ?? signatureQuality?.trainerSafe ?? false) || undefined,
      signatureMatches: Number(bookmark.payload?.signatureMatches ?? 0) || undefined,
    })
    if (feature) {
      addInvestigationStep({
        title: 'Feature Trainer créée depuis bookmark',
        detail: `${feature.name} · ${feature.action} · 0x${feature.address}`,
        status: 'success',
        tool: 'createTrainerFeatureFromBookmark',
        risk: feature.action === 'patch' ? 'patch' : 'safe',
        payload: { bookmarkId: bookmark.id, featureId: feature.id },
      })
    }
    return feature
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
    const before = workspaceBookmarks.value.length
    workspaceBookmarks.value = workspaceBookmarks.value.filter((item) => item.id !== id)
    if (workspaceBookmarks.value.length !== before) {
      saveWorkspaceBookmarks()
      addActionLog('workspace', 'Bookmark supprimé', `id=${id}`, 'warning')
    }
  }

  function clearWorkspaceBookmarks() {
    workspaceBookmarks.value = []
    saveWorkspaceBookmarks()
    addActionLog('workspace', 'Bookmarks vidés', 'Tous les bookmarks locaux ont été supprimés.', 'warning')
  }

  function saveWorkspaceProjects() {
    try {
      window.localStorage.setItem(workspaceProjectStorageKey, JSON.stringify({
        projects: workspaceProjects.value,
        id: workspaceProjectIdCounter.value,
      }))
    } catch {
      // Best-effort persistence.
    }
  }

  function loadWorkspaceProjects() {
    try {
      const raw = window.localStorage.getItem(workspaceProjectStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { projects?: WorkspaceProject[], id?: number }
      workspaceProjects.value = Array.isArray(parsed.projects) ? parsed.projects.slice(0, 50) : []
      workspaceProjectIdCounter.value = Number(parsed.id ?? 0)
    } catch {
      workspaceProjects.value = []
      workspaceProjectIdCounter.value = 0
    }
  }

  async function registerTrainerFeatureHotkey(id: number, combo: string) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    const trimmed = combo.trim()
    if (!feature || !trimmed) return
    const controller = backend.getController()
    if (!controller.registerGlobalHotkey) {
      trainerHotkeyStatus.value = 'Hotkeys globales non exposées par ce backend.'
      addActionLog('hotkey', 'Hotkey indisponible', trainerHotkeyStatus.value, 'warning')
      return
    }
    if (feature.hotkeyId && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(feature.hotkeyId)
    }
    const type = feature.action === 'patch' ? 'toggle_patch' : feature.action.startsWith('freeze') ? 'toggle_freeze' : 'write_value'
    const result = await controller.registerGlobalHotkey(trimmed, {
      type,
      targetId: String(feature.id),
      label: feature.name,
      payload: { featureId: feature.id },
    })
    if (result.success === true) {
      feature.hotkey = String(result.combo ?? trimmed)
      feature.hotkeyId = Number(result.id)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'hotkey_register', 'success', feature.hotkey)
      trainerHotkeyStatus.value = `Hotkey enregistrée: ${feature.hotkey}`
      saveTrainerFeatures()
      addActionLog('hotkey', 'Hotkey Trainer enregistrée', `${feature.hotkey} -> ${feature.name}`, 'success')
    } else {
      trainerHotkeyStatus.value = String(result.error ?? 'Hotkey refusée.')
      addTrainerFeatureHistory(feature, 'hotkey_register', 'warning', trainerHotkeyStatus.value)
      saveTrainerFeatures()
      addActionLog('hotkey', 'Hotkey Trainer refusée', trainerHotkeyStatus.value, 'warning')
    }
  }

  async function unregisterTrainerFeatureHotkey(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature?.hotkeyId) return
    const controller = backend.getController()
    if (controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(feature.hotkeyId)
    }
    feature.hotkey = ''
    feature.hotkeyId = undefined
    feature.updatedAt = new Date().toISOString()
    addTrainerFeatureHistory(feature, 'hotkey_unregister', 'success', feature.name)
    saveTrainerFeatures()
    addActionLog('hotkey', 'Hotkey Trainer supprimée', feature.name, 'success')
  }

  async function handleGlobalHotkey(event: Record<string, unknown>) {
    if (event.type === 'toggle_overlay') {
      addActionLog('hotkey', 'Hotkey: Overlay', String(event.type ?? ''), 'info')
      await setTrainerOverlay(!trainerOverlayVisible.value)
      return
    }
    const featureId = Number(event.targetId ?? (event.payload as Record<string, unknown> | undefined)?.featureId)
    const feature = trainerFeatures.value.find((item) => item.id === featureId)
    if (!feature) return
    addActionLog('hotkey', `Hotkey: ${feature.name}`, String(event.type ?? ''), 'info')
    if (feature.enabled) {
      await restoreTrainerFeature(feature.id)
    } else {
      await applyTrainerFeature(feature.id)
    }
  }

  async function registerOverlayHotkey(combo: string) {
    const trimmed = combo.trim()
    if (!trimmed) return
    const controller = backend.getController()
    if (!controller.registerGlobalHotkey) {
      trainerOverlayStatus.value = 'Hotkeys globales non exposées par ce backend.'
      addActionLog('hotkey', 'Hotkey overlay indisponible', trainerOverlayStatus.value, 'warning')
      return
    }
    if (trainerOverlayHotkeyId.value && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(trainerOverlayHotkeyId.value)
    }
    const result = await controller.registerGlobalHotkey(trimmed, {
      type: 'toggle_overlay',
      label: 'Overlay Trainer',
    })
    if (result.success === true) {
      trainerOverlayHotkey.value = String(result.combo ?? trimmed)
      trainerOverlayHotkeyId.value = Number(result.id)
      trainerOverlayStatus.value = `Hotkey overlay enregistrée: ${trainerOverlayHotkey.value}`
      saveOverlayHotkey()
      addActionLog('hotkey', 'Hotkey overlay enregistrée', trainerOverlayStatus.value, 'success')
    } else {
      trainerOverlayStatus.value = String(result.error ?? 'Hotkey overlay refusée.')
      addActionLog('hotkey', 'Hotkey overlay refusée', trainerOverlayStatus.value, 'warning')
    }
  }

  async function unregisterOverlayHotkey() {
    const controller = backend.getController()
    if (trainerOverlayHotkeyId.value && controller.unregisterGlobalHotkey) {
      await controller.unregisterGlobalHotkey(trainerOverlayHotkeyId.value)
    }
    trainerOverlayHotkey.value = ''
    trainerOverlayHotkeyId.value = undefined
    saveOverlayHotkey()
    addActionLog('hotkey', 'Hotkey overlay supprimée', '', 'success')
  }

  async function reregisterPersistedHotkeys() {
    // Le gestionnaire de hotkeys cote backend (GlobalHotkeyManager) repart a
    // zero a chaque lancement de KillEngine — les hotkeyId persistes en
    // localStorage ne correspondent plus a rien. Sans ce re-enregistrement,
    // une hotkey configuree lors d'une session precedente semble toujours la
    // (visible dans l'UI) mais ne declenche plus rien tant que l'utilisateur
    // ne la reconfigure pas manuellement.
    for (const feature of trainerFeatures.value) {
      if (feature.hotkey) {
        await registerTrainerFeatureHotkey(feature.id, feature.hotkey)
      }
    }
    if (trainerOverlayHotkey.value) {
      await registerOverlayHotkey(trainerOverlayHotkey.value)
    }
  }

  async function refreshTrainerOverlay() {
    const controller = backend.getController()
    if (!trainerOverlayVisible.value || !controller.updateTrainerOverlay) return
    const result = await controller.updateTrainerOverlay({
      title: processName.value ? `KillEngine Trainer - ${processName.value}` : 'KillEngine Trainer',
      features: trainerFeatures.value.map((feature) => ({
        name: feature.name,
        action: feature.action,
        enabled: feature.enabled,
        status: feature.status,
        hotkey: feature.hotkey,
      })),
    })
    trainerOverlayStatus.value = result.success === true ? 'Overlay mis à jour.' : String(result.error ?? 'Overlay non mis à jour.')
  }

  async function setTrainerOverlay(visible: boolean) {
    const controller = backend.getController()
    if (!controller.setTrainerOverlayVisible) {
      trainerOverlayStatus.value = 'Overlay Trainer non exposé par ce backend.'
      addActionLog('overlay', 'Overlay indisponible', trainerOverlayStatus.value, 'warning')
      return
    }
    const result = await controller.setTrainerOverlayVisible(visible, { x: 24, y: 24, width: 340, height: 180 })
    trainerOverlayVisible.value = result.success === true ? visible : trainerOverlayVisible.value
    trainerOverlayStatus.value = result.success === true ? (visible ? 'Overlay affiché.' : 'Overlay masqué.') : String(result.error ?? 'Overlay refusé.')
    addActionLog('overlay', visible ? 'Overlay Trainer affiché' : 'Overlay Trainer masqué', trainerOverlayStatus.value, result.success === true ? 'success' : 'warning')
    if (visible) await refreshTrainerOverlay()
  }

  function addTrainerFeatureHistory(
    feature: TrainerFeature,
    action: string,
    status: NonNullable<TrainerFeature['history']>[number]['status'],
    detail = '',
  ) {
    feature.history = [
      {
        time: new Date().toISOString(),
        action,
        status,
        detail,
      },
      ...(feature.history ?? []),
    ].slice(0, 30)
  }

  function createTrainerFeature(input: Partial<TrainerFeature>) {
    const address = String(input.address ?? selectedCandidateAddress.value ?? '').replace(/^0x/i, '').trim()
    if (!address) {
      addActionLog('trainer', 'Feature refusée', 'Adresse manquante.', 'warning')
      return null
    }
    trainerFeatureIdCounter.value += 1
    const now = new Date().toISOString()
    const signatureQuality = input.signatureQuality
    const signatureScore = Number(signatureQuality?.score ?? input.signatureScore ?? 0)
    const signatureFixedBytes = Number(signatureQuality?.fixedBytes ?? input.signatureFixedBytes ?? 0)
    const signatureWildcardBytes = Number(signatureQuality?.wildcardBytes ?? input.signatureWildcardBytes ?? 0)
    const signatureUniqueFixedBytes = Number(signatureQuality?.uniqueFixedBytes ?? input.signatureUniqueFixedBytes ?? 0)
    const signatureFixedRatio = Number(signatureQuality?.fixedRatio ?? input.signatureFixedRatio ?? 0)
    const feature: TrainerFeature = {
      id: trainerFeatureIdCounter.value,
      name: String(input.name ?? `Feature 0x${address}`).trim() || `Feature 0x${address}`,
      processName: String(input.processName ?? processName.value),
      action: input.action ?? 'write',
      locatorKind: input.locatorKind ?? 'absolute',
      address,
      valueType: String(input.valueType ?? exactScanType.value ?? 'Int32'),
      value: String(input.value ?? writeValue.value ?? ''),
      patchBytes: input.patchBytes,
      aobPattern: input.aobPattern,
      signatureQuality,
      signatureScore: Number.isFinite(signatureScore) && signatureScore > 0 ? signatureScore : undefined,
      signatureLevel: signatureQuality?.level ?? input.signatureLevel,
      signatureWarning: signatureQuality?.warning ?? input.signatureWarning,
      signatureFixedBytes: Number.isFinite(signatureFixedBytes) && signatureFixedBytes > 0 ? signatureFixedBytes : undefined,
      signatureWildcardBytes: Number.isFinite(signatureWildcardBytes) && signatureWildcardBytes >= 0 ? signatureWildcardBytes : undefined,
      signatureUniqueFixedBytes: Number.isFinite(signatureUniqueFixedBytes) && signatureUniqueFixedBytes > 0 ? signatureUniqueFixedBytes : undefined,
      signatureFixedRatio: Number.isFinite(signatureFixedRatio) && signatureFixedRatio > 0 ? signatureFixedRatio : undefined,
      trainerSafe: signatureQuality?.trainerSafe ?? input.trainerSafe,
      signatureMatches: input.signatureMatches,
      hotkey: input.hotkey,
      hotkeyId: input.hotkeyId,
      enabled: false,
      status: 'idle',
      lastError: '',
      history: [],
      createdAt: now,
      updatedAt: now,
    }
    addTrainerFeatureHistory(feature, 'created', 'success', `${feature.action} 0x${feature.address}`)
    trainerFeatures.value.unshift(feature)
    saveTrainerFeatures()
    void refreshTrainerOverlay()
    addActionLog('trainer', `Feature créée: ${feature.name}`, `${feature.action} 0x${feature.address}.`, 'success')
    return feature
  }

  function createTrainerFeatureFromCheckpoint(checkpoint: Record<string, unknown>) {
    const patchBytes = String(checkpoint.patchBytes ?? '').trim()
    const payload = (checkpoint.payload && typeof checkpoint.payload === 'object')
      ? checkpoint.payload as Record<string, unknown>
      : {}
    const signatureQuality = (checkpoint.signatureQuality ?? payload.signatureQuality) as AobPatternQuality | undefined
    return createTrainerFeature({
      name: String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? 'Feature checkpoint'),
      action: patchBytes ? 'patch' : 'write',
      locatorKind: checkpoint.aobPattern ? 'aob' : 'absolute',
      address: String(checkpoint.address ?? ''),
      valueType: String(checkpoint.type ?? exactScanType.value),
      value: String(checkpoint.value ?? writeValue.value),
      patchBytes: patchBytes || undefined,
      aobPattern: String(checkpoint.aobPattern ?? '').trim() || undefined,
      signatureQuality,
      signatureScore: Number(checkpoint.signatureScore ?? payload.signatureScore ?? 0) || undefined,
      signatureLevel: String(checkpoint.signatureLevel ?? payload.signatureLevel ?? ''),
      signatureWarning: String(checkpoint.signatureWarning ?? payload.signatureWarning ?? ''),
      signatureFixedBytes: Number(checkpoint.signatureFixedBytes ?? payload.signatureFixedBytes ?? 0) || undefined,
      signatureWildcardBytes: Number(checkpoint.signatureWildcardBytes ?? payload.signatureWildcardBytes ?? 0) || undefined,
      signatureUniqueFixedBytes: Number(checkpoint.signatureUniqueFixedBytes ?? payload.signatureUniqueFixedBytes ?? 0) || undefined,
      signatureFixedRatio: Number(checkpoint.signatureFixedRatio ?? payload.signatureFixedRatio ?? 0) || undefined,
      trainerSafe: Boolean(checkpoint.trainerSafe ?? payload.trainerSafe ?? false) || undefined,
      signatureMatches: Number(checkpoint.signatureMatches ?? payload.signatureMatches ?? 0) || undefined,
    })
  }

  function trainerFeatureSignatureQuality(feature: TrainerFeature): AobPatternQuality | null {
    if (feature.signatureQuality) return feature.signatureQuality
    if (feature.signatureScore === undefined && !feature.signatureLevel) return null
    return {
      score: feature.signatureScore ?? 0,
      level: feature.signatureLevel || 'unknown',
      warning: feature.signatureWarning || '',
      fixedBytes: feature.signatureFixedBytes ?? 0,
      wildcardBytes: feature.signatureWildcardBytes ?? 0,
      uniqueFixedBytes: feature.signatureUniqueFixedBytes ?? 0,
      fixedRatio: feature.signatureFixedRatio ?? 0,
      trainerSafe: feature.trainerSafe ?? false,
    }
  }

  function trainerFeaturePatchBlockReason(feature: TrainerFeature): string {
    if (feature.action !== 'patch') return ''
    if (!feature.patchBytes?.trim()) return 'Patch incomplet : bytes manquants.'
    if (feature.locatorKind !== 'aob') return ''
    if (!feature.aobPattern?.trim()) return 'AOB manquant : sauvegarde une signature stable avant activation.'
    const quality = trainerFeatureSignatureQuality(feature)
    if (!quality) return ''
    const score = Number(quality.score ?? 0)
    const fixedBytes = Number(quality.fixedBytes ?? 0)
    if (fixedBytes < 3 || score < 35) {
      return `AOB trop faible (${score}/100, ${fixedBytes} octet(s) fixe(s)).`
    }
    return ''
  }

  async function resolveTrainerPatchAddress(feature: TrainerFeature): Promise<{ address: string, error: string }> {
    if (feature.action !== 'patch' || feature.locatorKind !== 'aob' || !feature.aobPattern?.trim()) {
      return { address: feature.address, error: '' }
    }
    const controller = backend.getController()
    if (!controller.scanAobPattern) {
      return { address: '', error: 'Scan AOB non expose par ce backend.' }
    }
    const scan = await controller.scanAobPattern(feature.aobPattern, {
      executableOnly: true,
      imageOnly: true,
      maxResults: 2,
    })
    if (scan.signatureQuality) {
      feature.signatureQuality = scan.signatureQuality
      feature.signatureScore = scan.signatureQuality.score
      feature.signatureLevel = scan.signatureQuality.level
      feature.signatureWarning = scan.signatureQuality.warning
      feature.signatureFixedBytes = scan.signatureQuality.fixedBytes
      feature.signatureWildcardBytes = scan.signatureQuality.wildcardBytes
      feature.signatureUniqueFixedBytes = scan.signatureQuality.uniqueFixedBytes
      feature.signatureFixedRatio = scan.signatureQuality.fixedRatio
      feature.trainerSafe = scan.signatureQuality.trainerSafe
    }
    feature.signatureMatches = Number(scan.matchesFound ?? scan.matches?.length ?? 0)
    if (scan.success !== true) {
      return { address: '', error: scan.error || 'Resolution AOB impossible.' }
    }
    if (feature.signatureMatches !== 1 || !scan.matches?.[0]?.address) {
      return { address: '', error: `AOB non unique (${feature.signatureMatches} match(es)). Regénère une signature plus spécifique.` }
    }
    return { address: String(scan.matches[0].address).replace(/^0x/i, '').toUpperCase(), error: '' }
  }

  async function applyTrainerFeature(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature || trainerBusy.value) return
    const blocked = trainerFeaturePatchBlockReason(feature)
    if (blocked) {
      feature.status = 'error'
      feature.lastError = blocked
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'apply_blocked', 'warning', blocked)
      saveTrainerFeatures()
      addActionLog('trainer', `Feature bloquée: ${feature.name}`, blocked, 'warning')
      return
    }
    const trainerRisk = feature.action === 'patch' ? 'patch' : (feature.action === 'write' && kernelMemoryModeActive.value ? 'injection' : 'write')
    if (!await confirmRiskAction(trainerRisk, `Activer feature Trainer: ${feature.name}`, `${feature.action} 0x${feature.address} ${feature.valueType} ${feature.value || feature.patchBytes || ''}${feature.action === 'write' && kernelMemoryModeActive.value ? ' via driver kernel' : ''}`)) return

    trainerBusy.value = true
    try {
      const controller = backend.getController()
      let ok = false
      let error = ''
      if (feature.action === 'write') {
        const result = await writeMemoryValueByMode(feature.address, feature.valueType, feature.value)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_polling') {
        const result = await controller.setFreezeValue(feature.address, feature.valueType, feature.value, true)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_breakpoint') {
        if (!controller.freezeWithBreakpoint) {
          error = 'Freeze BP non expose par ce backend.'
        } else {
          const result = await controller.freezeWithBreakpoint(feature.address, feature.valueType, feature.value, { mode: 'rewrite' })
          ok = result.success === true
          error = result.error ?? ''
        }
      } else if (feature.action === 'patch') {
        if (!controller.applyCodePatch) {
          error = 'Patch code non expose par ce backend.'
        } else {
          const resolved = await resolveTrainerPatchAddress(feature)
          if (resolved.error) {
            ok = false
            error = resolved.error
          } else {
            feature.address = resolved.address
            const result = await controller.applyCodePatch(resolved.address, feature.patchBytes ?? '', { verify: true })
            ok = result.success === true
            error = result.error ?? ''
          }
        }
      }
      feature.enabled = ok && feature.action !== 'write'
      feature.status = ok ? (feature.action === 'write' ? 'idle' : 'active') : 'error'
      feature.lastError = error
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'apply', ok ? 'success' : 'error', error || `0x${feature.address}`)
      addActionLog('trainer', ok ? `Feature activée: ${feature.name}` : `Feature échouée: ${feature.name}`, error || `0x${feature.address}`, ok ? 'success' : 'error')
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'apply', 'error', String(e))
      addActionLog('trainer', `Feature échouée: ${feature.name}`, String(e), 'error')
    } finally {
      trainerBusy.value = false
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }
  }

  async function restoreTrainerFeature(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature || trainerBusy.value) return
    if (!await confirmRiskAction(feature.action === 'patch' ? 'patch' : 'write', `Restaurer feature Trainer: ${feature.name}`, `${feature.action} 0x${feature.address}.`)) return

    trainerBusy.value = true
    try {
      const controller = backend.getController()
      let ok = true
      let error = ''
      if (feature.action === 'freeze_polling') {
        const result = await controller.setFreezeValue(feature.address, feature.valueType, feature.value, false)
        ok = result.success === true
        error = result.error ?? ''
      } else if (feature.action === 'freeze_breakpoint') {
        if (controller.stopBreakpointFreeze) {
          const result = await controller.stopBreakpointFreeze()
          ok = result.success === true
          error = result.error ?? ''
        }
      } else if (feature.action === 'patch') {
        if (controller.restoreCodePatch) {
          const result = await controller.restoreCodePatch(feature.address)
          ok = result.success === true
          error = result.error ?? ''
        } else {
          ok = false
          error = 'Restore patch non expose par ce backend.'
        }
      }
      feature.enabled = false
      feature.status = ok ? 'idle' : 'error'
      feature.lastError = error
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'restore', ok ? 'success' : 'warning', error || `0x${feature.address}`)
      addActionLog('trainer', ok ? `Feature restaurée: ${feature.name}` : `Restauration échouée: ${feature.name}`, error || `0x${feature.address}`, ok ? 'success' : 'warning')
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'restore', 'error', String(e))
      addActionLog('trainer', `Restauration échouée: ${feature.name}`, String(e), 'error')
    } finally {
      trainerBusy.value = false
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }
  }

  async function saveTrainerFeatureToProfile(id: number) {
    const feature = trainerFeatures.value.find((item) => item.id === id)
    if (!feature) return
    const controller = backend.getController()
    const profileName = (feature.processName || processName.value || 'KillEngineTrainer')
      .replace(/\.[^.]+$/, '')
      .replace(/[^a-z0-9_.-]+/gi, '_')
      .slice(0, 80) || 'KillEngineTrainer'
    try {
      let result: Record<string, unknown>
      if (feature.action === 'patch') {
        const blocked = trainerFeaturePatchBlockReason(feature)
        if (blocked) {
          feature.status = 'error'
          feature.lastError = blocked
          feature.updatedAt = new Date().toISOString()
          addTrainerFeatureHistory(feature, 'save_profile_blocked', 'warning', blocked)
          saveTrainerFeatures()
          addActionLog('trainer', `Sauvegarde profil bloquée: ${feature.name}`, blocked, 'warning')
          return
        }
        if (!controller.saveProfileCodePatch) {
          throw new Error('Sauvegarde patch profil non exposee par ce backend.')
        }
        result = await controller.saveProfileCodePatch(
          profileName,
          feature.name,
          feature.address,
          feature.aobPattern ?? '',
          feature.patchBytes ?? '',
          {
            source: 'TrainerView',
            action: feature.action,
            valueType: feature.valueType,
            createdAt: feature.createdAt,
            signatureQuality: feature.signatureQuality,
            signatureScore: feature.signatureScore,
            signatureLevel: feature.signatureLevel,
            signatureWarning: feature.signatureWarning,
            signatureFixedBytes: feature.signatureFixedBytes,
            signatureWildcardBytes: feature.signatureWildcardBytes,
            signatureUniqueFixedBytes: feature.signatureUniqueFixedBytes,
            signatureFixedRatio: feature.signatureFixedRatio,
            trainerSafe: feature.trainerSafe,
            signatureMatches: feature.signatureMatches,
          },
        )
      } else {
        result = await controller.saveProfileTarget(
          profileName,
          feature.name,
          feature.address,
          feature.valueType,
          `Trainer ${feature.action} = ${feature.value}`,
        )
      }
      feature.updatedAt = new Date().toISOString()
      feature.lastError = result.success === false ? String(result.error ?? 'Sauvegarde profil echouee.') : ''
      if (result.success === false) feature.status = 'error'
      addTrainerFeatureHistory(feature, 'save_profile', result.success === false ? 'warning' : 'success', `${profileName} · ${feature.lastError || 'OK'}`)
      saveTrainerFeatures()
      addActionLog('trainer', `Profil sauvegarde: ${feature.name}`, `${profileName} · ${feature.lastError || 'OK'}`, result.success === false ? 'warning' : 'success')
    } catch (e) {
      feature.status = 'error'
      feature.lastError = String(e)
      feature.updatedAt = new Date().toISOString()
      addTrainerFeatureHistory(feature, 'save_profile', 'error', String(e))
      saveTrainerFeatures()
      addActionLog('trainer', `Sauvegarde profil echouee: ${feature.name}`, String(e), 'error')
    }
  }

  async function applyAllTrainerFeatures() {
    for (const feature of trainerFeatures.value) {
      if (!feature.enabled) await applyTrainerFeature(feature.id)
    }
  }

  async function restoreAllTrainerFeatures() {
    for (const feature of trainerFeatures.value) {
      if (feature.enabled) await restoreTrainerFeature(feature.id)
    }
  }

  function deleteTrainerFeature(id: number) {
    trainerFeatures.value = trainerFeatures.value.filter((item) => item.id !== id)
    saveTrainerFeatures()
    void refreshTrainerOverlay()
  }

  function clearTrainerFeatures() {
    trainerFeatures.value = []
    saveTrainerFeatures()
    void refreshTrainerOverlay()
    addActionLog('trainer', 'Trainer vidé', 'Toutes les features locales ont été supprimées.', 'warning')
  }

  function exportTrainerFeaturesJson(): string {
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      processName: processName.value,
      features: trainerFeatures.value,
    }, null, 2)
  }

  function exportTrainerFeaturesMarkdown(): string {
    const lines = [
      '# Trainer Features',
      '',
      `Export: ${new Date().toISOString()}`,
      `Processus: ${processName.value || 'non attache'}`,
      `Features: ${trainerFeatures.value.length}`,
      '',
    ]
    if (trainerFeatures.value.length === 0) {
      lines.push('Aucune feature Trainer locale.')
      return lines.join('\n')
    }

    for (const feature of trainerFeatures.value) {
      lines.push(
        `## ${feature.name}`,
        '',
        `- Action: ${feature.action}`,
        `- Statut: ${feature.status}${feature.enabled ? ' / active' : ''}`,
        `- Processus: ${feature.processName || '-'}`,
        `- Locator: ${feature.locatorKind}`,
        `- Adresse: 0x${feature.address}`,
        `- Type: ${feature.valueType}`,
        `- Valeur/patch: ${feature.value || feature.patchBytes || '-'}`,
        `- Hotkey: ${feature.hotkey || '-'}`,
        `- AOB qualite: ${feature.signatureLevel || '-'}${feature.signatureScore !== undefined ? ` (${feature.signatureScore}/100)` : ''}`,
        `- Derniere erreur: ${feature.lastError || '-'}`,
        '- Historique:',
        ...(feature.history?.length
          ? feature.history.slice(0, 8).map((item) => `  - ${item.time} [${item.status}] ${item.action}: ${item.detail}`)
          : ['  - aucun']),
        '',
      )
    }
    return lines.join('\n')
  }

  function exportWorkspaceJson(): string {
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      app: {
        version: version.value,
        processName: processName.value,
        attached: isAttached.value,
        workflowStatus: workflowStatus.value,
      },
      settings: {
        language: appLanguage.value,
        defaultValueType: settingDefaultValueType.value,
        performanceMode: settingPerformanceMode.value,
        autoRiskMode: settingAutoRiskMode.value,
        modelEnabled: settingModelEnabled.value,
        modelPath: settingModelPath.value,
        modelThreads: settingModelThreads.value,
        scanMaxResults: settingScanMaxResults.value,
        unknownSnapshotMaxMb: settingUnknownSnapshotMaxMb.value,
      },
      workflowPresets: {
        lastPresetId: lastWorkflowPresetId.value,
        available: workflowPresets.value.map((preset) => ({
          id: preset.id,
          title: preset.title,
          mode: preset.mode,
          risk: preset.risk,
          nextStep: preset.nextStep,
        })),
      },
      investigation: {
        active: activeInvestigation.value,
        archive: investigationArchive.value,
      },
      trainer: {
        features: trainerFeatures.value,
      },
      structures: {
        templates: structureTemplates.value,
      },
      bookmarks: {
        items: workspaceBookmarks.value,
      },
      audit: {
        entries: actionLog.value.slice(0, 200),
      },
      autoResolve: {
        report: autoResolveReport.value,
      },
      aiModel: {
        status: aiModelStatus.value,
      },
      diagnostics: {
        logFilePath: logFilePath.value,
        smartSearchDebugFilePath: smartSearchDebugFilePath.value,
        scanTelemetryFilePath: scanTelemetryFilePath.value,
      },
    }, null, 2)
  }

  function workspaceBookmarkMarkdownLine(bookmark: WorkspaceBookmark): string {
    const payload = bookmark.payload ?? {}
    const details = [
      bookmark.address ? `0x${bookmark.address}` : '',
      bookmark.type ? `type ${bookmark.type}` : '',
      bookmark.value !== undefined ? `valeur ${bookmark.value}` : '',
      payload.confidenceLabel ? String(payload.confidenceLabel) : '',
      Number(payload.confidenceScore ?? 0) > 0 ? `score ${String(payload.confidenceScore)}/100` : '',
      payload.requiresConfirmation === true ? 'confirmation requise' : '',
      payload.aobPattern ? `AOB ${String(payload.aobPattern).slice(0, 80)}` : '',
      payload.patchBytes ? `patch ${String(payload.patchBytes).slice(0, 40)}` : '',
      payload.signatureLevel ? `qualite ${String(payload.signatureLevel)}${payload.signatureScore ? ` ${String(payload.signatureScore)}/100` : ''}` : '',
      payload.signatureMatches !== undefined ? `${String(payload.signatureMatches)} match(es)` : '',
    ].filter(Boolean)
    return `- ${bookmark.kind} ${bookmark.label}${details.length > 0 ? ` - ${details.join(' · ')}` : ''}${bookmark.note ? ` - ${bookmark.note}` : ''}`
  }

  function exportWorkspaceMarkdown(): string {
    const report = autoResolveReport.value
    const lines = [
      '# KillEngine Workspace',
      '',
      `Export: ${new Date().toISOString()}`,
      `Version: ${version.value}`,
      `Processus: ${processName.value || 'non attache'}`,
      `Workflow: ${workflowStatus.value}`,
      `Preset: ${lastWorkflowPresetId.value || 'aucun'}`,
      `Mode Auto: ${settingAutoRiskMode.value}`,
      `IA locale: ${aiModelStatus.value?.ready ? 'llama.cpp' : 'indisponible'}`,
      '',
      '## Presets Disponibles',
      '',
      ...workflowPresets.value.map((preset) => `- ${preset.title}: ${preset.mode} / ${preset.risk} - ${preset.nextStep}`),
      '',
      '## Investigation',
      '',
      `Active: ${activeInvestigation.value ? activeInvestigation.value.objective : 'aucune'}`,
      `Etapes actives: ${activeInvestigation.value?.steps.length ?? 0}`,
      `Archives: ${investigationArchive.value.length}`,
      '',
      '## Trainer',
      '',
      `Features: ${trainerFeatures.value.length}`,
      ...trainerFeatures.value.slice(0, 12).map((feature) => `- ${feature.name}: ${feature.action} 0x${feature.address} (${feature.status})`),
      '',
      '## Structures',
      '',
      `Templates: ${structureTemplates.value.length}`,
      ...structureTemplates.value.slice(0, 12).map((template) => `- ${template.name}: ${template.fieldCount} champ(s), base 0x${template.baseAddress}`),
      ...structureTemplates.value.slice(0, 5).flatMap((template) => [
        '',
        `### ${template.name}`,
        ...template.fields.slice(0, 20).map((field) =>
          `- ${field.offset >= 0 ? '+' : ''}${field.offset} ${field.type} ${field.label || '-'} = ${field.sampleValue || '-'}${field.note ? ` (${field.note})` : ''}`,
        ),
      ]),
      '',
      '## Bookmarks',
      '',
      `Bookmarks: ${workspaceBookmarks.value.length}`,
      ...workspaceBookmarks.value.slice(0, 20).map((bookmark) => workspaceBookmarkMarkdownLine(bookmark)),
      '',
      '## Audit',
      '',
      `Entrées: ${actionLog.value.length}`,
      ...actionLog.value.slice(0, 30).map((entry) => `- ${entry.time} [${entry.status}] ${entry.kind} - ${entry.title}${entry.detail ? `: ${entry.detail}` : ''}`),
      '',
      '## Rapport Auto',
      '',
      report
        ? `Strategie: ${String(report.preferredStrategy?.label ?? 'non determinee')}`
        : 'Aucun rapport Auto charge.',
      report?.summary ? `Résumé: ${report.summary}` : '',
      '',
      '## Diagnostics',
      '',
      `Log: ${logFilePath.value || '-'}`,
      `Smart Search JSONL: ${smartSearchDebugFilePath.value || '-'}`,
      `Telemetry JSONL: ${scanTelemetryFilePath.value || '-'}`,
      '',
    ].filter((line) => line !== '')
    return lines.join('\n')
  }

  function previewWorkspaceImport(raw: string) {
    try {
      const parsed = JSON.parse(raw) as Record<string, unknown>
      const investigation = parsed.investigation as Record<string, unknown> | undefined
      const trainer = parsed.trainer as Record<string, unknown> | undefined
      const structures = parsed.structures as Record<string, unknown> | undefined
      const bookmarksRoot = parsed.bookmarks as Record<string, unknown> | undefined
      const auditRoot = parsed.audit as Record<string, unknown> | undefined
      const settings = parsed.settings as Record<string, unknown> | undefined
      const presetsRoot = parsed.workflowPresets as Record<string, unknown> | undefined
      const active = investigation?.active && typeof investigation.active === 'object' ? 1 : 0
      const archive = Array.isArray(investigation?.archive) ? investigation.archive.length : 0
      const features = Array.isArray(trainer?.features) ? trainer.features.length : 0
      const templates = Array.isArray(structures?.templates) ? structures.templates.length : 0
      const bookmarks = Array.isArray(bookmarksRoot?.items) ? bookmarksRoot.items.length : 0
      const audit = Array.isArray(auditRoot?.entries) ? auditRoot.entries.length : 0
      const presetId = String(presetsRoot?.lastPresetId ?? '')
      return {
        success: true,
        version: Number(parsed.version ?? 0),
        exportedAt: String(parsed.exportedAt ?? ''),
        activeInvestigation: active,
        archiveCount: archive,
        trainerFeatureCount: features,
        structureTemplateCount: templates,
        bookmarkCount: bookmarks,
        auditCount: audit,
        lastPresetId: presetId,
        hasSettings: Boolean(settings),
      }
    } catch (e) {
      return {
        success: false,
        error: String(e),
      }
    }
  }

  function importWorkspaceJson(raw: string) {
    const preview = previewWorkspaceImport(raw)
    if (preview.success !== true) return preview

    const parsed = JSON.parse(raw) as Record<string, unknown>
    const investigation = parsed.investigation as Record<string, unknown> | undefined
    const trainer = parsed.trainer as Record<string, unknown> | undefined
    const structures = parsed.structures as Record<string, unknown> | undefined
    const bookmarksRoot = parsed.bookmarks as Record<string, unknown> | undefined
    const auditRoot = parsed.audit as Record<string, unknown> | undefined
    const settings = parsed.settings as Record<string, unknown> | undefined
    const presetsRoot = parsed.workflowPresets as Record<string, unknown> | undefined

    if (investigation) {
      activeInvestigation.value =
        investigation.active && typeof investigation.active === 'object'
          ? investigation.active as InvestigationRun
          : null
      investigationArchive.value = Array.isArray(investigation.archive)
        ? (investigation.archive as InvestigationRun[]).slice(0, 20)
        : []
      investigationStepIdCounter.value = Math.max(
        investigationStepIdCounter.value,
        activeInvestigation.value?.steps.reduce((max, step) => Math.max(max, Number(step.id) || 0), 0) ?? 0,
        ...investigationArchive.value.map((run) => run.steps.reduce((max, step) => Math.max(max, Number(step.id) || 0), 0)),
      )
      investigationRunIdCounter.value = Math.max(
        investigationRunIdCounter.value,
        Number(activeInvestigation.value?.id ?? 0),
        ...investigationArchive.value.map((run) => Number(run.id) || 0),
      )
      saveInvestigations()
    }

    if (trainer && Array.isArray(trainer.features)) {
      trainerFeatures.value = (trainer.features as TrainerFeature[]).slice(0, 200)
      trainerFeatureIdCounter.value = Math.max(0, ...trainerFeatures.value.map((feature) => Number(feature.id) || 0))
      saveTrainerFeatures()
      void refreshTrainerOverlay()
    }

    if (structures && Array.isArray(structures.templates)) {
      structureTemplates.value = (structures.templates as StructureTemplate[]).slice(0, 100)
      structureTemplateIdCounter.value = Math.max(0, ...structureTemplates.value.map((template) => Number(template.id) || 0))
      saveStructureTemplates()
    }

    if (bookmarksRoot && Array.isArray(bookmarksRoot.items)) {
      workspaceBookmarks.value = (bookmarksRoot.items as WorkspaceBookmark[]).slice(0, 500)
      workspaceBookmarkIdCounter.value = Math.max(0, ...workspaceBookmarks.value.map((bookmark) => Number(bookmark.id) || 0))
      saveWorkspaceBookmarks()
    }

    if (auditRoot && Array.isArray(auditRoot.entries)) {
      actionLog.value = (auditRoot.entries as UserActionLogEntry[]).slice(0, 200)
      actionLogIdCounter.value = Math.max(0, ...actionLog.value.map((entry) => Number(entry.id) || 0))
      saveActionLog()
    }

    if (settings) {
      if (settings.language === 'fr' || settings.language === 'en') appLanguage.value = settings.language
      if (typeof settings.defaultValueType === 'string') settingDefaultValueType.value = settings.defaultValueType
      if (['Auto', 'Eco', 'Normal', 'Performance', 'Max'].includes(String(settings.performanceMode))) {
        settingPerformanceMode.value = settings.performanceMode as AppSettings['performanceMode']
      }
      if (['Safe', 'Expert', 'Trainer'].includes(String(settings.autoRiskMode))) {
        settingAutoRiskMode.value = settings.autoRiskMode as AppSettings['autoRiskMode']
      }
      if (typeof settings.modelEnabled === 'boolean') settingModelEnabled.value = settings.modelEnabled
      if (typeof settings.modelPath === 'string') settingModelPath.value = settings.modelPath
      if (Number.isFinite(Number(settings.modelThreads))) settingModelThreads.value = Number(settings.modelThreads)
      if (Number.isFinite(Number(settings.scanMaxResults))) settingScanMaxResults.value = Number(settings.scanMaxResults)
      if (Number.isFinite(Number(settings.unknownSnapshotMaxMb))) settingUnknownSnapshotMaxMb.value = Number(settings.unknownSnapshotMaxMb)
    }

    const presetId = String(presetsRoot?.lastPresetId ?? '')
    if (presetId && workflowPresets.value.some((preset) => preset.id === presetId)) {
      lastWorkflowPresetId.value = presetId
    }

    addActionLog(
      'workspace',
      'Workspace importé',
      `${preview.trainerFeatureCount} feature(s), ${preview.structureTemplateCount} template(s), ${preview.bookmarkCount} bookmark(s), ${preview.archiveCount} archive(s), ${preview.auditCount ?? 0} audit(s).`,
      'success',
    )
    addInvestigationStep({
      title: 'Workspace importé',
      detail: `${preview.trainerFeatureCount} feature(s), ${preview.structureTemplateCount} template(s), ${preview.bookmarkCount} bookmark(s), ${preview.archiveCount} archive(s), ${preview.auditCount ?? 0} audit(s), settings=${preview.hasSettings ? 'oui' : 'non'}, preset=${String(preview.lastPresetId || '-')}.`,
      status: 'success',
      tool: 'importWorkspaceJson',
      risk: 'safe',
      payload: preview,
    })
    return {
      ...preview,
      imported: true,
    }
  }

  function saveCurrentWorkspaceProject(name?: string) {
    workspaceProjectIdCounter.value += 1
    const now = new Date().toISOString()
    const fallbackName = processName.value
      ? `${processName.value.replace(/\.[^.]+$/, '')} workspace`
      : 'KillEngine workspace'
    const snapshotJson = exportWorkspaceJson()
    const project: WorkspaceProject = {
      id: workspaceProjectIdCounter.value,
      name: String(name ?? fallbackName).trim() || fallbackName,
      processName: processName.value,
      snapshotJson,
      investigationCount: (activeInvestigation.value ? 1 : 0) + investigationArchive.value.length,
      trainerFeatureCount: trainerFeatures.value.length,
      structureTemplateCount: structureTemplates.value.length,
      bookmarkCount: workspaceBookmarks.value.length,
      auditCount: actionLog.value.length,
      createdAt: now,
      updatedAt: now,
    }
    workspaceProjects.value.unshift(project)
    workspaceProjects.value = workspaceProjects.value.slice(0, 50)
    saveWorkspaceProjects()
    addActionLog('workspace', `Projet sauvegardé: ${project.name}`, `${project.trainerFeatureCount} feature(s), ${project.structureTemplateCount} template(s).`, 'success')
    return project
  }

  function loadWorkspaceProject(id: number) {
    const project = workspaceProjects.value.find((item) => item.id === id)
    if (!project) return { success: false, error: 'Projet introuvable.' }
    const result = importWorkspaceJson(project.snapshotJson)
    if (result.success === true) {
      addActionLog('workspace', `Projet chargé: ${project.name}`, project.processName || '-', 'success')
      addInvestigationStep({
        title: 'Projet workspace chargé',
        detail: `${project.name} · ${project.trainerFeatureCount} feature(s), ${project.structureTemplateCount} template(s), ${project.bookmarkCount} bookmark(s), ${project.auditCount ?? 0} audit(s).`,
        status: 'success',
        tool: 'loadWorkspaceProject',
        risk: 'safe',
        payload: { projectId: project.id, projectName: project.name },
      })
    }
    return result
  }

  function deleteWorkspaceProject(id: number) {
    const before = workspaceProjects.value.length
    workspaceProjects.value = workspaceProjects.value.filter((item) => item.id !== id)
    if (workspaceProjects.value.length !== before) {
      saveWorkspaceProjects()
      addActionLog('workspace', 'Projet supprimé', `id=${id}`, 'warning')
    }
  }

  function clearWorkspaceProjects() {
    workspaceProjects.value = []
    saveWorkspaceProjects()
    addActionLog('workspace', 'Projets vidés', 'Tous les projets locaux ont été supprimés.', 'warning')
  }

  async function clearAutoResolveMemory(allProcesses = false) {
    const controller = backend.getController()
    if (!controller.clearAutoResolveMemory) {
      addActionLog('ai_memory', 'Mémoire Auto indisponible', 'Backend non exposé.', 'warning')
      return { success: false, error: 'Backend non exposé.' }
    }
    const result = await controller.clearAutoResolveMemory(allProcesses)
    addActionLog('ai_memory', 'Mémoire Auto vidée', String(result.message ?? ''), result.success === false ? 'warning' : 'success')
    return result
  }

  function logAiAudit(event: string, payload: Record<string, unknown>) {
    const controller = backend.getController()
    if (!controller.logAiAudit) return
    void controller.logAiAudit(event, {
      ...payload,
      autoRiskMode: settingAutoRiskMode.value,
      investigationId: activeInvestigation.value?.id ?? null,
      objective: activeInvestigation.value?.objective ?? searchQuery.value,
      timestamp: new Date().toISOString(),
    }).catch(() => {})
  }

  async function confirmRiskAction(
    risk: NonNullable<InvestigationStep['risk']>,
    title: string,
    detail: string,
  ): Promise<boolean> {
    lastRiskBlockReason.value = ''
    const mode = settingAutoRiskMode.value
    const blocked =
      (mode === 'Safe' && (risk === 'debug' || risk === 'patch' || risk === 'injection')) ||
      (mode === 'Expert' && risk === 'injection')
    if (blocked) {
      const message =
        risk === 'injection'
          ? 'Passe le niveau Auto en Trainer dans Settings pour autoriser injection/hook.'
          : 'Passe le niveau Auto en Expert ou Trainer dans Settings pour autoriser debug/patch.'
      lastRiskBlockReason.value = `Bloqué par le mode Auto actuel (${mode}). ${message}`
      addActionLog('risk_gate', `Bloqué par mode ${mode}: ${title}`, `${detail} ${message}`, 'warning')
      logAiAudit('risk_blocked', { risk, title, detail, mode, reason: message })
      addInvestigationStep({
        title: `Risque bloqué: ${title}`,
        detail: `${detail} ${message}`,
        status: 'warning',
        risk,
        tool: 'RiskGate',
        payload: { accepted: false, blocked: true, mode, title, detail },
      })
      return false
    }
    const accepted = await new Promise<boolean>((resolve) => {
      if (riskDialogResolver) {
        riskDialogResolver(false)
      }
      riskDialogResolver = resolve
      riskDialog.value = { open: true, risk, title, detail, mode }
    })
    addActionLog('risk_gate', accepted ? `Confirmé: ${title}` : `Refusé: ${title}`, detail, accepted ? 'success' : 'warning')
    logAiAudit(accepted ? 'risk_confirmed' : 'risk_refused', { risk, title, detail, mode })
    addInvestigationStep({
      title: accepted ? `Risque confirmé: ${title}` : `Risque refusé: ${title}`,
      detail,
      status: accepted ? 'checkpoint' : 'warning',
      risk,
      tool: 'RiskGate',
      payload: { accepted, title, detail },
    })
    return accepted
  }

  function resolveRiskDialog(accepted: boolean) {
    const resolver = riskDialogResolver
    riskDialogResolver = null
    riskDialog.value = null
    if (resolver) {
      resolver(accepted)
    }
  }

  function checkpointAddress(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.address ?? checkpoint.instructionPointer ?? checkpoint.rip ?? '')
      .replace(/^0x/i, '')
      .trim()
  }

  function checkpointType(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.type ?? checkpoint.valueType ?? exactScanType.value ?? 'Int32')
  }

  function checkpointValue(checkpoint: Record<string, unknown>): string {
    return String(checkpoint.value ?? checkpoint.targetValue ?? writeValue.value ?? exactScanValue.value ?? '')
  }

  function buildCheckpointActionPlan(checkpoint: Record<string, unknown>): RuntimeActionPlan {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint).trim()
    const kind = String(checkpoint.kind ?? 'checkpoint')
    const isCode =
      kind.toLowerCase().includes('code') ||
      kind.toLowerCase().includes('aob') ||
      Boolean(checkpoint.patchBytes || checkpoint.aobPattern || checkpoint.instructionPointer || checkpoint.rip)
    const hasAddress = Boolean(address)
    const hasWritableValue = Boolean(hasAddress && value && !isCode)
    const hasCodeTarget = Boolean(hasAddress && (isCode || checkpoint.sourceAddress))
    const actions: RuntimeActionPlanItem[] = [
      {
        id: 'watch',
        label: 'Watch',
        risk: 'safe',
        enabled: hasAddress && !isCode,
        reason: hasAddress && !isCode ? 'Surveiller la valeur live sans écrire.' : 'Réservé aux checkpoints mémoire avec adresse.',
      },
      {
        id: 'write',
        label: 'Préparer write',
        risk: 'write',
        enabled: hasWritableValue,
        reason: hasWritableValue ? 'Tester la valeur sous confirmation explicite.' : 'Adresse mémoire et valeur cible requises.',
      },
      {
        id: 'freeze_polling',
        label: 'Freeze',
        risk: 'write',
        enabled: hasWritableValue,
        reason: hasWritableValue ? 'Stabiliser par freeze polling sous confirmation.' : 'Adresse mémoire et valeur cible requises.',
      },
      {
        id: 'find_writes',
        label: 'Find What Writes',
        risk: 'debug',
        enabled: hasAddress && !isCode,
        reason: hasAddress && !isCode ? 'Capturer l’instruction qui modifie cette adresse.' : 'Le debugger part d’une adresse mémoire, pas d’un RIP déjà capturé.',
      },
      {
        id: 'aob_patch',
        label: 'AOB/Patch',
        risk: 'patch',
        enabled: hasCodeTarget,
        reason: hasCodeTarget ? 'Générer une signature et proposer un patch réversible.' : 'Nécessite un RIP, une signature ou une source code.',
      },
      {
        id: 'force_value',
        label: 'Forcer valeur (hook)',
        risk: 'patch',
        enabled: kind === 'code_writer' && hasAddress,
        reason: kind === 'code_writer' && hasAddress
          ? 'Installer un trampoline sur ce RIP pour forcer une valeur, même si la source est un registre.'
          : 'Réservé aux checkpoints Find What Writes (RIP capturé).',
      },
      {
        id: 'bookmark',
        label: 'Bookmark',
        risk: 'safe',
        enabled: true,
        reason: 'Conserver la piste dans le workspace avec ses preuves.',
      },
      {
        id: 'trainer',
        label: 'Créer Trainer',
        risk: isCode ? 'patch' : 'write',
        enabled: hasAddress,
        reason: hasAddress ? 'Transformer la piste en feature réutilisable.' : 'Une feature Trainer nécessite une adresse ou signature.',
      },
    ]
    return {
      label: String(checkpoint.label ?? checkpoint.name ?? checkpoint.address ?? checkpoint.id ?? 'Checkpoint'),
      address,
      type,
      value,
      kind,
      isCode,
      safeCount: actions.filter((action) => action.enabled && action.risk === 'safe').length,
      riskyCount: actions.filter((action) => action.enabled && action.risk !== 'safe').length,
      actions,
    }
  }

  async function executeCheckpointWrite(checkpoint: Record<string, unknown>, freeze = false) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint)
    if (!address || !value.trim()) {
      addActionLog('checkpoint', 'Checkpoint incomplet', 'Adresse ou valeur manquante.', 'warning')
      return null
    }
    const title = freeze ? 'Checkpoint freeze polling' : 'Checkpoint écriture'
    const risk = !freeze && kernelMemoryModeActive.value ? 'injection' : 'write'
    const route = !freeze && kernelMemoryModeActive.value ? ' via driver kernel' : ''
    if (!await confirmRiskAction(risk, title, `0x${address} ${type} = ${value}${route}.`)) return null

    try {
      const controller = backend.getController()
      const result = freeze
        ? await controller.setFreezeValue(address, type, value, true)
        : await writeMemoryValueByMode(address, type, value)
      writeResult.value = result as MemoryWriteResult
      if (result.success === true) addAddressToWatch(address, type)
      addActionLog(
        'checkpoint',
        result.success === true ? `${title} OK` : `${title} échoué`,
        String(result.error || `0x${address}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? `${title} exécuté` : `${title} échoué`,
        detail: String(result.error || `0x${address} ${type} = ${value}`),
        status: result.success === true ? 'success' : 'error',
        tool: freeze ? 'setFreezeValue' : (kernelMemoryModeActive.value ? 'writeMemoryValueKernel' : 'writeMemoryValue'),
        risk,
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit(freeze ? 'checkpoint_freeze_executed' : 'checkpoint_write_executed', {
        success: result.success === true,
        address,
        type,
        value,
        error: result.error ?? '',
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', `${title} échoué`, String(e), 'error')
      return null
    }
  }

  async function executeCheckpointKernelWrite(checkpoint: Record<string, unknown>) {
    const address = checkpointAddress(checkpoint)
    const type = checkpointType(checkpoint)
    const value = checkpointValue(checkpoint)
    if (!address || !value.trim()) {
      addActionLog('checkpoint', 'Écriture kernel impossible', 'Adresse ou valeur manquante.', 'warning')
      return null
    }
    if (!await confirmRiskAction('injection', 'Écriture mémoire via driver noyau', `0x${address} ${type} = ${value} (contourne les protections mémoire usermode).`)) return null
    const controller = backend.getController()
    if (!controller.writeMemoryValueKernel) {
      addActionLog('checkpoint', 'Écriture kernel indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const result = await controller.writeMemoryValueKernel(address, type, value)
      writeResult.value = result as unknown as MemoryWriteResult
      if (result.success === true) addAddressToWatch(address, type)
      addActionLog(
        'checkpoint',
        result.success === true ? 'Écriture kernel OK' : 'Écriture kernel échouée',
        String(result.error || `0x${address}`),
        result.success === true ? 'success' : 'error',
      )
      addInvestigationStep({
        title: result.success === true ? 'Écriture kernel exécutée' : 'Écriture kernel échouée',
        detail: String(result.error || `0x${address} ${type} = ${value} via driver noyau`),
        status: result.success === true ? 'success' : 'error',
        tool: 'writeMemoryValueKernel',
        risk: 'injection',
        payload: result as unknown as Record<string, unknown>,
      })
      logAiAudit('checkpoint_kernel_write_executed', {
        success: result.success === true,
        address,
        type,
        value,
        error: result.error ?? '',
      })
      return result
    } catch (e) {
      addActionLog('checkpoint', 'Écriture kernel échouée', String(e), 'error')
      return null
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
          const key = address.toLowerCase()
          if (freezeInstabilityNotified.has(key)) return
          freezeInstabilityNotified.add(key)
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
    kernelDriverStatusLoading.value = true
    kernelDriverStatusError.value = ''
    try {
      const controller = backend.getController()
      if (!controller.probeKernelDriver) {
        kernelDriverStatus.value = null
        kernelDriverStatusError.value = 'Probe driver noyau non exposé par ce backend.'
        return
      }
      kernelDriverStatus.value = await controller.probeKernelDriver()
    } catch (e) {
      kernelDriverStatus.value = null
      kernelDriverStatusError.value = String(e)
    } finally {
      kernelDriverStatusLoading.value = false
    }
  }

  async function readMemoryKernel(addressHex: string, size: number) {
    kernelMemoryReadBusy.value = true
    try {
      const controller = backend.getController()
      if (!controller.readMemoryKernel) {
        kernelMemoryReadResult.value = { success: false, error: 'Lecture kernel non exposée par ce backend.' }
        return
      }
      kernelMemoryReadResult.value = await controller.readMemoryKernel(addressHex, size)
      addActionLog(
        'kernel_read',
        kernelMemoryReadResult.value.success ? `Lecture kernel 0x${addressHex}` : `Lecture kernel échouée 0x${addressHex}`,
        kernelMemoryReadResult.value.success
          ? `${kernelMemoryReadResult.value.bytesRead} octet(s) lus via le driver noyau.`
          : (kernelMemoryReadResult.value.error ?? ''),
        kernelMemoryReadResult.value.success ? 'success' : 'error',
      )
      logAiAudit('kernel_memory_read', { address: addressHex, size, success: kernelMemoryReadResult.value.success })
    } catch (e) {
      kernelMemoryReadResult.value = { success: false, error: String(e) }
    } finally {
      kernelMemoryReadBusy.value = false
    }
  }

  async function writeMemoryKernel(addressHex: string, hexBytes: string) {
    // Contourne les protections mémoire usermode normales (VirtualProtect,
    // PAGE_GUARD) en écrivant directement depuis le ring 0 -- traité comme
    // une injection, le palier de risque le plus strict déjà utilisé dans
    // ce store (voir confirmRiskAction), pas comme un simple 'write'.
    if (!await confirmRiskAction('injection', 'Écriture mémoire via driver noyau', `0x${addressHex} = ${hexBytes.trim()} (contourne les protections mémoire usermode).`)) return

    kernelMemoryWriteBusy.value = true
    try {
      const controller = backend.getController()
      if (!controller.writeMemoryKernel) {
        kernelMemoryWriteResult.value = { success: false, error: 'Écriture kernel non exposée par ce backend.' }
        return
      }
      kernelMemoryWriteResult.value = await controller.writeMemoryKernel(addressHex, hexBytes)
      addActionLog(
        'kernel_write',
        kernelMemoryWriteResult.value.success ? `Écriture kernel 0x${addressHex}` : `Écriture kernel échouée 0x${addressHex}`,
        kernelMemoryWriteResult.value.success
          ? `${kernelMemoryWriteResult.value.bytesWritten} octet(s) écrits via le driver noyau.`
          : (kernelMemoryWriteResult.value.error ?? ''),
        kernelMemoryWriteResult.value.success ? 'success' : 'error',
      )
      logAiAudit('kernel_memory_write', { address: addressHex, bytes: hexBytes, success: kernelMemoryWriteResult.value.success })
    } catch (e) {
      kernelMemoryWriteResult.value = { success: false, error: String(e) }
    } finally {
      kernelMemoryWriteBusy.value = false
    }
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

  async function attach(pid: number, mode: 'standard' | 'kernel' = memoryAccessMode.value) {
    try {
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
    const controller = backend.getController()
    if (!controller.getClrInspectorStatus) {
      clrInspectorStatus.value = null
      clrInspectorError.value = 'Inspecteur CLR non exposé par ce backend.'
      return
    }
    try {
      clrInspectorStatus.value = await controller.getClrInspectorStatus()
      clrInspectorError.value = clrInspectorStatus.value.error ?? ''
    } catch (e) {
      clrInspectorStatus.value = null
      clrInspectorError.value = String(e)
    }
  }

  async function attachClrInspector() {
    const controller = backend.getController()
    if (!controller.attachClrInspector) {
      clrLastResult.value = { success: false, error: 'Inspecteur CLR non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const result = await controller.attachClrInspector()
      clrLastResult.value = result
      clrInspectorError.value = result.success ? '' : (result.error ?? 'Attache CLR échouée.')
      addActionLog(
        'clr_inspector',
        result.success ? 'Inspecteur CLR attaché' : 'Inspecteur CLR refusé',
        result.success ? `PID ${clrInspectorStatus.value?.pid ?? ''}` : clrInspectorError.value,
        result.success ? 'success' : 'warning',
      )
      await refreshClrInspectorStatus()
    } catch (e) {
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function detachClrInspector() {
    const controller = backend.getController()
    if (!controller.detachClrInspector) return
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.detachClrInspector()
      clrSelectedObject.value = null
      clrObjects.value = []
      clrRoots.value = []
      await refreshClrInspectorStatus()
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function shutdownClrInspector() {
    const controller = backend.getController()
    if (!controller.shutdownClrInspector) return
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.shutdownClrInspector()
      clrSelectedObject.value = null
      clrObjects.value = []
      clrRoots.value = []
      await refreshClrInspectorStatus()
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function flushClrInspectorCache() {
    const controller = backend.getController()
    if (!controller.flushClrInspectorCache) {
      clrLastResult.value = { success: false, error: 'flushCachedData non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      clrLastResult.value = await controller.flushClrInspectorCache()
      if (!clrLastResult.value.success) clrInspectorError.value = clrLastResult.value.error ?? ''
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function findClrObjects(typeSubstring = clrTypeFilter.value) {
    const controller = backend.getController()
    if (!controller.findClrObjectsByType) {
      clrLastResult.value = { success: false, error: 'findObjectsByType non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    clrInspectorError.value = ''
    try {
      const result = await controller.findClrObjectsByType(typeSubstring.trim() || 'KillEngine.ClrTestTarget')
      clrLastResult.value = result
      clrObjects.value = result.success && Array.isArray(result.result) ? result.result : []
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } catch (e) {
      clrObjects.value = []
      clrLastResult.value = { success: false, error: String(e) }
      clrInspectorError.value = String(e)
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function readClrObject(addressHex: string) {
    const controller = backend.getController()
    if (!controller.readClrObject) {
      clrLastResult.value = { success: false, error: 'readObject non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      const result = await controller.readClrObject(addressHex)
      clrLastResult.value = result
      clrSelectedObject.value = result.success && result.result ? result.result : null
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } finally {
      clrInspectorBusy.value = false
    }
  }

  async function enumerateClrRoots(typeSubstring = clrTypeFilter.value) {
    const controller = backend.getController()
    if (!controller.enumerateClrRoots) {
      clrLastResult.value = { success: false, error: 'enumerateRoots non exposé par ce backend.' }
      return
    }
    clrInspectorBusy.value = true
    try {
      const result = await controller.enumerateClrRoots(typeSubstring.trim())
      clrLastResult.value = result
      clrRoots.value = result.success && Array.isArray(result.result) ? result.result : []
      clrInspectorError.value = result.success ? '' : (result.error ?? '')
    } finally {
      clrInspectorBusy.value = false
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

  async function refreshWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.getWriteHistorySequence) {
      writeHistorySequence.value = []
      return
    }
    const result = await controller.getWriteHistorySequence()
    writeHistorySequence.value = result.success
      ? ((result.sequence as Array<Record<string, unknown>>) ?? [])
      : []
  }

  async function replayWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.replayWriteHistorySequence) {
      addActionLog('write-history', 'Replay indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    if (!await confirmRiskAction('write', 'Rejouer la séquence d\'écritures', `Rejouer ${writeHistorySequence.value.length} écriture(s) confirmée(s) dans l'ordre pour ce processus.`)) return null
    try {
      const result = await controller.replayWriteHistorySequence()
      addActionLog(
        'write-history',
        result.success ? 'Replay terminé' : 'Replay échoué',
        `${result.replayedCount ?? 0} rejouée(s), ${result.skippedCount ?? 0} ignorée(s), ${result.failedCount ?? 0} échouée(s).`,
        result.success ? 'success' : 'warning',
      )
      return result
    } catch (e) {
      addActionLog('write-history', 'Replay échoué', String(e), 'error')
      return null
    }
  }

  async function clearWriteHistorySequence() {
    const controller = backend.getController()
    if (!controller.clearWriteHistorySequence) return null
    const result = await controller.clearWriteHistorySequence()
    await refreshWriteHistorySequence()
    return result
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

  function applySettings(settings: AppSettings) {
    appLanguage.value = settings.language === 'en' ? 'en' : 'fr'
    settingDefaultValueType.value = settings.defaultValueType || 'Int32'
    exactScanType.value = settingDefaultValueType.value
    unknownScanType.value = 'Auto'
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
    settingAutoRiskMode.value = settings.autoRiskMode || 'Safe'
    settingModelPath.value = settings.modelPath || ''
    settingModelEnabled.value = settings.modelEnabled !== false
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
      autoRiskMode: settingAutoRiskMode.value,
      modelPath: settingModelPath.value,
      modelEnabled: settingModelEnabled.value,
      modelThreads: settingModelThreads.value,
    }
  }

  async function loadSettings() {
    try {
      const settings = await backend.getController().getSettings()
      applySettings(settings)
      settingsLoaded.value = true
      settingsStatus.value = ''
      await refreshAiModelStatus()
      return settings
    } catch (e) {
      settingsStatus.value = 'Impossible de charger les paramètres : ' + String(e)
      return null
    }
  }

  async function refreshAiModelStatus() {
    aiModelStatusLoading.value = true
    aiModelStatusError.value = ''
    try {
      const controller = backend.getController()
      if (!controller.getAiModelStatus) {
        aiModelStatus.value = null
        aiModelStatusError.value = 'Statut IA non exposé par ce backend.'
        return null
      }
      const status = await controller.getAiModelStatus()
      aiModelStatus.value = status
      aiModelStatusError.value = status.success === false ? String(status.error ?? status.message ?? 'Statut IA indisponible.') : ''
      return status
    } catch (e) {
      aiModelStatus.value = null
      aiModelStatusError.value = String(e)
      return null
    } finally {
      aiModelStatusLoading.value = false
    }
  }

  async function browseForModel() {
    const controller = backend.getController()
    if (!controller.browseForModelFile) {
      aiModelStatusError.value = 'Sélecteur de fichier non exposé par ce backend.'
      return
    }
    const result = await controller.browseForModelFile()
    if (result.success === true && typeof result.path === 'string') {
      settingModelPath.value = result.path
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
      await refreshAiModelStatus()
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
        label: `Scan chiffre 0x${String(match.address ?? '').replace(/^0x/i, '')}`,
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
        reason: 'String affichee candidate; analyser les sources numeriques avant toute ecriture.',
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
        title: 'Sources UI auto analysées',
        detail: `${autoUiStringSources.value.length} source(s) numérique(s) depuis Trace UI fallback.`,
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
      lines.push(`Meilleure piste actuelle : 0x${address} (${type}) · score ${score}%.`)
      if (reasons) lines.push(`Pourquoi : ${reasons}.`)
      lines.push('Prochaine action recommandée : Watch cette piste, puis lance Écrit par si elle suit bien la valeur affichée.')
    } else {
      lines.push("Je n'ai pas encore de piste scorée. Lance Trace UI string, puis Analyser sources ou Démarrer enquête.")
    }

    const hitCount = Number(report.debugger?.hitCount ?? 0)
    if (hitCount > 0) {
      lines.push(`${hitCount} hit(s) debugger capturé(s) : tu peux analyser le RIP puis stabiliser l'AOB avant de sauver le trainer.`)
    } else if (report.debugger?.cancelled) {
      lines.push('La dernière capture debugger a été annulée.')
    }

    const matchesFound = Number(report.aob?.matchesFound ?? Number.NaN)
    if (Number.isFinite(matchesFound)) {
      const weakQuality = Number(report.aob?.weakQualityCount ?? 0)
      const blockedTrainer = Number(report.aob?.trainerBlockedCount ?? 0)
      if (weakQuality > 0 || blockedTrainer > 0) {
        lines.push(`AOB : ${weakQuality} signature(s) faible(s), ${blockedTrainer} blocage(s) Trainer. Stabilise avant de sauver/appliquer.`)
      } else if (matchesFound === 1) lines.push('AOB : signature unique et qualité acceptable, bonne candidate pour un patch trainer.')
      else if (matchesFound > 1) lines.push(`AOB : ${matchesFound} matches, il faut stabiliser avant de patcher.`)
      else lines.push('AOB : aucune signature exploitable pour le moment.')
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
      if (!/nouvelle recherche|oublie|annule|rollback|abandonne|laisse tomber/i.test(query)) {
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
        intentRationale: "L'utilisateur demande une décision sur l'enquête mémoire courante.",
      })
      searchQuery.value = ''
      return
    }

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

  async function doAutoResolve() {
    const query = searchQuery.value.trim()
    if (!query || isSearching.value) return

    const controller = backend.getController()
    if (!controller.startAutoResolve) {
      pushMessage('assistant', 'Auto-résolution non exposée par ce backend.', { isError: true })
      return
    }

    pushMessage('user', `Auto: ${query}`)
    startInvestigation(query, 'Auto Resolve')
    addInvestigationStep({
      title: 'Objectif utilisateur',
      detail: query,
      status: 'running',
      tool: 'startAutoResolve',
      risk: 'safe',
    })
    const thinkingMessage = pushMessage('assistant', 'Je prépare un plan auto et je lance la première action sûre...', { isThinking: true })
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
            title: `Auto safe: ${String(step.tool ?? 'outil')}`,
            detail: String(step.detail ?? step.status ?? ''),
            status: String(step.status ?? '') === 'error' ? 'warning' : 'success',
            tool: String(step.tool ?? 'auto_safe_action'),
            risk: 'safe',
            payload: step,
          })
        }
      } else if (result.firstAction) {
        addInvestigationStep({
          title: String(result.safeAction ?? result.actionStatus ?? 'Action safe executee'),
          detail: `${candidatePage.value?.totalCount ?? extractCandidateCount(result) ?? 0} candidat(s)`,
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
            return `${index + 1}. ${String(row.description ?? row.type ?? 'Étape')}`
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
        ? `Priorité assistant: ${String(nextBestAction.label ?? nextBestAction.id)}${Number.isFinite(Number(nextBestAction.confidence)) ? ` (${Number(nextBestAction.confidence)}/100)` : ''} — ${String(nextBestAction.reason ?? '')}`
        : ''
      const reportSummary = String(contextReport.summary ?? '').trim()
      const learnedProfile = typeof contextReport.learnedProfile === 'object' && contextReport.learnedProfile !== null
        ? contextReport.learnedProfile as Record<string, unknown>
        : {}
      const learnedStarts = Number(learnedProfile.starts ?? 0)
      const learnedNoCandidate = Number(learnedProfile.noCandidateCount ?? 0)
      const learnedHint = learnedStarts > 0
        ? `Mémoire jeu: ${learnedStarts} passe(s) Auto, ${learnedNoCandidate} sans candidat.`
        : ''
      const preferredStrategy = typeof contextReport.preferredStrategy === 'object' && contextReport.preferredStrategy !== null
        ? contextReport.preferredStrategy as Record<string, unknown>
        : {}
      const strategyLabel = String(preferredStrategy.label ?? '').trim()
      const strategyScore = Number(preferredStrategy.score ?? Number.NaN)
      const strategyReason = String(preferredStrategy.reason ?? '').trim()
      const strategyHint = strategyLabel
        ? `Stratégie: ${strategyLabel}${Number.isFinite(strategyScore) ? ` (${strategyScore})` : ''}${strategyReason ? ` — ${strategyReason}` : ''}`
        : ''
      const insightHint = telemetryInsights.length > 0
        ? `Signal telemetry: ${String(telemetryInsights[0].label ?? telemetryInsights[0].id)} — ${String(telemetryInsights[0].nextAction ?? telemetryInsights[0].reason ?? '')}`
        : ''
      const displayValueHint = displayValueReport.enabled === true
        ? `Rapport valeurs affichées: ${String(displayValueReport.recommendation ?? 'Trace UI string puis sources numeriques confirmees.')}`
        : ''

      const baseMessage = String(result.message ?? result.error ?? 'Plan auto généré.').trim()
      const messageParts = [baseMessage]
      if (nextBestHint) messageParts.push(nextBestHint)
      if (reportSummary) messageParts.push(`Contexte: ${reportSummary}`)
      if (learnedHint) messageParts.push(learnedHint)
      if (strategyHint) messageParts.push(strategyHint)
      if (insightHint) messageParts.push(insightHint)
      if (displayValueHint) messageParts.push(displayValueHint)
      if (planLines.length > 0) messageParts.push(`Plan:\n${planLines.join('\n')}`)
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
        intentRationale: 'Planification proactive avec exécution des actions sûres uniquement.',
      })
      if (result.requiresConfirmation) {
        addInvestigationStep({
          title: 'Checkpoint confirmation',
          detail: String(result.confirmationReason ?? 'Confirmation utilisateur requise.'),
          status: 'checkpoint',
          risk: 'write',
          payload: result as Record<string, unknown>,
        })
      }
      await refreshSmartSearchContext()
    } catch (e) {
      addInvestigationStep({
        title: 'Erreur Auto Resolve',
        detail: String(e),
        status: 'error',
        tool: 'startAutoResolve',
        risk: 'safe',
      })
      updateMessage(thinkingMessage.id, 'Erreur auto-résolution : ' + String(e), { isThinking: false, isError: true })
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
      const result = await writeMemoryValueByMode(normalizedAddress, type, value)
      writeResult.value = result
      pushMessage('assistant',
        result.success
          ? `J'ai écrit ${value} uniquement sur 0x${normalizedAddress}${kernelMemoryModeActive.value ? ' via le driver kernel' : ''}. Vérifie dans le jeu si c'est la bonne adresse.`
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

    // Le mode Auto (multi-type) est crucial pour les cibles modernes : il cherche
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
      .map((entry): GroupScanEntryInput => ({ offset: Number(entry.offset), type: entry.type, value: entry.value.trim() }))
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
      const preview = await readMemoryPreviewByMode(entry.finalAddress, valueTypeReadSize(entry.type))
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

  function setWatchedPointerChainsLiveEnabled(enabled: boolean) {
    watchedPointerChainsLiveEnabled.value = enabled
    if (watchedPointerChainsLiveTimer) {
      clearInterval(watchedPointerChainsLiveTimer)
      watchedPointerChainsLiveTimer = null
    }
    if (enabled) {
      void refreshWatchedPointerChains()
      watchedPointerChainsLiveTimer = setInterval(() => {
        void refreshWatchedPointerChains()
      }, 1000)
    }
    addActionLog(
      'watch',
      enabled ? 'Watch expressions live activé' : 'Watch expressions live arrêté',
      `${watchedPointerChains.value.length} chaîne(s).`,
      enabled ? 'success' : 'info',
    )
  }

async function doEncryptedScan() {
    if (!exactScanValue.value.trim() || scanBusy.value) return

    const controller = backend.getController()
    if (!controller.scanEncryptedValue) {
      encryptedScanResult.value = {
        success: false,
        partial: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: 'Scan chiffré non exposé par ce backend.',
        matches: [],
      }
      addActionLog('scan', 'Scan chiffré indisponible', encryptedScanResult.value.error, 'warning')
      return
    }

    try {
      scanBusy.value = true
      setScanProgress(0)
      scanStatusText.value = 'Scan chiffré en cours...'
      addActionLog('scan', `Scan chiffré ${encryptedScanMode.value.toUpperCase()} ${exactScanValue.value}`, `${exactScanType.value}.`, 'info')
      encryptedScanResult.value = await controller.scanEncryptedValue(
        exactScanValue.value,
        exactScanType.value === 'Auto' ? 'Int32' : exactScanType.value,
        {
          mode: encryptedScanMode.value,
          key: encryptedScanKey.value,
          keySearchBits: encryptedScanKeySearchBits.value,
          startAddress: expertStartAddress.value.trim() || undefined,
          stopAddress: expertStopAddress.value.trim() || undefined,
          alignment: expertAlignment.value > 0 ? expertAlignment.value : undefined,
          writableOnly: true,
          executableOnly: expertExecutableOnly.value,
          copyOnWriteOnly: expertCopyOnWriteOnly.value,
          maxResults: 1000,
        },
      )
      setScanProgress(100)
      scanStatusText.value = encryptedScanResult.value.success ? 'Scan chiffré terminé.' : 'Scan chiffré échoué.'
      addActionLog(
        'scan',
        encryptedScanResult.value.success ? 'Scan chiffré terminé' : 'Scan chiffré échoué',
        `${encryptedScanResult.value.matchesFound} match(es), ${encryptedScanResult.value.regionsScanned} région(s).`,
        encryptedScanResult.value.success ? 'success' : 'warning',
      )
    } catch (e) {
      encryptedScanResult.value = {
        success: false,
        partial: false,
        regionsScanned: 0,
        bytesScanned: 0,
        matchesFound: 0,
        matchesReturned: 0,
        error: String(e),
        matches: [],
      }
      scanStatusText.value = 'Scan chiffré échoué.'
      addActionLog('scan', 'Scan chiffré échoué', String(e), 'error')
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

  async function runAutoEncryptedScan(value: string) {
    const trimmed = value.trim()
    if (!trimmed || scanBusy.value) return
    exactScanValue.value = trimmed
    exactScanType.value = 'Int32'
    encryptedScanKey.value = '0'
    encryptedScanKeySearchBits.value = 16
    addInvestigationStep({
      title: 'Scan chiffre guide',
      detail: `XOR/Add/Sub/NOT 16-bit borne depuis la valeur ${trimmed}.`,
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
      title: 'Scan chiffre termine',
      detail: `${encryptedScanResult.value?.matchesFound ?? 0} match(es), ${encryptedScanResult.value?.matchesReturned ?? 0} retourne(s).`,
      status: encryptedScanResult.value?.success ? 'success' : 'warning',
      tool: 'scanEncryptedValue',
      risk: 'safe',
      payload: encryptedScanResult.value as unknown as Record<string, unknown>,
    })
    pushMessage(
      'assistant',
      encryptedScanResult.value?.success
        ? `Scan chiffré guidé terminé : ${encryptedScanResult.value.matchesFound} match(es).`
        : `Scan chiffré guidé sans résultat exploitable : ${encryptedScanResult.value?.error ?? 'aucun match'}.`,
      { isError: encryptedScanResult.value?.success === false },
    )
  }

  async function runAutoTraceUiString(value: string) {
    const trimmed = value.trim()
    if (!trimmed || scanBusy.value) return
    const controller = backend.getController()
    if (!controller.scanUiStrings) {
      pushMessage('assistant', 'Trace UI string non exposé par ce backend.', { isError: true })
      return
    }
    scanBusy.value = true
    addInvestigationStep({
      title: 'Trace UI string guide',
      detail: `Recherche de la valeur affichee "${trimmed}" en ASCII/UTF-16.`,
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
          title: 'Trace UI tracking',
          detail: `${tracked.survivors.length} string(s) suivie(s) vers "${trimmed}".`,
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
        title: 'Trace UI string termine',
        detail: `${autoUiStringScanResult.value.matchesFound} string(s), ${autoUiStringSources.value.length} source(s) numerique(s).`,
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
            label: 'Sources numeriques proches des strings UI',
            reason: `${autoUiStringSources.value.length} source(s) trouvee(s); tester par petits lots avant freeze.`,
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
          ? `Trace UI string a trouvé ${autoUiStringScanResult.value.matchesFound} string(s) et ${autoUiStringSources.value.length} source(s) numérique(s). Les meilleures sources sont dans Investigation/Trainer.`
          : `Trace UI string n'a pas donné de piste exploitable : ${autoUiStringScanResult.value.error || 'aucun match'}.`,
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
        title: 'Trace UI string echoue',
        detail: String(e),
        status: 'error',
        tool: 'scanUiStrings',
        risk: 'safe',
      })
    } finally {
      scanBusy.value = false
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
      autoUnknownAwaitingObservation.value = unknownSnapshotResult.value.success === true
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
    autoUnknownAwaitingObservation.value = status !== 'error' && afterCount > 25
  }

  async function runAutoUnknownObservation(observation: string, thinkingMessageId?: number) {
    const trimmed = observation.trim()
    if (!trimmed) return false

    if (!unknownSnapshotResult.value?.success) {
      addInvestigationStep({
        title: 'Unknown Auto capture',
        detail: 'Aucun snapshot actif : capture initiale avant observation.',
        status: 'running',
        tool: 'captureUnknownSnapshotAsync',
        risk: 'safe',
      })
      await captureUnknownSnapshot()
      const message = unknownSnapshotResult.value?.success
        ? 'Snapshot Unknown capturé. Fais varier la valeur dans le processus, puis donne-moi la nouvelle observation pour que je lance increased/decreased/changed/stable.'
        : `Capture Unknown impossible : ${unknownSnapshotResult.value?.error ?? 'erreur inconnue'}.`
      if (thinkingMessageId !== undefined) {
        updateMessage(thinkingMessageId, message, { isThinking: false, isError: unknownSnapshotResult.value?.success !== true })
      } else {
        pushMessage('assistant', message, { isError: unknownSnapshotResult.value?.success !== true })
      }
      return true
    }

    const mode = inferUnknownModeFromObservation(trimmed)
    addInvestigationStep({
      title: 'Unknown Auto comparaison',
      detail: `Observation "${trimmed}" -> mode ${mode}.`,
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
      title: 'Unknown Auto terminé',
      detail: total > 0
        ? `${total} candidat(s) après ${mode}.${total <= 25 ? ' Checkpoints prêts.' : ' Continue avec une autre variation.'}`
        : `Aucun candidat après ${mode}.`,
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
      ? `Unknown ${mode} terminé : ${total} candidat(s). ${total <= 25 ? 'J’ai préparé des checkpoints dans Investigation pour tester/freeze sous confirmation.' : 'Fais encore varier la valeur et redonne-moi la nouvelle observation.'}`
      : `Unknown ${mode} n’a rien gardé. Prochaine piste : Trace UI string ou scan chiffré borné.`
    if (thinkingMessageId !== undefined) {
      updateMessage(thinkingMessageId, message, { isThinking: false, isError: Boolean(unknownNextScanResult.value?.error), candidateCount: total })
    } else {
      pushMessage('assistant', message, { isError: Boolean(unknownNextScanResult.value?.error), candidateCount: total })
    }
    await refreshSmartSearchContext()
    return true
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
    if (addresses.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    const risk = kernelMemoryModeActive.value ? 'injection' : 'write'
    if (!await confirmRiskAction(risk, 'Ecriture memoire multiple', `${addresses.length} adresse(s), type ${type}, valeur ${value}${kernelMemoryModeActive.value ? ' via driver kernel' : ''}.`)) return
    try {
      const results: MemoryWriteResult[] = []
      for (const address of addresses) {
        const result = await writeMemoryValueByMode(address, type, value)
        results.push(result)
      }
      writeResult.value = results[results.length - 1]
      scanStatusText.value = results.every((r) => r.success)
        ? `${results.length} adresse(s) écrite(s).`
        : `Écriture partielle: ${results.filter((r) => r.success).length}/${results.length} réussie(s).`
      addActionLog('write', `Écriture multiple ${value}`, `${results.filter((r) => r.success).length}/${results.length} réussie(s)${kernelMemoryModeActive.value ? ' via kernel' : ''}.`, results.every((r) => r.success) ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture multiple échouée.'
      addActionLog('write', 'Écriture multiple échouée', String(e), 'error')
    }
  }

  async function writeSelectedTargets(targets: MemoryWriteTarget[], value: string) {
    if (targets.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune cible sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    const risk = kernelMemoryModeActive.value ? 'injection' : 'write'
    if (!await confirmRiskAction(risk, 'Ecriture memoire avec variants', `${targets.length} cible(s), valeur affichee ${value}${kernelMemoryModeActive.value ? ' via driver kernel' : ''}.`)) return
    try {
      const controller = backend.getController()
      if (kernelMemoryModeActive.value) {
        const results: MemoryWriteResult[] = []
        for (const target of targets) {
          results.push(await writeMemoryValueByMode(target.address, target.type, value))
        }
        const written = results.filter((result) => result.success).length
        writeResult.value = {
          success: written === targets.length,
          verified: results.every((result) => result.verified),
          bytesWritten: results.reduce((sum, result) => sum + (result.bytesWritten ?? 0), 0),
          written,
          total: targets.length,
          results,
          error: written === targets.length ? '' : `Écriture kernel partielle: ${written}/${targets.length}.`,
        } as MemoryWriteBatchResult
        scanStatusText.value = written === targets.length
          ? `${written} adresse(s) écrite(s) via kernel.`
          : `Écriture kernel partielle: ${written}/${targets.length} réussie(s).`
        for (const target of targets) addAddressToWatch(target.address, target.type)
        addActionLog('write', `Écriture auto kernel ${value}`, `${written}/${targets.length} réussie(s).`, written === targets.length ? 'success' : 'warning')
        return
      }
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

  // H3 (docs/STRATEGY_ROOM.md, session Solitaire du 19/08/2026) : contrairement
  // à writeSelectedAddresses (une écriture à la fois, l'une après l'autre),
  // écrit toutes les adresses dans la même fenêtre critique (threads de la
  // cible suspendues) — pour les cibles qui maintiennent des copies
  // redondantes d'une même valeur et resynchronisent une écriture isolée.
  async function writeSelectedAtomic(addresses: string[], type: string, value: string) {
    if (addresses.length === 0) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée.' }
      return
    }
    if (!value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire.' }
      return
    }
    if (!await confirmRiskAction('write', 'Écriture atomique multi-adresses', `${addresses.length} adresse(s) en même temps (threads de la cible suspendues), type ${type}, valeur ${value}.`)) return
    const controller = backend.getController()
    if (!controller.writeMemoryValuesAtomic) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Écriture atomique indisponible sur ce backend.' }
      return
    }
    try {
      const targets: AtomicWriteTarget[] = addresses.map((address) => ({ address, type, value }))
      const result: MemoryWriteBatchResult = await controller.writeMemoryValuesAtomic(targets, {})
      writeResult.value = result
      const written = result.written ?? result.results?.filter((r) => r.success).length ?? 0
      scanStatusText.value = result.success
        ? `${written} adresse(s) écrite(s) ensemble (atomique).`
        : `Écriture atomique partielle: ${written}/${addresses.length} réussie(s).`
      for (const address of addresses) {
        addAddressToWatch(address, type)
      }
      addActionLog('write', `Écriture atomique ${value}`, `${written}/${addresses.length} réussie(s), ${result.suspendedThreadCount ?? 0} thread(s) suspendue(s).`, result.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e) }
      scanStatusText.value = 'Écriture atomique échouée.'
      addActionLog('write', 'Écriture atomique échouée', String(e), 'error')
    }
  }

  // Automatisation IA : après une écriture réussie sur une adresse unique,
  // cherche silencieusement (lecture seule, bornée) une chaîne de pointeurs
  // stable, sans que l'utilisateur ait besoin de savoir que ce bouton existe
  // dans Expert. Ne notifie que si une chaîne est réellement trouvée — pas de
  // bruit pour chaque écriture. Dédupliqué par adresse pour la session en
  // cours pour ne pas ressasher la même suggestion à chaque nouvelle écriture
  // sur la même adresse (freeze, retest, etc.).
  const stableLocatorSuggested = new Set<string>()

  async function autoSuggestStableLocatorIfWorthwhile(addressHex: string) {
    const key = addressHex.toLowerCase()
    if (!addressHex || stableLocatorSuggested.has(key)) return
    stableLocatorSuggested.add(key)
    try {
      const controller = backend.getController()
      if (!controller.suggestStableLocatorForAddress) return
      const result = await controller.suggestStableLocatorForAddress(addressHex, {})
      if (result.success && result.bestChain) {
        pushMessage(
          'assistant',
          `🔗 J'ai trouvé une chaîne de pointeurs stable pour 0x${addressHex} (${result.message ?? 'profondeur ' + (result.bestChain.depth ?? '?')}). ` +
            `Elle survivra à un redémarrage du jeu — ouvre Expert > Write et clique "Sauvegarder dans un profil" pour la garder.`,
        )
      }
    } catch {
      // Suggestion best-effort : ne doit jamais interrompre le flux d'écriture principal.
    }
  }

  async function writeSelectedValue() {
    // Avant : retour silencieux si rien n'est sélectionné/rempli — l'utilisateur
    // clique Écrire, rien ne se passe, aucun indice pourquoi. Message explicite
    // à la place, affiché au même endroit que les autres erreurs d'écriture.
    if (!selectedCandidateAddress.value) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Aucune adresse sélectionnée : clique une adresse dans Candidats avant d\'écrire.' }
      return
    }
    if (!writeValue.value.trim()) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: 'Entre une valeur à écrire avant de cliquer sur Écrire.' }
      return
    }
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Écriture bloquée', writeSafetyWarning.value, 'warning')
      return
    }
    const risk = kernelMemoryModeActive.value ? 'injection' : 'write'
    if (!await confirmRiskAction(risk, 'Ecriture memoire', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}${kernelMemoryModeActive.value ? ' via driver kernel' : ''}.`)) return
    try {
      writeResult.value = await writeMemoryValueByMode(selectedCandidateAddress.value, exactScanType.value, writeValue.value)
      addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
      addActionLog(
        'write',
        `Écriture 0x${selectedCandidateAddress.value}`,
        `${exactScanType.value} = ${writeValue.value}${kernelMemoryModeActive.value ? ' via kernel' : ''}${writeSafetyWarning.value ? ` · ${writeSafetyWarning.value}` : ''}.`,
        writeResult.value.success ? 'success' : 'error',
      )
      if (writeResult.value.success && writeResult.value.verified) {
        void autoSuggestStableLocatorIfWorthwhile(selectedCandidateAddress.value)
      }
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
    if (!await confirmRiskAction('write', 'Freeze memoire', `0x${normalized} ${type} = ${currentValue}.`)) return

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
    if (nextState && !await confirmRiskAction('write', 'Freeze memoire', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}.`)) return
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

  async function startBreakpointFreeze() {
    if (!selectedCandidateAddress.value || !writeValue.value.trim()) return
    updateWriteSafetyWarning()
    if (writeSafetyWarning.value && !writeSafetyAcknowledged.value) {
      addActionLog('write_guard', 'Freeze BP bloqué', writeSafetyWarning.value, 'warning')
      return
    }
    if (!await confirmRiskAction('debug', 'Freeze par hardware breakpoint', `0x${selectedCandidateAddress.value} ${exactScanType.value} = ${writeValue.value}. Debug registers/attach requis.`)) return

    const controller = backend.getController()
    if (!controller.freezeWithBreakpoint) {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: 'Freeze par breakpoint non exposé par ce backend.',
        enabled: breakpointFreezeEnabled.value,
      }
      addActionLog('freeze', 'Freeze BP indisponible', writeResult.value.error, 'warning')
      return
    }

    try {
      writeResult.value = await controller.freezeWithBreakpoint(
        selectedCandidateAddress.value,
        exactScanType.value,
        writeValue.value,
        { mode: 'rewrite' },
      )
      breakpointFreezeEnabled.value = writeResult.value.success === true
      if (breakpointFreezeEnabled.value) {
        addAddressToWatch(selectedCandidateAddress.value, exactScanType.value)
      }
      addActionLog(
        'freeze',
        breakpointFreezeEnabled.value ? 'Freeze BP activé' : 'Freeze BP échoué',
        `0x${selectedCandidateAddress.value} = ${writeValue.value}.`,
        breakpointFreezeEnabled.value ? 'success' : 'error',
      )
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: breakpointFreezeEnabled.value }
      addActionLog('freeze', 'Freeze BP échoué', String(e), 'error')
    }
  }

  // Escalade proposee par le chat Assistant apres freezeInstabilityDetected
  // (freeze polling qui derive) : contrairement a startBreakpointFreeze(),
  // l'utilisateur n'a que l'adresse en main, pas le type/la valeur — le
  // backend reutilise directement la FreezeEntry polling existante.
  async function escalateFreezeToBreakpoint(address: string) {
    if (!address.trim()) return
    if (!await confirmRiskAction('debug', 'Freeze par hardware breakpoint', `0x${address} : le freeze polling ne tient pas, passage en Freeze BP. Debug registers/attach requis.`)) return

    const controller = backend.getController()
    if (!controller.escalatePollingFreezeToBreakpoint) {
      pushMessage('assistant', 'Freeze par breakpoint non exposé par ce backend.')
      addActionLog('freeze', 'Freeze BP indisponible', 'escalatePollingFreezeToBreakpoint absent du backend.', 'warning')
      return
    }

    try {
      const result = await controller.escalatePollingFreezeToBreakpoint(address)
      const ok = result.success === true
      breakpointFreezeEnabled.value = ok || breakpointFreezeEnabled.value
      if (ok) {
        selectedCandidateAddress.value = address
        if (result.type) exactScanType.value = String(result.type)
        addAddressToWatch(address, String(result.type ?? exactScanType.value))
      }
      addActionLog(
        'freeze',
        ok ? 'Freeze BP activé (escalade)' : 'Freeze BP échoué (escalade)',
        `0x${address}. ${String(result.error ?? '')}`.trim(),
        ok ? 'success' : 'error',
      )
      pushMessage(
        'assistant',
        ok
          ? `Freeze BP actif sur 0x${address} : l'écriture est maintenant bloquée à la source, ça devrait tenir même si la cible réécrit vite.`
          : `Échec du passage en Freeze BP sur 0x${address}${result.error ? ` : ${String(result.error)}` : '.'}`,
      )
    } catch (e) {
      addActionLog('freeze', 'Freeze BP échoué (escalade)', String(e), 'error')
      pushMessage('assistant', `Échec du passage en Freeze BP sur 0x${address} : ${String(e)}`)
    }
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

  async function stopBreakpointFreeze() {
    const controller = backend.getController()
    if (!controller.stopBreakpointFreeze) {
      writeResult.value = {
        success: false,
        verified: false,
        bytesWritten: 0,
        error: 'Arrêt du freeze par breakpoint non exposé par ce backend.',
        enabled: breakpointFreezeEnabled.value,
      }
      return
    }

    try {
      writeResult.value = await controller.stopBreakpointFreeze()
      if (writeResult.value.success) {
        breakpointFreezeEnabled.value = false
      }
      const hits = writeResult.value.hits !== undefined ? ` hits=${writeResult.value.hits}` : ''
      const rewrites = writeResult.value.rewrites !== undefined ? ` rewrites=${writeResult.value.rewrites}` : ''
      addActionLog('freeze', 'Freeze BP arrêté', `${hits}${rewrites}`.trim() || 'Session arrêtée.', writeResult.value.success ? 'success' : 'warning')
    } catch (e) {
      writeResult.value = { success: false, verified: false, bytesWritten: 0, error: String(e), enabled: breakpointFreezeEnabled.value }
      addActionLog('freeze', 'Arrêt Freeze BP échoué', String(e), 'error')
    }
  }

  async function setFreezeInterval(intervalMs: number) {
    const requested = Math.round(Number(intervalMs))
    const clamped = Math.min(2000, Math.max(10, Number.isFinite(requested) ? requested : 100))
    freezeIntervalMs.value = clamped
    try {
      const result = await backend.getController().setFreezeInterval(clamped)
      freezeIntervalResult.value = result
      if (result.success === false) {
        addActionLog('freeze', 'Intervalle freeze refusé', String(result.error ?? 'Erreur inconnue.'), 'warning')
        return
      }
      const applied = Math.round(Number(result.intervalMs ?? clamped))
      if (Number.isFinite(applied)) freezeIntervalMs.value = applied
      addActionLog('freeze', 'Intervalle freeze', `${freezeIntervalMs.value} ms.`, 'success')
    } catch (e) {
      freezeIntervalResult.value = { success: false, error: String(e) }
      addActionLog('freeze', 'Intervalle freeze échoué', String(e), 'error')
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
    showOnboarding,
    dismissOnboarding,
    openUserGuide,
    defenderExclusionResult,
    defenderExclusionBusy,
    requestWindowsDefenderExclusion,
    kernelDriverStatus,
    kernelDriverStatusLoading,
    kernelDriverStatusError,
    refreshKernelDriverStatus,
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
    refreshClrInspectorStatus,
    attachClrInspector,
    detachClrInspector,
    shutdownClrInspector,
    flushClrInspectorCache,
    findClrObjects,
    readClrObject,
    enumerateClrRoots,
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
    settingAutoRiskMode,
    lastRiskBlockReason,
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
    executeCheckpointKernelWrite,
    prepareCheckpointAob,
    executeCheckpointForceValue,
    createTrainerFeature,
    createTrainerFeatureFromCheckpoint,
    applyTrainerFeature,
    restoreTrainerFeature,
    applyAllTrainerFeatures,
    restoreAllTrainerFeatures,
    deleteTrainerFeature,
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
