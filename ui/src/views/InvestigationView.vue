<script setup lang="ts">
import { computed, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const exportText = ref('')
const statusFilter = ref('all')
const riskFilter = ref('all')
const toolFilter = ref('all')
const searchFilter = ref('')

const run = computed(() => store.activeInvestigation)
const autoReport = computed(() => store.autoResolveReport)
const steps = computed(() => run.value?.steps ?? [])
const hypotheses = computed(() => run.value?.hypotheses ?? [])
const checkpoints = computed(() => run.value?.checkpoints ?? [])
const reportRecommendations = computed(() => autoReport.value?.recommendations ?? [])
const reportGuardrails = computed(() => autoReport.value?.guardrails ?? [])
const sortedCheckpoints = computed(() => [...checkpoints.value].sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0)))
const bestNextAction = computed(() => {
  const action = autoReport.value?.nextBestAction
  if (action && Object.keys(action).length > 0) {
    return {
      label: String(action.label ?? action.id ?? 'Action'),
      reason: String(action.reason ?? 'Priorité calculée par le rapport IA.'),
      risk: String(action.risk ?? (action.safe === false ? 'write' : 'safe')),
      confidence: Number(action.confidence ?? 0),
      source: 'Rapport IA',
    }
  }
  const checkpoint = sortedCheckpoints.value[0]
  if (checkpoint) {
    return {
      label: checkpointTitle(checkpoint),
      reason: checkpointDetail(checkpoint) || String(checkpoint.reason ?? 'Checkpoint le mieux scoré.'),
      risk: checkpoint.requiresConfirmation === true ? 'confirmation' : 'safe',
      confidence: Number(checkpoint.confidenceScore ?? 0),
      source: 'Checkpoint',
    }
  }
  return null
})
const toolOptions = computed(() => {
  const tools = new Set<string>()
  for (const step of steps.value) {
    const tool = String(step.tool ?? '').trim()
    if (tool) tools.add(tool)
  }
  return [...tools].sort((a, b) => a.localeCompare(b))
})
const filteredSteps = computed(() => {
  const text = searchFilter.value.trim().toLowerCase()
  return steps.value.filter((step) => {
    if (statusFilter.value !== 'all' && step.status !== statusFilter.value) return false
    if (riskFilter.value !== 'all' && step.risk !== riskFilter.value) return false
    if (toolFilter.value !== 'all' && step.tool !== toolFilter.value) return false
    if (!text) return true
    return [
      step.title,
      step.detail,
      step.tool,
      step.risk,
      step.status,
    ].some((value) => String(value ?? '').toLowerCase().includes(text))
  })
})

function statusLabel(status: string): string {
  if (status === 'active') return 'active'
  if (status === 'checkpoint') return 'checkpoint'
  if (status === 'done') return 'terminee'
  if (status === 'blocked') return 'bloquee'
  return status
}

function stepClass(status: string): string {
  if (status === 'success') return 'ok'
  if (status === 'warning' || status === 'checkpoint') return 'warn'
  if (status === 'error') return 'bad'
  if (status === 'running') return 'run'
  return 'plan'
}

function showJson() {
  exportText.value = store.exportInvestigationJson()
}

function showMarkdown() {
  exportText.value = store.exportInvestigationMarkdown()
}

function checkpointTitle(item: Record<string, unknown>): string {
  return String(item.label ?? item.address ?? item.id ?? item.kind ?? 'Checkpoint')
}

function checkpointDetail(item: Record<string, unknown>): string {
  const parts = [
    item.reason ? String(item.reason) : '',
    item.type ? `type ${String(item.type)}` : '',
    item.value ? `valeur ${String(item.value)}` : '',
    item.confidenceLabel ? String(item.confidenceLabel) : '',
  ].filter(Boolean)
  return parts.join(' · ')
}

function checkpointKindLabel(item: Record<string, unknown>): string {
  const kind = String(item.kind ?? 'checkpoint')
  return kind.replace(/_/g, ' ')
}

