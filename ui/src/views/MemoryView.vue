<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { backend } from '@/services/backend'

const store = useAppStore()
const filter = ref<'all' | 'committed' | 'readable' | 'writable' | 'executable'>('all')
// Edition hex inline : ecriture de bytes bruts depuis l'inspecteur memoire
const hexEditMode = ref(false)
const hexEditValue = ref('')
const hexEditBusy = ref(false)
const hexEditResult = ref<Record<string, unknown> | null>(null)
const lastWrittenHex = ref('')

// Dump region : export binaire de la zone previewee
const dumpSize = ref(256)
const dumpBusy = ref(false)
const dumpResult = ref<Record<string, unknown> | null>(null)

// Visualiseur hexadecimal navigable (item roadmap : au-dela de l'apercu 64 octets)
const hexViewerJumpAddress = ref('')
const hexViewerRowEditAddress = ref<string | null>(null)
const hexViewerRowEditValue = ref('')
const hexViewerRowBusy = ref(false)
const hexViewerRowResult = ref<Record<string, unknown> | null>(null)

const regions = computed(() => store.memoryMap?.regions ?? [])
const stats = computed(() => store.memoryMap?.stats)
// Filtre de region par nom de module (roadmap I) : store.processModules est deja
// peuple a l'attache (voir ProcessView), on resout juste baseAddress -> module ici
// plutot que d'ajouter un champ module au format MemoryRegion cote backend.
const moduleFilter = ref('')

function regionModuleName(region: Record<string, unknown>): string {
  const base = BigInt(`0x${String(region.baseAddress || '0')}`)
  for (const mod of store.processModules) {
    const modBase = BigInt(`0x${mod.baseAddress}`)
    if (base >= modBase && base < modBase + BigInt(mod.size)) return mod.name
  }
  return ''
}

const filteredRegions = computed(() => {
  const moduleQuery = moduleFilter.value.trim().toLowerCase()
  return regions.value.filter((region) => {
    if (filter.value === 'committed' && region.state !== 'committed') return false
    if (filter.value === 'readable' && !region.readable) return false
    if (filter.value === 'writable' && !region.writable) return false
    if (filter.value === 'executable' && !region.executable) return false
    if (moduleQuery && !regionModuleName(region).toLowerCase().includes(moduleQuery)) return false
    return true
  })
})

function previewRegion(address: string) {
  void store.readMemoryPreview(address, 64)
}

function openRegionInExpert(region: Record<string, unknown>) {
  store.openExpertForRegion(region)
}

async function copyPreviewAddress() {
  if (!store.memoryPreviewAddress) return
  await navigator.clipboard?.writeText(`0x${store.memoryPreviewAddress}`)
}

function scanAroundPreview(type = store.exactScanType) {
  void store.scanAroundPreview(store.exactScanValue, type)
}
function startHexEdit() {
  if (!store.memoryPreviewAddress) return
  hexEditMode.value = !hexEditMode.value
  if (hexEditMode.value && !hexEditValue.value) {
    hexEditValue.value = store.memoryPreview?.hex ?? ''
  }
  hexEditResult.value = null
}

async function applyHexEdit() {
  if (!store.memoryPreviewAddress || !hexEditValue.value.trim()) return
  const risk = store.kernelMemoryModeActive ? 'injection' : 'write'
  if (!await store.confirmRiskAction(risk, 'Edition hex', 'Ecriture de bytes bruts a 0x' + store.memoryPreviewAddress + (store.kernelMemoryModeActive ? ' via driver kernel.' : '.'))) {
    return
  }
  hexEditBusy.value = true
  hexEditResult.value = null
  try {
    const result = await store.writeMemoryHexByMode(store.memoryPreviewAddress, hexEditValue.value)
    hexEditResult.value = result
    if (result.success) {
      lastWrittenHex.value = String(result.previousHex ?? '')
      await store.readMemoryPreview(store.memoryPreviewAddress, store.memoryPreview?.requestedBytes ?? 64)
    }
  } catch (e) {
    hexEditResult.value = { success: false, error: String(e) }
  } finally {
    hexEditBusy.value = false
  }
}

