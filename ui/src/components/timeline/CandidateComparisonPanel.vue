<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, nextTick } from 'vue'
import { useI18n } from 'vue-i18n'
import { useCandidateComparisonStore, type ComparisonPoint } from '@/stores/candidateComparison'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const { t } = useI18n()
const store = useCandidateComparisonStore()

const seriesColors = ['#4CAF50', '#2196F3', '#FF9800', '#E91E63', '#9C27B0', '#00BCD4']
function colorForIndex(index: number) {
  return seriesColors[index % seriesColors.length]
}

// -- Démarrage / configuration ----------------------------------------------

const intervalOptions = [50, 100, 250, 500, 1000]
const durationOptions = [
  { value: 30000, labelKey: 'candidateComparison.duration30s' },
  { value: 60000, labelKey: 'candidateComparison.duration60s' },
  { value: 120000, labelKey: 'candidateComparison.duration120s' },
]

async function handleStart() {
  await store.startComparison()
}

async function handleStop() {
  await store.stopComparison()
}

// -- Rafraîchissement pendant la capture -------------------------------------

let pollTimer: ReturnType<typeof setInterval> | undefined

function startPolling() {
  if (pollTimer) return
  pollTimer = setInterval(async () => {
    await store.refreshStatus()
    if (store.activeSeries.length > 0) {
      await store.refreshAllSamples()
      await store.refreshCorrelations()
    }
  }, 1000)
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = undefined
  }
}

watch(() => store.collecting, (collecting) => {
  if (collecting) startPolling()
  else stopPolling()
})

onMounted(() => {
  if (store.collecting) startPolling()
})
onUnmounted(() => {
  stopPolling()
})

// -- Mode d'affichage ---------------------------------------------------------

const displayMode = ref<'values' | 'variations'>('values')
const referenceSeriesId = ref('')

watch(() => store.activeSeries, (series) => {
  if (series.length > 0 && !series.some((s) => s.id === referenceSeriesId.value)) {
    referenceSeriesId.value = series[0].id
  }
}, { immediate: true })

// -- Statistiques par série ----------------------------------------------------

interface SeriesStats {
  id: string
  label: string
  color: string
  pointCount: number
  validCount: number
  validRatePercent: number
  first?: ComparisonPoint
  last?: ComparisonPoint
  minNumeric?: number
  maxNumeric?: number
  deltaFromFirst?: number
}

const statsBySeries = computed<SeriesStats[]>(() => store.activeSeries.map((series, index) => {
  const points = store.samplesBySeries.get(series.id) ?? []
  const validPoints = points.filter((p) => p.isValid && p.ok)
  const numericValues = validPoints.map((p) => p.numericValue)
  const first = validPoints[0]
  const last = validPoints[validPoints.length - 1]
  return {
    id: series.id,
    label: series.label,
    color: colorForIndex(index),
    pointCount: points.length,
    validCount: validPoints.length,
    validRatePercent: points.length > 0 ? Math.round((validPoints.length / points.length) * 100) : 0,
    first,
    last,
    minNumeric: numericValues.length > 0 ? Math.min(...numericValues) : undefined,
    maxNumeric: numericValues.length > 0 ? Math.max(...numericValues) : undefined,
    deltaFromFirst: (first && last) ? last.numericValue - first.numericValue : undefined,
  }
}))

// -- Graphique multi-séries (canvas immédiat, même motif que drawChart de
// MemoryTimelineView.vue, généralisé à N séries + un choix explicite de mode
// d'affichage -- voir plan UX-PRODUIT-16). ------------------------------------

const chartContainer = ref<HTMLElement>()
const chartCanvas = ref<HTMLCanvasElement>()

