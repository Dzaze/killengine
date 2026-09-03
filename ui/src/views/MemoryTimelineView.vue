<template>
  <div class="memory-timeline-view">
    <PanelIntro
      what="Un visualiseur de l'évolution temporelle des valeurs mémoire — courbe de valeur au fil du temps pour un ensemble d'adresses surveillées."
      purpose="Repérer des cycles (timers, animations), des adresses volatiles vs stables, et suivre visuellement comment une valeur évolue pendant que le jeu tourne."
      how="Ajoute une ou plusieurs adresses, démarre la collecte, puis sélectionne une adresse pour voir sa courbe. Détection de patterns, profil comportemental, prédiction, corrélations entre adresses et rapport texte disponibles depuis l'onglet Analyse rapide."
    />

    <!-- Configuration Panel -->
    <div class="config-panel">
      <h3>Configuration</h3>
      <div class="config-grid">
        <div class="config-item">
          <label>Intervalle d'échantillonnage (ms)</label>
          <input
            v-model.number="config.samplingIntervalMs"
            type="number"
            min="10"
            max="5000"
            :disabled="isCollecting"
          />
        </div>
        <div class="config-item">
          <label>Durée max (ms)</label>
          <input
            v-model.number="config.maxDurationMs"
            type="number"
            min="1000"
            max="300000"
            :disabled="isCollecting"
          />
        </div>
        <div class="config-item checkbox">
          <label>
            <input
              v-model="config.trackOnlyChanges"
              type="checkbox"
              :disabled="isCollecting"
            />
            Ne tracker que les changements
          </label>
        </div>
      </div>
    </div>

    <!-- Address Management -->
    <div class="address-panel">
      <h3>Adresses à surveiller ({{ watchedAddresses.length }})</h3>
      <div class="address-input">
        <input
          v-model="newAddress"
          placeholder="0x..."
          @keyup.enter="addAddress"
        />
        <select v-model="newAddressSize">
          <option :value="4">Int32 (4 bytes)</option>
          <option :value="8">Int64 (8 bytes)</option>
          <option :value="2">Int16 (2 bytes)</option>
          <option :value="1">Byte (1 byte)</option>
        </select>
        <button @click="addAddress" :disabled="!newAddress">Ajouter</button>
      </div>
      
      <div class="address-list">
        <div
          v-for="addr in watchedAddresses"
          :key="addr"
          class="address-item"
          :class="{ active: selectedAddress === addr }"
          @click="selectAddress(addr)"
        >
          <span class="address-hex">{{ addr }}</span>
          <button class="remove-btn" @click.stop="removeAddress(addr)">×</button>
        </div>
      </div>
    </div>

    <!-- Control Panel -->
    <div class="control-panel">
      <button
        class="btn-primary"
        @click="startCollection"
        :disabled="isCollecting || watchedAddresses.length === 0"
      >
        {{ isCollecting ? 'Collecte en cours...' : 'Démarrer' }}
      </button>
      <button
        class="btn-secondary"
        @click="stopCollection"
        :disabled="!isCollecting"
      >
        Arrêter
      </button>
      <button class="btn-secondary" @click="clearAll">Tout effacer</button>
      <button class="btn-secondary" @click="exportData">Exporter JSON</button>
    </div>

    <!-- Progress -->
    <div v-if="isCollecting" class="progress-panel">
      <div class="progress-bar">
        <div class="progress-fill" :style="{ width: progressPercent + '%' }"></div>
      </div>
      <span class="progress-text">{{ progressStatus }}</span>
    </div>

    <!-- Stats Panel -->
    <div v-if="stats.watchedAddressCount > 0" class="stats-panel">
      <h3>Statistiques</h3>
      <div class="stats-grid">
        <div class="stat-item">
          <span class="stat-label">Points collectés</span>
          <span class="stat-value">{{ stats.totalDataPoints }}</span>
        </div>
        <div class="stat-item">
          <span class="stat-label">Adresses</span>
          <span class="stat-value">{{ stats.watchedAddressCount }}</span>
        </div>
      </div>
    </div>

    <!-- Timeline Chart -->
    <div v-if="selectedSeries" class="chart-panel">
      <h3>Timeline - {{ selectedAddress }}</h3>
      <div class="chart-container" ref="chartContainer">
        <canvas ref="timelineCanvas" @mousemove="onChartHover" @mouseleave="onChartLeave"></canvas>
        <div v-if="hoverData" class="chart-tooltip" :style="tooltipStyle">
          <div>t: {{ hoverData.timestampMs }}ms</div>
          <div>val: {{ hoverData.valueHex }}</div>
        </div>
      </div>
      
      <!-- Series Stats -->
      <div class="series-stats">
        <div class="stat">
          <span class="label">Changements:</span>
          <span class="value">{{ selectedSeries.changeCount }}</span>
        </div>
        <div class="stat">
          <span class="label">Volatilité:</span>
          <span class="value" :class="volatilityClass">{{ (selectedSeries.volatilityScore * 100).toFixed(1) }}%</span>
        </div>
        <div class="stat">
          <span class="label">Intervalle moyen:</span>
          <span class="value">{{ selectedSeries.averageIntervalMs.toFixed(0) }}ms</span>
        </div>
      </div>
    </div>

    <!-- Pattern Detection -->
    <div v-if="selectedAddress" class="patterns-panel">
      <h3>Patterns détectés</h3>
      <button class="btn-small" @click="detectPatterns">Analyser</button>
      <div v-if="patterns.length > 0" class="patterns-list">
        <div
          v-for="(pattern, idx) in patterns"
          :key="idx"
          class="pattern-item"
          :class="'pattern-' + pattern.type"
        >
          <span class="pattern-type">{{ getPatternTypeName(pattern.type) }}</span>
          <span class="pattern-confidence">{{ (pattern.confidence * 100).toFixed(0) }}%</span>
          <span class="pattern-desc">{{ pattern.description }}</span>
        </div>
      </div>
      <div v-else-if="analyzed" class="no-patterns">
        Aucun pattern significatif détecté
      </div>
    </div>

    <!-- Quick Actions -->
    <div class="quick-actions">
      <h3>Actions rapides</h3>
      <div class="action-buttons">
        <button @click="findVolatile">Trouver volatiles</button>
        <button @click="findStable">Trouver stables</button>
        <button @click="analyzeBehavior">Profil comportemental</button>
        <button @click="predictNext">Prédiction</button>
        <button @click="findCorrelations">Corrélations</button>
        <button @click="generateReport">Rapport texte</button>
      </div>
      
      <!-- Results -->
      <div v-if="quickResults.length > 0" class="quick-results">
        <div
          v-for="(result, idx) in quickResults"
          :key="idx"
          class="result-item"
        >
          {{ result }}
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue'
import { useAppStore } from '../stores/app'
import PanelIntro from '../components/common/PanelIntro.vue'

