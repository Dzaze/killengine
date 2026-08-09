<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const selectedCandidateAddresses = ref<string[]>([])

const candidatePageTotal = computed(() => {
  if (!store.candidatePage) return 1
  return Math.max(1, Math.ceil(store.candidatePage.totalCount / store.candidatePage.pageSize))
})
const currentPageCandidates = computed(() => store.candidatePage?.candidates ?? [])

function formatNumber(value: number | undefined) {
  return new Intl.NumberFormat('fr-FR').format(value ?? 0)
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
      <button class="btn btn-secondary" @click="store.doPing()">
        {{ $t('actions.ping') }}
      </button>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      Attache un processus pour utiliser les outils expert.
    </div>

    <template v-else>
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

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.exact') }}</h2>
          <span v-if="store.exactScanResult?.partial">{{ $t('scan.partial') }}</span>
        </div>
        <div class="controls exact-controls">
          <input
            v-model="store.exactScanValue"
            :placeholder="$t('scan.value')"
            class="input"
            @keyup.enter="store.doExactScan()"
          />
          <select v-model="store.exactScanType" class="input select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <button class="btn btn-primary" :disabled="!store.exactScanValue.trim()" @click="store.doExactScan()">
            {{ $t('scan.button') }}
          </button>
        </div>
        <div v-if="store.exactScanResult" class="metrics">
          <span>{{ $t('scan.matches') }}: {{ formatNumber(store.exactScanResult.matchesFound) }}</span>
          <span>{{ $t('scan.regions') }}: {{ formatNumber(store.exactScanResult.regionsScanned) }}</span>
          <span>{{ $t('scan.stored') }}: {{ formatNumber(store.exactScanResult.candidateStoreSize) }}</span>
        </div>
        <p v-if="store.exactScanResult?.error" class="error">{{ store.exactScanResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('scan.nextScan') }}</h2>
        </div>
        <div class="controls next-controls">
          <select v-model="store.nextScanMode" class="input select">
            <option value="exact">{{ $t('scan.modeExact') }}</option>
            <option value="changed">{{ $t('scan.modeChanged') }}</option>
            <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
            <option value="increased">{{ $t('scan.modeIncreased') }}</option>
            <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
            <option value="delta">{{ $t('scan.modeDelta') }}</option>
          </select>
          <input
            v-model="store.nextScanValue"
            :disabled="store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta'"
            :placeholder="$t('scan.nextValue')"
            class="input"
            @keyup.enter="store.doNextScan()"
          />
          <button class="btn btn-primary" @click="store.doNextScan()">
            {{ $t('scan.nextScan') }}
          </button>
        </div>
        <div v-if="store.nextScanResult" class="metrics">
          <span>{{ $t('scan.remaining') }}: {{ formatNumber(store.nextScanResult.remaining) }}</span>
          <span>{{ $t('scan.checked') }}: {{ formatNumber(store.nextScanResult.checked) }}</span>
          <span>{{ $t('scan.unreadable') }}: {{ formatNumber(store.nextScanResult.unreadable) }}</span>
        </div>
        <p v-if="store.nextScanResult?.error" class="error">{{ store.nextScanResult.error }}</p>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('unknown.title') }}</h2>
        </div>
        <div class="controls unknown-controls">
          <select v-model="store.unknownScanType" class="input select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <select v-model="store.unknownScanMode" class="input select">
            <option value="changed">{{ $t('scan.modeChanged') }}</option>
            <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
            <option value="increased">{{ $t('scan.modeIncreased') }}</option>
            <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
          </select>
          <button class="btn btn-secondary" @click="store.captureUnknownSnapshot()">
            {{ $t('unknown.capture') }}
          </button>
          <button class="btn btn-primary" @click="store.doUnknownNextScan()">
            {{ $t('unknown.compare') }}
          </button>
        </div>
        <div v-if="store.unknownSnapshotResult || store.unknownNextScanResult" class="metrics">
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.regions') }}: {{ formatNumber(store.unknownSnapshotResult.regionsCaptured) }}</span>
          <span v-if="store.unknownSnapshotResult">{{ $t('unknown.bytes') }}: {{ formatNumber(store.unknownSnapshotResult.bytesCaptured) }}</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.matches') }}: {{ formatNumber(store.unknownNextScanResult.matchesFound) }}</span>
          <span v-if="store.unknownNextScanResult">{{ $t('scan.stored') }}: {{ formatNumber(store.unknownNextScanResult.stored) }}</span>
        </div>
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
            :disabled="!store.candidatePage || (store.candidatePageIndex + 1) * store.candidatePageSize >= store.candidatePage.totalCount"
            @click="store.nextCandidatePage()"
          >
            {{ $t('scan.next') }}
          </button>
        </div>
        <div class="selection-toolbar">
          <button class="btn btn-secondary compact" :disabled="currentPageCandidates.length === 0" @click="toggleCurrentPageSelection()">
            Sélection page
          </button>
          <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="selectedCandidateAddresses = []">
            Effacer
          </button>
          <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0" @click="useSelectedCandidatesInAssistant()">
            Utiliser sélection
          </button>
        </div>
        <div class="page-info">
          {{ store.candidatePage ? store.candidatePage.pageIndex + 1 : 1 }} / {{ candidatePageTotal }}
        </div>
        <div class="candidate-list">
          <div v-for="match in currentPageCandidates" :key="match.address" class="candidate-row">
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
            <span>{{ match.type }}</span>
            <button class="btn btn-secondary compact" @click="useCandidateInAssistant(match.address, match.type)">
              Utiliser
            </button>
          </div>
        </div>
      </section>

      <section class="panel">
        <div class="panel-title">
          <h2>{{ $t('write.title') }}</h2>
          <span v-if="store.writeResult">{{ store.writeResult.success ? 'OK' : 'FAIL' }}</span>
        </div>
        <div class="controls write-controls">
          <input v-model="store.selectedCandidateAddress" class="input" :placeholder="$t('write.address')" />
          <select v-model="store.exactScanType" class="input select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <input v-model="store.writeValue" class="input" :placeholder="$t('write.value')" @keyup.enter="store.writeSelectedValue()" />
          <button class="btn btn-primary" :disabled="!store.selectedCandidateAddress || !store.writeValue.trim()" @click="store.writeSelectedValue()">
            {{ $t('write.write') }}
          </button>
          <button class="btn btn-secondary" @click="store.rollbackLastWrite()">
            {{ $t('write.rollback') }}
          </button>
          <button class="btn btn-secondary" :disabled="!store.selectedCandidateAddress || !store.writeValue.trim()" @click="store.toggleFreeze()">
            {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
          </button>
        </div>
        <div v-if="store.writeResult" class="metrics">
          <span>{{ store.writeResult.bytesWritten }} B</span>
          <span v-if="store.writeResult.verified">{{ $t('write.verified') }}</span>
          <span v-if="store.writeResult.enabled !== undefined">freeze: {{ store.writeResult.enabled ? 'on' : 'off' }}</span>
        </div>
        <p v-if="store.writeResult?.error" class="error">{{ store.writeResult.error }}</p>
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

.next-controls {
  grid-template-columns: 150px 1fr auto;
}

.unknown-controls {
  grid-template-columns: 120px 150px auto auto;
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

.candidate-list {
  display: flex;
  max-height: 260px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.candidate-row {
  display: grid;
  grid-template-columns: 28px minmax(170px, 1fr) 90px auto;
  gap: 10px;
  align-items: center;
  padding: 7px 8px;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
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

@media (max-width: 980px) {
  .summary-grid,
  .exact-controls,
  .next-controls,
  .unknown-controls,
  .write-controls,
  .candidate-toolbar,
  .candidate-row {
    grid-template-columns: 1fr;
  }
}
</style>