function drawChart() {
  const canvas = chartCanvas.value
  const container = chartContainer.value
  if (!canvas || !container || store.activeSeries.length === 0) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  canvas.width = container.clientWidth
  canvas.height = 220
  ctx.clearRect(0, 0, canvas.width, canvas.height)

  const allSeries = store.activeSeries.map((series, index) => {
    const points = (store.samplesBySeries.get(series.id) ?? []).filter((p) => p.isValid && p.ok)
    return { series, index, points }
  }).filter((entry) => entry.points.length >= 2)

  if (allSeries.length === 0) return

  const allTimestamps = allSeries.flatMap((entry) => entry.points.map((p) => p.timestampMs))
  const minT = Math.min(...allTimestamps)
  const maxT = Math.max(...allTimestamps)
  const timeRange = maxT - minT || 1

  // Grille.
  ctx.strokeStyle = 'rgba(255,255,255,0.08)'
  ctx.lineWidth = 1
  for (let i = 0; i <= 4; i++) {
    const y = (canvas.height / 4) * i
    ctx.beginPath()
    ctx.moveTo(0, y)
    ctx.lineTo(canvas.width, y)
    ctx.stroke()
  }

  for (const entry of allSeries) {
    const rawValues = entry.points.map((p) => p.numericValue)
    // Mode "Variations depuis le début" : normalise chaque série par sa
    // propre première valeur, clairement étiqueté -- ne prétend jamais que
    // deux séries ont la même amplitude réelle (voir tableau exact
    // ci-dessous pour les vraies valeurs).
    const values = displayMode.value === 'variations'
      ? rawValues.map((v) => v - rawValues[0])
      : rawValues
    const minV = Math.min(...values)
    const maxV = Math.max(...values)
    const valueRange = maxV - minV || 1

    ctx.strokeStyle = colorForIndex(entry.index)
    ctx.lineWidth = 2
    ctx.beginPath()
    entry.points.forEach((point, i) => {
      const x = ((point.timestampMs - minT) / timeRange) * canvas.width
      const y = canvas.height - ((values[i] - minV) / valueRange) * (canvas.height - 20) - 10
      if (i === 0) ctx.moveTo(x, y)
      else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }
}

watch([() => store.samplesBySeries, displayMode, () => store.activeSeries], async () => {
  await nextTick()
  drawChart()
}, { deep: true })

onMounted(() => {
  window.addEventListener('resize', drawChart)
})
onUnmounted(() => {
  window.removeEventListener('resize', drawChart)
})

// -- Marqueurs (16C) -----------------------------------------------------------

const markerText = ref('')
const markerBusy = ref(false)
const markerError = ref('')

async function handleAddMarker() {
  if (!markerText.value.trim() || markerBusy.value) return
  markerBusy.value = true
  markerError.value = ''
  try {
    const result = await store.addMarker(markerText.value)
    if (result.success) {
      markerText.value = ''
    } else {
      markerError.value = result.error ?? ''
    }
  } finally {
    markerBusy.value = false
  }
}

// -- Export (16C) ----------------------------------------------------------

const exportBusy = ref(false)
const exportMessage = ref('')

async function handleExport() {
  exportBusy.value = true
  exportMessage.value = ''
  try {
    const result = await store.exportComparison()
    exportMessage.value = result.success
      ? t('candidateComparison.exportSuccess', { path: result.filepath ?? '' })
      : (result.error ?? t('candidateComparison.exportFailed'))
  } finally {
    exportBusy.value = false
  }
}

// -- Consigner cette comparaison (16C partiel) --------------------------------
// AUDIT-PIPE-A8 : consigne désormais via store.recordObservation(), qui
// passe le captureId de la capture réellement démarrée (identité serveur
// figée) plutôt que le formulaire générique effectProofStore.recordProof()
// (identité recalculée depuis la session ACTUELLEMENT attachée -- pouvait
// attribuer une preuve sur la capture A à la cible B après un changement
// d'attache). Label/source/conditions sont désormais construits côté
// backend à partir de la provenance figée, plus depuis store.activeSeries
// (état live qui peut avoir changé). Niveau toujours "unverified" côté
// backend pour cette voie -- plus de sélecteur de niveau ici (il n'aurait
// plus d'effet réel).
const consignOpen = ref(false)
const consignNote = ref('')
const consignError = ref('')

function openConsignPanel() {
  consignNote.value = ''
  consignError.value = ''
  consignOpen.value = true
}

async function submitConsign() {
  consignError.value = ''
  const result = await store.recordObservation(consignNote.value)
  if (result.success === true) {
    consignOpen.value = false
  } else {
    consignError.value = result.error ?? ''
  }
}
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('candidateComparison.title') }}</h2>
        <InfoDot topic="candidateComparison" />
        <RiskBadge level="read" />
      </div>
    </div>
    <p class="panel-hint">{{ $t('candidateComparison.hint') }}</p>

    <!-- Sélection en attente -->
    <div v-if="!store.collecting && store.pendingSeries.length > 0" class="pending-series">
      <h3>{{ $t('candidateComparison.pendingTitle', { count: store.pendingSeries.length }) }}</h3>
      <div class="pending-list">
        <div v-for="(series, index) in store.pendingSeries" :key="series.id" class="pending-row">
          <span class="swatch" :style="{ backgroundColor: colorForIndex(index) }" />
          <span class="pending-label">{{ series.label }}</span>
          <span class="pending-meta">{{ series.type }} · x{{ series.factor }}</span>
          <button class="btn btn-secondary compact" type="button" @click="store.removeStagedById(series.id)">
            {{ $t('candidateComparison.remove') }}
          </button>
        </div>
      </div>
      <div class="start-controls">
        <label>
          {{ $t('candidateComparison.interval') }}
          <select v-model.number="store.intervalMs" class="input select compact-input">
            <option v-for="ms in intervalOptions" :key="ms" :value="ms">{{ ms }} ms</option>
          </select>
        </label>
        <label>
          {{ $t('candidateComparison.duration') }}
          <select v-model.number="store.maxDurationMs" class="input select compact-input">
            <option v-for="opt in durationOptions" :key="opt.value" :value="opt.value">{{ $t(opt.labelKey) }}</option>
          </select>
        </label>
        <button class="btn btn-primary compact" type="button" :disabled="!store.canStart || store.busy" @click="handleStart()">
          {{ $t('candidateComparison.start') }}
        </button>
        <button class="btn btn-secondary compact" type="button" @click="store.clearStaged()">
          {{ $t('candidateComparison.clear') }}
        </button>
      </div>
      <p v-if="store.error" class="error-text">{{ store.error }}</p>
    </div>

    <div v-else-if="!store.collecting && !store.hasResults" class="hint">
      {{ $t('candidateComparison.empty') }}
    </div>

    <!-- Comparaison active ou terminée avec résultats -->
    <template v-if="store.hasResults">
      <div class="comparison-status">
        <span>{{ store.collecting ? $t('candidateComparison.collecting') : $t('candidateComparison.stopped', { reason: store.stopReason }) }}</span>
        <span>{{ $t('candidateComparison.tourCount', { count: store.tourCount }) }}</span>
        <span v-if="store.skippedTicks > 0" class="warn-text">{{ $t('candidateComparison.skippedTicks', { count: store.skippedTicks }) }}</span>
        <button v-if="store.collecting" class="btn btn-secondary compact" type="button" @click="handleStop()">
          {{ $t('candidateComparison.stop') }}
        </button>
      </div>

      <div v-if="store.collecting" class="marker-add">
        <input
          v-model="markerText"
          type="text"
          class="input compact-input marker-input"
          maxlength="500"
          :placeholder="$t('candidateComparison.markerPlaceholder')"
          @keyup.enter="handleAddMarker()"
        >
        <button class="btn btn-secondary compact" type="button" :disabled="!markerText.trim() || markerBusy" @click="handleAddMarker()">
          {{ $t('candidateComparison.markerAdd') }}
        </button>
        <p v-if="markerError" class="error-text">{{ markerError }}</p>
      </div>

      <div v-if="store.markers.length > 0" class="markers-block">
        <h3>{{ $t('candidateComparison.markerListTitle', { count: store.markers.length }) }}</h3>
        <div v-for="marker in store.markers" :key="marker.id" class="marker-row">
          <span class="marker-time">+{{ marker.timestampMs }} ms</span>
          <span class="marker-text">{{ marker.text }}</span>
        </div>
      </div>

      <div class="display-mode-switch">
        <button
          type="button"
          class="btn btn-secondary compact"
          :class="{ active: displayMode === 'values' }"
          @click="displayMode = 'values'"
        >
          {{ $t('candidateComparison.modeValues') }}
        </button>
        <button
          type="button"
          class="btn btn-secondary compact"
          :class="{ active: displayMode === 'variations' }"
          @click="displayMode = 'variations'"
        >
          {{ $t('candidateComparison.modeVariations') }}
        </button>
      </div>

      <div ref="chartContainer" class="chart-container">
        <canvas ref="chartCanvas" />
      </div>

      <div class="series-table">
        <div v-for="stat in statsBySeries" :key="stat.id" class="series-row">
          <span class="swatch" :style="{ backgroundColor: stat.color }" />
          <span class="series-label">{{ stat.label }}</span>
          <span>{{ $t('candidateComparison.lastValue') }}: <strong>{{ stat.last?.scaledValueText ?? '-' }}</strong></span>
          <span>{{ $t('candidateComparison.deltaFromStart') }}: {{ stat.deltaFromFirst !== undefined ? stat.deltaFromFirst : '-' }}</span>
          <span>min {{ stat.minNumeric ?? '-' }} / max {{ stat.maxNumeric ?? '-' }}</span>
          <span>{{ $t('candidateComparison.validRate', { percent: stat.validRatePercent }) }}</span>
        </div>
      </div>

      <div v-if="store.correlations.length > 0" class="correlations-block">
        <h3>{{ $t('candidateComparison.correlationsTitle') }}</h3>
        <div v-for="corr in store.correlations" :key="`${corr.seriesIdA}-${corr.seriesIdB}`" class="correlation-row">
          <span>{{ corr.seriesIdA }} ↔ {{ corr.seriesIdB }}</span>
          <span v-if="corr.computable">
            {{ $t('candidateComparison.coefficient', { value: corr.coefficient.toFixed(3), count: corr.pairCount }) }}
          </span>
          <span v-else class="hint-inline">{{ $t('candidateComparison.notComputable') }} ({{ corr.reason }})</span>
        </div>
      </div>

      <div class="consign-block">
        <button v-if="!consignOpen" class="btn btn-secondary compact" type="button" :disabled="exportBusy" @click="handleExport()">
          {{ $t('candidateComparison.exportButton') }}
        </button>
        <p v-if="exportMessage" class="hint-inline">{{ exportMessage }}</p>
        <button v-if="!consignOpen" class="btn btn-secondary compact" type="button" @click="openConsignPanel()">
          {{ $t('candidateComparison.consignButton') }}
        </button>
        <div v-else class="consign-form">
          <h3>{{ $t('candidateComparison.consignTitle') }}</h3>
          <!-- AUDIT-PIPE-A8 : plus de sélecteur de niveau -- cette voie
               enregistre toujours "unverified" côté backend (aucune
               promotion automatique n'est jamais souhaitable pour une simple
               comparaison de valeurs, cf. UX-PRODUIT-16). -->
          <p class="hint-inline">{{ $t('candidateComparison.consignLevel') }}: {{ $t('investigation.effectProofLevelUnverified') }}</p>
          <textarea v-model="consignNote" class="input" :placeholder="$t('candidateComparison.consignNotePlaceholder')" rows="3" />
          <div class="consign-actions">
            <button class="btn btn-primary compact" type="button" :disabled="store.busy" @click="submitConsign()">
              {{ $t('candidateComparison.consignSubmit') }}
            </button>
            <button class="btn btn-secondary compact" type="button" @click="consignOpen = false">
              {{ $t('candidateComparison.cancel') }}
            </button>
          </div>
          <p v-if="consignError" class="error-text">{{ consignError }}</p>
        </div>
      </div>
    </template>
  </section>