const store = useAppStore()

// State
const config = ref({
  samplingIntervalMs: 100,
  maxDurationMs: 60000,
  trackOnlyChanges: true
})

const newAddress = ref('')
const newAddressSize = ref(4)
const watchedAddresses = ref<string[]>([])
const selectedAddress = ref('')
const isCollecting = ref(false)
const progressPercent = ref(0)
const progressStatus = ref('')
const stats = ref({
  watchedAddressCount: 0,
  totalDataPoints: 0
})

const selectedSeries = ref<any>(null)
const patterns = ref<any[]>([])
const analyzed = ref(false)
const quickResults = ref<string[]>([])

// Chart
const chartContainer = ref<HTMLElement>()
const timelineCanvas = ref<HTMLCanvasElement>()
const hoverData = ref<any>(null)
const tooltipStyle = ref({})

// Pattern type names
const patternTypeNames: Record<number, string> = {
  0: 'Inconnu',
  1: 'Constant',
  2: 'Step',
  3: 'Linéaire',
  4: 'Cyclique',
  5: 'Aléatoire',
  6: 'Corrélé',
  7: 'Anti-cheat'
}

function getPatternTypeName(type: number): string {
  return patternTypeNames[type] || 'Inconnu'
}

const volatilityClass = computed(() => {
  if (!selectedSeries.value) return ''
  const score = selectedSeries.value.volatilityScore
  if (score < 0.3) return 'low'
  if (score < 0.7) return 'medium'
  return 'high'
})

// Methods
async function addAddress() {
  if (!newAddress.value) return
  
  // Normalize address
  let addr = newAddress.value.trim()
  if (!addr.startsWith('0x')) {
    addr = '0x' + addr
  }
  
  if (watchedAddresses.value.includes(addr)) {
    store.addActionLog('memory_timeline', 'Adresse déjà surveillée', addr, 'warning')
    return
  }
  
  const success = await store.addTimelineAddress(addr, newAddressSize.value)
  if (success) {
    watchedAddresses.value.push(addr)
    newAddress.value = ''
  }
}

