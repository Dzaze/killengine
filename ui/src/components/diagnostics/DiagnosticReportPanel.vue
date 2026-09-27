<script setup lang="ts">
import { ref, computed } from 'vue'
import { useAppStore } from '@/stores/app'
import { formatBytes } from '@/utils/format'

const store = useAppStore()

const steps = ref('')
const expected = ref('')
const observed = ref('')
const includeSmartSearchDebug = ref(false)
const includeScanTelemetry = ref(false)
const includeCrashReports = ref(false)

// UX-PRODUIT-17 : toute modification après une préparation invalide
// localement l'aperçu -- le bouton Exporter reste désactivé jusqu'à une
// nouvelle préparation explicite (exigence de la fiche).
function onOptionsChanged() {
  store.markPreparedDiagnosticReportStale()
}

async function handlePrepare() {
  openSectionId.value = null
  sectionContent.value = ''
  await store.prepareDiagnosticReport({
    steps: steps.value,
    expected: expected.value,
    observed: observed.value,
    includeSmartSearchDebug: includeSmartSearchDebug.value,
    includeScanTelemetry: includeScanTelemetry.value,
    includeCrashReports: includeCrashReports.value,
  })
}

async function handleExport() {
  await store.exportPreparedDiagnosticReport()
}

const canExport = computed(() =>
  Boolean(store.preparedReportPreview) && !store.preparedReportStale && !store.preparedReportBusy)

const provenance = computed(() => (store.preparedReportPreview?.provenance as Record<string, unknown>) ?? {})
const sections = computed(() => (store.preparedReportPreview?.sections as Array<Record<string, unknown>>) ?? [])
const events = computed(() => (store.preparedReportPreview?.events as Array<Record<string, unknown>>) ?? [])
const totalBytes = computed(() => Number(store.preparedReportPreview?.totalBytes ?? 0))

const openSectionId = ref<string | null>(null)
const sectionContent = ref('')
const sectionNextOffset = ref(-1)
const sectionLoadError = ref('')

async function toggleSectionOpen(sectionId: string) {
  if (openSectionId.value === sectionId) {
    openSectionId.value = null
    sectionContent.value = ''
    return
  }
  openSectionId.value = sectionId
  sectionContent.value = ''
  sectionNextOffset.value = -1
  sectionLoadError.value = ''
  const result = await store.fetchPreparedDiagnosticReportSection(sectionId, 0, 16384)
  if (result.success === true) {
    sectionContent.value = String(result.data ?? '')
    sectionNextOffset.value = Number(result.nextOffset ?? -1)
  } else {
    sectionLoadError.value = String(result.error ?? '')
  }
}

async function loadMoreSectionContent() {
  if (!openSectionId.value || sectionNextOffset.value < 0) return
  const result = await store.fetchPreparedDiagnosticReportSection(openSectionId.value, sectionNextOffset.value, 16384)
  if (result.success === true) {
    sectionContent.value += String(result.data ?? '')
    sectionNextOffset.value = Number(result.nextOffset ?? -1)
  } else {
    sectionLoadError.value = String(result.error ?? '')
  }
}
</script>