function checkpointScoreLabel(item: Record<string, unknown>): string {
  if (item.confidenceLabel) return String(item.confidenceLabel)
  const score = Number(item.confidenceScore ?? Number.NaN)
  return Number.isFinite(score) && score > 0 ? `score ${score}/100` : ''
}

function checkpointRiskLabel(item: Record<string, unknown>): string {
  return item.requiresConfirmation === true ? 'confirmation requise' : 'safe'
}

function checkpointAddress(item: Record<string, unknown>): string {
  return String(item.address ?? item.instructionPointer ?? '').trim()
}

function checkpointIsCode(item: Record<string, unknown>): boolean {
  const kind = String(item.kind ?? '')
  return kind.includes('code') || kind.includes('aob') || Boolean(item.patchBytes || item.aobPattern || item.instructionPointer)
}

function checkpointCanWrite(item: Record<string, unknown>): boolean {
  return Boolean(checkpointAddress(item) && (item.value || item.targetValue) && !checkpointIsCode(item))
}

function checkpointCanDebug(item: Record<string, unknown>): boolean {
  return Boolean(checkpointAddress(item) && !checkpointIsCode(item))
}

function checkpointCanAob(item: Record<string, unknown>): boolean {
  return Boolean(checkpointAddress(item) && (checkpointIsCode(item) || item.sourceAddress))
}

function checkpointPlan(item: Record<string, unknown>) {
  return store.buildCheckpointActionPlan(item)
}

function checkpointPlanReason(item: Record<string, unknown>, actionId: string): string {
  return checkpointPlan(item).actions.find((action) => action.id === actionId)?.reason ?? ''
}

function createFromCheckpoint(item: Record<string, unknown>) {
  store.createTrainerFeatureFromCheckpoint(item)
}

function bookmarkCheckpoint(item: Record<string, unknown>) {
  store.createWorkspaceBookmarkFromCheckpoint(item)
}

function watchCheckpoint(item: Record<string, unknown>) {
  const address = checkpointAddress(item)
  if (!address) return
  store.addAddressToWatch(address, String(item.type ?? 'Int32'))
  store.setWatchLiveEnabled(true)
}
</script>

