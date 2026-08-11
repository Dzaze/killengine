<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend, type PointerChainInfo, type PointerChainResolveResult, type PointerScanResult } from '@/services/backend'

const store = useAppStore()
const selectedCandidateAddresses = ref<string[]>([])

// Phase 14 — Pointer Chains
const pointerScanAddress = ref('')
const pointerScanValueType = ref('Int32')
const pointerScanMaxDepth = ref(3)
const pointerScanMaxOffset = ref(0x1000)
const pointerScanResult = ref<PointerScanResult | null>(null)
const pointerScanBusy = ref(false)
const pointerResolveResult = ref<PointerChainResolveResult | null>(null)
const selectedPointerChainIndex = ref<number>(-1)

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
  const profileName = window.prompt('Nom du profil :', 'StarCraft2')
  if (!profileName) return
  const targetName = window.prompt('Nom de la cible :', 'Minerals')
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

const candidatePageTotal = computed(() => {
  if (!store.candidatePage) return 1
  if (store.candidatePage.displaySuppressed) return 1
  return Math.max(1, Math.ceil(store.candidatePage.totalCount / store.candidatePage.pageSize))
})
const currentPageCandidates = computed(() => store.candidatePage?.candidates ?? [])
const displayedCandidates = computed(() => currentPageCandidates.value.filter(
  (candidate) => !store.ignoredCandidateAddresses.includes(candidate.address),
))
const expertDense = computed(() => store.uiMode === 'expert')
const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
const exactScanButtonLabel = computed(() => hasCandidateContext.value ? 'Nouveau scan' : 'Premier scan')
const unknownGuideReady = computed(() => Boolean(store.unknownSnapshotResult?.success) || hasCandidateContext.value)
const unknownGuideActions = [
  { mode: 'increased', label: 'ça augmente' },
  { mode: 'decreased', label: 'ça diminue' },
  { mode: 'unchanged', label: 'stable' },
  { mode: 'changed', label: 'ça change' },
] as const

function formatNumber(value: number | undefined) {
  return new Intl.NumberFormat('fr-FR').format(value ?? 0)
}

function formatRate(value: number | undefined) {
  return `${formatNumber(Math.round(value ?? 0))}/s`
}

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} Go`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} Mo`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} Ko`
  return `${formatNumber(bytes)} o`
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
    return
  }
  selectedCandidateAddresses.value = [...selectedCandidateAddresses.value, address]
}

function toggleCurrentPageSelection() {
  const pageAddresses = currentPageCandidates.value.map((candidate) => candidate.address)
  const allPageSelected = pageAddresses.length > 0
    && pageAddresses.every((address) => selectedCandidateAddresses.value.includes(address))
  if (allPageSelected) {
    selectedCandidateAddresses.value = selectedCandidateAddresses.value.filter((address) => !pageAddresses.includes(address))
    return
  }
  selectedCandidateAddresses.value = Array.from(new Set([...selectedCandidateAddresses.value, ...pageAddresses]))
}

function useSelectedCandidatesInAssistant() {
  if (selectedCandidateAddresses.value.length === 0) return
  const addresses = selectedCandidateAddresses.value.map((address) => `0x${address}`).join(' ')
  store.searchQuery = `j'utilise ces mémoires ${addresses}`
  void store.doSearch()
}

function writeSelectedCandidates() {
  if (selectedCandidateAddresses.value.length === 0 || !store.writeValue.trim()) return
  void store.writeSelectedAddresses(
    selectedCandidateAddresses.value,
    store.exactScanType,
    store.writeValue,
  )
}

