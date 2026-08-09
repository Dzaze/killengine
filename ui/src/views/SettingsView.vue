<script setup lang="ts">
import { computed, onMounted } from 'vue'
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

async function refreshAll() {
  await store.doPing()
  await store.refreshDiagnostics()
}

onMounted(() => {
  void store.refreshDiagnostics()
})
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
          <span>{{ locale === 'fr' ? 'Français' : 'English' }}</span>
        </div>
        <div class="segmented">
          <button :class="{ active: locale === 'fr' }" @click="locale = 'fr'">FR</button>
          <button :class="{ active: locale === 'en' }" @click="locale = 'en'">EN</button>
        </div>
      </div>
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
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>Session</h2>
      </div>
      <div class="actions-row">
        <button class="btn btn-secondary" @click="store.resetWorkflow()">
          Réinitialiser le workflow
        </button>
        <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.detach()">
          {{ $t('process.detach') }}
        </button>
      </div>
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

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

@media (max-width: 850px) {
  .runtime-grid,
  .path-row {
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