async function removeAddress(addr: string) {
  await store.removeTimelineAddress(addr)
  watchedAddresses.value = watchedAddresses.value.filter(a => a !== addr)
  if (selectedAddress.value === addr) {
    selectedAddress.value = ''
    selectedSeries.value = null
  }
}

function selectAddress(addr: string) {
  selectedAddress.value = addr
  loadSeriesData()
}

async function loadSeriesData() {
  if (!selectedAddress.value) return
  selectedSeries.value = await store.getTimelineSeries(selectedAddress.value)
  nextTick(() => drawChart())
}

async function startCollection() {
  // Apply config
  await store.setTimelineConfig(config.value)
  
  const success = await store.startTimelineCollection()
  if (success) {
    isCollecting.value = true
    progressPercent.value = 0
    progressStatus.value = 'Démarrage...'
  }
}

async function stopCollection() {
  await store.stopTimelineCollection()
  isCollecting.value = false
  loadSeriesData()
}

function clearAll() {
  store.clearTimelineAddresses()
  watchedAddresses.value = []
  selectedAddress.value = ''
  selectedSeries.value = null
  patterns.value = []
}

async function exportData() {
  const filepath = await store.exportTimelineToJson()
  if (filepath) {
    store.addActionLog('memory_timeline', 'Timeline exportée', filepath, 'success')
  }
}

async function detectPatterns() {
  if (!selectedAddress.value) return
  analyzed.value = true
  patterns.value = await store.detectTimelinePatterns(selectedAddress.value)
}

async function findVolatile() {
  const addresses = await store.findVolatileTimelineAddresses(0.5)
  quickResults.value = addresses.map((a: string) => `Volatile: ${a}`)
}

async function findStable() {
  const addresses = await store.findStableTimelineAddresses(5000)
  quickResults.value = addresses.map((a: string) => `Stable: ${a}`)
}

async function analyzeBehavior() {
  if (!selectedAddress.value) return
  const profile = await store.analyzeTimelineBehavior(selectedAddress.value)
  quickResults.value = [
    `Changes/sec: ${Number(profile.changesPerSecond).toFixed(2)}`,
    `Régularité: ${(Number(profile.regularityScore) * 100).toFixed(0)}%`,
    `Burst: ${profile.hasBurstBehavior ? 'Oui' : 'Non'}`
  ]
}

async function predictNext() {
  if (!selectedAddress.value) return
  const prediction = await store.predictTimelineNextValue(selectedAddress.value)
  quickResults.value = [
    `Probabilité de changement: ${(Number(prediction.changeProbability) * 100).toFixed(1)}%`,
    `Valeur prédite: ${String(prediction.predictedValueHex)}`
  ]
}

async function findCorrelations() {
  const correlations = await store.findTimelineCorrelations()
  quickResults.value = correlations.length > 0
    ? correlations.map((c: any) =>
        `${c.addressA} ↔ ${c.addressB} : r=${Number(c.pearsonCoefficient).toFixed(2)}` +
        (c.timeLagMs ? `, décalage ${Number(c.timeLagMs).toFixed(0)}ms` : '') +
        (c.isLeading ? ' (A mène B)' : ''))
    : ['Aucune corrélation trouvée (au moins 2 adresses surveillées avec des données requises)']
}

async function generateReport() {
  const report = await store.generateTimelineReport()
  quickResults.value = report ? report.split('\n').filter(line => line.trim() !== '') : ['Rapport non disponible']
}