<template>
  <div class="diagnostic-report-panel">
    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('diagnosticReport.title') }}</h2>
      </div>
      <p class="hint">{{ $t('diagnosticReport.hint') }}</p>

      <div class="form-grid">
        <label>
          <span>{{ $t('diagnosticReport.steps') }}</span>
          <textarea v-model="steps" class="input textarea" rows="3" @input="onOptionsChanged" />
        </label>
        <label>
          <span>{{ $t('diagnosticReport.expected') }}</span>
          <textarea v-model="expected" class="input textarea" rows="2" @input="onOptionsChanged" />
        </label>
        <label>
          <span>{{ $t('diagnosticReport.observed') }}</span>
          <textarea v-model="observed" class="input textarea" rows="2" @input="onOptionsChanged" />
        </label>
      </div>

      <div class="toggles-row">
        <label class="checkbox-inline">
          <input v-model="includeSmartSearchDebug" type="checkbox" @change="onOptionsChanged" />
          {{ $t('diagnosticReport.includeSmartSearchDebug') }}
        </label>
        <label class="checkbox-inline">
          <input v-model="includeScanTelemetry" type="checkbox" @change="onOptionsChanged" />
          {{ $t('diagnosticReport.includeScanTelemetry') }}
        </label>
        <label class="checkbox-inline">
          <input v-model="includeCrashReports" type="checkbox" @change="onOptionsChanged" />
          {{ $t('diagnosticReport.includeCrashReports') }}
        </label>
      </div>

      <div class="panel-actions">
        <button class="btn btn-primary compact" :disabled="store.preparedReportBusy" @click="handlePrepare()">
          {{ $t('diagnosticReport.prepare') }}
        </button>
        <button class="btn btn-secondary compact" :disabled="!canExport" @click="handleExport()">
          {{ $t('diagnosticReport.export') }}
        </button>
      </div>

      <p v-if="store.preparedReportError" class="error">{{ store.preparedReportError }}</p>
      <p v-if="store.preparedReportCancelled" class="hint">{{ $t('diagnosticReport.cancelled') }}</p>
      <p v-if="store.preparedReportExportPath" class="status-line">
        {{ $t('diagnosticReport.exported') }} <code>{{ store.preparedReportExportPath }}</code>
      </p>
    </section>

    <section v-if="store.preparedReportPreview" class="panel">
      <div class="panel-title">
        <h2>{{ $t('diagnosticReport.previewTitle') }}</h2>
        <span v-if="store.preparedReportStale" class="warning">{{ $t('diagnosticReport.stale') }}</span>
      </div>

      <div class="preview-grid">
        <div class="path-row"><span>{{ $t('diagnosticReport.engineVersion') }}</span><code>{{ provenance.engineVersion }}</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.buildId') }}</span><code>{{ provenance.buildId || '-' }}</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.executableSha256') }}</span><code>{{ provenance.executableSha256 || '-' }}</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.uiBundleOrigin') }}</span><code>{{ provenance.uiBundleOrigin }}</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.uiFingerprintStatus') }}</span><code>{{ provenance.uiFingerprintStatus }}</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.os') }}</span><code>{{ provenance.os }} ({{ provenance.architecture }})</code></div>
        <div class="path-row"><span>{{ $t('diagnosticReport.totalBytes') }}</span><code>{{ formatBytes(totalBytes) }}</code></div>
      </div>

      <div class="section-list">
        <div v-for="section in sections" :key="String(section.id)" class="section-row">
          <div class="section-head">
            <strong>{{ section.title || section.id }}</strong>
            <span>{{ formatBytes(Number(section.sizeBytes ?? 0)) }}</span>
            <span v-if="section.truncated" class="warning">
              {{ $t('diagnosticReport.truncated', { bytes: formatBytes(Number(section.omittedBytes ?? 0)) }) }}
            </span>
            <button class="btn btn-secondary compact" @click="toggleSectionOpen(String(section.id))">
              {{ openSectionId === section.id ? $t('diagnosticReport.hideContent') : $t('diagnosticReport.showContent') }}
            </button>
          </div>
          <template v-if="openSectionId === section.id">
            <pre class="log-viewer">{{ sectionContent }}</pre>
            <p v-if="sectionLoadError" class="error">{{ sectionLoadError }}</p>
            <button v-if="sectionNextOffset >= 0" class="btn btn-secondary compact" @click="loadMoreSectionContent()">
              {{ $t('diagnosticReport.loadMore') }}
            </button>
          </template>
        </div>
      </div>

      <div v-if="events.length > 0" class="events-list">
        <h3>{{ $t('diagnosticReport.eventsTitle', { count: events.length }) }}</h3>
        <div v-for="(event, idx) in events.slice(0, 30)" :key="idx" class="event-row">
          <span class="event-source">{{ event.source }}</span>
          <span>{{ event.kind }}</span>
          <span>{{ event.state }}</span>
          <span class="event-summary">{{ event.summary }}</span>
        </div>
      </div>
    </section>
  </div>
</template>

<style scoped>
.panel {
  margin-bottom: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.panel-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 10px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.hint {
  margin-bottom: 10px;
  color: var(--text-muted);
  font-size: 12px;
}

.form-grid {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-bottom: 10px;
}

.form-grid label {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.form-grid label span {
  color: var(--text-muted);
  font-size: 12px;
}

.input.textarea {
  width: 100%;
  min-height: 34px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: inherit;
  font-size: 13px;
  resize: vertical;
}

.toggles-row {
  display: flex;
  flex-wrap: wrap;
  gap: 14px;
  margin-bottom: 12px;
}

.checkbox-inline {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--text-secondary);
  font-size: 12px;
}

.checkbox-inline input {
  width: 15px;
  height: 15px;
  accent-color: var(--accent);
}

.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
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
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

.error {
  margin-top: 8px;
  color: var(--error);
}

.warning {
  color: var(--warning);
}

.status-line {
  margin-top: 8px;
  color: var(--text-muted);
  font-size: 12px;
}

code {
  overflow-wrap: anywhere;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.preview-grid {
  display: flex;
  flex-direction: column;
  gap: 2px;
  margin-bottom: 12px;
}

.path-row {
  display: grid;
  grid-template-columns: 180px minmax(0, 1fr);
  gap: 12px;
  align-items: center;
  padding: 6px 0;
  border-top: 1px solid var(--border);
}

.path-row:first-of-type {
  border-top: none;
}

.path-row span {
  color: var(--text-muted);
  font-size: 12px;
}

.section-list {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.section-row {
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.section-head {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 10px;
}

.section-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.section-head span {
  color: var(--text-muted);
  font-size: 12px;
}

.log-viewer {
  max-height: 280px;
  overflow: auto;
  margin-top: 8px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.events-list {
  margin-top: 14px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.events-list h3 {
  margin-bottom: 8px;
  color: var(--text-primary);
  font-size: 13px;
}

.event-row {
  display: grid;
  grid-template-columns: 90px 120px 90px minmax(0, 1fr);
  gap: 10px;
  align-items: baseline;
  padding: 4px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  font-size: 12px;
}

.event-row:first-of-type {
  border-top: none;
}

.event-source {
  color: var(--text-muted);
}

.event-summary {
  overflow: hidden;
  color: var(--text-secondary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 850px) {
  .path-row,
  .event-row {
    grid-template-columns: 1fr;
  }
}
</style>