</template>

<style scoped>
.pending-series, .comparison-status, .display-mode-switch, .series-table, .correlations-block, .consign-block, .marker-add, .markers-block {
  margin-top: 12px;
}
.marker-add {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}
.marker-input {
  flex: 1;
  min-width: 200px;
}
.markers-block {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.marker-row {
  display: flex;
  align-items: baseline;
  gap: 10px;
}
.marker-time {
  color: var(--text-muted, #888);
  font-variant-numeric: tabular-nums;
  min-width: 80px;
}
.pending-list, .series-table {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.pending-row, .series-row, .correlation-row {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}
.swatch {
  display: inline-block;
  width: 12px;
  height: 12px;
  border-radius: 2px;
  flex-shrink: 0;
}
.pending-label, .series-label {
  font-weight: 600;
  min-width: 160px;
}
.pending-meta {
  color: var(--text-muted, #888);
}
.start-controls, .comparison-status, .display-mode-switch, .consign-actions {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}
.chart-container {
  width: 100%;
  margin-top: 10px;
}
.chart-container canvas {
  width: 100%;
  height: 220px;
  display: block;
}
.warn-text {
  color: var(--warning, #e0a030);
}
.error-text {
  color: var(--danger, #e05050);
}
.hint-inline {
  color: var(--text-muted, #888);
  font-size: 0.9em;
}
.consign-form {
  display: flex;
  flex-direction: column;
  gap: 8px;
  max-width: 480px;
}
.btn.active {
  border-color: var(--accent, #4CAF50);
}
</style>
