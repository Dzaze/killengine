<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { useInvestigationNotebookStore, type InvestigationHypothesis } from '@/stores/investigationNotebook'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const notebook = useInvestigationNotebookStore()
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
const notebookSections = computed(() => [
  { id: 'confirmed', title: 'Confirmées', items: notebook.confirmed },
  { id: 'active', title: 'Actives', items: notebook.active },
  { id: 'refuted', title: 'Réfutées', items: notebook.refuted },
])
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

function confidenceClass(score: number): string {
  if (score >= 90) return 'confirmed'
  if (score < 10) return 'refuted'
  if (score >= 70) return 'strong'
  if (score <= 30) return 'weak'
  return 'active'
}

function statusText(status: string): string {
  if (status === 'confirmed') return 'confirmée'
  if (status === 'refuted') return 'réfutée'
  return 'active'
}

function evidencePreview(item: InvestigationHypothesis): string {
  return item.evidenceLog.length > 0 ? item.evidenceLog[item.evidenceLog.length - 1] : ''
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

function checkpointCanForceValue(item: Record<string, unknown>): boolean {
  return Boolean(checkpointAddress(item) && String(item.kind ?? '') === 'code_writer')
}

const forceValueTarget = ref<Record<string, unknown> | null>(null)
const forceValueInput = ref('')
const forceValueBusy = ref(false)
const forceValueResult = ref<Record<string, unknown> | null>(null)

function selectForceValueTarget(item: Record<string, unknown>) {
  forceValueTarget.value = item
  forceValueInput.value = ''
  forceValueResult.value = null
}

async function applyForceValue() {
  const item = forceValueTarget.value
  if (!item || !forceValueInput.value.trim()) return
  forceValueBusy.value = true
  try {
    forceValueResult.value = await store.executeCheckpointForceValue(item, forceValueInput.value.trim())
  } finally {
    forceValueBusy.value = false
  }
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

onMounted(() => {
  // Auto-charge le rapport IA si une enquete est active, pour que les
  // annotations "Pourquoi" de la timeline/checkpoints soient deja peuplees
  // sans obliger l'utilisateur a cliquer "Rapport IA" en plus.
  if (run.value) {
    void store.refreshAutoResolveReport()
  }
  void notebook.refreshNotebook()
})

// Relie le rapport IA (deja calcule cote backend, rien de nouveau a produire)
// a la timeline : pour un outil/checkpoint donne, retrouve le strategyScore
// ou le telemetryInsight du rapport qui explique *pourquoi* cette action a
// ete choisie, pas seulement *quoi* a ete fait. Correlation par mots-cles sur
// le nom d'outil/le kind de checkpoint, aucune nouvelle donnee backend.
function reasonFromReportMatch(match: Record<string, unknown> | undefined): string {
  if (!match) return ''
  const label = String(match.label ?? '')
  const reason = String(match.reason ?? '')
  const score = match.score
  return score !== undefined ? `${label} (score ${score}/100) — ${reason}` : `${label} — ${reason}`
}

function matchReportEntry(keyword: string): Record<string, unknown> | undefined {
  const report = autoReport.value as Record<string, unknown> | null
  if (!report) return undefined
  const scores = (report.strategyScores as Array<Record<string, unknown>>) ?? []
  const insights = (report.telemetryInsights as Array<Record<string, unknown>>) ?? []
  const findScore = (id: string) => scores.find((s) => String(s.id ?? '') === id)
  const findInsight = (id: string) => insights.find((i) => String(i.id ?? '') === id)

  switch (keyword) {
    case 'aob':
      return findInsight('aob_multimatch_guard') ?? findInsight('aob_quality_guard')
    case 'write':
      return findInsight('freeze_instability_detected') ?? findInsight('audit_risky_actions')
    case 'uistring':
      return findScore('trace_ui_string') ?? findInsight('trace_ui_sources_overflow') ?? findInsight('trace_ui_sources_ready')
    case 'unknown':
      return findScore('unknown_capture') ?? findInsight('unknown_too_large')
    case 'encrypted':
      return findScore('encrypted_scan')
    case 'reduce':
      return findScore('reduce_with_new_value')
    case 'exact':
      return findScore('exact_or_multitype') ?? findInsight('exact_zero_fallback')
    default:
      return undefined
  }
}

function stepStrategyReason(step: Record<string, unknown>): string {
  const tool = String(step.tool ?? '').toLowerCase()
  if (!tool) return ''
  if (tool.includes('aob') || tool.includes('patch') || tool === 'forcewriteinstructionvalue') {
    return reasonFromReportMatch(matchReportEntry('aob'))
  }
  if (tool.includes('findwhatwrites')) return ''
  if (tool === 'writememoryvalue' || tool === 'setfreezevalue') {
    return reasonFromReportMatch(matchReportEntry('write'))
  }
  if (tool.includes('uistring')) return reasonFromReportMatch(matchReportEntry('uistring'))
  if (tool.includes('unknown')) return reasonFromReportMatch(matchReportEntry('unknown'))
  if (tool.includes('encrypted')) return reasonFromReportMatch(matchReportEntry('encrypted'))
  if (tool.includes('reduce')) return reasonFromReportMatch(matchReportEntry('reduce'))
  if (tool.includes('exact')) return reasonFromReportMatch(matchReportEntry('exact'))
  return ''
}

function checkpointStrategyReason(item: Record<string, unknown>): string {
  const kind = String(item.kind ?? '')
  if (kind === 'code_writer' || kind === 'code_patch_suggestion' || kind === 'aob_signature') {
    return reasonFromReportMatch(matchReportEntry('aob'))
  }
  if (kind === 'suggested_write') return reasonFromReportMatch(matchReportEntry('write'))
  if (kind === 'ui_string_hit' || kind === 'ui_numeric_source') return reasonFromReportMatch(matchReportEntry('uistring'))
  if (kind === 'unknown_candidate') return reasonFromReportMatch(matchReportEntry('unknown'))
  if (kind === 'encrypted_hit') return reasonFromReportMatch(matchReportEntry('encrypted'))
  return ''
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

    <PanelIntro
      what="Le journal détaillé d'une session d'enquête menée par l'Assistant : hypothèses testées, étapes, résultats."
      purpose="Comprendre après coup ce que l'IA a essayé et pourquoi, exporter un rapport, ou reprendre une enquête interrompue."
      how="Se remplit automatiquement pendant une recherche guidée par l'Assistant ; utilise Markdown/JSON pour exporter, ou Archiver/Effacer pour clôturer."
    />

    <PanelIntro
      what="Un carnet manuel qui garde plusieurs hypothèses concurrentes avec un score de confiance."
      purpose="Comparer les pistes pendant une enquête sans s'accrocher trop longtemps à la première idée plausible."
      how="Ajoute une hypothèse, note chaque preuve, puis confirme ou contredis le résultat du test observé."
    />

    <section class="panel notebook-panel">
      <div class="section-head">
        <h2>Carnet d'hypothèses</h2>
        <span>{{ notebook.totalCount }} hypothèse(s)</span>
      </div>
      <form class="notebook-form" @submit.prevent="notebook.addHypothesis()">
        <input
          v-model="notebook.hypothesisDraft"
          class="filter-input"
          placeholder="Ex: la valeur affichée est une copie recalculée"
          :disabled="notebook.busy"
        />
        <label>
          <span>Score départ</span>
          <input
            v-model.number="notebook.baselineScore"
            class="filter-input score-input"
            type="number"
            min="0"
            max="100"
            :disabled="notebook.busy"
          />
        </label>
        <button class="btn primary" type="submit" :disabled="notebook.busy || !notebook.hypothesisDraft.trim()">Ajouter</button>
        <button class="btn" type="button" :disabled="notebook.busy || notebook.totalCount === 0" @click="notebook.resetNotebook()">Nouveau carnet</button>
      </form>
      <p v-if="notebook.error" class="error">{{ notebook.error }}</p>
      <div class="notebook-grid">
        <section v-for="section in notebookSections" :key="section.id" class="notebook-column">
          <div class="notebook-column-head">
            <h3>{{ section.title }}</h3>
            <span>{{ section.items.length }}</span>
          </div>
          <article v-for="item in section.items" :key="item.id" class="hypothesis-card" :class="confidenceClass(item.confidenceScore)">
            <div class="hypothesis-head">
              <strong>{{ item.description }}</strong>
              <span>{{ statusText(item.status) }}</span>
            </div>
            <div class="confidence-meter" :class="confidenceClass(item.confidenceScore)">
              <span :style="{ width: `${item.confidenceScore}%` }"></span>
            </div>
            <div class="meta">
              <span>{{ item.id }}</span>
              <span>{{ item.confidenceScore }}/100</span>
            </div>
            <p v-if="evidencePreview(item)" class="evidence-preview">{{ evidencePreview(item) }}</p>
            <div v-if="item.status === 'active'" class="evidence-actions">
              <input
                v-model="notebook.evidenceNotes[item.id]"
                class="filter-input"
                placeholder="Note de preuve observée"
                :disabled="notebook.busy"
              />
              <button class="btn mini" type="button" :disabled="notebook.busy" @click="notebook.recordTestResult(item.id, true)">Confirme</button>
              <button class="btn mini" type="button" :disabled="notebook.busy" @click="notebook.recordTestResult(item.id, false)">Contredit</button>
            </div>
          </article>
          <p v-if="section.items.length === 0" class="muted">Aucune hypothèse.</p>
        </section>
      </div>
    </section>

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
      <h2>Aucune enquête en cours</h2>
      <p>Ce panneau montre les étapes suivies par l'IA pour trouver une valeur dans le jeu (par exemple la vie ou l'argent) : c'est la suite d'une enquête, pas son point de départ.</p>
      <p>Pour en démarrer une : ouvre l'onglet <strong>Assistant</strong>, décris ce que tu cherches, puis clique sur le bouton <strong>Auto</strong>.</p>
      <button class="btn primary" @click="store.activeView = 'assistant'">Aller à l'Assistant</button>
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
          <p v-if="stepStrategyReason(step)" class="step-reason">Pourquoi : {{ stepStrategyReason(step) }}</p>
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
            <p v-if="checkpointStrategyReason(item)" class="step-reason">Pourquoi : {{ checkpointStrategyReason(item) }}</p>
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
              <button v-if="checkpointCanForceValue(item)" class="btn mini" :title="checkpointPlanReason(item, 'force_value')" @click="selectForceValueTarget(item)">Forcer valeur (hook)</button>
              <button class="btn mini" @click="bookmarkCheckpoint(item)">Bookmark</button>
              <button v-if="checkpointAddress(item)" class="btn mini" :title="checkpointPlanReason(item, 'trainer')" @click="createFromCheckpoint(item)">Créer Trainer</button>
            </div>
            <div v-if="forceValueTarget === item" class="checkpoint-actions">
              <input
                v-model="forceValueInput"
                class="input"
                placeholder="Valeur : 999"
                :disabled="forceValueBusy"
                @keyup.enter="applyForceValue()"
              />
              <button class="btn mini" type="button" :disabled="forceValueBusy || !forceValueInput.trim()" @click="applyForceValue()">Appliquer</button>
            </div>
            <p v-if="forceValueTarget === item && forceValueResult" :class="forceValueResult.success ? 'hint' : 'error'">
              {{ forceValueResult.success
                ? `Valeur forcée : trampoline actif à 0x${forceValueResult.patchAddress}.`
                : forceValueResult.error }}
            </p>
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

:deep(.panel-intro) {
  margin-top: 14px;
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

.btn.primary {
  border-color: var(--accent);
  background: var(--accent);
  color: white;
  font-weight: 600;
}

.btn.primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

.empty {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 8px;
  padding: 20px;
}

.empty p {
  max-width: 640px;
}

.empty .btn {
  margin-top: 6px;
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

.notebook-panel {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: 14px;
}

.notebook-form {
  display: grid;
  grid-template-columns: minmax(220px, 1fr) minmax(110px, 0.18fr) auto auto;
  gap: 8px;
  align-items: end;
}

.notebook-form label {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 4px;
  color: var(--text-dim);
  font-size: 11px;
}

.score-input {
  max-width: 120px;
}

.notebook-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.notebook-column {
  min-width: 0;
}

.notebook-column-head,
.hypothesis-head {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 10px;
}

.notebook-column-head span,
.hypothesis-head span {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.hypothesis-card {
  margin-top: 8px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 8px;
  background: var(--bg-primary);
  padding: 10px;
}

.hypothesis-card.confirmed {
  border-color: rgba(158, 206, 106, 0.38);
}

.hypothesis-card.refuted {
  border-color: rgba(247, 118, 142, 0.36);
}

.hypothesis-card.strong {
  border-color: rgba(122, 162, 247, 0.34);
}

.hypothesis-card.weak {
  border-color: rgba(224, 175, 104, 0.34);
}

.hypothesis-head strong {
  min-width: 0;
  color: var(--text-primary);
  line-height: 1.35;
}

.confidence-meter {
  overflow: hidden;
  height: 7px;
  margin-top: 9px;
  border-radius: 999px;
  background: rgba(255, 255, 255, 0.08);
}

.confidence-meter span {
  display: block;
  height: 100%;
  border-radius: inherit;
  background: var(--accent);
}

.confidence-meter.confirmed span {
  background: var(--success);
}

.confidence-meter.refuted span {
  background: var(--error);
}

.confidence-meter.strong span {
  background: rgb(122, 162, 247);
}

.confidence-meter.weak span {
  background: var(--warning);
}

.evidence-preview {
  margin-top: 8px;
  font-size: 12px;
}

.evidence-actions {
  display: grid;
  grid-template-columns: minmax(140px, 1fr) auto auto;
  gap: 6px;
  margin-top: 9px;
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

.step-reason {
  color: var(--text-dim);
  font-size: 11px;
  font-style: italic;
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
  .report-lists,
  .notebook-form,
  .notebook-grid,
  .evidence-actions {
    grid-template-columns: 1fr;
  }

  .score-input {
    max-width: none;
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
