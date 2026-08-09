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

export interface MemoryReadPreview {
  success: boolean
  partial: boolean
  cancelled: boolean
  bytesRead: number
  requestedBytes: number
  error: string
  hex: string
}

export interface ExactScanMatch {
  address: string
  type: string
}

export interface ExactScanResult {
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
}

export interface CandidatePage {
  pageIndex: number
  pageSize: number
  totalCount: number
  candidates: ExactScanMatch[]
}

export interface NextScanResult {
  success: boolean
  checked: number
  unreadable: number
  remaining: number
  error: string
}

export interface UnknownSnapshotResult {
  success: boolean
  partial: boolean
  cancelled: boolean
  regionsCaptured: number
  regionsSkipped: number
  bytesCaptured: number
  error: string
}

export interface UnknownNextScanResult {
  success: boolean
  partial: boolean
  checkedBytes: number
  matchesFound: number
  stored: number
  error: string
}

export interface MemoryWriteResult {
  success: boolean
  verified: boolean
  bytesWritten: number
  error: string
  enabled?: boolean
}

export interface ExpertScanOptions {
  startAddress?: string
  stopAddress?: string
  alignment?: number
  writableOnly?: boolean
  executableOnly?: boolean
  copyOnWriteOnly?: boolean
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

export interface ChatMemoryTargetsResult {
  success: boolean
  count: number
  targets: Array<Record<string, unknown>>
  cleared?: number
}

export interface BackendController {
  getVersion(): Promise<string>
  getProcesses(): Promise<ProcessInfo[]>
  getProcessModules(pid: number): Promise<ProcessModuleInfo[]>
  attachProcess(pid: number): Promise<boolean>
  detachProcess(): Promise<void>
  getMemoryMap(): Promise<MemoryMapResult>
  readMemoryPreview(addressHex: string, size: number): Promise<MemoryReadPreview>
  startExactScan(value: string, valueType: string): Promise<ExactScanResult>
  startExactScanExpert(
    value: string,
    valueType: string,
    expertOptions: ExpertScanOptions,
  ): Promise<ExactScanResult>
  nextScan(mode: string, value: string): Promise<NextScanResult>
  getCandidates(pageIndex: number, pageSize: number, addressFilter: string): Promise<CandidatePage>
  captureUnknownSnapshot(): Promise<UnknownSnapshotResult>
  unknownNextScan(mode: string, valueType: string): Promise<UnknownNextScanResult>
  writeMemoryValue(addressHex: string, valueType: string, value: string): Promise<MemoryWriteResult>
  rollbackLastWrite(): Promise<MemoryWriteResult>
  rollbackLastWriteBatch(): Promise<Record<string, unknown>>
  setFreezeValue(addressHex: string, valueType: string, value: string, enabled: boolean): Promise<MemoryWriteResult>
  startSmartSearch(query: string): Promise<SmartSearchResult>
  ping(message: string): Promise<string>
  getLogFilePath(): Promise<string>
  getSmartSearchDebugFilePath(): Promise<string>
  getSmartSearchDebugEvents(maxEvents: number): Promise<SmartSearchDebugEventsResult>
  clearSmartSearchDebugEvents(): Promise<Record<string, unknown>>
  getActiveChatMemoryTargets(): Promise<ChatMemoryTargetsResult>
  clearActiveChatMemoryTargets(): Promise<ChatMemoryTargetsResult>

  // Phase 11 — Profils
  saveProfileTarget(
    profileName: string,
    targetName: string,
    addressHex: string,
    valueType: string,
    description: string,
  ): Promise<Record<string, unknown>>
  listProfiles(): Promise<Array<Record<string, unknown>>>
  loadProfile(profileName: string): Promise<Record<string, unknown>>
  deleteProfile(profileName: string): Promise<boolean>
  resolveProfileTarget(profileName: string, targetName: string): Promise<Record<string, unknown>>
  activateProfileTarget(profileName: string, targetName: string): Promise<Record<string, unknown>>
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
      async getCandidates(pageIndex: number, pageSize: number, _addressFilter: string) {
        return {
          pageIndex,
          pageSize,
          totalCount: 0,
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
      async unknownNextScan(_mode: string, _valueType: string) {
        return {
          success: false,
          partial: false,
          checkedBytes: 0,
          matchesFound: 0,
          stored: 0,
          error: 'Mock backend',
        }
      },
      async writeMemoryValue(_addressHex: string, _valueType: string, _value: string) {
        return { success: false, verified: false, bytesWritten: 0, error: 'Mock backend' }
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
      async startSmartSearch(query: string) {
        return {
          status: 'mock',
          message: 'Mock backend — Smart Search non disponible',
          query,
        }
      },
      async ping(message: string) {
        return `pong (mock): ${message}`
      },
      async getLogFilePath() {
        return 'mock://no-log-file'
      },
      async getSmartSearchDebugFilePath() {
        return 'mock://no-smart-search-debug'
      },
      async getSmartSearchDebugEvents() {
        return { success: true, path: 'mock://no-smart-search-debug', events: [], error: '' }
      },
      async clearSmartSearchDebugEvents() {
        return { success: true, error: '' }
      },
      async getActiveChatMemoryTargets() {
        return { success: true, count: 0, targets: [] }
      },
      async clearActiveChatMemoryTargets() {
        return { success: true, count: 0, cleared: 0, targets: [] }
      },
      async saveProfileTarget() {
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
      async activateProfileTarget() {
        return { success: false, error: 'Mock backend' }
      },
    }
  }
}

export const backend = new BackendService()
