<script setup lang="ts">
// ANALYSE-CLINE-1 — vue dédiée Memory Heatmap (03/09/2026, Claude). Le
// backend (core/visualization/memory_heatmap_collector.*,
// apps/desktop/memory_heatmap_manager.*) et les 4 méthodes Q_INVOKABLE
// existaient déjà depuis c9991c3 (02/09/2026) mais sans aucune vue —
// pilotable uniquement via le pipe d'automatisation. Voir
// docs/PHASE_TRACKER.md "ANALYSE-CLINE-1".
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()

const minAddress = ref('')
const regionSize = ref(4096)
const samplingIntervalMs = ref(100)
const trackReads = ref(true)
const trackWrites = ref(true)

const isCollecting = ref(false)
const busy = ref(false)
const stats = ref<Record<string, unknown>>({})
const topRegions = ref<Array<Record<string, unknown>>>([])

let pollTimer: ReturnType<typeof setInterval> | null = null

function stopPolling() {
  if (pollTimer !== null) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

function startPolling() {
  stopPolling()
  pollTimer = setInterval(refresh, 1000)
}

async function refresh() {
  const result = await store.getMemoryHeatmapData()
  if (result) {
    stats.value = (result.stats as Record<string, unknown>) ?? {}
    topRegions.value = (result.topRegions as Array<Record<string, unknown>>) ?? []
  }
}

async function syncStatus() {
  const status = await store.getMemoryHeatmapStatus()
  if (!status) return
  isCollecting.value = status.collecting === true
  stats.value = (status.stats as Record<string, unknown>) ?? {}
  if (isCollecting.value) {
    await refresh()
    startPolling()
  }
}

async function start() {
  busy.value = true
  try {
    const options: Record<string, unknown> = {
      regionSize: regionSize.value,
      samplingIntervalMs: samplingIntervalMs.value,
      trackReads: trackReads.value,
      trackWrites: trackWrites.value,
    }
    const ok = await store.startMemoryHeatmap(minAddress.value.trim(), options)
    if (ok) {
      isCollecting.value = true
      await refresh()
      startPolling()
    }
  } finally {
    busy.value = false
  }
}

async function stop() {
  busy.value = true
  stopPolling()
  try {
    const result = await store.stopMemoryHeatmap()
    isCollecting.value = false
    if (result) {
      stats.value = (result.stats as Record<string, unknown>) ?? {}
      topRegions.value = (result.topRegions as Array<Record<string, unknown>>) ?? []
    }
  } finally {
    busy.value = false
  }
}

function formatBytes(size: unknown): string {
  const n = Number(size ?? 0)
  if (n >= 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} Mo`
  if (n >= 1024) return `${(n / 1024).toFixed(1)} Ko`
  return `${n} o`
}

function intensityPercent(value: unknown): number {
  return Math.round(Number(value ?? 0) * 100)
}

const hasStats = computed(() => Object.keys(stats.value).length > 0)

onMounted(() => {
  void syncStatus()
})

onUnmounted(() => {
  stopPolling()
})
</script>

<template>
  <div class="memory-heatmap-view">
    <div class="header">
      <div>
        <h1>Memory Heatmap</h1>
        <p>{{ store.isAttached ? store.processName : 'Aucun processus attaché' }}</p>
      </div>
    </div>

    <PanelIntro
      what="Une carte d'intensité d'accès mémoire — quelles régions du process attaché sont lues/écrites le plus souvent."
      purpose="Repérer rapidement les zones actives (état de jeu qui bouge en continu) parmi tout l'espace mémoire, sans poser d'adresse précise au départ."
      how="Démarre la collecte (adresse de départ optionnelle pour borner le scan), laisse tourner quelques secondes pendant que le jeu joue, puis regarde les régions les plus actives."
    />

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache d'abord un processus autorisé pour observer ses accès mémoire.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <section class="panel config-panel">
        <div class="field-row">
          <label>Adresse de départ (optionnel)</label>
          <input v-model="minAddress" placeholder="7ff600000000" :disabled="isCollecting" />
        </div>
        <div class="field-row">
          <label>Taille de région</label>
          <select v-model.number="regionSize" :disabled="isCollecting">
            <option :value="4096">4096 (page)</option>
            <option :value="65536">65536 (64 Ko)</option>
            <option :value="1048576">1048576 (1 Mo)</option>
          </select>
        </div>
        <div class="field-row">
          <label>Intervalle d'échantillonnage (ms)</label>
          <input v-model.number="samplingIntervalMs" type="number" min="10" max="5000" :disabled="isCollecting" />
        </div>
        <div class="field-row checkbox">
          <label><input v-model="trackReads" type="checkbox" :disabled="isCollecting" /> Lectures</label>
          <label><input v-model="trackWrites" type="checkbox" :disabled="isCollecting" /> Écritures</label>
        </div>
        <div class="actions-row">
          <button class="btn btn-primary" :disabled="busy || isCollecting" @click="start">
            {{ isCollecting ? 'Collecte en cours…' : 'Démarrer' }}
          </button>
          <button class="btn btn-secondary" :disabled="busy || !isCollecting" @click="stop">Arrêter</button>
          <button class="btn btn-secondary compact" :disabled="busy" @click="refresh">Rafraîchir</button>
        </div>
      </section>

      <section v-if="hasStats" class="panel stats-panel">
        <div class="stat"><span class="stat-label">Régions suivies</span><span class="stat-value">{{ stats.totalRegions ?? 0 }}</span></div>
        <div class="stat"><span class="stat-label">Régions actives</span><span class="stat-value">{{ stats.activeRegions ?? 0 }}</span></div>
        <div class="stat"><span class="stat-label">Accès totaux</span><span class="stat-value">{{ stats.totalAccesses ?? 0 }}</span></div>
        <div class="stat"><span class="stat-label">Écritures</span><span class="stat-value">{{ stats.totalWrites ?? 0 }}</span></div>
        <div class="stat"><span class="stat-label">Lectures</span><span class="stat-value">{{ stats.totalReads ?? 0 }}</span></div>
        <div class="stat"><span class="stat-label">Intensité moyenne</span><span class="stat-value">{{ intensityPercent(stats.averageIntensity) }}%</span></div>
      </section>

      <section v-if="topRegions.length > 0" class="panel">
        <h3>Régions les plus actives ({{ topRegions.length }})</h3>
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Adresse</th>
                <th>Taille</th>
                <th>Intensité</th>
                <th>Lectures</th>
                <th>Écritures</th>
                <th>Accès</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="region in topRegions" :key="String(region.baseAddress)">
                <td class="mono">{{ region.baseAddress }}</td>
                <td>{{ formatBytes(region.size) }}</td>
                <td>
                  <div class="intensity-bar">
                    <div class="intensity-fill" :style="{ width: intensityPercent(region.intensity) + '%' }"></div>
                    <span>{{ intensityPercent(region.intensity) }}%</span>
                  </div>
                </td>
                <td>{{ region.readCount }}</td>
                <td>{{ region.writeCount }}</td>
                <td>{{ region.accessCount }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>

      <section v-else-if="isCollecting" class="panel empty-hint">
        <p>Collecte en cours — aucune région active pour l'instant.</p>
      </section>
    </template>
  </div>
</template>

<style scoped>
.memory-heatmap-view {
  padding: 24px 32px;
  max-width: 1100px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  color: var(--text-dim);
  margin-top: 4px;
}

.empty-state,
.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
}

.empty-state {
  padding: 32px;
  color: var(--text-secondary);
}

.panel {
  padding: 18px;
  margin-bottom: 14px;
}

.panel h3 {
  margin: 0 0 12px 0;
  font-size: 15px;
  color: var(--text-primary);
}

.config-panel {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
  align-items: flex-end;
}

.field-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.field-row label {
  font-size: 12px;
  color: var(--text-dim);
}

.field-row input,
.field-row select {
  padding: 7px 10px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-primary);
}

.field-row.checkbox {
  flex-direction: row;
  gap: 16px;
}

.field-row.checkbox label {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
}

.actions-row {
  display: flex;
  gap: 10px;
  margin-left: auto;
}

.stats-panel {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: 16px;
}

.stat {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.stat-label {
  font-size: 12px;
  color: var(--text-dim);
}

.stat-value {
  font-size: 20px;
  font-weight: 600;
  color: var(--text-primary);
}

.table-wrap {
  overflow-x: auto;
}

table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

th {
  text-align: left;
  color: var(--text-dim);
  font-weight: 500;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
}

td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-secondary);
}

.mono {
  font-family: 'Cascadia Code', 'Consolas', monospace;
  color: var(--text-primary);
}

.intensity-bar {
  position: relative;
  width: 100px;
  height: 16px;
  background: var(--bg-primary);
  border-radius: 3px;
  overflow: hidden;
}

.intensity-fill {
  height: 100%;
  background: var(--accent);
  opacity: 0.7;
}

.intensity-bar span {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 11px;
  color: var(--text-primary);
}

.empty-hint {
  color: var(--text-dim);
  text-align: center;
}
</style>