<template>
  <div class="investigation-view">
    <header class="topbar">
      <div>
        <h1>Investigation</h1>
        <p>Timeline d'enquete IA, hypotheses, checkpoints et exports.</p>
      </div>
      <div class="actions">
        <button class="btn" @click="store.refreshAutoResolveReport()">Rapport IA</button>
        <button class="btn" :disabled="!run" @click="showMarkdown()">Markdown</button>
        <button class="btn" :disabled="!run" @click="showJson()">JSON</button>
        <button class="btn" :disabled="!run" @click="store.saveCurrentWorkspaceProject()">Sauver projet</button>
        <button class="btn" :disabled="!run" @click="store.finishInvestigation('done')">Archiver</button>
        <button class="btn danger" :disabled="!run" @click="store.clearInvestigation()">Effacer</button>
      </div>
    </header>

    <section v-if="run" class="summary">
      <div>
        <span>Objectif</span>
        <strong>{{ run.objective }}</strong>
      </div>
      <div>
        <span>Processus</span>
        <strong>{{ run.processName || 'non attache' }}</strong>
      </div>
      <div>
        <span>Statut</span>
        <strong>{{ statusLabel(run.status) }}</strong>
      </div>
      <div>
        <span>Strategie</span>
        <strong>{{ run.preferredStrategy?.label ?? 'non determinee' }}</strong>
      </div>
    </section>

    <section v-if="autoReport" class="panel report">
      <div class="section-head">
        <h2>Rapport IA</h2>
        <span>{{ autoReport.processName || 'non attaché' }} · {{ autoReport.candidateCount }} candidat(s)</span>
      </div>
      <p>{{ autoReport.summary || 'Rapport disponible.' }}</p>
      <div class="report-grid">
        <div>
          <span>Stratégie</span>
          <strong>{{ autoReport.preferredStrategy?.label ?? 'non déterminée' }}</strong>
        </div>
        <div>
          <span>Workflow</span>
          <strong>{{ autoReport.workflow || '-' }}</strong>
        </div>
        <div>
          <span>Valeur</span>
          <strong>{{ autoReport.initialValue || autoReport.targetValue || '-' }}</strong>
        </div>
      </div>
      <div class="report-lists">
        <div>
          <h3>Recommandations</h3>
          <p v-for="item in reportRecommendations.slice(0, 4)" :key="String(item.id ?? item.label)" class="report-item">
            {{ item.label ?? item.id }} · {{ item.reason ?? '' }}
          </p>
          <p v-if="reportRecommendations.length === 0" class="muted">Aucune recommandation.</p>
        </div>
        <div>
          <h3>Garde-fous</h3>
          <p v-for="item in reportGuardrails.slice(0, 4)" :key="String(item.id ?? item.label)" class="report-item">
            {{ item.label ?? item.id }} · {{ item.reason ?? '' }}
          </p>
          <p v-if="reportGuardrails.length === 0" class="muted">Aucun garde-fou remonté.</p>
        </div>
      </div>
    </section>

    <section v-if="bestNextAction" class="panel next-action">
      <div class="section-head">
        <h2>Meilleure prochaine action</h2>
        <span>{{ bestNextAction.source }}</span>
      </div>
      <div class="next-action-body">
        <strong>{{ bestNextAction.label }}</strong>
        <p>{{ bestNextAction.reason }}</p>
        <div class="meta">
          <span>{{ bestNextAction.risk }}</span>
          <span v-if="bestNextAction.confidence">confiance {{ bestNextAction.confidence }}/100</span>
        </div>
      </div>
    </section>

    <div v-if="!run" class="empty">
      <h2>Aucune investigation active</h2>
      <p>Lance Auto depuis l'Assistant pour creer une timeline.</p>
    </div>

    <div v-else class="grid">
      <section class="panel timeline">
        <div class="section-head">
          <h2>Etapes</h2>
          <span>{{ filteredSteps.length }} / {{ steps.length }}</span>
        </div>
        <div class="filters">
          <input v-model="searchFilter" class="filter-input" placeholder="Chercher dans la timeline" />
          <select v-model="statusFilter" class="filter-input">
            <option value="all">Tous statuts</option>
            <option value="planned">planned</option>
            <option value="running">running</option>
            <option value="success">success</option>
            <option value="warning">warning</option>
            <option value="error">error</option>
            <option value="checkpoint">checkpoint</option>
          </select>
          <select v-model="riskFilter" class="filter-input">
            <option value="all">Tous risques</option>
            <option value="safe">safe</option>
            <option value="write">write</option>
            <option value="debug">debug</option>
            <option value="patch">patch</option>
            <option value="inject">inject</option>
          </select>
          <select v-model="toolFilter" class="filter-input">
            <option value="all">Tous outils</option>
            <option v-for="tool in toolOptions" :key="tool" :value="tool">{{ tool }}</option>
          </select>
        </div>
        <article v-for="step in filteredSteps" :key="step.id" class="step" :class="stepClass(step.status)">
          <div class="step-head">
            <strong>{{ step.title }}</strong>
            <span>{{ step.time }}</span>
          </div>
          <p>{{ step.detail }}</p>
          <div class="meta">
            <span>{{ step.status }}</span>
            <span v-if="step.tool">{{ step.tool }}</span>
            <span v-if="step.risk">{{ step.risk }}</span>
          </div>
        </article>
        <p v-if="filteredSteps.length === 0" class="muted">Aucune étape ne correspond aux filtres.</p>
      </section>

      <aside class="side">
        <section class="panel">
          <h2>Hypotheses</h2>
          <article v-for="item in hypotheses" :key="String(item.id ?? item.label)" class="card">
            <strong>{{ item.label ?? item.id }}</strong>
            <p>{{ item.reason ?? 'Hypothese proposee par Auto.' }}</p>
          </article>
          <p v-if="hypotheses.length === 0" class="muted">Aucune hypothese pour l'instant.</p>
        </section>

        <section class="panel">
          <h2>Checkpoints</h2>
          <article v-for="item in sortedCheckpoints" :key="String(item.id ?? item.address ?? item.label)" class="card checkpoint">
            <strong>{{ checkpointTitle(item) }}</strong>
            <div class="checkpoint-badges">
              <span>{{ checkpointKindLabel(item) }}</span>
              <span v-if="checkpointScoreLabel(item)">{{ checkpointScoreLabel(item) }}</span>
              <span :class="item.requiresConfirmation === true ? 'risk-badge' : 'safe-badge'">{{ checkpointRiskLabel(item) }}</span>
            </div>
            <p>{{ checkpointDetail(item) || 'Confirmation requise avant action.' }}</p>
            <div class="action-plan">
              <span
                v-for="action in checkpointPlan(item).actions"
                :key="action.id"
                :class="[{ disabled: !action.enabled }, `risk-${action.risk}`]"
                :title="action.reason"
              >
                {{ action.label }}
              </span>
            </div>
            <div class="checkpoint-actions">
              <button v-if="checkpointAddress(item)" class="btn mini" :title="checkpointPlanReason(item, 'watch')" @click="watchCheckpoint(item)">Watch</button>
              <button v-if="checkpointCanWrite(item)" class="btn mini" :title="checkpointPlanReason(item, 'write')" @click="store.executeCheckpointWrite(item, false)">Préparer write</button>
              <button v-if="checkpointCanWrite(item)" class="btn mini" :title="checkpointPlanReason(item, 'freeze_polling')" @click="store.executeCheckpointWrite(item, true)">Freeze</button>
              <button v-if="checkpointCanDebug(item)" class="btn mini" :title="checkpointPlanReason(item, 'find_writes')" @click="store.executeCheckpointFindWhatWrites(item)">Find What Writes</button>
              <button v-if="checkpointCanAob(item)" class="btn mini" :title="checkpointPlanReason(item, 'aob_patch')" @click="store.prepareCheckpointAob(item)">AOB/Patch</button>
              <button class="btn mini" @click="bookmarkCheckpoint(item)">Bookmark</button>
              <button v-if="checkpointAddress(item)" class="btn mini" :title="checkpointPlanReason(item, 'trainer')" @click="createFromCheckpoint(item)">Créer Trainer</button>
            </div>
          </article>
          <p v-if="checkpoints.length === 0" class="muted">Aucun checkpoint actif.</p>
        </section>
      </aside>
    </div>

    <section v-if="exportText" class="panel export">
      <div class="export-head">
        <h2>Export</h2>
        <button class="btn" @click="exportText = ''">Fermer</button>
      </div>
      <pre>{{ exportText }}</pre>
    </section>

    <section class="panel archive">
      <div class="export-head">
        <h2>Archives</h2>
        <button class="btn" :disabled="store.investigationArchive.length === 0" @click="store.clearInvestigationArchive()">Vider</button>
      </div>
      <article v-for="item in store.investigationArchive" :key="item.id" class="archive-row">
        <div>
          <strong>{{ item.objective }}</strong>
          <span>{{ statusLabel(item.status) }} · {{ item.steps.length }} etape(s)</span>
        </div>
        <button class="btn mini" @click="store.restoreInvestigationFromArchive(item.id)">Restaurer</button>
      </article>
      <p v-if="store.investigationArchive.length === 0" class="muted">Aucune archive.</p>
    </section>
  </div>
