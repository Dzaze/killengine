<script setup lang="ts">
import { computed, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const { locale } = useI18n()

const runtimeRows = computed(() => [
  { label: 'Backend', value: store.isConnected ? 'connecté' : 'déconnecté' },
  { label: 'Processus', value: store.isAttached ? store.processName : 'aucun' },
  { label: 'Version', value: store.version },
  { label: 'Workflow', value: store.workflowStatus },
])

const debugEvents = computed(() => [...store.smartSearchDebugEvents].reverse())
const valueTypes = ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64']
const performanceModes = ['Auto', 'Eco', 'Normal', 'Performance', 'Max']
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096, 8192]
const unknownDepthLabel = (mb: number) => (mb === -1 ? 'Auto' : `${mb} Mo`)

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} Go`
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} Mo`
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} Ko`
  return `${bytes} o`
}

function eventSummary(event: Record<string, unknown>) {
  const parts = [
    event.intent ? `intent=${String(event.intent)}` : '',
    event.tool ? `tool=${String(event.tool)}` : '',
    event.workflowStatus ? `workflow=${String(event.workflowStatus)}` : '',
    event.actionStatus ? `action=${String(event.actionStatus)}` : '',
    event.candidateCount !== undefined ? `candidats=${String(event.candidateCount)}` : '',
    event.targetValue ? `cible=${String(event.targetValue)}` : '',
  ].filter(Boolean)
  return parts.join('  ')
}

async function refreshAll() {
  await store.doPing()
  await store.loadSettings()
  await store.refreshDiagnostics()
}

onMounted(() => {
  void refreshAll()
})

watch(
  () => store.appLanguage,
  (language) => {
    locale.value = language
  },
)

watch(
  () => locale.value,
  (language) => {
    store.appLanguage = language === 'en' ? 'en' : 'fr'
  },
  { immediate: true },
)

async function saveAll() {
  locale.value = store.appLanguage
  await store.saveSettings()
}
</script>

<template>
  <div class="settings-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.settings') }}</h1>
        <p>{{ store.statusText }}</p>
      </div>
      <button class="btn btn-secondary" @click="refreshAll">
        Rafraîchir
      </button>
    </div>

    <section class="panel">
      <div class="panel-title">
        <h2>Interface</h2>
      </div>
      <div class="setting-row">
        <div>
          <strong>Langue</strong>
          <span>{{ store.appLanguage === 'fr' ? 'Français' : 'English' }}</span>
        </div>
        <div class="segmented">
          <button :class="{ active: store.appLanguage === 'fr' }" @click="store.appLanguage = 'fr'">FR</button>
          <button :class="{ active: store.appLanguage === 'en' }" @click="store.appLanguage = 'en'">EN</button>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Scan</h2>
      </div>
      <div class="settings-grid">
        <label>
          <span>Type par défaut</span>
          <select v-model="store.settingDefaultValueType" class="input select">
            <option v-for="type in valueTypes" :key="type">{{ type }}</option>
          </select>
        </label>
        <label>
          <span>Résultats maximum</span>
          <input v-model.number="store.settingScanMaxResults" class="input" type="number" min="1000" max="10000000" step="1000" />
        </label>
        <label>
          <span>Mode performance</span>
          <select v-model="store.settingPerformanceMode" class="input select">
            <option v-for="mode in performanceModes" :key="mode">{{ mode }}</option>
          </select>
        </label>
        <label>
          <span>Chunk mémoire (Mo, 0 = Auto)</span>
          <input v-model.number="store.settingScanChunkSizeMb" class="input" type="number" min="0" max="64" step="1" />
        </label>
        <label>
          <span>Threads max (0 = Auto)</span>
          <input v-model.number="store.settingScanMaxWorkerThreads" class="input" type="number" min="0" max="128" step="1" />
        </label>
        <label>
          <span>Mémoire scan en vol (Mo, 0 = Auto)</span>
          <input v-model.number="store.settingScanMaxInFlightMb" class="input" type="number" min="0" max="32768" step="64" />
        </label>
        <label>
          <span>Seuil fichier candidats</span>
          <input v-model.number="store.settingCandidateFileBackedThreshold" class="input" type="number" min="1" max="5000000" step="1000" />
        </label>
        <label>
          <span>Snapshot unknown max</span>
          <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select">
            <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
          </select>
        </label>
        <label class="toggle-row">
          <input v-model="store.settingFastScan" type="checkbox" />
          <span>Fast scan par défaut</span>
        </label>
      </div>
      <p class="hint">
        Gros process : mets le seuil fichier à 1 pour purger la RAM plus tôt. Les fichiers temporaires sont supprimés au nouveau scan ou à la fermeture.
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Stockage temporaire</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" :disabled="store.scanBusy" @click="store.refreshTemporaryStorageStatus()">
            Actualiser
          </button>
          <button class="btn btn-primary compact" :disabled="store.scanBusy" @click="store.clearTemporaryStorage()">
            Nettoyer maintenant
          </button>
        </div>
      </div>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>Total</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.totalBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Actif</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.activeBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Orphelins</span>
          <strong>{{ store.temporaryStorageStatus?.orphanFileCount ?? 0 }} · {{ formatBytes(store.temporaryStorageStatus?.orphanBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>Fichiers actifs</span>
          <strong>{{ store.temporaryStorageStatus?.activeFileCount ?? 0 }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>Dossier temp</span>
        <code>{{ store.temporaryStorageStatus?.tempPath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Candidats</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.candidateBytes) }} · {{ store.temporaryStorageStatus?.candidateFileBacked ? 'fichier' : 'RAM' }}</code>
      </div>
      <div class="path-row">
        <span>Undo / snapshot</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.undoBytes) }} / {{ formatBytes(store.temporaryStorageStatus?.snapshotBytes) }}</code>
      </div>
      <p v-if="store.temporaryStorageCleanupResult" class="status-line">
        {{ store.temporaryStorageCleanupResult.message || (store.temporaryStorageCleanupResult.success ? 'Nettoyage terminé.' : store.temporaryStorageCleanupResult.error) }}
      </p>
      <p v-if="store.temporaryStorageError" class="error">{{ store.temporaryStorageError }}</p>
      <p class="hint">
        Le nettoyage ferme le contexte de scan courant, vide l'undo et le snapshot unknown, puis supprime les fichiers `killengine_candidates_*.kecand` et `killengine_snapshot_*.kesnap` restants.
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>IA locale</h2>
      </div>
      <div class="settings-grid">
        <label class="wide">
          <span>Chemin modèle GGUF</span>
          <input v-model="store.settingModelPath" class="input" placeholder="Optionnel : chemin complet vers qwen.gguf" />
        </label>
        <label>
          <span>Threads modèle</span>
          <input v-model.number="store.settingModelThreads" class="input" type="number" min="1" max="32" step="1" />
        </label>
      </div>
      <p class="hint">
        Le runtime actuel continue d'utiliser la détection automatique ou KILLENGINE_QWEN_GGUF ; ce champ prépare le Model Manager.
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>État</h2>
      </div>
      <div class="runtime-grid">
        <div v-for="row in runtimeRows" :key="row.label" class="runtime-cell">
          <span>{{ row.label }}</span>
          <strong>{{ row.value }}</strong>
        </div>
      </div>
      <div class="ping-line">
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
        <code>{{ store.pingResult || '-' }}</code>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Diagnostic</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" @click="store.refreshLogTail()">
            Logs
          </button>
          <button class="btn btn-secondary compact" @click="store.exportDiagnostics()">
            Exporter
          </button>
        </div>
      </div>
      <div class="path-row">
        <span>Log</span>
        <code>{{ store.logFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Smart Search JSON</span>
        <code>{{ store.smartSearchDebugFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Scan telemetry JSON</span>
        <code>{{ store.scanTelemetryFilePath || '-' }}</code>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>Debug Smart Search</strong>
          <span>{{ store.settingSmartSearchDebugEnabled ? 'activé' : 'désactivé' }}</span>
        </div>
        <label class="toggle-row">
          <input v-model="store.settingSmartSearchDebugEnabled" type="checkbox" />
          <span>Écrire le JSONL</span>
        </label>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>Événements affichés</strong>
          <span>{{ store.settingSmartSearchDebugMaxEvents }}</span>
        </div>
        <input v-model.number="store.settingSmartSearchDebugMaxEvents" class="input short-input" type="number" min="5" max="200" step="5" />
      </div>
      <p v-if="store.diagnosticExportPath" class="status-line">
        Diagnostic exporté : <code>{{ store.diagnosticExportPath }}</code>
      </p>
      <p v-if="store.diagnosticExportError" class="error">{{ store.diagnosticExportError }}</p>
      <p v-if="store.diagnosticOpenFolderError" class="warning">{{ store.diagnosticOpenFolderError }}</p>
      <p v-if="store.logError" class="error">{{ store.logError }}</p>
      <p v-if="store.smartSearchDebugError" class="error">{{ store.smartSearchDebugError }}</p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Log principal</h2>
        <span>{{ store.logLines.length }}</span>
      </div>
      <div v-if="store.logLines.length === 0" class="empty-line">
        Aucun log chargé.
      </div>
      <pre v-else class="log-viewer">{{ store.logLines.join('\n') }}</pre>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Événements Smart Search</h2>
        <div class="panel-actions">
          <span>{{ debugEvents.length }}</span>
          <button class="btn btn-secondary compact" :disabled="debugEvents.length === 0" @click="store.clearSmartSearchDebug()">
            Vider
          </button>
        </div>
      </div>
      <div v-if="debugEvents.length === 0" class="empty-line">
        Aucun événement debug enregistré.
      </div>
      <div v-else class="debug-list">
        <div v-for="(event, index) in debugEvents" :key="index" class="debug-row">
          <div class="debug-head">
            <strong>{{ String(event.event ?? '-') }}</strong>
            <span>{{ String(event.timestamp ?? '') }}</span>
          </div>
          <code v-if="event.query">{{ event.query }}</code>
          <p v-if="eventSummary(event)">{{ eventSummary(event) }}</p>
          <p v-if="event.intentRationale">{{ event.intentRationale }}</p>
          <p v-if="event.message">{{ event.message }}</p>
          <p v-if="event.error" class="error">{{ event.error }}</p>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Session</h2>
      </div>
      <div class="actions-row">
        <button class="btn btn-primary" :disabled="store.settingsSaving" @click="saveAll">
          {{ store.settingsSaving ? 'Sauvegarde...' : 'Sauvegarder les paramètres' }}
        </button>
        <button class="btn btn-secondary" @click="store.resetWorkflow()">
          Réinitialiser le workflow
        </button>
        <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.detach()">
          {{ $t('process.detach') }}
        </button>
      </div>
      <p v-if="store.settingsStatus" class="status-line">{{ store.settingsStatus }}</p>
    </section>
  </div>
</template>

<style scoped>
.settings-view {
  max-width: 980px;
  padding: 24px 32px;
}

.header,
.panel-title,
.setting-row,
.ping-line,
.actions-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.panel-title span,
.empty-line {
  color: var(--text-dim);
  font-size: 12px;
}

.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.header {
  margin-bottom: 18px;
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

.panel {
  margin-bottom: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.panel-title {
  margin-bottom: 12px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.setting-row strong,
.runtime-cell strong {
  display: block;
  color: var(--text-primary);
  font-size: 14px;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.settings-grid label,
.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.settings-grid label span,
.toggle-row span,
.hint,
.status-line {
  color: var(--text-dim);
  font-size: 12px;
}

.input {
  min-width: 0;
  width: 100%;
  min-height: 34px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  transition: border-color 0.15s, box-shadow 0.15s, background 0.15s;
}

.input::placeholder {
  color: var(--text-dim);
}

.input:focus {
  border-color: rgba(122, 162, 247, 0.8);
  box-shadow: 0 0 0 2px rgba(122, 162, 247, 0.15);
}

.input:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.input[type="number"] {
  appearance: textfield;
  -moz-appearance: textfield;
}

.input[type="number"]::-webkit-outer-spin-button,
.input[type="number"]::-webkit-inner-spin-button {
  margin: 0;
  appearance: none;
  -webkit-appearance: none;
}

.select {
  cursor: pointer;
}

.select option {
  background: var(--bg-primary);
  color: var(--text-primary);
}

.settings-grid .wide {
  grid-column: span 2;
}

.toggle-row {
  align-items: flex-start;
  justify-content: center;
}

.settings-grid .toggle-row,
.inline-setting .toggle-row {
  flex-direction: row;
  align-items: center;
  justify-content: flex-start;
  min-height: 34px;
}

.toggle-row input {
  width: 15px;
  height: 15px;
  margin: 0;
  accent-color: var(--accent);
}

.inline-setting {
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.short-input {
  max-width: 120px;
}

.hint,
.status-line {
  margin-top: 10px;
}

.setting-row span,
.runtime-cell span,
.path-row span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.segmented {
  display: inline-flex;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.segmented button {
  min-width: 42px;
  padding: 6px 10px;
  border: none;
  border-radius: 4px;
  background: transparent;
  color: var(--text-dim);
  cursor: pointer;
}

.segmented button.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.runtime-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
}

.runtime-cell {
  min-height: 72px;
  padding: 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.runtime-cell strong {
  overflow: hidden;
  margin-top: 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ping-line {
  margin-top: 12px;
}

.path-row {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  gap: 12px;
  align-items: center;
  padding: 8px 0;
  border-top: 1px solid var(--border);
}

.path-row:first-of-type {
  border-top: none;
}

.debug-list {
  display: flex;
  max-height: 360px;
  flex-direction: column;
  gap: 6px;
  overflow-y: auto;
}

.log-viewer {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.debug-row {
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.debug-head {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 5px;
}

.debug-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.debug-head span,
.debug-row p {
  color: var(--text-dim);
  font-size: 12px;
}

.debug-row p {
  margin-top: 5px;
}

.error {
  color: var(--error) !important;
}

code {
  overflow-wrap: anywhere;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}

.compact {
  padding: 5px 9px;
  font-size: 12px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.btn-primary {
  background: var(--accent);
  color: #0b1020;
}

@media (max-width: 850px) {
  .runtime-grid,
  .path-row,
  .settings-grid {
    grid-template-columns: 1fr;
  }

  .setting-row,
  .ping-line,
  .actions-row {
    align-items: stretch;
    flex-direction: column;
  }
}
</style>
