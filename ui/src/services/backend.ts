/**
 * KillEngine - Backend Service
 *
 * Pont entre le frontend Vue et le backend C++ via QWebChannel.
 * Fournit une API TypeScript typée pour appeler les méthodes C++.
 */

export interface ProcessInfo {
  pid: number
  name: string
  path: string
  arch: string
  hasWindow: boolean
  moduleCount: number
}

export interface ProcessModuleInfo {
  name: string
  path: string
  baseAddress: string
  size: number
}

export interface ProcessSaveFileInfo {
  path: string
  sizeBytes: number
  lastWriteTime: string
}

export interface ProcessSaveFileDiscoveryResult {
  success: boolean
  familyName?: string
  files: ProcessSaveFileInfo[]
  error?: string
}

export interface ProcessSaveFileTextResult {
  success: boolean
  path?: string
  text?: string
  truncated?: boolean
  error?: string
}

export interface ProcessLocalSettingsValue {
  keyPath: string
  name: string
  type: string
  preview: string
  dataSizeBytes: number
}

export interface ProcessLocalSettingsResult {
  success: boolean
  familyName?: string
  settingsPath?: string
  values: ProcessLocalSettingsValue[]
  count?: number
  error?: string
}

export interface SaveFileWatchResult {
  success: boolean
  started?: boolean
  requestId?: number
  path?: string
  changed?: boolean
  changeType?: string
  cancelled?: boolean
  error?: string
}

export interface SaveFilePatchResult {
  success: boolean
  path?: string
  occurrencesFound?: number
  error?: string
}

export interface MemoryRegionInfo {
  baseAddress: string
  allocationBase: string
  size: number
  protection: string
  state: string
  type: string
  readable: boolean
  writable: boolean
  executable: boolean
  guarded: boolean
}

export interface MemoryMapStats {
  regionCount: number
  committedCount: number
  readableCount: number
  writableCount: number
  executableCount: number
  totalBytes: number
  committedBytes: number
  readableBytes: number
  writableBytes: number
  executableBytes: number
}

export interface MemoryMapResult {
  attached: boolean
  pid?: number
  processName?: string
  stats: MemoryMapStats
  regions: MemoryRegionInfo[]
}

export interface KernelDriverStatus {
  success: boolean
  status: 'unavailable' | 'connected' | 'access_denied' | 'incompatible' | 'error' | string
  devicePath: string
  message: string
  error?: string
  serviceName?: string
  serviceState?: number
  started?: boolean
  alreadyRunning?: boolean
  capabilities: {
    protocolVersion: number
    healthProbe: boolean
    processMemoryAccess: boolean
    privilegedInstrumentation: boolean
  }
}

export interface MemoryReadPreview {
  success: boolean
  partial: boolean
  cancelled: boolean
  bytesRead: number
  requestedBytes: number
  error: string
  hex: string
}

export interface KernelMemoryReadResult {
  success: boolean
  bytesRead?: number
  hex?: string
  error?: string
}

export interface KernelMemoryWriteResult {
  success: boolean
  bytesWritten?: number
  error?: string
}

export interface ClrInspectorStatus {
  success: boolean
  available: boolean
  running: boolean
  attachedProcess: boolean
  pid?: number
  processName?: string
  helperPath?: string
  pipeName?: string
  error?: string
}

export interface ClrRpcResult<T = unknown> {
  success: boolean
  method?: string
  result?: T
  error?: string
}

export interface ClrObjectSummary {
  address: string
  typeName: string
  size?: number
  identityField?: string
  identityValue?: unknown
}

export interface ClrFieldLocatorResult {
  success: boolean
  typeSubstring: string
  fieldName: string
  expectedValue: string
  scannedObjects: number
  typeMatches: number
  matchesReturned: number
  maxResults: number
  matches: ClrObjectSummary[]
}

export interface ClrFieldInfo {
  name: string
  typeName?: string
  elementType?: string
  kind?: string
  value?: unknown
  address?: string
  size?: number
  writable?: boolean
  objectTypeName?: string
  error?: string
}

export interface ClrObjectReadResult {
  address: string
  typeName: string
  size?: number
  fields?: Record<string, unknown>
  fieldDetails?: ClrFieldInfo[]
}

export interface ClrPathWriteOperation {
  path: string
  value: string
}

export interface ClrCallInstanceMethodResult {
  success: boolean
  objectAddress?: string
  methodName?: string
  nativeCodeAddress?: string
  parameterType?: string
  /** Chantier "setters a parametre objet/string" : true si le parametre resolu
   *  est un type reference (classe/string) -- valueText doit alors etre une
   *  adresse hex (0x...) d'un objet DEJA EXISTANT sur le tas, pas une valeur
   *  primitive. */
  parameterIsReferenceType?: boolean
  threadCompleted?: boolean
  verified?: boolean
  error?: string
}

export interface ClrRootInfo {
  rootAddress?: string
  address?: string
  rootKind?: string
  isPinned?: boolean
  objectAddress?: string
  objectTypeName?: string
  name?: string
}

/** Une étape du chemin root -> ... -> objet cible (chantier "GCRoot chain complet"). */
export interface ClrGcRootPathStep {
  kind: string
  fieldName?: string | null
  index?: number | null
  objectAddress: string
  typeName?: string
}

/** Un noeud du graphe visite par generateObjectReport (chantier "rapport d'objet"). */
export interface ClrObjectReportNode {
  address: string
  depth: number
  discoveredVia?: { parentAddress: string; kind: string; fieldName?: string | null; index?: number | null } | null
  node: ClrObjectReadResult
}

export interface ClrObjectReportResult {
  success: boolean
  rootAddress?: string
  rootTypeName?: string
  generatedAtUtc?: string
  nodeCount?: number
  maxDepth?: number
  maxNodes?: number
  truncated?: boolean
  truncatedByDepth?: boolean
  truncatedByNodes?: boolean
  truncatedByTime?: boolean
  elapsedMs?: number
  gcRootChain?: ClrGcRootPathResult | null
  nodes?: ClrObjectReportNode[]
  error?: string
}

export interface ClrGcRootPathResult {
  success: boolean
  targetAddress?: string
  rootKind?: string
  rootAddress?: string
  rootObjectAddress?: string
  rootObjectTypeName?: string
  depth?: number
  path?: ClrGcRootPathStep[]
  rootsScanned?: number
  nodesVisited?: number
  elapsedMs?: number
  budgetExceeded?: boolean
  shortestPathGuaranteed?: boolean
  note?: string
  message?: string
  error?: string
}

export interface ClrDisassembledInstruction {
  address: string
  length: number
  disassembly?: string
  mnemonicHint?: string
  rawBytesText?: string
  decoder?: string
}

export interface ClrDisassembleMethodResult {
  success: boolean
  objectAddress?: string
  methodName?: string
  nativeCodeAddress?: string
  instructions?: ClrDisassembledInstruction[]
  requestedInstructionCount?: number
  returnedInstructionCount?: number
  truncated?: boolean
  error?: string
}

export interface LuaScriptingStatus {
  success: boolean
  available: boolean
  luaPath?: string
  helperAvailable?: boolean
  helperPath?: string
  helperDirectory?: string
  pipeName?: string
  automationPipeOptIn?: boolean
  message?: string
  error?: string
}

export interface LuaScriptRunResult {
  success: boolean
  started?: boolean
  timedOut?: boolean
  cancelled?: boolean
  exitCode?: number
  luaPath?: string
  helperPath?: string
  stdout?: string
  stderr?: string
  error?: string
}

export interface LuaScriptRunStartResult {
  success: boolean
  started: boolean
  requestId?: number
  error?: string
}

export interface UiStringCandidate {
  address: string
  encoding: 'ascii' | 'utf16' | string
  text: string
  byteLength: number
  bytesHex?: string
  movedFrom?: string
  movedDistanceBytes?: number
  regionBase?: string
  regionSize?: number
  protection?: string
  memoryType?: string
  writable?: boolean
}

export interface UiStringScanResult {
  success: boolean
  partial?: boolean
  matchesFound: number
  matchesReturned: number
  maxResults?: number
  regionsScanned: number
  bytesScanned: number
  elapsedMs?: number
  writableOnly?: boolean
  error: string
  matches: UiStringCandidate[]
}

export interface UiStringTrackResult {
  success: boolean
  checked: number
  unreadable: number
  moved?: number
  remaining: number
  error: string
  survivors: UiStringCandidate[]
}

export interface UiStringSourceCandidate {
  address: string
  type: string
  confidence?: number
  variantLabel?: string
  lastValueHex?: string
  lastValueNumber?: number
  previousValueNumber?: number
  expectedHex?: string
  trackHits?: number
  distanceBytes?: number
  offsetFromString?: number
  regionBase?: string
  protection?: string
  memoryType?: string
}

export interface UiStringSourceResult {
  success: boolean
  partial?: boolean
  matchesFound: number
  matchesReturned: number
  bytesScanned: number
  windowStart?: string
  windowEnd?: string
  radiusBytes?: number
  error: string
  candidates: UiStringSourceCandidate[]
}

export interface UiStringSourceTrackResult {
  success: boolean
  checked: number
  unreadable: number
  incompatible?: number
  remaining: number
  error: string
  survivors: UiStringSourceCandidate[]
}

export interface UiStringPointerRef {
  address: string
  pointsTo: string
  nearestString: string
  distanceToString?: number
  regionBase?: string
  protection?: string
  memoryType?: string
  writable?: boolean
}

export interface UiStringOriginTarget {
  address: string
  offsetFromCluster?: number
  regionBase?: string
  protection?: string
  memoryType?: string
}

export interface UiStringOriginResult {
  success: boolean
  partial?: boolean
  targetCount: number
  clusterStart?: string
  clusterEnd?: string
  clusterSpanBytes?: number
  commonStrideBytes?: number
  pointerRefsFound: number
  bytesScanned: number
  regionsScanned: number
  error: string
  targets: UiStringOriginTarget[]
  pointerRefs: UiStringPointerRef[]
}

export interface UiStringInvestigationChange {
  address: string
  offset?: number
  length?: number
  label?: string
  reason?: string
  beforeHex?: string
  afterHex?: string
  beforeInt32?: number
  afterInt32?: number
  beforeFloat32?: number
  afterFloat32?: number
}

export interface UiStringInvestigationStartResult {
  success: boolean
  windows: number
  bytesCaptured?: number
  probeBlocks?: number
  probeBytesCaptured?: number
  probeRegions?: number
  probeUnreadable?: number
  unreadable?: number
  radiusBytes?: number
  error: string
}

export interface UiStringInvestigationFinishResult {
  success: boolean
  windowsChecked: number
  capturedWindows?: number
  unreadable: number
  changedBytes: number
  changesFound: number
  globalValueHits?: UiStringSourceCandidate[]
  globalValueHitsFound?: number
  probeBlocksCaptured?: number
  probeBlocksChecked?: number
  probeBlocksChanged?: number
  probeBytesChecked?: number
  probeChangedBytes?: number
  probeUnreadable?: number
  partial?: boolean
  elapsedMs?: number
  error: string
  changes: UiStringInvestigationChange[]
}