// Chart drawing
function drawChart() {
  const canvas = timelineCanvas.value
  const container = chartContainer.value
  if (!canvas || !container || !selectedSeries.value) return
  
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  
  // Resize canvas
  canvas.width = container.clientWidth
  canvas.height = 200
  
  const points = selectedSeries.value.points || []
  if (points.length < 2) return
  
  // Clear
  ctx.clearRect(0, 0, canvas.width, canvas.height)
  
  // Calculate scales
  const timestamps = points.map((p: any) => p.timestampMs)
  const minT = Math.min(...timestamps)
  const maxT = Math.max(...timestamps)
  const timeRange = maxT - minT || 1
  
  // Extract numeric values for Y scale
  const values = points.map((p: any) => {
    const hex = p.valueHex.replace('0x', '')
    return parseInt(hex, 16) || 0
  })
  const minV = Math.min(...values)
  const maxV = Math.max(...values)
  const valueRange = maxV - minV || 1
  
  // Draw grid
  ctx.strokeStyle = '#333'
  ctx.lineWidth = 1
  ctx.beginPath()
  for (let i = 0; i <= 4; i++) {
    const y = (canvas.height / 4) * i
    ctx.moveTo(0, y)
    ctx.lineTo(canvas.width, y)
  }
  ctx.stroke()
  
  // Draw line
  ctx.strokeStyle = '#4CAF50'
  ctx.lineWidth = 2
  ctx.beginPath()
  
  points.forEach((point: any, i: number) => {
    const x = ((point.timestampMs - minT) / timeRange) * canvas.width
    const v = parseInt(point.valueHex.replace('0x', ''), 16) || 0
    const y = canvas.height - ((v - minV) / valueRange) * (canvas.height - 20) - 10
    
    if (i === 0) {
      ctx.moveTo(x, y)
    } else {
      ctx.lineTo(x, y)
    }
  })
  
  ctx.stroke()
  
  // Draw points
  ctx.fillStyle = '#4CAF50'
  points.forEach((point: any) => {
    const x = ((point.timestampMs - minT) / timeRange) * canvas.width
    const v = parseInt(point.valueHex.replace('0x', ''), 16) || 0
    const y = canvas.height - ((v - minV) / valueRange) * (canvas.height - 20) - 10
    
    ctx.beginPath()
    ctx.arc(x, y, 3, 0, Math.PI * 2)
    ctx.fill()
  })
}

function onChartHover(e: MouseEvent) {
  if (!selectedSeries.value || !timelineCanvas.value) return
  
  const canvas = timelineCanvas.value
  const rect = canvas.getBoundingClientRect()
  const x = e.clientX - rect.left
  
  const points = selectedSeries.value.points || []
  if (points.length < 2) return
  
  // Find closest point
  const timestamps = points.map((p: any) => p.timestampMs)
  const minT = Math.min(...timestamps)
  const maxT = Math.max(...timestamps)
  const timeRange = maxT - minT || 1
  
  const targetT = minT + (x / canvas.width) * timeRange
  
  let closest = points[0]
  let minDiff = Math.abs(points[0].timestampMs - targetT)
  
  for (const p of points) {
    const diff = Math.abs(p.timestampMs - targetT)
    if (diff < minDiff) {
      minDiff = diff
      closest = p
    }
  }
  
  if (minDiff < timeRange / points.length * 2) {
    hoverData.value = closest
    tooltipStyle.value = {
      left: `${x + 10}px`,
      top: `${e.clientY - rect.top - 40}px`
    }
  } else {
    hoverData.value = null
  }
}

function onChartLeave() {
  hoverData.value = null
}

// Lifecycle
onMounted(() => {
  // Load initial state
  store.getTimelineWatchedAddresses().then(addrs => {
    watchedAddresses.value = addrs
  })
  
  // Listen for updates
  window.addEventListener('timeline-data', ((e: CustomEvent) => {
    if (e.detail.address === selectedAddress.value) {
      loadSeriesData()
    }
  }) as EventListener)
  
  window.addEventListener('timeline-progress', ((e: CustomEvent) => {
    progressPercent.value = e.detail.percent
    progressStatus.value = e.detail.status
  }) as EventListener)
  
  window.addEventListener('timeline-finished', () => {
    isCollecting.value = false
    loadSeriesData()
  })
})

onUnmounted(() => {
  if (isCollecting.value) {
    stopCollection()
  }
})

watch(selectedAddress, loadSeriesData)
</script>

<style scoped>
.memory-timeline-view {
  padding: 20px;
  max-width: 1200px;
  margin: 0 auto;
}

.config-panel,
.address-panel,
.control-panel,
.progress-panel,
.stats-panel,
.chart-panel,
.patterns-panel,
.quick-actions {
  background: var(--panel-bg, #1a1a2e);
  border-radius: 8px;
  padding: 16px;
  margin-bottom: 16px;
}

.config-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: 16px;
}