async function restoreLastHexEdit() {
  if (!store.memoryPreviewAddress || !lastWrittenHex.value) return
  hexEditValue.value = lastWrittenHex.value
  await applyHexEdit()
}

async function dumpPreviewRegion() {
  if (!store.memoryPreviewAddress) return
  dumpBusy.value = true
  dumpResult.value = null
  try {
    const controller = backend.getController()
    if (!controller.dumpMemoryRegion) {
      dumpResult.value = { success: false, error: 'dumpMemoryRegion non disponible dans ce backend.' }
      return
    }
    dumpResult.value = await controller.dumpMemoryRegion(
      store.memoryPreviewAddress,
      dumpSize.value,
      'dump_' + store.memoryPreviewAddress,
    )
  } catch (e) {
    dumpResult.value = { success: false, error: String(e) }
  } finally {
    dumpBusy.value = false
  }
}

function openHexViewer() {
  if (!store.memoryPreviewAddress) return
  void store.openHexViewer(store.memoryPreviewAddress)
}

function hexViewerJump() {
  if (!hexViewerJumpAddress.value.trim()) return
  void store.hexViewerJumpTo(hexViewerJumpAddress.value)
}

function hexViewerPrevPage() {
  void store.hexViewerGoToOffset(-store.hexViewerPageSize)
}

function hexViewerNextPage() {
  void store.hexViewerGoToOffset(store.hexViewerPageSize)
}

function startRowEdit(row: { address: string; bytes: string[] }) {
  hexViewerRowEditAddress.value = hexViewerRowEditAddress.value === row.address ? null : row.address
  hexViewerRowEditValue.value = row.bytes.join(' ')
  hexViewerRowResult.value = null
}

async function applyRowEdit() {
  if (!hexViewerRowEditAddress.value || !hexViewerRowEditValue.value.trim()) return
  const address = hexViewerRowEditAddress.value
  const risk = store.kernelMemoryModeActive ? 'injection' : 'write'
  if (!await store.confirmRiskAction(risk, 'Edition hex', 'Ecriture de bytes bruts a 0x' + address + (store.kernelMemoryModeActive ? ' via driver kernel.' : '.'))) {
    return
  }
  hexViewerRowBusy.value = true
  hexViewerRowResult.value = null
  try {
    hexViewerRowResult.value = await store.hexViewerWriteRow(address, hexViewerRowEditValue.value)
    if (hexViewerRowResult.value?.success) {
      hexViewerRowEditAddress.value = null
    }
  } finally {
    hexViewerRowBusy.value = false
  }
}