export interface ExactScanMatch {
  address: string
  type: string
  confidence?: number
  variantLabel?: string
  lastValueHex?: string
  lastValueNumber?: number
  /** Cette adresse a déjà reçu une écriture confirmée avec succès (getCandidates uniquement) — distinct de la confiance de scan. */
  writeVerified?: boolean
}

export interface ExactScanResult {
  requestId?: number
  success: boolean
  partial: boolean
  cancelled: boolean
  regionsScanned: number
  bytesScanned: number
  matchesFound: number
  matchesReturned: number
  error: string
  matches: ExactScanMatch[]
  candidateStoreSize: number
  elapsedMs?: number
  bytesPerSecond?: number
  matchesPerSecond?: number
  candidateStoreFileBacked?: boolean
  candidateStoreBytes?: number
  candidateStoreMemoryBytes?: number
}

export interface EncryptedScanResult {
  success: boolean
  partial?: boolean
  regionsScanned: number
  bytesScanned: number
  matchesFound: number
  matchesReturned: number
  maxResults?: number
  error: string
  matches: ExactScanMatch[]
  elapsedMs?: number
  mode?: string
  key?: string
  keySearchBits?: number
}

export interface CandidatePage {
  pageIndex: number
  pageSize: number
  totalCount: number
  displaySuppressed?: boolean
  displayLimit?: number
  fileBacked?: boolean
  candidateStorePath?: string
  candidateStoreBytes?: number
  candidateStoreMemoryBytes?: number
  candidates: ExactScanMatch[]
}

export interface NextScanResult {
  requestId?: number
  kind?: string
  success: boolean
  cancelled?: boolean
  checked: number
  unreadable: number
  remaining: number
  error: string
  debugBeforeCount?: number
  debugMode?: string
  debugValue?: string
  streamInput?: boolean
  streamOutput?: boolean
  fileBacked?: boolean
  candidateStorePath?: string
  elapsedMs?: number
  candidatesPerSecond?: number
  candidateStoreBytes?: number
  candidateStoreMemoryBytes?: number
  debugSamples?: Array<Record<string, unknown>>
  valueHistoryUpdates?: Array<Record<string, unknown>>
  diagnostic?: string
  /** H1 : petit groupe de candidats identiques depuis plusieurs cycles d'affilée — probablement des copies redondantes. */
  stableGroupCycles?: number
  stableGroupAddresses?: string[]
  stableGroupHint?: string
}

export interface UndoCandidateScanResult {
  success: boolean
  restored: boolean
  count: number
  fileBacked?: boolean
  candidateStorePath?: string
  candidateStoreBytes?: number
  candidateStoreMemoryBytes?: number
  error: string
}

export interface UnknownSnapshotResult {
  requestId?: number
  kind?: string
  success: boolean
  partial: boolean
  cancelled: boolean
  regionsCaptured: number
  regionsSkipped: number
  bytesCaptured: number
  captureLimitBytes?: number
  captureLimitReached?: boolean
  compressedBytes?: number
  mappedStorage?: boolean
  writableOnly?: boolean
  executableOnly?: boolean
  copyOnWriteOnly?: boolean
  suggestedDepthMb?: number
  relevantBytes?: number
  autoDepthApplied?: boolean
  error: string
}

export interface UnknownNextScanResult {
  requestId?: number
  kind?: string
  success: boolean
  partial: boolean
  cancelled?: boolean
  checkedBytes: number
  matchesFound: number
  stored: number
  valueType?: string
  typePasses?: Array<{
    type: string
    success: boolean
    partial?: boolean
    checkedBytes?: number
    matchesFound?: number
    stored?: number
    error?: string
  }>
  error: string
  diagnostic?: string
  refinedFromCandidates?: boolean
}

export interface MemoryWriteResult {
  success: boolean
  verified: boolean
  protectionChanged?: boolean
  bytesWritten: number
  error: string
  address?: string
  type?: string
  variantLabel?: string
  encodedHex?: string
  displayValue?: string
  written?: number
  total?: number
  protectionChangedCount?: number
  results?: MemoryWriteResult[]
  enabled?: boolean
  confirmationMode?: boolean
  temporaryVerified?: boolean
  restoredBeforeFinal?: boolean
  finalVerified?: boolean
  mode?: string
  hits?: number
  rewrites?: number
  blocks?: number
  errors?: number
  breakpointSize?: number
  freezeMode?: string
  /** H5 : avertissement non-bloquant (ex. freeze sur une adresse jamais write-vérifiée). */
  warning?: string
  /** H3 (writeMemoryValuesAtomic) : nombre de threads de la cible suspendues pendant l'écriture groupée. */
  suspendedThreadCount?: number
  parseErrors?: Array<Record<string, unknown>>
}

export interface MemoryWriteTarget {
  address: string
  type: string
  variantLabel?: string
}

export interface MemoryWriteBatchResult extends MemoryWriteResult {
  written?: number
  total?: number
  results?: MemoryWriteResult[]
}

/** Cible pour writeMemoryValuesAtomic — H3, adresse + type + valeur explicites (pas de variante x100/x65536). */
export interface AtomicWriteTarget {
  address: string
  type: string
  value: string
}

export interface AobScanMatch {
  address: string
  regionBase?: string
  regionSize?: number
  protection?: string
  memoryType?: string
  module?: string
  moduleOffset?: string
}

export interface AobPatternQuality {
  score: number
  level: 'strong' | 'medium' | 'weak' | 'invalid' | string
  warning?: string
  patternBytes?: number
  fixedBytes?: number
  wildcardBytes?: number
  uniqueFixedBytes?: number
  fixedRatio?: number
  trainerSafe?: boolean
}

export interface AobScanResult {
  success: boolean
  partial?: boolean
  error?: string
  bytesScanned?: number
  regionsScanned?: number
  matchesFound?: number
  matches?: AobScanMatch[]
  patternBytes?: number
  executableOnly?: boolean
  imageOnly?: boolean
  signatureQuality?: AobPatternQuality
  signatureRisk?: string
  signatureWarning?: string
}

export interface AobSignatureResult {
  success: boolean
  partial?: boolean
  error?: string
  warning?: string
  startAddress?: string
  instructionAddress?: string
  bytesRead?: number
  requestedBytes?: number
  hex?: string
  pattern?: string
  patternBytes?: number
  signatureQuality?: AobPatternQuality
  signatureRisk?: string
  module?: string
  moduleOffset?: string
}

export interface BackwardDisassemblyInstruction {
  address: string
  bytes: string
  disassembly?: string
  mnemonicHint?: string
  category?: string
  memBaseRegister?: string
  memDisplacement?: number
  // true quand l'instruction a un operande memoire [base+deplacement] simple
  // exploitable (source Zydis) : indice qu'il s'agit potentiellement d'un
  // champ "actuel"/"cible" source d'un compteur anime, a proposer en
  // priorite pour une ecriture, plutot que le champ affiche d'origine.
  isCandidateField?: boolean
}

export interface BackwardDisassemblyResult {
  success: boolean
  error?: string
  warning?: string
  address?: string
  codeReadProtected?: boolean
  instructions?: BackwardDisassemblyInstruction[]
  candidateFields?: BackwardDisassemblyInstruction[]
}

// Verdict du test automatique d'un champ candidat (testCandidateFieldsAsync) :
// remplace la lecture manuelle d'assembleur par une preuve empirique (écrit
// une valeur test, attend, relit, restaure) — voir docs/USER_GUIDE.md
// "Compteurs animés".
export interface CandidateFieldTestOutcome {
  address: string
  memBaseRegister?: string
  memDisplacement?: number
  valueType?: string
  verdict: 'holds' | 'reverts' | 'error'
  ticksSurvived?: number
  restored?: boolean
  error?: string
}

export interface CandidateFieldTestResult {
  requestId?: number
  kind?: string
  success: boolean
  error?: string
  results?: CandidateFieldTestOutcome[]
  cancelled?: boolean
}

export interface CandidateFieldTestStartResult {
  success: boolean
  started?: boolean
  requestId?: number
  candidateCount?: number
  warning?: string
  error?: string
}

// Statut du speedhack (roadmap section J) : accélère/ralentit le temps perçu
// par le processus attaché via un composant injecté. factor=1.0 vitesse
// normale, factor=0.0 pause. hooksInstalledMask est un bitmask diagnostic
// (voir killcore::kSpeedhackHook* côté C++), pas destiné à être décodé côté UI.
export interface SpeedhackStatus {
  success: boolean
  active: boolean
  installError?: boolean
  factor: number
  hooksInstalledMask?: number
  pid?: number
  error?: string
}

// Statut de blockProcessNetwork()/unblockProcessNetwork() : coupe le réseau
// du processus attaché via une règle pare-feu Windows dédiée à son exécutable.
export interface ProcessNetworkBlockStatus {
  success: boolean
  blocked?: boolean
  cancelled?: boolean
  ruleName?: string
  ruleOutbound?: string
  ruleInbound?: string
  exePath?: string
  error?: string
}

// Statut de l interception de fonctions (roadmap section B) : hook MinHook
// injecte qui compte les appels d une fonction (mode 0) ou force son retour
// (mode 1). callCount est lu en direct depuis l IPC partagee.
export interface ApiHookStatus {
  success: boolean
  active: boolean
  installError?: boolean
  resolveError?: boolean
  callCount?: number
  finalCallCount?: number
  pid?: number
  error?: string
}

export interface CodePatchResult {
  success: boolean
  verified?: boolean
  active?: boolean
  error?: string
  address?: string
  matchedAddress?: string
  profileName?: string
  patchName?: string
  matchCount?: number
  patchBytes?: string
  originalBytes?: string
  writtenBytes?: string
  restoredBytes?: string
  bytesWritten?: number
  protectionChanged?: boolean
  // Rempli par applyProfileCodePatch : signale que ce patch a été enregistré
  // pour une version différente de l'exécutable attaché (hash SHA-256 du
  // binaire différent) — la cause la plus probable quand une signature AOB
  // qui marchait avant ne matche plus rien après une mise à jour du jeu.
  executableVersionMismatch?: boolean
  executableVersionWarning?: string
}

export interface CodePatchSuggestion {
  label: string
  bytesText: string
  description?: string
  category?: string
  riskLevel?: 'low' | 'medium' | 'high' | string
  risky?: boolean
  // Quand vrai, bytesText n'est que le point de depart (bytes originaux) :
  // il faut demander une valeur a l'utilisateur et reconstruire les bytes en
  // substituant valueSize octets (little-endian) a partir de valueOffset.
  needsValueInput?: boolean
  valueOffset?: number
  valueSize?: number
}

export interface CodePatchSuggestionResult {
  success: boolean
  error?: string
  warning?: string
  address?: string
  instructionSuccess?: boolean
  instructionLength?: number
  mnemonicHint?: string
  disassembly?: string
  decoder?: string
  category?: string
  stableAobPattern?: string
  signatureQuality?: AobPatternQuality
  signatureRisk?: string
  bytesRead?: number
  bytes?: string
  suggestions?: CodePatchSuggestion[]
  // Registre de base + déplacement de l'opérande mémoire destination (vide
  // si non exploitable) : permet de proposer "Forcer une valeur (hook)"
  // même quand l'instruction n'a pas d'immédiat (source registre).
  memBaseRegister?: string
  memDisplacement?: number
}