function watchCandidate(address: string, type: string) {
  store.addAddressToWatch(address, type)
  if (!store.watchLiveEnabled) store.setWatchLiveEnabled(true)
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
        <button class="btn btn-secondary" :disabled="store.scanBusy" @click="store.resetWorkflow()">
          Nouveau scan
        </button>
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
      </div>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      Attache un processus pour utiliser les outils expert.
    </div>

    <template v-else>
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

      <section v-if="store.expertRegionSize || store.expertRegionProtection" class="panel region-context">
        <div class="panel-title">
          <h2>Région active</h2>
          <span>{{ formatBytes(store.expertRegionSize) }}</span>
        </div>
        <div class="metrics">
          <span>Début: {{ store.expertStartAddress || '-' }}</span>
          <span>Fin: {{ store.expertStopAddress || '-' }}</span>
          <span>Protection: {{ store.expertRegionProtection || '-' }}</span>
          <span>État: {{ store.expertRegionState || '-' }}</span>
          <span>Type: {{ store.expertRegionType || '-' }}</span>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.exact') }}</h2>
          <div class="panel-actions">
            <span v-if="store.exactScanResult?.partial">{{ $t('scan.partial') }}</span>
            <button class="btn btn-secondary compact" type="button" :disabled="store.scanBusy" @click="store.resetWorkflow()">
              Nouveau scan
            </button>
          </div>
        </div>
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
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
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
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.nextScan') }}</h2>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="store.scanBusy || !store.candidatePage?.totalCount"
            @click="store.undoCandidateScan()"
          >
            Restaurer réduction
          </button>
        </div>
        <div class="controls next-controls">
          <select v-model="store.nextScanMode" class="input select" :disabled="store.scanBusy || !hasCandidateContext">
            <option value="exact">{{ $t('scan.modeExact') }}</option>
            <option value="changed">{{ $t('scan.modeChanged') }}</option>
            <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
            <option value="increased">{{ $t('scan.modeIncreased') }}</option>
            <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
            <option value="delta">{{ $t('scan.modeDelta') }}</option>
          </select>
          <input
            v-model="store.nextScanValue"
            :disabled="store.scanBusy || !hasCandidateContext || (store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta')"
            :placeholder="$t('scan.nextValue')"
            class="input"
            @keyup.enter="store.doNextScan()"
          />
          <button class="btn btn-primary" :disabled="store.scanBusy || !hasCandidateContext" @click="store.doNextScan()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Scan...' : $t('scan.nextScan') }}</span>
          </button>
        </div>
        <div v-if="store.nextScanResult" class="metrics">
          <span>{{ $t('scan.remaining') }}: {{ formatNumber(store.nextScanResult.remaining) }}</span>
          <span>{{ $t('scan.checked') }}: {{ formatNumber(store.nextScanResult.checked) }}</span>
          <span>{{ $t('scan.unreadable') }}: {{ formatNumber(store.nextScanResult.unreadable) }}</span>
          <span v-if="store.nextScanResult.elapsedMs">Temps: {{ formatNumber(store.nextScanResult.elapsedMs) }} ms</span>
          <span v-if="store.nextScanResult.candidatesPerSecond">Débit: {{ formatRate(store.nextScanResult.candidatesPerSecond) }}</span>
        </div>
        <p v-if="store.nextScanResult?.error" class="error">{{ store.nextScanResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('unknown.title') }}</h2>
        </div>
        <div class="controls unknown-controls">
          <select v-model="store.unknownScanType" class="input select" :disabled="store.scanBusy">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <button class="btn btn-secondary" :disabled="store.scanBusy" @click="store.captureUnknownSnapshot()">
            <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
            <span>{{ store.scanBusy ? 'Capture...' : $t('unknown.capture') }}</span>
          </button>
        </div>
        <div class="unknown-guide">
          <button
            v-for="action in unknownGuideActions"
            :key="action.mode"
            class="btn btn-secondary compact guide-btn"
            type="button"
            :class="{ active: store.unknownScanMode === action.mode }"
            :disabled="store.scanBusy || !unknownGuideReady || (action.mode === 'unchanged' && !hasCandidateContext)"
            @click="store.runUnknownGuideStep(action.mode)"
          >
            {{ action.label }}
          </button>
        </div>
        <div v-if="store.unknownSnapshotResult || store.unknownNextScanResult" class="metrics">
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.regions') }}: {{ formatNumber(store.unknownSnapshotResult.regionsCaptured) }}</span>
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.bytes') }}: {{ formatNumber(store.unknownSnapshotResult.bytesCaptured) }}</span>
          <span v-if="store.unknownSnapshotResult?.compressedBytes !== undefined">Compressé: {{ formatBytes(store.unknownSnapshotResult.compressedBytes) }}</span>
          <span v-if="store.unknownSnapshotResult?.mappedStorage">Stockage fichier temporaire</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.matches') }}: {{ formatNumber(store.unknownNextScanResult.matchesFound) }}</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.stored') }}: {{ formatNumber(store.unknownNextScanResult.stored) }}</span>
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

      <section class="panel">
        <div class="panel-title">
          <h2>Candidats</h2>
          <span>{{ formatNumber(store.candidatePage?.totalCount) }} · {{ selectedCandidateAddresses.length }} sélectionné(s)</span>
        </div>
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
          <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="selectedCandidateAddresses = []">
            Effacer
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0" @click="useSelectedCandidatesInAssistant()">
            Utiliser sélection
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0 || !store.writeValue.trim()" @click="writeSelectedCandidates()">
            Écrire sur sélection
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
        <div v-if="store.candidatePage?.displaySuppressed" class="candidate-suppressed">
          {{ formatNumber(store.candidatePage.totalCount) }} candidats trouvés. Réduis avec un next scan ou filtre une adresse pour afficher une page.
        </div>
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
              <span class="visual-state">{{ store.candidateVisualState(match) }}</span>
              <span v-if="store.watchedAddresses.some((item) => item.address === match.address)" class="live-dot">watch</span>
            </div>
            <div class="candidate-actions">
              <button class="btn btn-secondary compact" @click="useCandidateInAssistant(match.address, match.type)">
                Utiliser
              </button>
              <button class="btn btn-secondary compact" @click="watchCandidate(match.address, match.type)">
                Watch
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

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('write.title') }}</h2>
          <span v-if="store.writeResult">{{ store.writeResult.success ? 'OK' : 'FAIL' }}</span>
        </div>
        <div class="controls write-controls">
          <input
            v-model="store.selectedCandidateAddress"
            class="input"
            :placeholder="$t('write.address')"
            @input="store.updateWriteSafetyWarning()"
            @blur="store.updateWriteSafetyWarning()"
          />
          <select v-model="store.exactScanType" class="input select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <input v-model="store.writeValue" class="input" :placeholder="$t('write.value')" @keyup.enter="store.writeSelectedValue()" />
          <button class="btn btn-primary" :disabled="!store.canWriteSelectedValue" @click="store.writeSelectedValue()">
            {{ $t('write.write') }}
          </button>
          <button class="btn btn-secondary" @click="store.rollbackLastWrite()">
            {{ $t('write.rollback') }}
          </button>
          <button class="btn btn-secondary" :disabled="store.freezeEnabled ? !store.selectedCandidateAddress : !store.canWriteSelectedValue" @click="store.toggleFreeze()">
            {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
          </button>
        </div>
        <div v-if="store.writeSafetyWarning" class="write-safety">
          <p class="warning">{{ store.writeSafetyWarning }}</p>
          <label class="safety-ack">
            <input v-model="store.writeSafetyAcknowledged" type="checkbox" />
            Je confirme cette écriture mémoire
          </label>
        </div>
        <div v-if="store.writeResult" class="metrics">
          <span>{{ store.writeResult.bytesWritten }} B</span>
          <span v-if="store.writeResult.verified">{{ $t('write.verified') }}</span>
          <span v-if="store.writeResult.enabled !== undefined">freeze: {{ store.writeResult.enabled ? 'on' : 'off' }}</span>
        </div>
        <p v-if="store.writeResult?.error" class="error">{{ store.writeResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>Watch live</h2>
          <button
            class="btn btn-secondary compact"
            type="button"
            :disabled="store.watchedAddresses.length === 0"
            @click="store.setWatchLiveEnabled(!store.watchLiveEnabled)"
          >
            {{ store.watchLiveEnabled ? 'Arrêter' : 'Démarrer' }}
          </button>
        </div>
        <div v-if="store.watchedAddresses.length === 0" class="hint">Sélectionne un candidat ou clique Watch pour surveiller une adresse.</div>
        <div v-else class="watch-list">
          <div v-for="item in store.watchedAddresses" :key="item.address" class="watch-row" :class="{ changed: item.changed }">
            <code>0x{{ item.address }}</code>
            <span>{{ item.type }}</span>
            <strong>{{ item.value || '-' }}</strong>
            <span v-if="item.previousValue">avant: {{ item.previousValue }}</span>
            <span>{{ item.updatedAt }}</span>
            <button class="btn btn-secondary compact" @click="store.removeAddressFromWatch(item.address)">Retirer</button>
          </div>
        </div>
      </section>

      <section class="panel pointer-chain-panel">
        <div class="panel-title">
          <h2>Pointer Chains <span class="hint-inline">(StarCraft 2 / jeux modernes)</span></h2>
          <span v-if="pointerScanResult">{{ formatNumber(pointerScanResult.chainCount) }} chaine(s)</span>
        </div>
        <p class="hint">
          Pour les jeux modernes (StarCraft 2, etc.), les ressources sont allouees dynamiquement.
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
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
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
            </div>
          </div>
        </div>
        <p v-if="pointerScanResult?.error" class="error">{{ pointerScanResult.error }}</p>
        <p v-if="pointerResolveResult" class="hint">
          Resolution : {{ pointerResolveResult.success ? 'OK 0x' + pointerResolveResult.finalAddress : 'ECHEC ' + pointerResolveResult.error }}
        </p>
      </section>

      <section v-if="expertDense" class="panel">
        <div class="panel-title">
          <h2>Journal utilisateur</h2>
          <span>{{ store.actionLog.length }} entrée(s)</span>
        </div>
        <div class="action-log">
          <div v-for="entry in store.actionLog.slice(0, 18)" :key="entry.id" class="action-entry" :class="entry.status">
            <span>{{ entry.time }}</span>
            <strong>{{ entry.title }}</strong>
            <em>{{ entry.detail }}</em>
          </div>
        </div>
      </section>
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

.empty-state {
  padding: 32px;
  color: var(--text-dim);
  text-align: center;
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

.panel-title {
  margin-bottom: 10px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
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
  grid-template-columns: 120px minmax(220px, 1fr);
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

.write-controls {
  grid-template-columns: minmax(170px, 1fr) 110px minmax(140px, 1fr) auto auto auto;
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

.candidate-list {
  display: flex;
  max-height: 260px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.candidate-row {
  display: grid;
  grid-template-columns: 28px minmax(170px, 1fr) minmax(240px, 1.1fr) minmax(270px, auto);
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
  .write-controls,
  .candidate-toolbar,
  .candidate-row,
  .pointer-chain-controls {
    grid-template-columns: 1fr;
  }
}
</style>