function formatBytes(bytes: number | undefined) {
  if (!bytes) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit += 1
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`
}

onMounted(() => {
  if (store.isAttached) {
    store.refreshMemoryMap()
  }
})
</script>

<template>
  <div class="memory-view">
    <div class="header">
      <div>
        <h1>{{ $t('memory.title') }}</h1>
        <p v-if="store.isAttached">{{ store.processName }}</p>
      </div>
      <button
        class="btn btn-secondary"
        :disabled="!store.isAttached"
        :title="!store.isAttached ? $t('memory.attachFirst') : ''"
        @click="store.refreshMemoryMap()"
      >
        {{ $t('memory.refresh') }}
      </button>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      <p>{{ $t('memory.attachFirst') }}</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">{{ $t('memory.goToProcess') }}</button>
    </div>

    <template v-else>
      <div class="stats-grid">
        <div class="stat">
          <span>{{ $t('memory.regions') }}</span>
          <strong>{{ stats?.regionCount ?? 0 }}</strong>
        </div>
        <div class="stat">
          <span>{{ $t('memory.committed') }}</span>
          <strong>{{ stats?.committedCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.committedBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.readable') }}</span>
          <strong>{{ stats?.readableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.readableBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.writable') }}</span>
          <strong>{{ stats?.writableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.writableBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.executable') }}</span>
          <strong>{{ stats?.executableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.executableBytes) }}</small>
        </div>
      </div>

      <div class="toolbar">
        <button class="chip" :class="{ active: filter === 'all' }" @click="filter = 'all'">
          {{ $t('memory.all') }}
        </button>
        <button class="chip" :class="{ active: filter === 'committed' }" @click="filter = 'committed'">
          {{ $t('memory.committed') }}
        </button>
        <button class="chip" :class="{ active: filter === 'readable' }" @click="filter = 'readable'">
          {{ $t('memory.readable') }}
        </button>
        <button class="chip" :class="{ active: filter === 'writable' }" @click="filter = 'writable'">
          {{ $t('memory.writable') }}
        </button>
        <button class="chip" :class="{ active: filter === 'executable' }" @click="filter = 'executable'">
          {{ $t('memory.executable') }}
        </button>
        <input
          v-model="moduleFilter"
          class="input module-filter"
          :placeholder="$t('memory.moduleFilterPlaceholder')"
        />
      </div>

      <div
        v-if="store.memoryPreview || store.memoryPreviewLoading"
        class="preview-panel"
        :class="{ loading: store.memoryPreviewLoading, error: store.memoryPreview && !store.memoryPreview.success }"
      >
        <div class="preview-title">
          <div>
            <strong>{{ $t('memory.preview') }}</strong>
            <code v-if="store.memoryPreviewAddress">0x{{ store.memoryPreviewAddress }}</code>
          </div>
          <div class="preview-actions">
            <span v-if="store.memoryPreview">
              {{ store.memoryPreview.bytesRead }}/{{ store.memoryPreview.requestedBytes }} B
            </span>
            <button
              v-if="store.memoryPreviewAddress"
              class="preview-action-btn"
              type="button"
              @click="copyPreviewAddress()"
            >
              Copier
            </button>
            <button
              v-if="store.memoryPreviewAddress"
              class="preview-action-btn"
              type="button"
              :disabled="!store.exactScanValue.trim()"
              @click="scanAroundPreview()"
            >
              Scanner autour
            </button>
            <button
              v-if="store.selectedMemoryRegion"
              class="preview-action-btn"
              type="button"
              @click="openRegionInExpert(store.selectedMemoryRegion)"
            >
              Basculer en expert
            </button>
              <button
                v-if="store.memoryPreviewAddress"
                class="preview-action-btn"
                type="button"
                @click="startHexEdit()"
              >
                {{ hexEditMode ? 'Fermer hex' : 'Editer hex' }}
              </button>
              <button
                v-if="store.memoryPreviewAddress"
                class="preview-action-btn"
                type="button"
                @click="openHexViewer()"
              >
                Vue hexadécimale
              </button>
              <button
                v-if="store.memoryPreviewAddress"
                class="preview-action-btn"
                type="button"
                :disabled="dumpBusy"
                @click="dumpPreviewRegion()"
              >
                {{ dumpBusy ? 'Dump...' : 'Dump' }}
              </button>
          </div>
          <div v-if="hexEditMode" class="hex-edit-row">
            <input
              v-model="hexEditValue"
              class="hex-edit-input"
              type="text"
              placeholder="48 8B 00 90 ..."
              spellcheck="false"
            />
            <button
              class="preview-action-btn"
              type="button"
              :disabled="hexEditBusy || !hexEditValue.trim()"
              @click="applyHexEdit()"
            >
              {{ hexEditBusy ? 'Ecriture...' : 'Ecrire bytes' }}
            </button>
            <button
              v-if="lastWrittenHex"
              class="preview-action-btn"
              type="button"
              :disabled="hexEditBusy"
              @click="restoreLastHexEdit()"
            >
              Restaurer
            </button>
          </div>
          <p v-if="hexEditResult" :class="hexEditResult.success ? 'hex-result-ok' : 'hex-result-err'">
            {{ hexEditResult.success ? 'Ecriture OK (' + (hexEditResult.bytesWritten ?? 0) + ' octets)' : String(hexEditResult.error ?? 'Echec') }}
          </p>
          <div class="dump-row">
            <label class="dump-label">Taille dump</label>
            <select v-model.number="dumpSize" class="dump-select">
              <option :value="256">256 B</option>
              <option :value="1024">1 KB</option>
              <option :value="16384">16 KB</option>
              <option :value="262144">256 KB</option>
              <option :value="1048576">1 MB</option>
            </select>
            <p v-if="dumpResult" :class="dumpResult.success ? 'hex-result-ok' : 'hex-result-err'">
              {{ dumpResult.success ? 'Dump OK : ' + String(dumpResult.filePath ?? '') : String(dumpResult.error ?? 'Echec') }}
            </p>
          </div>
        </div>
        <div v-if="store.memoryPreviewLoading" class="preview-loading">
          Lecture de la mémoire...
        </div>
        <div v-else-if="store.memoryPreview?.hex" class="preview-grid">
          <pre>{{ store.memoryPreview.hex }}</pre>
          <pre class="ascii">{{ store.memoryPreviewAscii }}</pre>
          <div class="decoded-values">
            <button
              v-for="decoded in store.memoryPreviewDecoded"
              :key="decoded.label"
              class="decoded-pill"
              type="button"
              @click="scanAroundPreview(decoded.label)"
            >
              <span>{{ decoded.label }}</span>
              <strong>{{ decoded.value }}</strong>
            </button>
          </div>
        </div>
        <p v-else-if="store.memoryPreview?.error">{{ store.memoryPreview.error }}</p>
        <p v-else>Aucune donnée lisible à cette adresse.</p>
      </div>

      <div v-if="store.hexViewerOpen" class="hex-viewer-panel">
        <div class="hex-viewer-toolbar">
          <strong>Vue hexadécimale</strong>
          <code>0x{{ store.hexViewerAddress }}</code>
          <input
            v-model="hexViewerJumpAddress"
            class="hex-edit-input hex-viewer-jump-input"
            type="text"
            placeholder="Aller à (0x...)"
            spellcheck="false"
            @keyup.enter="hexViewerJump()"
          />
          <button class="preview-action-btn" type="button" @click="hexViewerJump()">Aller</button>
          <button class="preview-action-btn" type="button" :disabled="store.hexViewerLoading" @click="hexViewerPrevPage()">
            ← Page préc.
          </button>
          <button class="preview-action-btn" type="button" :disabled="store.hexViewerLoading" @click="hexViewerNextPage()">
            Page suiv. →
          </button>
          <select
            class="dump-select"
            :value="store.hexViewerPageSize"
            @change="store.hexViewerSetPageSize(Number(($event.target as HTMLSelectElement).value))"
          >
            <option :value="256">256 B</option>
            <option :value="512">512 B</option>
            <option :value="4096">4 KB</option>
            <option :value="16384">16 KB</option>
          </select>
          <button class="preview-action-btn" type="button" @click="store.closeHexViewer()">Fermer</button>
        </div>
        <div v-if="store.hexViewerLoading" class="preview-loading">Lecture de la mémoire...</div>
        <p v-else-if="store.hexViewerData && !store.hexViewerData.success">{{ store.hexViewerData.error }}</p>
        <div v-else class="hex-viewer-grid">
          <div class="hex-viewer-header-row">
            <span class="hv-address">Adresse</span>
            <span class="hv-bytes">Octets</span>
            <span class="hv-ascii">ASCII</span>
            <span class="hv-edit"></span>
          </div>
          <div v-for="row in store.hexViewerRows" :key="row.address" class="hex-viewer-row">
            <span class="hv-address">{{ row.address }}</span>
            <span class="hv-bytes">{{ row.bytes.join(' ') }}</span>
            <span class="hv-ascii">{{ row.ascii }}</span>
            <button class="hv-edit-btn" type="button" @click="startRowEdit(row)">
              {{ hexViewerRowEditAddress === row.address ? 'Annuler' : 'Éditer' }}
            </button>
            <div v-if="hexViewerRowEditAddress === row.address" class="hex-edit-row hv-edit-form">
              <input
                v-model="hexViewerRowEditValue"
                class="hex-edit-input"
                type="text"
                spellcheck="false"
              />
              <button
                class="preview-action-btn"
                type="button"
                :disabled="hexViewerRowBusy"
                @click="applyRowEdit()"
              >
                {{ hexViewerRowBusy ? 'Écriture...' : 'Écrire' }}
              </button>
            </div>
          </div>
        </div>
        <p v-if="hexViewerRowResult" :class="hexViewerRowResult.success ? 'hex-result-ok' : 'hex-result-err'">
          {{ hexViewerRowResult.success ? 'Écriture OK (' + (hexViewerRowResult.bytesWritten ?? 0) + ' octets)' : String(hexViewerRowResult.error ?? 'Échec') }}
        </p>
      </div>

      <div class="region-list">
        <div v-for="region in filteredRegions.slice(0, 250)" :key="`${region.baseAddress}-${region.size}`" class="region-row">
          <div class="address">0x{{ region.baseAddress }}</div>
          <div class="size">{{ formatBytes(region.size) }}</div>
          <div class="protection">{{ region.protection }}</div>
          <div class="state">{{ region.state }}</div>
          <div class="type">{{ region.type }}</div>
          <div class="module">{{ regionModuleName(region) || '-' }}</div>
          <div class="region-actions">
            <button
              class="preview-btn"
              :class="{ active: store.memoryPreviewAddress === region.baseAddress }"
              :disabled="!region.readable || (store.memoryPreviewLoading && store.memoryPreviewAddress === region.baseAddress)"
              @click="store.selectedMemoryRegion = region; previewRegion(region.baseAddress)"
            >
              {{ store.memoryPreviewLoading && store.memoryPreviewAddress === region.baseAddress ? 'Lecture...' : $t('memory.preview') }}
            </button>
            <button
              class="preview-btn expert-btn"
              :disabled="!region.readable"
              @click="openRegionInExpert(region)"
            >
              Expert
            </button>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.memory-view {
  max-width: 1100px;
  padding: 24px 32px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 13px;
}

.empty-state {
  padding: 32px;
  color: var(--text-dim);
  text-align: center;
}

.empty-state .btn {
  margin-top: 10px;
}

.stats-grid {
  display: grid;
  grid-template-columns: repeat(5, minmax(120px, 1fr));
  gap: 8px;
  margin-bottom: 16px;
}

.stat {
  min-height: 82px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.stat span,
.stat small {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.stat strong {
  display: block;
  margin: 6px 0 2px;
  color: var(--text-primary);
  font-size: 22px;
}

.toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 10px;
}

.module-filter {
  min-width: 180px;
  padding: 5px 8px;
  font-size: 12px;
}

.module {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.chip,
.btn {
  border: none;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.15s;
}

.chip {
  padding: 7px 10px;
  background: var(--bg-tertiary);
  color: var(--text-dim);
  font-size: 12px;
}

.chip.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.btn {
  padding: 8px 16px;
  font-size: 13px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.region-list {
  display: flex;
  max-height: 510px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.region-row {
  display: grid;
  grid-template-columns: 150px 90px 120px 90px 90px 130px 150px;
  gap: 12px;
  align-items: center;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-dim);
  font-size: 12px;
}

.address,
.protection {
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
}

.preview-btn {
  padding: 5px 8px;
  border: none;
  border-radius: 6px;
  background: var(--bg-accent);
  color: var(--text-secondary);
  cursor: pointer;
  font-size: 12px;
}

.region-actions {
  display: flex;
  gap: 6px;
}

.expert-btn {
  color: var(--accent);
}

.preview-btn:disabled {
  cursor: not-allowed;
  opacity: 0.35;
}

.preview-btn.active {
  color: var(--accent);
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--accent) 45%, transparent);
}

.preview-panel {
  margin: 0 0 12px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.preview-panel.loading {
  border-color: color-mix(in srgb, var(--accent) 35%, var(--border));
}

.preview-panel.error {
  border-color: color-mix(in srgb, #ef4444 35%, var(--border));
}

.preview-title {
  display: flex;
  gap: 12px;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}

.preview-title div,
.preview-actions {
  display: flex;
  min-width: 0;
  gap: 10px;
  align-items: center;
}

.preview-actions {
  flex-shrink: 0;
}

.preview-action-btn {
  padding: 5px 9px;
  border: none;
  border-radius: 6px;
  background: var(--bg-accent);
  color: var(--accent);
  cursor: pointer;
  font-size: 12px;
}

.preview-action-btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.preview-title strong {
  color: var(--text-primary);
}

.preview-title code {
  color: var(--accent);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.preview-title span,
.preview-panel p {
  color: var(--text-dim);
  font-size: 12px;
}

.preview-loading {
  color: var(--text-secondary);
  font-size: 13px;
}

.preview-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.3fr) minmax(180px, 0.7fr);
  gap: 10px;
}

.preview-panel pre {
  max-height: 160px;
  overflow: auto;
  overflow-wrap: anywhere;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  white-space: pre-wrap;
}

.preview-panel pre.ascii {
  color: var(--text-secondary);
}

.decoded-values {
  display: grid;
  grid-column: 1 / -1;
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: 6px;
}

.decoded-pill {
  display: flex;
  min-width: 0;
  justify-content: space-between;
  gap: 8px;
  padding: 7px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-accent);
  color: var(--text-dim);
  cursor: pointer;
  font-size: 12px;
}

.decoded-pill strong {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.hex-edit-row {
  display: flex;
  gap: 8px;
  align-items: center;
  margin: 8px 0;
}

.hex-edit-input {
  flex: 1;
  min-width: 0;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.hex-result-ok {
  color: #22c55e;
  font-size: 12px;
  word-break: break-all;
}

.hex-result-err {
  color: #ef4444;
  font-size: 12px;
}

.dump-row {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-top: 6px;
}

.dump-label {
  color: var(--text-dim);
  font-size: 12px;
}

.dump-select {
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}

.hex-viewer-panel {
  margin: 0 0 12px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.hex-viewer-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-bottom: 10px;
}

.hex-viewer-toolbar strong {
  color: var(--text-primary);
}

.hex-viewer-toolbar code {
  color: var(--accent);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.hex-viewer-jump-input {
  width: 160px;
  flex: none;
}

.hex-viewer-grid {
  display: flex;
  max-height: 420px;
  flex-direction: column;
  gap: 2px;
  overflow-y: auto;
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.hex-viewer-header-row,
.hex-viewer-row {
  display: grid;
  grid-template-columns: 110px minmax(280px, 1fr) 150px 70px;
  gap: 10px;
  align-items: start;
  padding: 3px 6px;
  border-radius: 4px;
}

.hex-viewer-header-row {
  color: var(--text-dim);
  font-size: 11px;
  text-transform: uppercase;
}

.hex-viewer-row {
  flex-wrap: wrap;
  color: var(--text-secondary);
}

.hex-viewer-row:hover {
  background: var(--bg-accent);
}

.hv-address {
  color: var(--accent);
}

.hv-bytes {
  overflow-wrap: anywhere;
  color: var(--text-primary);
}

.hv-ascii {
  color: var(--text-dim);
}

.hv-edit-btn {
  padding: 2px 6px;
  border: none;
  border-radius: 4px;
  background: var(--bg-accent);
  color: var(--accent);
  cursor: pointer;
  font-size: 11px;
}

.hv-edit-form {
  grid-column: 1 / -1;
  margin: 4px 0 2px;
}

@media (max-width: 900px) {
  .stats-grid {
    grid-template-columns: repeat(2, minmax(120px, 1fr));
  }

  .region-row {
    grid-template-columns: 1fr;
    gap: 4px;
  }

  .hex-viewer-header-row,
  .hex-viewer-row {
    grid-template-columns: 1fr;
  }
}
</style>