</template>

<style scoped>
.investigation-view {
  height: 100%;
  overflow-y: auto;
  padding: 18px 24px;
}

.topbar,
.export-head,
.section-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
}

h1 {
  color: var(--text-primary);
  font-size: 24px;
}

h2 {
  color: var(--text-primary);
  font-size: 15px;
}

h3 {
  color: var(--text-primary);
  font-size: 13px;
}

p {
  color: var(--text-dim);
}

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.section-head span {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.btn {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  cursor: pointer;
  padding: 7px 10px;
}

.btn:hover:not(:disabled) {
  background: var(--bg-accent);
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn.danger {
  border-color: var(--error);
  background: var(--error);
  color: white;
}

.btn.danger:hover:not(:disabled) {
  filter: brightness(1.08);
}

.btn.mini {
  padding: 5px 7px;
  font-size: 12px;
}

.summary {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 10px;
  margin: 18px 0;
}

.summary div,
.panel,
.empty {
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
}

.summary div {
  min-width: 0;
  padding: 10px;
}

.summary span,
.meta span {
  color: var(--text-dim);
  font-size: 11px;
}

.summary strong {
  display: block;
  overflow: hidden;
  margin-top: 4px;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.grid {
  display: grid;
  grid-template-columns: minmax(0, 1.5fr) minmax(280px, 0.8fr);
  gap: 14px;
}

.report {
  margin-bottom: 14px;
}

.next-action {
  margin-bottom: 14px;
  border-color: rgba(122, 162, 247, 0.34);
  background: rgba(122, 162, 247, 0.07);
}

.next-action-body {
  margin-top: 8px;
}

.next-action-body strong {
  color: var(--text-primary);
}

.next-action-body p {
  margin-top: 6px;
}

.report-grid,
.report-lists {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
  margin-top: 10px;
}

.report-lists {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.report-grid div {
  min-width: 0;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  padding: 8px;
}

.report-grid span {
  color: var(--text-dim);
  font-size: 11px;
}

.report-grid strong {
  display: block;
  overflow: hidden;
  margin-top: 4px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.report-item {
  margin-top: 6px;
}

.panel,
.empty {
  padding: 12px;
}

.timeline {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.filters {
  display: grid;
  grid-template-columns: minmax(160px, 1fr) repeat(3, minmax(120px, 0.45fr));
  gap: 8px;
}

.filter-input {
  min-width: 0;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  padding: 7px 9px;
}

.step,
.card,
.archive-row {
  margin-top: 8px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 8px;
  background: var(--bg-primary);
  padding: 10px;
}

.step-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
}

.step-head span {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.step p,
.card p {
  margin-top: 6px;
}

.meta {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.meta span {
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 3px 7px;
}

.step.ok {
  border-color: rgba(158, 206, 106, 0.35);
}

.step.warn,
.checkpoint {
  border-color: rgba(224, 175, 104, 0.42);
}

.checkpoint-badges {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
  margin-top: 7px;
}

.checkpoint-badges span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
  padding: 3px 7px;
}

.checkpoint-badges .safe-badge {
  border-color: rgba(158, 206, 106, 0.35);
  color: var(--success);
}

.checkpoint-badges .risk-badge {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.action-plan {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
  margin-top: 8px;
}

.action-plan span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-secondary);
  font-size: 10px;
  padding: 3px 6px;
}

.action-plan span.disabled {
  color: var(--text-dim);
  opacity: 0.45;
}

.action-plan .risk-safe {
  border-color: rgba(158, 206, 106, 0.28);
}

.action-plan .risk-write,
.action-plan .risk-debug,
.action-plan .risk-patch {
  border-color: rgba(224, 175, 104, 0.35);
}

.checkpoint-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 9px;
}

.step.bad {
  border-color: rgba(247, 118, 142, 0.42);
}

.step.run {
  border-color: rgba(122, 162, 247, 0.42);
}

.side {
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.muted {
  margin-top: 8px;
}

.archive-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 4px;
}

.archive-row span {
  color: var(--text-dim);
}

@media (max-width: 980px) {
  .summary,
  .grid,
  .filters,
  .report-grid,
  .report-lists {
    grid-template-columns: 1fr;
  }
}

.export,
.archive {
  margin-top: 14px;
}

pre {
  overflow-x: auto;
  margin-top: 10px;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  white-space: pre-wrap;
}

.archive-row {
  display: flex;
  justify-content: space-between;
  gap: 12px;
}

.archive-row span {
  color: var(--text-dim);
}

@media (max-width: 980px) {
  .summary,
  .grid {
    grid-template-columns: 1fr;
  }
}
</style>