.config-item {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.config-item.checkbox {
  flex-direction: row;
  align-items: center;
  gap: 8px;
}

.config-item label {
  font-size: 12px;
  color: #888;
}

.config-item input[type="number"] {
  padding: 8px;
  border: 1px solid #333;
  border-radius: 4px;
  background: #0f0f1a;
  color: #fff;
}

.address-input {
  display: flex;
  gap: 8px;
  margin-bottom: 12px;
}

.address-input input {
  flex: 1;
  padding: 8px 12px;
  border: 1px solid #333;
  border-radius: 4px;
  background: #0f0f1a;
  color: #fff;
}

.address-input select {
  padding: 8px;
  border: 1px solid #333;
  border-radius: 4px;
  background: #0f0f1a;
  color: #fff;
}

.address-list {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.address-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 12px;
  background: #252540;
  border-radius: 4px;
  cursor: pointer;
  transition: background 0.2s;
}

.address-item:hover,
.address-item.active {
  background: #3a3a60;
}

.address-hex {
  font-family: monospace;
  font-size: 13px;
}

.remove-btn {
  background: none;
  border: none;
  color: #f44336;
  cursor: pointer;
  font-size: 18px;
  padding: 0;
  width: 20px;
  height: 20px;
  display: flex;
  align-items: center;
  justify-content: center;
}

.control-panel {
  display: flex;
  gap: 12px;
  flex-wrap: wrap;
}

button {
  padding: 10px 20px;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 14px;
  transition: opacity 0.2s;
}

button:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.btn-primary {
  background: #4CAF50;
  color: white;
}

.btn-secondary {
  background: #333;
  color: white;
}

.btn-small {
  padding: 6px 12px;
  font-size: 12px;
  background: #2196F3;
  color: white;
}

.progress-bar {
  height: 8px;
  background: #333;
  border-radius: 4px;
  overflow: hidden;
  margin-bottom: 8px;
}

.progress-fill {
  height: 100%;
  background: linear-gradient(90deg, #4CAF50, #8BC34A);
  transition: width 0.3s;
}

.progress-text {
  font-size: 12px;
  color: #888;
}

.stats-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 16px;
}

.stat-item {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.stat-label {
  font-size: 12px;
  color: #888;
}

.stat-value {
  font-size: 24px;
  font-weight: bold;
  color: #4CAF50;
}

.chart-container {
  position: relative;
  height: 200px;
  background: #0f0f1a;
  border-radius: 4px;
  margin: 16px 0;
}

.chart-container canvas {
  width: 100%;
  height: 100%;
}

.chart-tooltip {
  position: absolute;
  background: rgba(0, 0, 0, 0.9);
  border: 1px solid #333;
  border-radius: 4px;
  padding: 8px;
  font-size: 12px;
  pointer-events: none;
  z-index: 100;
}

.series-stats {
  display: flex;
  gap: 24px;
  flex-wrap: wrap;
}

.series-stats .stat {
  display: flex;
  gap: 8px;
}

.series-stats .label {
  color: #888;
}

.series-stats .value {
  font-weight: bold;
}

.series-stats .value.low { color: #4CAF50; }
.series-stats .value.medium { color: #FF9800; }
.series-stats .value.high { color: #f44336; }

.patterns-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
  margin-top: 12px;
}

.pattern-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px;
  background: #252540;
  border-radius: 4px;
  border-left: 3px solid #666;
}

.pattern-1 { border-left-color: #4CAF50; } /* Constant */
.pattern-2 { border-left-color: #2196F3; } /* Step */
.pattern-3 { border-left-color: #9C27B0; } /* Linear */
.pattern-4 { border-left-color: #FF9800; } /* Cyclic */
.pattern-7 { border-left-color: #f44336; } /* Anti-cheat */

.pattern-type {
  font-weight: bold;
  min-width: 100px;
}

.pattern-confidence {
  background: #333;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
}

.pattern-desc {
  color: #888;
  font-size: 13px;
}

.no-patterns {
  color: #666;
  font-style: italic;
  margin-top: 12px;
}

.action-buttons {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  margin-bottom: 12px;
}

.action-buttons button {
  padding: 8px 16px;
  background: #3a3a60;
  color: white;
}

.quick-results {
  background: #0f0f1a;
  border-radius: 4px;
  padding: 12px;
}

.result-item {
  padding: 6px 0;
  border-bottom: 1px solid #333;
  font-family: monospace;
  font-size: 13px;
}

.result-item:last-child {
  border-bottom: none;
}

h3 {
  margin: 0 0 16px 0;
  font-size: 16px;
  color: #fff;
}
</style>