export interface ExpertScanOptions {
  startAddress?: string
  stopAddress?: string
  alignment?: number
  writableOnly?: boolean
  executableOnly?: boolean
  copyOnWriteOnly?: boolean
  unknownSnapshotMaxMb?: number
}

export interface SmartSearchResult {
  status: string
  message?: string
  query: string
  tool?: string
  args?: Record<string, unknown>
  actionStatus?: string
  actionResult?: Record<string, unknown>
  autoWriteResult?: Record<string, unknown>
  autoWriteResults?: Array<Record<string, unknown>>
  suggestedWrite?: Record<string, unknown>
  suggestedWrites?: Array<Record<string, unknown>>
  requiresConfirmation?: boolean
  confirmationReason?: string
  error?: string
  [key: string]: unknown
}

export interface SmartSearchDebugEventsResult {
  success: boolean
  path: string
  events: Array<Record<string, unknown>>
  error: string
}

export interface LogTailResult {
  success: boolean
  path: string
  lines: string[]
  error: string
}

export interface TemporaryStorageStatus {
  success: boolean
  tempPath: string
  activeBytes: number
  activeFileCount: number
  candidateBytes: number
  candidateFileBacked: boolean
  undoBytes: number
  undoFileBacked: boolean
  snapshotBytes: number
  snapshotFileBacked: boolean
  orphanBytes: number
  orphanFileCount: number
  totalBytes: number
  orphanFiles?: Array<Record<string, unknown>>
  error?: string
}

export interface ChatMemoryTargetsResult {
  success: boolean
  count: number
  targets: Array<Record<string, unknown>>
  cleared?: number
}

export interface SmartSearchContextResult {
  success: boolean
  active: boolean
  workflow: string
  initialValue: string
  targetValue: string
  valueType: string
  candidateCount: number
  hasUndoReduction: boolean
  chatTargets: Array<Record<string, unknown>>
  profileTargets: Array<Record<string, unknown>>
  lastAutoWriteCount: number
  writeHistory?: string[]
}

export interface AutoResolveReportResult {
  success: boolean
  attached: boolean
  processName: string
  candidateCount: number
  workflow: string
  initialValue: string
  targetValue: string
  valueType: string
  activeChatTargetCount: number
  activeProfileTargetCount: number
  eventCounts: Record<string, number>
  recentSignals: Array<Record<string, unknown>>
  recommendations: Array<Record<string, unknown>>
  guardrails: Array<Record<string, unknown>>
  nextBestAction?: Record<string, unknown>
  telemetryInsights?: Array<Record<string, unknown>>
  displayValueReport?: Record<string, unknown>
  learnedProfile?: Record<string, unknown>
  strategyScores?: Array<Record<string, unknown>>
  preferredStrategy?: Record<string, unknown>
  summary: string
  error?: string
}

export interface AppSettings {
  success?: boolean
  language: 'fr' | 'en'
  defaultValueType: string
  scanMaxResults: number
  scanChunkSizeMb: number
  performanceMode: 'Auto' | 'Eco' | 'Normal' | 'Performance' | 'Max'
  scanMaxWorkerThreads: number
  scanMaxInFlightMb: number
  candidateFileBackedThreshold: number
  unknownSnapshotMaxMb: number
  fastScan: boolean
  smartSearchDebugEnabled: boolean
  smartSearchDebugMaxEvents: number
  modelPath: string
  modelEnabled: boolean
  modelThreads: number
}

