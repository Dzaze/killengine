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
const valueTypes = ['Int32', 'Int64', 'Float32', 'Float64']

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
          <span>Chunk mémoire</span>
          <input v-model.number="store.settingScanChunkSizeMb" class="input" type="number" min="1" max="64" step="1" />
        </label>
        <label class="toggle-row">
          <input v-model="store.settingFastScan" type="checkbox" />
          <span>Fast scan par défaut</span>
        </label>
      </div>
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
      </div>
      <div class="path-row">
        <span>Log</span>
        <code>{{ store.logFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>Smart Search JSON</span>
        <code>{{ store.smartSearchDebugFilePath || '-' }}</code>
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
      <p v-if="store.smartSearchDebugError" class="error">{{ store.smartSearchDebugError }}</p>
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

.settings-grid .wide {
  grid-column: span 2;
}

.toggle-row {
  align-items: flex-start;
  justify-content: center;
}

.toggle-row input {
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