export interface AiModelStatus {
  success: boolean
  ready: boolean
  available?: boolean
  enabled?: boolean
  backend: 'llama.cpp' | 'deterministic' | string
  configuredModelPath: string
  envModelPath: string
  envExecutablePath: string
  modelFound: boolean
  modelPath: string
  modelSource: string
  modelError: string
  executableFound: boolean
  executablePath: string
  modelCandidates: Array<Record<string, unknown>>
  executableCandidates: Array<Record<string, unknown>>
  embeddedAgents?: Array<Record<string, unknown>>
  embeddedAgentCount?: number
  embeddedModelFolders?: Array<Record<string, unknown>>
  threads: number
  message: string
  error?: string
}

  export interface PointerChainInfo {
    module: string
    baseOffset: string
    offsets: string[]
    depth?: number
    label?: string
  }

  export interface PointerScanResult {
    success: boolean
    partial?: boolean
    cancelled?: boolean
    pointersScanned?: number
    bytesScanned?: number
    elapsedMs?: number
    chainCount?: number
    chains?: PointerChainInfo[]
    error?: string
  }

  export interface PointerChainResolveResult {
    success: boolean
    finalAddress?: string
    steps?: string[]
    error?: string
  }

  export interface PointerScanOptions {
    maxDepth?: number
    maxOffset?: number
    maxResults?: number
    onlyModuleBase?: boolean
    baseModules?: string[]
    alignment?: number
  }

  export interface StableLocatorSuggestion {
    success: boolean
    chainCount: number
    bestChain?: PointerChainInfo
    elapsedMs?: number
    message?: string
    error?: string
  }

  export interface BackendController {
  getVersion(): Promise<string>
  getProcesses(): Promise<ProcessInfo[]>
  getProcessModules(pid: number): Promise<ProcessModuleInfo[]>
  discoverProcessSaveFiles(maxResults: number): Promise<ProcessSaveFileDiscoveryResult>
  inspectProcessLocalSettings(maxValues: number): Promise<ProcessLocalSettingsResult>
  readProcessSaveFileText(path: string, maxBytes: number): Promise<ProcessSaveFileTextResult>
  /** Surveille un fichier de sauvegarde (bloquant) — voir startSaveFileWatchAsync pour la version non bloquante utilisée par l'UI. */
  watchSaveFileForChanges?(path: string, options: Record<string, unknown>): Promise<SaveFileWatchResult>
  /** Version non bloquante de watchSaveFileForChanges. Le résultat arrive via saveFileWatchFinished. */
  startSaveFileWatchAsync?(path: string, options: Record<string, unknown>): Promise<SaveFileWatchResult>
  cancelSaveFileWatch?(): Promise<Record<string, unknown>>
  saveFileWatchFinished?: QWebChannelSignal<Record<string, unknown>>
  /** Edite en place une séquence d'octets (find/replace hex, même longueur, occurrence unique) dans un fichier de sauvegarde. */
  patchProcessSaveFileBytes?(path: string, findHex: string, replaceHex: string): Promise<SaveFilePatchResult>
  attachProcess(pid: number): Promise<boolean>
  detachProcess(): Promise<void>
  getMemoryMap(): Promise<MemoryMapResult>
  readMemoryPreview(addressHex: string, size: number): Promise<MemoryReadPreview>
  /** Lecture large (jusqu'à 64 Ko) pour le visualiseur hexadécimal navigable — pagination distincte de l'aperçu compact. */
  readMemoryBlock?(addressHex: string, size: number): Promise<MemoryReadPreview>
  scanUiStrings?(value: string, options: ExpertScanOptions & Record<string, unknown>): Promise<UiStringScanResult>
  trackUiStringCandidates?(candidates: UiStringCandidate[], value: string): Promise<UiStringTrackResult>
  analyzeUiStringSources?(
    stringCandidate: UiStringCandidate,
    value: string,
    options: Record<string, unknown>,
  ): Promise<UiStringSourceResult>
  trackUiStringSources?(sourceCandidates: UiStringSourceCandidate[], value: string): Promise<UiStringSourceTrackResult>
  inspectUiStringOrigins?(stringCandidates: UiStringCandidate[], options: Record<string, unknown>): Promise<UiStringOriginResult>
  startUiStringInvestigation?(
    stringCandidates: UiStringCandidate[],
    sourceCandidates: UiStringSourceCandidate[],
    options: Record<string, unknown>,
  ): Promise<UiStringInvestigationStartResult>
  finishUiStringInvestigation?(options: Record<string, unknown>): Promise<UiStringInvestigationFinishResult>
  startChangedPagesDiff?(options: Record<string, unknown>): Promise<Record<string, unknown>>
  finishChangedPagesDiff?(previousValue: string, currentValue: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  startExactScan(value: string, valueType: string): Promise<ExactScanResult>
  startExactScanExpert(
    value: string,
    valueType: string,
    expertOptions: ExpertScanOptions,
  ): Promise<ExactScanResult>
  startExactScanAsync(
    value: string,
    valueType: string,
    expertOptions: ExpertScanOptions,
  ): Promise<Record<string, unknown>>
  /** Phase 13 : scan multi-type + variantes de representation (precision de recherche). */
  startExactScanMultiType?(value: string, valueType: string): Promise<ExactScanResult>
  scanEncryptedValue?(value: string, valueType: string, options: Record<string, unknown>): Promise<EncryptedScanResult>
  scanStarted?: QWebChannelSignal<void>
  scanProgress?: QWebChannelSignal<number>
  scanStatsUpdated?: QWebChannelSignal<number>
  scanFinished?: QWebChannelSignal<ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult>
  nextScan(mode: string, value: string): Promise<NextScanResult>
  nextScanAsync(mode: string, value: string): Promise<Record<string, unknown>>
  undoCandidateScan(): Promise<UndoCandidateScanResult>
  clearScanContext(): Promise<Record<string, unknown>>
  cancelActiveScan(): Promise<Record<string, unknown>>
  getCandidates(pageIndex: number, pageSize: number, addressFilter: string): Promise<CandidatePage>
  captureUnknownSnapshot(): Promise<UnknownSnapshotResult>
  captureUnknownSnapshotWithOptions?(expertOptions: ExpertScanOptions): Promise<UnknownSnapshotResult>
  captureUnknownSnapshotAsync(): Promise<Record<string, unknown>>
  captureUnknownSnapshotAsyncWithOptions?(expertOptions: ExpertScanOptions): Promise<Record<string, unknown>>
  unknownNextScan(mode: string, valueType: string): Promise<UnknownNextScanResult>
  unknownNextScanAsync(mode: string, valueType: string): Promise<Record<string, unknown>>
  writeMemoryValue(addressHex: string, valueType: string, value: string): Promise<MemoryWriteResult>
  writeMemoryValuesWithVariants?(targets: MemoryWriteTarget[], value: string): Promise<MemoryWriteBatchResult>
  /** H3 : écrit plusieurs adresses dans la même fenêtre critique (threads de la cible suspendues) — pour les cibles à copies redondantes. */
  writeMemoryValuesAtomic?(targets: AtomicWriteTarget[], options: Record<string, unknown>): Promise<MemoryWriteBatchResult>
  rollbackLastWrite(): Promise<MemoryWriteResult>
  rollbackLastWriteBatch(): Promise<Record<string, unknown>>
  setFreezeValue(addressHex: string, valueType: string, value: string, enabled: boolean): Promise<MemoryWriteResult>
  analyzeStructureMemory?(addressHex: string, size: number): Promise<Record<string, unknown>>
  freezeWithBreakpoint?(addressHex: string, valueType: string, value: string, options: Record<string, unknown>): Promise<MemoryWriteResult>
  stopBreakpointFreeze?(): Promise<MemoryWriteResult>
  /** Stats live du freeze BP actif (hits/rewrites/errors) sans attendre l'arrêt. */
  getBreakpointFreezeStats?(): Promise<Record<string, unknown>>
  /** Fait passer une adresse déjà en freeze polling vers Freeze BP sans que l'appelant reconnaisse type/valeur (réutilise la FreezeEntry existante). */
  escalatePollingFreezeToBreakpoint?(addressHex: string): Promise<Record<string, unknown>>
  /** Émis quand un freeze par polling ne tient pas (détecté automatiquement, voir applyFreezeTick côté C++). */
  freezeInstabilityDetected?: QWebChannelSignal<Record<string, unknown>>
  /** Émis quand une écriture confirmée repart toute seule peu après (détecté automatiquement, voir applyWriteWatchTick côté C++). */
  writeDidNotHold?: QWebChannelSignal<Record<string, unknown>>
  findWhatWrites?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  findWhatWritesAsync?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  cancelFindWhatWrites?(): Promise<Record<string, unknown>>
  findWhatWritesFinished?: QWebChannelSignal<Record<string, unknown>>
  /** Alternative à findWhatWritesAsync qui ne passe pas par le canal de debug Win32 : PAGE_GUARD + handler injecté. */
  startPageGuardWatchAsync?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  cancelPageGuardWatch?(): Promise<Record<string, unknown>>
  pageGuardWatchFinished?: QWebChannelSignal<Record<string, unknown>>
findWhatAccessesAsync?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  findWhatAccessesFinished?: QWebChannelSignal<Record<string, unknown>>
  findWhatExecutes?(instructionAddressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  /** PHASE 130 : classifie une adresse candidate "probablement champ affiché recalculé" vs "probablement source événementielle" via une capture findWhatWrites passive. Lecture seule, aucune écriture. */
  analyzeFieldStability?(addressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  startInProcessExecuteWatchAsync?(instructionAddressHex: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  readAttachedWindowText?(options: Record<string, unknown>): Promise<Record<string, unknown>>
  scanGroupScan?(entries: Array<{ offset: number, type: string, value: string }>, options: Record<string, unknown>): Promise<EncryptedScanResult>
  writeMemoryHex?(addressHex: string, hexString: string): Promise<Record<string, unknown>>
  dumpMemoryRegion?(addressHex: string, size: number, fileName: string): Promise<Record<string, unknown>>
  globalHotkeyTriggered?: QWebChannelSignal<Record<string, unknown>>
  scanAobPattern?(pattern: string, options: Record<string, unknown>): Promise<AobScanResult>
  generateAobSignature?(addressHex: string, options: Record<string, unknown>): Promise<AobSignatureResult>
  applyCodePatch?(addressHex: string, bytesText: string, options: Record<string, unknown>): Promise<CodePatchResult>
  suggestCodePatches?(addressHex: string, options: Record<string, unknown>): Promise<CodePatchSuggestionResult>
  restoreCodePatch?(addressHex: string): Promise<CodePatchResult>
  /** Désassemble en arrière depuis un RIP connu (ex: hit findWhatWrites) pour repérer les champs sources d'un compteur animé. Lecture seule. */
  disassembleBackward?(addressHex: string, options: Record<string, unknown>): Promise<BackwardDisassemblyResult>
  /** Teste automatiquement lequel des champs candidats de disassembleBackward tient réellement (écriture test + restauration). Non bloquant, résultat via candidateFieldTestFinished. */
  testCandidateFieldsAsync?(writeInstructionAddressHex: string, knownWriteTargetAddressHex: string, options: Record<string, unknown>): Promise<CandidateFieldTestStartResult>
  cancelCandidateFieldTest?(): Promise<Record<string, unknown>>
  candidateFieldTestFinished?: QWebChannelSignal<CandidateFieldTestResult>
  /** Roadmap section J — Speedhack : accélère/ralentit le temps perçu par la cible attachée. */
  startSpeedhack?(factor: number): Promise<SpeedhackStatus>
  setSpeedhackFactor?(factor: number): Promise<SpeedhackStatus>
  stopSpeedhack?(): Promise<SpeedhackStatus>
  getSpeedhackStatus?(): Promise<SpeedhackStatus>
  /** Coupe/rétablit le réseau du processus attaché (règle pare-feu dédiée, invite UAC). Utile pour isoler une synchro serveur en arrière-plan comme cause d'instabilité mémoire. */
  blockProcessNetwork?(): Promise<ProcessNetworkBlockStatus>
  unblockProcessNetwork?(): Promise<ProcessNetworkBlockStatus>
  getProcessNetworkBlockStatus?(): Promise<ProcessNetworkBlockStatus>
  /** Roadmap section B - interception de fonctions : hook MinHook injecte sur module!fonction. mode: 0=compter, 1=forcer retour. */
  startApiHook?(moduleName: string, functionName: string, mode: number, forcedReturnValue: number): Promise<ApiHookStatus>
  stopApiHook?(): Promise<ApiHookStatus>
  getApiHookStatus?(): Promise<ApiHookStatus>
  /** Phase 20 — outils Expert manuels gardés par confirmRiskAction('injection', ...) côté store. */
  injectDllIntoProcess?(dllPath: string): Promise<Record<string, unknown>>
  installFunctionHook?(targetAddressHex: string, hookAddressHex: string): Promise<Record<string, unknown>>
  removeFunctionHook?(targetAddressHex: string): Promise<Record<string, unknown>>
  /** Roadmap section I — résout "module!fonction" (ex. kernel32.dll!CreateFileW) en adresse absolue via la table d'export PE distante. */
  resolveSymbolAddress?(moduleName: string, functionName: string): Promise<Record<string, unknown>>
  parseAutoAssemblerScript?(scriptText: string): Promise<Record<string, unknown>>
  executeAutoAssemblerScript?(scriptText: string): Promise<Record<string, unknown>>
  restoreAutoAssemblerScript?(): Promise<Record<string, unknown>>
  /** Génère et exécute automatiquement un script auto-assembler (trampoline + redirection du site) qui force `value` à la destination mémoire de l'instruction capturée par "Écrit par", même quand la source est un registre. Restaurable via restoreAutoAssemblerScript. */
  forceWriteInstructionValue?(
    ripHex: string,
    instructionLength: number,
    memBaseRegister: string,
    memDisplacement: number,
    valueType: string,
    value: string,
  ): Promise<Record<string, unknown>>
  setFreezeInterval(intervalMs: number): Promise<Record<string, unknown>>
  registerGlobalHotkey?(combo: string, action: Record<string, unknown>): Promise<Record<string, unknown>>
  unregisterGlobalHotkey?(id: number): Promise<Record<string, unknown>>
  getGlobalHotkeys?(): Promise<Record<string, unknown>>
  clearGlobalHotkeys?(): Promise<Record<string, unknown>>
  setTrainerOverlayVisible?(visible: boolean, options: Record<string, unknown>): Promise<Record<string, unknown>>
  updateTrainerOverlay?(state: Record<string, unknown>): Promise<Record<string, unknown>>
  startSmartSearch(query: string): Promise<SmartSearchResult>
  /** RiskGate chat (PHASE, 29/08/2026) : exécute réellement l'écriture/freeze/réécriture sur les adresses mémoire actives dans le chat (m_chatMemoryTargets côté backend) — appelé UNIQUEMENT après confirmRiskAction, jamais directement depuis startSmartSearch qui renvoie désormais une confirmation. */
  confirmChatMemoryWrite?(value: string): Promise<Record<string, unknown>>
  confirmChatMemoryFreeze?(value: string): Promise<Record<string, unknown>>
  confirmRewriteLastAutoWrite?(value: string): Promise<Record<string, unknown>>
  startAutoResolve?(query: string, options: Record<string, unknown>): Promise<SmartSearchResult>
  getAutoResolveReport?(maxEvents: number): Promise<AutoResolveReportResult>
  clearAutoResolveMemory?(allProcesses: boolean): Promise<Record<string, unknown>>
  logAiAudit?(event: string, payload: Record<string, unknown>): Promise<Record<string, unknown>>
  /** Motifs mémorisés (module+offset relatif) pour le jeu attaché — roadmap H.2. */
  getRememberedPatterns?(): Promise<Record<string, unknown>>
  /** Séquence ordonnée (ordre + doublons conservés) des dernières écritures confirmées — roadmap I, replay inter-session. */
  getWriteHistorySequence?(): Promise<Record<string, unknown>>
  /** Rejoue dans l'ordre la séquence persistée d'écritures pour l'exécutable attaché. */
  replayWriteHistorySequence?(): Promise<Record<string, unknown>>
  /** Vide la séquence d'écritures persistée pour l'exécutable attaché. */
  clearWriteHistorySequence?(): Promise<Record<string, unknown>>
  ping(message: string): Promise<string>
  getSettings(): Promise<AppSettings>
  getAiModelStatus?(): Promise<AiModelStatus>
  /** Sélecteur de fichier natif pour le chemin GGUF personnalisé (remplace la saisie manuelle). */
  browseForModelFile?(): Promise<Record<string, unknown>>
  /** Modale de bienvenue première ouverture (QSettings, survit à un profil Windows différent). */
  hasSeenOnboarding?(): Promise<boolean>
  setOnboardingSeen?(seen: boolean): Promise<void>
  openUserGuide?(): Promise<boolean>
  /** Demande une exclusion Windows Defender pour KillEngine.exe (invite UAC visible, jamais silencieux). */
  requestWindowsDefenderExclusion?(): Promise<{ success: boolean; cancelled?: boolean; error?: string }>
  /** Inspecteur CLR/ClrMD externe via helper .NET et named pipe. */
  getClrInspectorStatus?(): Promise<ClrInspectorStatus>
  attachClrInspector?(): Promise<ClrRpcResult>
  detachClrInspector?(): Promise<ClrRpcResult>
  shutdownClrInspector?(): Promise<ClrRpcResult>
  flushClrInspectorCache?(): Promise<ClrRpcResult>
  findClrObjectsByType?(typeSubstring: string): Promise<ClrRpcResult<ClrObjectSummary[]>>
  findClrObjectsByFieldValue?(typeSubstring: string, fieldName: string, expectedValue: string, maxResults: number): Promise<ClrRpcResult<ClrFieldLocatorResult>>
  readClrObject?(addressHex: string): Promise<ClrRpcResult<ClrObjectReadResult>>
  writeClrPrimitiveField?(objectAddressHex: string, fieldName: string, value: string): Promise<ClrRpcResult>
  writeClrPrimitivePath?(objectAddressHex: string, path: string, value: string): Promise<ClrRpcResult>
  writeClrPrimitivePathBatch?(objectAddressHex: string, operations: ClrPathWriteOperation[]): Promise<ClrRpcResult>
  /**
   * PHASE 59 : variantes "locator" de writeClrPrimitivePath/Batch --
   * relocalisent l'objet root via findClrObjectsByFieldValue juste avant
   * d'ecrire, au lieu d'exiger une adresse potentiellement perimee
   * (deplacee par un GC compactant depuis la derniere lecture). Erreur
   * claire si 0 ou plus d'1 objet ne correspond au locator.
   */
  writeClrPrimitivePathByLocator?(typeSubstring: string, identityField: string, identityValue: string, path: string, value: string): Promise<ClrRpcResult>
  writeClrPrimitivePathBatchByLocator?(typeSubstring: string, identityField: string, identityValue: string, operations: ClrPathWriteOperation[]): Promise<ClrRpcResult>
  /**
   * PHASE 59 : variante "atomique" de writeClrPrimitivePathBatch -- suspend
   * toutes les threads du processus attache pendant tout l'appel RPC vers
   * le helper ClrMD (killcore::ProcessThreadsSuspendGuard, meme primitive
   * que l'ecriture memoire atomique). Best-effort honnete : pas une
   * atomicite parfaite, voir la doc native pour le risque documente.
   */
  writeClrPrimitivePathBatchAtomic?(objectAddressHex: string, operations: ClrPathWriteOperation[]): Promise<ClrRpcResult>
  enumerateClrRoots?(typeSubstring: string): Promise<ClrRpcResult<ClrRootInfo[]>>
  /**
   * Reconstruit un chemin root -> ... -> objet cible a travers plusieurs
   * sauts de references (chantier "GCRoot chain complet") -- equivalent
   * approximatif de `!gcroot` SOS/WinDbg, pas une implementation exacte.
   * Chantier le plus exploratoire : un chemin trouve n'est pas garanti le
   * plus court, et l'appel peut etre lent sur un gros tas (timeout natif
   * volontairement large, voir ApplicationController::findClrGcRootPath).
   */
  findClrGcRootPath?(targetObjectAddressHex: string, maxDepth: number, maxRootsScanned: number): Promise<ClrRpcResult<ClrGcRootPathResult>>
  /**
   * Desassemble le code natif deja JITte d'une methode CLR resolue (meme
   * resolution d'adresse que callClrInstanceMethod, mais lecture seule --
   * aucune injection/execution). Utile pour inspecter le setter reel avant
   * de decider de l'appeler.
   */
  disassembleClrMethod?(objectAddressHex: string, methodName: string, instructionCount: number): Promise<ClrRpcResult<ClrDisassembleMethodResult>>
  /**
   * Genere un rapport borne (BFS sur les references, chantier "rapport
   * d'objet") de l'objet donne et de son graphe atteignable, plus
   * optionnellement le chemin GCRoot vers lui. maxDepth/maxNodes a 0 =
   * valeurs par defaut cote helper (3/50) -- volontairement plus faibles que
   * findClrGcRootPath, voir ApplicationController::generateClrObjectReport.
   */
  generateClrObjectReport?(objectAddressHex: string, maxDepth: number, maxNodes: number, includeGcRootChain: boolean): Promise<ClrRpcResult<ClrObjectReportResult>>
  /**
   * Appelle REELLEMENT un setter de propriete C# d'instance dans le
   * processus attache (resolution d'adresse native JITtee via ClrMD +
   * injection shellcode x64 + relecture) -- pas une ecriture memoire brute
   * du champ backing. Categoriquement plus a risque que writeClrPrimitive*
   * (injecte et EXECUTE du code cible) : passe par confirmRiskAction('injection', ...)
   * cote store, meme discipline que injectDllIntoProcess/installFunctionHook.
   * valueType peut etre vide si le setter n'a pas de parametre ou si l'appelant
   * laisse le type reel resolu par ClrMD piloter l'encodage.
   */
  callClrInstanceMethod?(objectAddressHex: string, methodName: string, valueText: string, valueType: string): Promise<ClrRpcResult<ClrCallInstanceMethodResult>>
  /** Probe le driver noyau optionnel KillEngineKernel.sys (health check uniquement). */
  probeKernelDriver?(): Promise<KernelDriverStatus>
  /** Démarre le service Windows KillEngineKernel s'il est installé mais arrêté, puis relance le probe. */
  startKernelDriver?(): Promise<KernelDriverStatus>
  /** Lit `size` octets sur le processus attaché via le driver noyau (KeStackAttachProcess, hors WriteProcessMemory/ReadProcessMemory usermode). Nécessite capabilities.processMemoryAccess=true. */
  readMemoryKernel?(addressHex: string, size: number): Promise<KernelMemoryReadResult>
  /** Écrit des octets (hex, ex: "90 90 90") sur le processus attaché via le driver noyau. Action à risque équivalente à une injection : passe par confirmRiskAction côté store. */
  writeMemoryKernel?(addressHex: string, hexBytes: string): Promise<KernelMemoryWriteResult>
  /** Comme writeMemoryKernel, mais avec une valeur typée (valueType/value) au lieu d'octets hex bruts — utilisé par l'escalade kernel Expert et l'Assistant (checkpoint kernel_write). Même risque, même confirmRiskAction côté store. */
  writeMemoryValueKernel?(addressHex: string, valueType: string, value: string): Promise<KernelMemoryWriteResult>
  saveSettings(settings: AppSettings): Promise<AppSettings>
  getLogFilePath(): Promise<string>
  getSmartSearchDebugFilePath(): Promise<string>
  getScanTelemetryFilePath?(): Promise<string>
  getSmartSearchDebugEvents(maxEvents: number): Promise<SmartSearchDebugEventsResult>
  clearSmartSearchDebugEvents(): Promise<Record<string, unknown>>
  getLogTail(maxLines: number): Promise<LogTailResult>
  exportDiagnostics(): Promise<Record<string, unknown>>
  getTemporaryStorageStatus(): Promise<TemporaryStorageStatus>
  clearTemporaryStorage(): Promise<Record<string, unknown>>
  getActiveChatMemoryTargets(): Promise<ChatMemoryTargetsResult>
  clearActiveChatMemoryTargets(): Promise<ChatMemoryTargetsResult>
  getSmartSearchContext(): Promise<SmartSearchContextResult>
  acknowledgePendingSmartSearchRecovery?(): Promise<void>

  // Phase 11 — Profils
  saveProfileTarget(
    profileName: string,
    targetName: string,
    addressHex: string,
    valueType: string,
    description: string,
  ): Promise<Record<string, unknown>>
  saveClrFieldProfileTarget?(
    profileName: string,
    targetName: string,
    typeSubstring: string,
    identityField: string,
    identityValue: string,
    targetField: string,
    valueType: string,
    description: string,
  ): Promise<Record<string, unknown>>
  listProfiles(): Promise<Array<Record<string, unknown>>>
  loadProfile(profileName: string): Promise<Record<string, unknown>>
  deleteProfile(profileName: string): Promise<boolean>
  resolveProfileTarget(profileName: string, targetName: string): Promise<Record<string, unknown>>
  /** Roadmap section L — Pointer maps : résout toutes les cibles du profil d'un coup (diagnostic groupé après redémarrage). */
  comparePointerMapAcrossRestart?(profileName: string): Promise<Record<string, unknown>>
  /** Exporte les cibles pointer_chain d'un profil en JSON partageable. */
  exportPointerMap?(profileName: string): Promise<Record<string, unknown>>
  /** Importe/fusionne une pointer map JSON dans un profil existant ou nouveau. */
  importPointerMap?(profileName: string, pointerMapJson: string, options: Record<string, unknown>): Promise<Record<string, unknown>>
  /** Persiste les dépendances Trainer dans la cible de profil native. */
  setProfileTargetDependencies?(profileName: string, targetName: string, dependencyNames: string[]): Promise<Record<string, unknown>>
  /** Exporte les artefacts du profil vers Ghidra : JSON + script Python Ghidra. */
  exportGhidraArtifacts?(profileName: string): Promise<Record<string, unknown>>
  /** Importe des symboles Ghidra JSON/CSV et enrichit les cibles/patchs du profil. */
  importGhidraSymbols?(profileName: string, symbolsText: string): Promise<Record<string, unknown>>
  activateProfileTarget(profileName: string, targetName: string): Promise<Record<string, unknown>>
  saveProfileCodePatch?(
    profileName: string,
    patchName: string,
    addressHex: string,
    aobPattern: string,
    patchBytes: string,
    metadata: Record<string, unknown>,
  ): Promise<Record<string, unknown>>
  applyProfileCodePatch?(profileName: string, patchName: string): Promise<CodePatchResult>
  restoreProfileCodePatch?(profileName: string, patchName: string): Promise<CodePatchResult>
  applyAllProfileCodePatches?(profileName: string): Promise<Record<string, unknown>>
  restoreAllProfileCodePatches?(profileName: string): Promise<Record<string, unknown>>
  inspectProfileCodePatches?(profileName: string): Promise<Record<string, unknown>>
  /** Sauvegarde un script auto-assembleur (texte brut) dans un profil, rejouable sans le retaper. */
  saveProfileAutoAsmScript?(
    profileName: string,
    scriptName: string,
    scriptText: string,
    metadata: Record<string, unknown>,
  ): Promise<Record<string, unknown>>
  applyProfileAutoAsmScript?(profileName: string, scriptName: string): Promise<Record<string, unknown>>
  deleteProfileAutoAsmScript?(profileName: string, scriptName: string): Promise<Record<string, unknown>>
  /** Scripting Lua externe : exécute lua.exe/LuaJIT et laisse le script piloter KillEngine via le pipe d'automatisation. */
  getLuaScriptingStatus?(): Promise<LuaScriptingStatus>
  executeLuaScript?(scriptText: string, options: Record<string, unknown>): Promise<LuaScriptRunResult>
  /** Version non bloquante d'executeLuaScript, annulable via cancelLuaScriptExecution. Le résultat arrive via luaScriptExecutionFinished. */
  executeLuaScriptAsync?(scriptText: string, options: Record<string, unknown>): Promise<LuaScriptRunStartResult>
  cancelLuaScriptExecution?(): Promise<Record<string, unknown>>
  luaScriptExecutionFinished?: QWebChannelSignal<Record<string, unknown>>
  /** Sauvegarde un script Lua (texte brut) dans un profil, rejouable sans le retaper. */
  saveProfileLuaScript?(
    profileName: string,
    scriptName: string,
    scriptText: string,
    metadata: Record<string, unknown>,
  ): Promise<Record<string, unknown>>
  deleteProfileLuaScript?(profileName: string, scriptName: string): Promise<Record<string, unknown>>

  // Phase 14 — Pointer Chains (jeux modernes / applications dynamiques)
  scanPointerChains?(addressHex: string, scanOptions: PointerScanOptions): Promise<PointerScanResult>
  resolvePointerChain?(chain: PointerChainInfo): Promise<PointerChainResolveResult>
  savePointerChainProfileTarget?(
    profileName: string,
    targetName: string,
    chain: PointerChainInfo,
    valueType: string,
    description: string,
  ): Promise<Record<string, unknown>>
  /** Après une écriture confirmée : cherche une chaîne de pointeurs stable vers cette adresse. Lecture seule, bornée, à appeler explicitement (jamais automatiquement après chaque écriture). */
  suggestStableLocatorForAddress?(addressHex: string, options: PointerScanOptions): Promise<StableLocatorSuggestion>
}

class BackendService {
  private controller: BackendController | null = null
  private connected = false
  private connecting = false
  private listeners: (() => void)[] = []

  get isConnected(): boolean {
    return this.connected
  }

  /**
   * Initialise la connexion QWebChannel avec le backend C++.
   * Doit être appelée au démarrage de l'application.
   */
  async connect(): Promise<void> {
    if (this.connected || this.connecting) return
    this.connecting = true

    return new Promise((resolve, reject) => {
      const start = Date.now()

      const connectWhenReady = () => {
        if (typeof window.QWebChannel === 'undefined' || !window.qt?.webChannelTransport) {
          if (Date.now() - start < 2000) {
            window.setTimeout(connectWhenReady, 50)
            return
          }

          this.useMockBackend()
          resolve()
          return
        }

        new window.QWebChannel(window.qt.webChannelTransport, (channel) => {
          const obj = channel.objects['killengine'] as unknown as BackendController
          if (!obj) {
            this.connecting = false
            reject(new Error('Backend controller "killengine" not found'))
            return
          }
          this.controller = obj
          this.connected = true
          this.connecting = false
          this.notifyListeners()
          console.log('[KillEngine] Backend connected via QWebChannel')
          resolve()
        })
      }

      connectWhenReady()
    })
  }

  private useMockBackend(): void {
        // Mode développement sans Qt — on simule un backend mock
        console.warn('[KillEngine] QWebChannel not available. Using mock backend.')
        this.controller = this.createMockBackend()
        this.connected = true
        this.connecting = false
        this.notifyListeners()
  }

  /**
   * Retourne l'instance du contrôleur backend.
   * Lance une erreur si pas connecté.
   */
  getController(): BackendController {
    if (!this.controller) {
      throw new Error('Backend not connected. Call backend.connect() first.')
    }
    return this.controller
  }

  onConnectionChange(listener: () => void): () => void {
    this.listeners.push(listener)
    return () => {
      this.listeners = this.listeners.filter((l) => l !== listener)
    }
  }

  async startExactScanAsync(
    value: string,
    valueType: string,
    expertOptions: ExpertScanOptions,
  ): Promise<ExactScanResult> {
    const controller = this.getController()
    if (!controller.scanFinished) {
      return controller.startExactScanExpert(value, valueType, expertOptions)
    }

    return new Promise((resolve) => {
      let requestId: number | null = null
      let settled = false
      const earlyPayloads: Array<ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult> = []
      const timeout = window.setTimeout(() => {
        settled = true
        controller.scanFinished?.disconnect?.(handler)
        resolve({
          requestId: requestId ?? undefined,
          success: false,
          partial: false,
          cancelled: false,
          regionsScanned: 0,
          bytesScanned: 0,
          matchesFound: 0,
          matchesReturned: 0,
          error: 'Timeout du scan async.',
          matches: [],
          candidateStoreSize: 0,
        })
      }, 10 * 60 * 1000)

      const handler = (payload: ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult) => {
        if (requestId === null) {
          earlyPayloads.push(payload)
          return
        }
        if (Number(payload.requestId) !== requestId) return
        if ('kind' in payload && payload.kind === 'next_scan') return
        settled = true
        window.clearTimeout(timeout)
        controller.scanFinished?.disconnect?.(handler)
        resolve(payload as ExactScanResult)
      }
      controller.scanFinished?.connect(handler)

      void controller.startExactScanAsync(value, valueType, expertOptions).then((start) => {
        if (settled) return
        if (start.success !== true || start.started !== true) {
          settled = true
          window.clearTimeout(timeout)
          controller.scanFinished?.disconnect?.(handler)
          resolve({
            success: false,
            partial: false,
            cancelled: false,
            regionsScanned: 0,
            bytesScanned: 0,
            matchesFound: 0,
            matchesReturned: 0,
            error: String(start.error ?? 'Impossible de démarrer le scan async.'),
            matches: [],
            candidateStoreSize: 0,
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
        controller.scanFinished?.disconnect?.(handler)
        resolve({
          success: false,
          partial: false,
          cancelled: false,
          regionsScanned: 0,
          bytesScanned: 0,
          matchesFound: 0,
          matchesReturned: 0,
          error: String(error),
          matches: [],
          candidateStoreSize: 0,
        })
      })
    })
  }

  async startNextScanAsync(mode: string, value: string): Promise<NextScanResult> {
    const controller = this.getController()
    if (!controller.scanFinished || !controller.nextScanAsync) {
      return controller.nextScan(mode, value)
    }

    return new Promise((resolve) => {
      let requestId: number | null = null
      let settled = false
      const earlyPayloads: Array<ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult> = []
      const timeout = window.setTimeout(() => {
        settled = true
        controller.scanFinished?.disconnect?.(handler)
        resolve({
          requestId: requestId ?? undefined,
          kind: 'next_scan',
          success: false,
          cancelled: false,
          checked: 0,
          unreadable: 0,
          remaining: 0,
          error: 'Timeout du next scan async.',
        })
      }, 10 * 60 * 1000)

      const handler = (payload: ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult) => {
        if (requestId === null) {
          earlyPayloads.push(payload)
          return
        }
        if (Number(payload.requestId) !== requestId) return
        if ('kind' in payload && payload.kind && payload.kind !== 'next_scan') return
        settled = true
        window.clearTimeout(timeout)
        controller.scanFinished?.disconnect?.(handler)
        resolve(payload as NextScanResult)
      }
      controller.scanFinished?.connect(handler)

      void controller.nextScanAsync(mode, value).then((start) => {
        if (settled) return
        if (start.success !== true || start.started !== true) {
          settled = true
          window.clearTimeout(timeout)
          controller.scanFinished?.disconnect?.(handler)
          resolve({
            success: false,
            cancelled: false,
            checked: 0,
            unreadable: 0,
            remaining: 0,
            error: String(start.error ?? 'Impossible de démarrer le next scan async.'),
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
        controller.scanFinished?.disconnect?.(handler)
        resolve({
          success: false,
          cancelled: false,
          checked: 0,
          unreadable: 0,
          remaining: 0,
          error: String(error),
        })
      })
    })
  }

  async cancelActiveScan(): Promise<Record<string, unknown>> {
    return this.getController().cancelActiveScan()
  }

  async captureUnknownSnapshotAsync(expertOptions: ExpertScanOptions = {}): Promise<UnknownSnapshotResult> {
    const controller = this.getController()
    if (!controller.scanFinished || !controller.captureUnknownSnapshotAsync) {
      return controller.captureUnknownSnapshotWithOptions
        ? controller.captureUnknownSnapshotWithOptions(expertOptions)
        : controller.captureUnknownSnapshot()
    }

    const start = controller.captureUnknownSnapshotAsyncWithOptions
      ? await controller.captureUnknownSnapshotAsyncWithOptions(expertOptions)
      : await controller.captureUnknownSnapshotAsync()
    if (start.success !== true || start.started !== true) {
      return {
        success: false,
        partial: false,
        cancelled: false,
        regionsCaptured: 0,
        regionsSkipped: 0,
        bytesCaptured: 0,
        error: String(start.error ?? 'Impossible de démarrer la capture unknown async.'),
      }
    }

    const requestId = Number(start.requestId)
    return new Promise((resolve) => {
      const timeout = window.setTimeout(() => {
        resolve({
          requestId,
          kind: 'unknown_capture',
          success: false,
          partial: false,
          cancelled: false,
          regionsCaptured: 0,
          regionsSkipped: 0,
          bytesCaptured: 0,
          error: 'Timeout de la capture unknown async.',
        })
      }, 10 * 60 * 1000)

      const handler = (payload: ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult) => {
        if (Number(payload.requestId) !== requestId) return
        if ('kind' in payload && payload.kind && payload.kind !== 'unknown_capture') return
        window.clearTimeout(timeout)
        controller.scanFinished?.disconnect?.(handler)
        resolve(payload as UnknownSnapshotResult)
      }
      controller.scanFinished?.connect(handler)
    })
  }

  async unknownNextScanAsync(mode: string, valueType: string): Promise<UnknownNextScanResult> {
    const controller = this.getController()
    if (!controller.scanFinished || !controller.unknownNextScanAsync) {
      return controller.unknownNextScan(mode, valueType)
    }

    const start = await controller.unknownNextScanAsync(mode, valueType)
    if (start.success === true && start.started !== true) {
      const direct = start as UnknownNextScanResult & NextScanResult & Record<string, unknown>
      return {
        requestId: Number(direct.requestId ?? 0),
        kind: 'unknown_refine',
        success: direct.success,
        partial: Boolean(direct.partial ?? false),
        cancelled: Boolean(direct.cancelled ?? false),
        checkedBytes: Number(direct.checkedBytes ?? direct.checked ?? 0),
        matchesFound: Number(direct.matchesFound ?? direct.remaining ?? 0),
        stored: Number(direct.stored ?? direct.remaining ?? 0),
        error: String(direct.error ?? ''),
        diagnostic: direct.diagnostic ? String(direct.diagnostic) : undefined,
        refinedFromCandidates: true,
      }
    }
    if (start.success !== true || start.started !== true) {
      return {
        success: false,
        partial: false,
        cancelled: false,
        checkedBytes: 0,
        matchesFound: 0,
        stored: 0,
        error: String(start.error ?? 'Impossible de démarrer la comparaison unknown async.'),
      }
    }

    const requestId = Number(start.requestId)
    return new Promise((resolve) => {
      const timeout = window.setTimeout(() => {
        resolve({
          requestId,
          kind: 'unknown_next',
          success: false,
          partial: false,
          cancelled: false,
          checkedBytes: 0,
          matchesFound: 0,
          stored: 0,
          error: 'Timeout de la comparaison unknown async.',
        })
      }, 10 * 60 * 1000)

      const handler = (payload: ExactScanResult | NextScanResult | UnknownSnapshotResult | UnknownNextScanResult) => {
        if (Number(payload.requestId) !== requestId) return
        if ('kind' in payload && payload.kind && payload.kind !== 'unknown_next' && payload.kind !== 'next_scan') return
        window.clearTimeout(timeout)
        controller.scanFinished?.disconnect?.(handler)
        if ('kind' in payload && payload.kind === 'next_scan') {
          const nextPayload = payload as NextScanResult
          resolve({
            requestId,
            kind: 'unknown_refine',
            success: nextPayload.success,
            partial: false,
            cancelled: nextPayload.cancelled,
            checkedBytes: nextPayload.checked,
            matchesFound: nextPayload.remaining,
            stored: nextPayload.remaining,
            error: nextPayload.error,
            diagnostic: nextPayload.diagnostic,
            refinedFromCandidates: true,
          })
          return
        }
        resolve(payload as UnknownNextScanResult)
      }
      controller.scanFinished?.connect(handler)
    })
  }

  private notifyListeners(): void {
    this.listeners.forEach((l) => l())
  }

  // -------------------------------------------------------------------------
  // Mock backend pour le développement navigateur
  // -------------------------------------------------------------------------
  private createMockBackend(): BackendController {
    return {
      async getVersion() {
        return '0.1.0 (mock)'
      },
      async getProcesses() {
        return []
      },
      async getProcessModules(_pid: number) {
        return []
      },
      async discoverProcessSaveFiles(_maxResults: number) {
        return { success: false, files: [], error: 'Mock backend' }
      },
      async inspectProcessLocalSettings(_maxValues: number) {
        return { success: false, values: [], error: 'Mock backend' }
      },
      async readProcessSaveFileText(_path: string, _maxBytes: number) {
        return { success: false, path: _path, text: '', truncated: false, error: 'Mock backend' }
      },
      async startSaveFileWatchAsync(_path: string, _options: Record<string, unknown>) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async cancelSaveFileWatch() {
        return { success: false, error: 'Mock backend' }
      },
      async patchProcessSaveFileBytes(_path: string, _findHex: string, _replaceHex: string) {
        return { success: false, path: _path, occurrencesFound: 0, error: 'Mock backend' }
      },
      async attachProcess(_pid: number) {
        return true
      },
      async detachProcess() {},
      async getMemoryMap() {
        return {
          attached: false,
          stats: {
            regionCount: 0,
            committedCount: 0,
            readableCount: 0,
            writableCount: 0,
            executableCount: 0,
            totalBytes: 0,
            committedBytes: 0,
            readableBytes: 0,
            writableBytes: 0,
            executableBytes: 0,
          },
          regions: [],
        }
      },
      async readMemoryPreview(_addressHex: string, _size: number) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          bytesRead: 0,
          requestedBytes: 0,
          error: 'Mock backend',
          hex: '',
        }
      },
      async analyzeStructureMemory(_addressHex: string, _size: number) {
        return {
          success: false,
          error: 'Mock backend',
          fields: [],
        }
      },
      async startExactScan(_value: string, _valueType: string) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          regionsScanned: 0,
          bytesScanned: 0,
          matchesFound: 0,
          matchesReturned: 0,
          error: 'Mock backend',
          matches: [],
          candidateStoreSize: 0,
        }
      },
      async startExactScanExpert(_value: string, _valueType: string, _expertOptions: ExpertScanOptions) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          regionsScanned: 0,
          bytesScanned: 0,
          matchesFound: 0,
          matchesReturned: 0,
          error: 'Mock backend',
          matches: [],
          candidateStoreSize: 0,
        }
      },
      async scanEncryptedValue(_value: string, _valueType: string, _options: Record<string, unknown>) {
        return {
          success: false,
          partial: false,
          regionsScanned: 0,
          bytesScanned: 0,
          matchesFound: 0,
          matchesReturned: 0,
          error: 'Mock backend',
          matches: [],
        }
      },
      async startExactScanAsync(_value: string, _valueType: string, _expertOptions: ExpertScanOptions) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async getCandidates(pageIndex: number, pageSize: number, _addressFilter: string) {
        return {
          pageIndex,
          pageSize,
          totalCount: 0,
          displaySuppressed: false,
          displayLimit: 250000,
          fileBacked: false,
          candidateStorePath: '',
          candidates: [],
        }
      },
      async nextScan(_mode: string, _value: string) {
        return {
          success: false,
          checked: 0,
          unreadable: 0,
          remaining: 0,
          error: 'Mock backend',
        }
      },
      async nextScanAsync(_mode: string, _value: string) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async undoCandidateScan() {
        return { success: false, restored: false, count: 0, error: 'Mock backend' }
      },
      async clearScanContext() {
        return { success: true, clearedCandidates: 0, message: 'Contexte de scan vidé.' }
      },
      async cancelActiveScan() {
        return { success: false, error: 'Mock backend' }
      },
      async captureUnknownSnapshot() {
        return {
          success: false,
          partial: false,
          cancelled: false,
          regionsCaptured: 0,
          regionsSkipped: 0,
          bytesCaptured: 0,
          error: 'Mock backend',
        }
      },
      async captureUnknownSnapshotWithOptions(_expertOptions: ExpertScanOptions) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          regionsCaptured: 0,
          regionsSkipped: 0,
          bytesCaptured: 0,
          error: 'Mock backend',
        }
      },
      async captureUnknownSnapshotAsync() {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async captureUnknownSnapshotAsyncWithOptions(_expertOptions: ExpertScanOptions) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async unknownNextScan(_mode: string, _valueType: string) {
        return {
          success: false,
          partial: false,
          cancelled: false,
          checkedBytes: 0,
          matchesFound: 0,
          stored: 0,
          error: 'Mock backend',
        }
      },
      async unknownNextScanAsync(_mode: string, _valueType: string) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async writeMemoryValue(_addressHex: string, _valueType: string, _value: string) {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend' }
      },
      async writeMemoryValuesWithVariants(_targets: MemoryWriteTarget[], _value: string) {
        return { success: false, verified: false, bytesWritten: 0, written: 0, total: 0, results: [], error: 'Mock backend' }
      },
      async startUiStringInvestigation() {
        return { success: false, windows: 0, bytesCaptured: 0, unreadable: 0, radiusBytes: 0, error: 'Mock backend' }
      },
      async finishUiStringInvestigation() {
        return {
          success: false,
          windowsChecked: 0,
          unreadable: 0,
          changedBytes: 0,
          changesFound: 0,
          changes: [],
          error: 'Mock backend',
        }
      },
      async startChangedPagesDiff(_options: Record<string, unknown>) {
        return { success: false, blocksCaptured: 0, bytesCaptured: 0, error: 'Mock backend' }
      },
      async finishChangedPagesDiff(_previousValue: string, _currentValue: string, _options: Record<string, unknown>) {
        return { success: false, hits: [], hitsFound: 0, error: 'Mock backend' }
      },
      async rollbackLastWrite() {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend' }
      },
      async rollbackLastWriteBatch() {
        return { success: false, rolledBack: 0, total: 0, error: 'Mock backend' }
      },
      async setFreezeValue(_addressHex: string, _valueType: string, _value: string, enabled: boolean) {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend', enabled }
      },
      async freezeWithBreakpoint(_addressHex: string, _valueType: string, _value: string, _options: Record<string, unknown>) {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend', enabled: false }
      },
      async escalatePollingFreezeToBreakpoint(_addressHex: string) {
        return { success: false, enabled: false, mode: 'breakpoint', error: 'Mock backend' }
      },
      async getBreakpointFreezeStats() {
        return { active: false, mode: 'breakpoint', hits: 0, rewrites: 0, blocks: 0, errors: 0, healthy: true }
      },
      async suggestStableLocatorForAddress(_addressHex: string, _options: PointerScanOptions) {
        return { success: false, chainCount: 0, error: 'Mock backend' }
      },
      async stopBreakpointFreeze() {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend', enabled: false }
      },
      async findWhatWrites(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, hitCount: 0, hits: [], error: 'Mock backend' }
      },
      async findWhatWritesAsync(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async cancelFindWhatWrites() {
        return { success: false, error: 'Mock backend' }
      },
      async findWhatExecutes(_instructionAddressHex: string, _options: Record<string, unknown>) {
        return { success: false, hitCount: 0, hits: [], error: 'Mock backend' }
      },
      async analyzeFieldStability(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, verdict: 'no_writes_observed', writeCount: 0, rationale: '', error: 'Mock backend' }
      },
      async startInProcessExecuteWatchAsync(_instructionAddressHex: string, _options: Record<string, unknown>) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async readAttachedWindowText(_options: Record<string, unknown>) {
        return { success: false, windows: [], windowCount: 0, error: 'Mock backend' }
      },
      async scanAobPattern(_pattern: string, _options: Record<string, unknown>) {
        return { success: false, matches: [], matchesFound: 0, regionsScanned: 0, bytesScanned: 0, error: 'Mock backend' }
      },
      async generateAobSignature(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, pattern: '', error: 'Mock backend' }
      },
      async applyCodePatch(_addressHex: string, _bytesText: string, _options: Record<string, unknown>) {
        return { success: false, error: 'Mock backend' }
      },
      async suggestCodePatches(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, suggestions: [], error: 'Mock backend' }
      },
      async restoreCodePatch(_addressHex: string) {
        return { success: false, error: 'Mock backend' }
      },
      async disassembleBackward(_addressHex: string, _options: Record<string, unknown>) {
        return { success: false, instructions: [], candidateFields: [], error: 'Mock backend' }
      },
      async testCandidateFieldsAsync(_writeInstructionAddressHex: string, _knownWriteTargetAddressHex: string, _options: Record<string, unknown>) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async cancelCandidateFieldTest() {
        return { success: false, error: 'Mock backend' }
      },
      async startSpeedhack(_factor: number) {
        return { success: false, active: false, factor: 1.0, error: 'Mock backend' }
      },
      async setSpeedhackFactor(_factor: number) {
        return { success: false, active: false, factor: 1.0, error: 'Mock backend' }
      },
      async stopSpeedhack() {
        return { success: true, active: false, factor: 1.0 }
      },
      async getSpeedhackStatus() {
        return { success: true, active: false, factor: 1.0 }
      },
      async blockProcessNetwork() {
        return { success: false, blocked: false, error: 'Mock backend' }
      },
      async unblockProcessNetwork() {
        return { success: false, blocked: false, error: 'Mock backend' }
      },
      async getProcessNetworkBlockStatus() {
        return { success: true, blocked: false }
      },
      async startApiHook(_m: string, _fn: string, _mode: number, _ret: number) {
        return { success: false, active: false, error: 'Mock backend' }
      },
      async stopApiHook() {
        return { success: true, active: false }
      },
      async getApiHookStatus() {
        return { success: true, active: false }
      },
      async injectDllIntoProcess(_dllPath: string) {
        return { success: false, error: 'Mock backend' }
      },
      async installFunctionHook(_targetAddressHex: string, _hookAddressHex: string) {
        return { success: false, error: 'Mock backend' }
      },
      async removeFunctionHook(_targetAddressHex: string) {
        return { success: false, error: 'Mock backend' }
      },
      async resolveSymbolAddress(_moduleName: string, _functionName: string) {
        return { success: false, error: 'Mock backend' }
      },
      async parseAutoAssemblerScript(_scriptText: string) {
        return { success: false, parseSuccess: false, parseError: 'Mock backend', instructions: [], allocations: [], labels: [] }
      },
      async executeAutoAssemblerScript(_scriptText: string) {
        return { success: false, error: 'Mock backend' }
      },
      async restoreAutoAssemblerScript() {
        return { success: false, error: 'Mock backend' }
      },
      async forceWriteInstructionValue(
        _ripHex: string,
        _instructionLength: number,
        _memBaseRegister: string,
        _memDisplacement: number,
        _valueType: string,
        _value: string,
      ) {
        return { success: false, error: 'Mock backend' }
      },
      async setFreezeInterval(_intervalMs: number) {
        return { success: false, error: 'Mock backend' }
      },
      async registerGlobalHotkey(combo: string, action: Record<string, unknown>) {
        return { success: true, id: Math.floor(Math.random() * 100000), combo, ...action }
      },
      async unregisterGlobalHotkey(id: number) {
        return { success: true, id }
      },
      async getGlobalHotkeys() {
        return { success: true, hotkeys: [] }
      },
      async clearGlobalHotkeys() {
        return { success: true }
      },
      async setTrainerOverlayVisible(visible: boolean, _options: Record<string, unknown>) {
        return { success: true, visible }
      },
      async updateTrainerOverlay(state: Record<string, unknown>) {
        return { success: true, state }
      },
      async startSmartSearch(query: string) {
        return {
          status: 'mock',
          message: 'Mock backend — Smart Search non disponible',
          query,
        }
      },
      async startAutoResolve(query: string, _options: Record<string, unknown>) {
        return {
          status: 'mock',
          message: 'Mock backend — Auto-résolution non disponible',
          query,
        }
      },
      async getAutoResolveReport(_maxEvents: number) {
        return {
          success: true,
          attached: false,
          processName: 'mock',
          candidateCount: 0,
          workflow: 'idle',
          initialValue: '',
          targetValue: '',
          valueType: '',
          activeChatTargetCount: 0,
          activeProfileTargetCount: 0,
          eventCounts: {},
          recentSignals: [],
          recommendations: [],
          guardrails: [],
          nextBestAction: {
            id: 'attach_process',
            label: 'Attacher un processus',
            safe: true,
            confidence: 100,
            reason: 'Mock backend sans processus attaché.',
          },
          telemetryInsights: [],
          displayValueReport: {},
          strategyScores: [],
          preferredStrategy: {},
          summary: 'Mock backend — aucun rapport auto.',
        }
      },
      async clearAutoResolveMemory(_allProcesses: boolean) {
        return { success: true, message: 'Mock backend — mémoire Auto vidée.' }
      },
      async logAiAudit(event: string, payload: Record<string, unknown>) {
        return { success: true, event, payload }
      },
      async ping(message: string) {
        return `pong (mock): ${message}`
      },
      async getSettings() {
        return {
          language: 'fr',
          defaultValueType: 'Int32',
          scanMaxResults: 1000000,
          scanChunkSizeMb: 0,
          performanceMode: 'Auto',
          scanMaxWorkerThreads: 0,
          scanMaxInFlightMb: 0,
          candidateFileBackedThreshold: 250000,
          unknownSnapshotMaxMb: 128,
          fastScan: true,
          smartSearchDebugEnabled: true,
          smartSearchDebugMaxEvents: 30,
          modelPath: '',
          modelEnabled: true,
          modelThreads: 4,
        }
      },
      async saveSettings(settings: AppSettings) {
        return { ...settings, success: true }
      },
      async getAiModelStatus() {
        return {
          success: true,
          ready: false,
          available: false,
          enabled: true,
          backend: 'deterministic',
          configuredModelPath: '',
          envModelPath: '',
          envExecutablePath: '',
          modelFound: false,
          modelPath: '',
          modelSource: '',
          modelError: 'Mock backend.',
          executableFound: false,
          executablePath: '',
          modelCandidates: [],
          executableCandidates: [],
          embeddedAgents: [
            { id: 'assistant', displayName: 'Assistant IA', role: 'assistant', modelFound: false, valid: true, modelPath: '' },
            { id: 'auto_resolver', displayName: 'Auto Resolver IA', role: 'resolver', modelFound: false, valid: true, modelPath: '' },
          ],
          embeddedAgentCount: 2,
          embeddedModelFolders: [],
          threads: 4,
          message: 'IA embarquée indisponible dans le mock.',
        }
      },
      async browseForModelFile() {
        return { success: false, cancelled: true }
      },
      async hasSeenOnboarding() {
        return true
      },
      async setOnboardingSeen(_seen: boolean) {},
      async openUserGuide() {
        return false
      },
      async requestWindowsDefenderExclusion() {
        return { success: false, cancelled: true, error: 'Indisponible dans le mock.' }
      },
      async getClrInspectorStatus() {
        return {
          success: true,
          available: false,
          running: false,
          attachedProcess: false,
          helperPath: '',
          pipeName: 'KillEngineClrInspectorPipe_mock',
        }
      },
      async attachClrInspector() {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async detachClrInspector() {
        return { success: true, result: 'mock detach' }
      },
      async shutdownClrInspector() {
        return { success: true, result: 'mock shutdown' }
      },
      async flushClrInspectorCache() {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async findClrObjectsByType(_typeSubstring: string) {
        return { success: false, error: 'Indisponible dans le mock.', result: [] }
      },
      async findClrObjectsByFieldValue(_typeSubstring: string, _fieldName: string, _expectedValue: string, _maxResults: number) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async readClrObject(_addressHex: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitiveField(_objectAddressHex: string, _fieldName: string, _value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitivePath(_objectAddressHex: string, _path: string, _value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitivePathBatch(_objectAddressHex: string, _operations: ClrPathWriteOperation[]) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitivePathByLocator(_typeSubstring: string, _identityField: string, _identityValue: string, _path: string, _value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitivePathBatchByLocator(_typeSubstring: string, _identityField: string, _identityValue: string, _operations: ClrPathWriteOperation[]) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeClrPrimitivePathBatchAtomic(_objectAddressHex: string, _operations: ClrPathWriteOperation[]) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async enumerateClrRoots(_typeSubstring: string) {
        return { success: false, error: 'Indisponible dans le mock.', result: [] }
      },
      async findClrGcRootPath(_targetObjectAddressHex: string, _maxDepth: number, _maxRootsScanned: number) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async disassembleClrMethod(_objectAddressHex: string, _methodName: string, _instructionCount: number) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async generateClrObjectReport(_objectAddressHex: string, _maxDepth: number, _maxNodes: number, _includeGcRootChain: boolean) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async callClrInstanceMethod(_objectAddressHex: string, _methodName: string, _valueText: string, _valueType: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async probeKernelDriver() {
        return {
            success: true, // Indique que le probe a réussi
            status: 'connected', // Indique que le driver est connecté
            devicePath: '\\\\.\\KillEngineKernel', // Chemin du device
            message: 'Driver connecté en mode probe uniquement.', // Message de succès
            capabilities: {
                protocolVersion: 1, // Définis la version du protocole que le driver supporte
                healthProbe: true, // Permet au driver de répondre aux requêtes de santé
                processMemoryAccess: true, // Permet au driver de lire et écrire dans la mémoire des processus
                privilegedInstrumentation: true, // Permet au driver d'utiliser des fonctionnalités d'instrumentation privilégiées
            },
        };
    },
      async startKernelDriver() {
        return {
          success: true,
          status: 'connected',
          devicePath: '\\\\.\\KillEngineKernel',
          message: 'Driver connecté en mode probe uniquement.',
          serviceName: 'KillEngineKernel',
          started: true,
          alreadyRunning: false,
          capabilities: {
            protocolVersion: 1,
            healthProbe: true,
            processMemoryAccess: true,
            privilegedInstrumentation: true,
          },
        }
      },
      async readMemoryKernel(_addressHex: string, _size: number) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeMemoryKernel(_addressHex: string, _hexBytes: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async writeMemoryValueKernel(_addressHex: string, _valueType: string, _value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async confirmChatMemoryWrite(_value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async confirmChatMemoryFreeze(_value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async confirmRewriteLastAutoWrite(_value: string) {
        return { success: false, error: 'Indisponible dans le mock.' }
      },
      async getLogFilePath() {
        return 'mock://no-log-file'
      },
      async getSmartSearchDebugFilePath() {
        return 'mock://no-smart-search-debug'
      },
      async getScanTelemetryFilePath() {
        return 'mock://no-scan-telemetry'
      },
      async getSmartSearchDebugEvents() {
        return { success: true, path: 'mock://no-smart-search-debug', events: [], error: '' }
      },
      async clearSmartSearchDebugEvents() {
        return { success: true, error: '' }
      },
      async getLogTail() {
        return { success: true, path: 'mock://no-log-file', lines: [], error: '' }
      },
      async exportDiagnostics() {
        return { success: false, path: '', error: 'Mock backend' }
      },
      async getTemporaryStorageStatus() {
        return {
          success: true,
          tempPath: 'mock://temp',
          activeBytes: 0,
          activeFileCount: 0,
          candidateBytes: 0,
          candidateFileBacked: false,
          undoBytes: 0,
          undoFileBacked: false,
          snapshotBytes: 0,
          snapshotFileBacked: false,
          orphanBytes: 0,
          orphanFileCount: 0,
          totalBytes: 0,
          orphanFiles: [],
        }
      },
      async clearTemporaryStorage() {
        return { success: true, removedBytes: 0, removedFileCount: 0, message: 'Stockage temporaire nettoyé.' }
      },
      async getActiveChatMemoryTargets() {
        return { success: true, count: 0, targets: [] }
      },
      async clearActiveChatMemoryTargets() {
        return { success: true, count: 0, cleared: 0, targets: [] }
      },
      async getSmartSearchContext() {
        return {
          success: true,
          active: false,
          workflow: 'idle',
          initialValue: '',
          targetValue: '',
          valueType: 'Int32',
          candidateCount: 0,
          hasUndoReduction: false,
          chatTargets: [],
          profileTargets: [],
          lastAutoWriteCount: 0,
        }
      },
      async acknowledgePendingSmartSearchRecovery() {},
      async saveProfileTarget() {
        return { success: false, error: 'Mock backend' }
      },
      async saveClrFieldProfileTarget() {
        return { success: false, error: 'Mock backend' }
      },
      async listProfiles() {
        return []
      },
      async loadProfile() {
        return { success: false, error: 'Mock backend' }
      },
      async deleteProfile() {
        return false
      },
      async resolveProfileTarget() {
        return { success: false, error: 'Mock backend' }
      },
      async comparePointerMapAcrossRestart(_profileName: string) {
        return { success: false, error: 'Mock backend' }
      },
      async exportPointerMap(_profileName: string) {
        return { success: false, error: 'Mock backend' }
      },
      async importPointerMap(_profileName: string, _pointerMapJson: string, _options: Record<string, unknown>) {
        return { success: false, error: 'Mock backend' }
      },
      async setProfileTargetDependencies(_profileName: string, _targetName: string, _dependencyNames: string[]) {
        return { success: false, error: 'Mock backend' }
      },
      async exportGhidraArtifacts(_profileName: string) {
        return { success: false, error: 'Mock backend' }
      },
      async importGhidraSymbols(_profileName: string, _symbolsText: string) {
        return { success: false, error: 'Mock backend' }
      },
      async activateProfileTarget() {
        return { success: false, error: 'Mock backend' }
      },
      async saveProfileCodePatch() {
        return { success: false, error: 'Mock backend' }
      },
      async applyProfileCodePatch() {
        return { success: false, error: 'Mock backend' }
      },
      async restoreProfileCodePatch() {
        return { success: false, error: 'Mock backend' }
      },
      async applyAllProfileCodePatches() {
        return { success: false, results: [], error: 'Mock backend' }
      },
      async restoreAllProfileCodePatches() {
        return { success: false, results: [], error: 'Mock backend' }
      },
      async inspectProfileCodePatches() {
        return { success: false, states: [], error: 'Mock backend' }
      },
      async getLuaScriptingStatus() {
        return {
          success: true,
          available: false,
          helperAvailable: false,
          pipeName: 'KillEngineAutomationPipe',
          automationPipeOptIn: false,
          message: 'Mock backend — Lua externe non détecté.',
        }
      },
      async executeLuaScript(_scriptText: string, _options: Record<string, unknown>) {
        return {
          success: false,
          exitCode: -1,
          stdout: '',
          stderr: '',
          error: 'Mock backend',
        }
      },
      async executeLuaScriptAsync(_scriptText: string, _options: Record<string, unknown>) {
        return { success: false, started: false, error: 'Mock backend' }
      },
      async cancelLuaScriptExecution() {
        return { success: false, error: 'Mock backend' }
      },
      async saveProfileLuaScript() {
        return { success: false, error: 'Mock backend' }
      },
      async deleteProfileLuaScript() {
        return { success: false, error: 'Mock backend' }
      },
    }
  }
}

export const backend = new BackendService()
