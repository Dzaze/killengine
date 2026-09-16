<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import { useInvestigationNotebookStore, type InvestigationHypothesis } from '@/stores/investigationNotebook'
import { useEffectProofStore, type EffectProofLevel } from '@/stores/effectProof'
import type { ProfileKnowledgeNoteKind } from '@/services/backend'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const notebook = useInvestigationNotebookStore()
const effectProof = useEffectProofStore()
const { t } = useI18n()
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
  { id: 'confirmed', title: t('investigation.sectionConfirmed'), items: notebook.confirmed },
  { id: 'active', title: t('investigation.sectionActive'), items: notebook.active },
  { id: 'refuted', title: t('investigation.sectionRefuted'), items: notebook.refuted },
])
const effectProofLevelOptions = computed<Array<{ value: EffectProofLevel; label: string }>>(() => [
  { value: 'write_confirmed', label: t('investigation.effectProofLevelWriteConfirmed') },
  { value: 'effect_confirmed', label: t('investigation.effectProofLevelEffectConfirmed') },
  { value: 'durable_solution', label: t('investigation.effectProofLevelDurableSolution') },
  { value: 'inconclusive', label: t('investigation.effectProofLevelInconclusive') },
  { value: 'unverified', label: t('investigation.effectProofLevelUnverified') },
])
function effectProofLevelLabel(level: string): string {
  const found = effectProofLevelOptions.value.find((option) => option.value === level)
  return found ? found.label : level
}

// AM-5b : boutons de raccourci qui préremplissent le formulaire existant
// (jamais de soumission automatique -- l'utilisateur confirme via "Enregistrer").
const openHistoryKeys = ref<Set<string>>(new Set())
function toggleHistory(targetKey: string) {
  const next = new Set(openHistoryKeys.value)
  if (next.has(targetKey)) next.delete(targetKey)
  else next.add(targetKey)
  openHistoryKeys.value = next
}

// AM-5c : options de nature de note réutilisant le vocabulaire R4 existant
// (core/profiles/profile_store.h KnowledgeNoteKind), jamais un nouveau schéma.
const linkNoteKindOptions = computed<Array<{ value: ProfileKnowledgeNoteKind; label: string }>>(() => [
  { value: 'success_condition', label: t('investigation.effectProofLinkKindSuccessCondition') },
  { value: 'explained_failure', label: t('investigation.effectProofLinkKindExplainedFailure') },
  { value: 'discriminating_experiment', label: t('investigation.effectProofLinkKindDiscriminatingExperiment') },
  { value: 'recheck', label: t('investigation.effectProofLinkKindRecheck') },
])

// Le select combine kind+name dans une seule valeur ("target:Foo") pour éviter
// toute ambiguïté si une cible et un patch partagent le même nom -- voir le
// garde "ambiguous_entry_name" de ProfileStore::addKnowledgeNote.
function onLinkEntrySelected(combined: string) {
  const separatorIndex = combined.indexOf(':')
  if (separatorIndex < 0) return
  const kind = combined.slice(0, separatorIndex)
  const name = combined.slice(separatorIndex + 1)
  if (kind === 'target' || kind === 'patch') {
    effectProof.linkEntryKind = kind
  }
  effectProof.linkEntryName = name
}
const sortedCheckpoints = computed(() => [...checkpoints.value].sort((a, b) => Number(b.confidenceScore ?? 0) - Number(a.confidenceScore ?? 0)))
const bestNextAction = computed(() => {
  const action = autoReport.value?.nextBestAction
  if (action && Object.keys(action).length > 0) {
    return {
      label: String(action.label ?? action.id ?? 'Action'),
      reason: String(action.reason ?? t('investigation.priorityCalculatedByReport')),
      risk: String(action.risk ?? (action.safe === false ? 'write' : 'safe')),
      confidence: Number(action.confidence ?? 0),
      source: t('investigation.aiReport'),
    }
  }
  const checkpoint = sortedCheckpoints.value[0]
  if (checkpoint) {
    return {
      label: checkpointTitle(checkpoint),
      reason: checkpointDetail(checkpoint) || String(checkpoint.reason ?? t('investigation.bestScoredCheckpoint')),
      risk: checkpoint.requiresConfirmation === true ? 'confirmation' : 'safe',
      confidence: Number(checkpoint.confidenceScore ?? 0),
      source: t('investigation.checkpointSource'),
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
  if (status === 'active') return t('investigation.statusActive')
  if (status === 'checkpoint') return t('investigation.checkpointDefaultKind')
  if (status === 'done') return t('investigation.statusDone')
  if (status === 'blocked') return t('investigation.statusBlocked')
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
  return String(item.label ?? item.address ?? item.id ?? item.kind ?? t('investigation.checkpointFallback'))
}

function checkpointDetail(item: Record<string, unknown>): string {
  const parts = [
    item.reason ? String(item.reason) : '',
    item.type ? t('investigation.typeLabel', { type: String(item.type) }) : '',
    item.value ? t('investigation.valueLabel', { value: String(item.value) }) : '',
    item.confidenceLabel ? String(item.confidenceLabel) : '',
  ].filter(Boolean)
  return parts.join(' · ')
}

function checkpointKindLabel(item: Record<string, unknown>): string {
  const kind = String(item.kind ?? t('investigation.checkpointDefaultKind'))
  return kind.replace(/_/g, ' ')
}

function checkpointScoreLabel(item: Record<string, unknown>): string {
  if (item.confidenceLabel) return String(item.confidenceLabel)
  const score = Number(item.confidenceScore ?? Number.NaN)
  return Number.isFinite(score) && score > 0 ? t('investigation.scoreLabel', { score }) : ''
}

function checkpointRiskLabel(item: Record<string, unknown>): string {
  return item.requiresConfirmation === true ? t('investigation.confirmationRequired') : t('investigation.safe')
}

function confidenceClass(score: number): string {
  if (score >= 90) return 'confirmed'
  if (score < 10) return 'refuted'
  if (score >= 70) return 'strong'
  if (score <= 30) return 'weak'
  return 'active'
}

function statusText(status: string): string {
  if (status === 'confirmed') return t('investigation.statusConfirmed')
  if (status === 'refuted') return t('investigation.statusRefuted')
  return t('investigation.statusActive')
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
  void effectProof.refresh()
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
        <h1>{{ $t('investigation.title') }}</h1>
        <p>{{ $t('investigation.subtitle') }}</p>
      </div>
      <div class="actions">
        <button class="btn" @click="store.refreshAutoResolveReport()">{{ $t('investigation.aiReport') }}</button>
        <button class="btn" :disabled="!run" @click="showMarkdown()">{{ $t('investigation.markdown') }}</button>
        <button class="btn" :disabled="!run" @click="showJson()">{{ $t('investigation.json') }}</button>
        <button class="btn" :disabled="!run" @click="store.saveCurrentWorkspaceProject()">{{ $t('investigation.saveProject') }}</button>
        <button class="btn" :disabled="!run" @click="store.finishInvestigation('done')">{{ $t('investigation.archiveAction') }}</button>
        <button class="btn danger" :disabled="!run" @click="store.clearInvestigation()">{{ $t('investigation.clear') }}</button>
      </div>
    </header>

    <PanelIntro
      :what="$t('investigation.intro.what')"
      :purpose="$t('investigation.intro.purpose')"
      :how="$t('investigation.intro.how')"
    />

    <PanelIntro
      :what="$t('investigation.notebookIntro.what')"
      :purpose="$t('investigation.notebookIntro.purpose')"
      :how="$t('investigation.notebookIntro.how')"
    />

    <section class="panel notebook-panel">
      <div class="section-head">
        <h2>{{ $t('investigation.notebookTitle') }}</h2>
        <span>{{ $t('investigation.hypothesisCount', { count: notebook.totalCount }) }}</span>
      </div>
      <form class="notebook-plan-form" @submit.prevent="notebook.proposePlanFromSymptom()">
        <input
          v-model="notebook.symptomDraft"
          class="filter-input"
          :placeholder="$t('investigation.symptomPlaceholder')"
          :disabled="notebook.generationBusy"
        />
        <button class="btn primary" type="submit" :disabled="notebook.generationBusy || !notebook.symptomDraft.trim()">
          {{ $t('investigation.suggestLeads') }}
        </button>
      </form>
      <article v-if="notebook.suggestedNextTest" class="next-test-card">
        <div class="hypothesis-head">
          <strong>{{ notebook.suggestedNextTest.title }}</strong>
          <span>{{ notebook.suggestedNextTest.risk }}</span>
        </div>
        <p v-if="notebook.suggestedNextTest.rationale">{{ notebook.suggestedNextTest.rationale }}</p>
        <div class="meta">
          <span>{{ notebook.suggestedNextTest.tool || $t('investigation.observationFallback') }}</span>
          <span v-if="notebook.lastPlanSource">{{ notebook.lastPlanSource }}</span>
        </div>
        <div class="next-test-grid">
          <div>
            <h3>{{ $t('investigation.preconditions') }}</h3>
            <p v-for="item in notebook.suggestedNextTest.preconditions" :key="item">{{ item }}</p>
          </div>
          <div>
            <h3>{{ $t('investigation.ifConfirmed') }}</h3>
            <p>{{ notebook.suggestedNextTest.expectedIfTrue || '-' }}</p>
          </div>
          <div>
            <h3>{{ $t('investigation.ifContradicted') }}</h3>
            <p>{{ notebook.suggestedNextTest.expectedIfFalse || '-' }}</p>
          </div>
        </div>
      </article>
      <form class="notebook-form" @submit.prevent="notebook.addHypothesis()">
        <input
          v-model="notebook.hypothesisDraft"
          class="filter-input"
          :placeholder="$t('investigation.hypothesisPlaceholder')"
          :disabled="notebook.busy"
        />
        <label>
          <span>{{ $t('investigation.startingScore') }}</span>
          <input
            v-model.number="notebook.baselineScore"
            class="filter-input score-input"
            type="number"
            min="0"
            max="100"
            :disabled="notebook.busy"
          />
        </label>
        <button class="btn primary" type="submit" :disabled="notebook.busy || !notebook.hypothesisDraft.trim()">{{ $t('investigation.add') }}</button>
        <button class="btn" type="button" :disabled="notebook.busy || notebook.totalCount === 0" @click="notebook.resetNotebook()">{{ $t('investigation.newNotebook') }}</button>
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
                :placeholder="$t('investigation.evidenceNotePlaceholder')"
                :disabled="notebook.busy"
              />
              <button class="btn mini" type="button" :disabled="notebook.busy" @click="notebook.recordTestResult(item.id, true)">{{ $t('investigation.confirm') }}</button>
              <button class="btn mini" type="button" :disabled="notebook.busy" @click="notebook.recordTestResult(item.id, false)">{{ $t('investigation.contradict') }}</button>
            </div>
          </article>
          <p v-if="section.items.length === 0" class="muted">{{ $t('investigation.noHypothesis') }}</p>
        </section>
      </div>
    </section>

    <PanelIntro
      :what="$t('investigation.effectProofIntro.what')"
      :purpose="$t('investigation.effectProofIntro.purpose')"
      :how="$t('investigation.effectProofIntro.how')"
    />

    <section class="panel effect-proof-panel">
      <div class="section-head">
        <h2>{{ $t('investigation.effectProofTitle') }}</h2>
      </div>
      <p v-if="effectProof.overallNextAction" class="next-action-banner">{{ effectProof.overallNextAction }}</p>
      <form class="notebook-form" @submit.prevent="effectProof.recordProof()">
        <input
          v-model="effectProof.targetLabelDraft"
          class="filter-input"
          :placeholder="$t('investigation.effectProofTargetLabelPlaceholder')"
          :disabled="effectProof.busy"
        />
        <input
          v-model="effectProof.addressDraft"
          class="filter-input"
          :placeholder="$t('investigation.effectProofAddressPlaceholder')"
          :disabled="effectProof.busy"
        />
        <label>
          <span>{{ $t('investigation.effectProofLevelLabel') }}</span>
          <select v-model="effectProof.levelDraft" class="filter-input" :disabled="effectProof.busy">
            <option v-for="option in effectProofLevelOptions" :key="option.value" :value="option.value">{{ option.label }}</option>
          </select>
        </label>
        <input
          v-model="effectProof.sourceDraft"
          class="filter-input"
          :placeholder="$t('investigation.effectProofSourcePlaceholder')"
          :disabled="effectProof.busy"
        />
        <input
          v-model="effectProof.conditionsDraft"
          class="filter-input"
          :placeholder="$t('investigation.effectProofConditionsPlaceholder')"
          :disabled="effectProof.busy"
        />
        <input
          v-model="effectProof.noteDraft"
          class="filter-input"
          :placeholder="$t('investigation.effectProofNotePlaceholder')"
          :disabled="effectProof.busy"
        />
        <button
          class="btn primary"
          type="submit"
          :disabled="effectProof.busy || (!effectProof.targetLabelDraft.trim() && !effectProof.addressDraft.trim())"
        >
          {{ $t('investigation.effectProofRecord') }}
        </button>
        <button
          class="btn"
          type="button"
          :disabled="effectProof.busy || (effectProof.known.length === 0 && effectProof.uncertain.length === 0)"
          @click="effectProof.resetLedger()"
        >
          {{ $t('investigation.effectProofReset') }}
        </button>
      </form>
      <p v-if="effectProof.error" class="error">{{ effectProof.error }}</p>
      <div class="notebook-grid">
        <section class="notebook-column">
          <div class="notebook-column-head">
            <h3>{{ $t('investigation.effectProofKnownSection') }}</h3>
            <span>{{ effectProof.known.length }}</span>
          </div>
          <article v-for="item in effectProof.known" :key="item.targetKey" class="hypothesis-card confirmed">
            <div class="hypothesis-head">
              <strong>{{ item.targetLabel || item.address }}</strong>
              <span>{{ effectProofLevelLabel(item.bestLevel) }}</span>
            </div>
            <p v-if="item.address && item.targetLabel">{{ item.address }}</p>
            <p class="evidence-preview">{{ item.nextAction }}</p>
            <div class="effect-proof-actions">
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'observed')">{{ $t('investigation.effectProofQuickObserved') }}</button>
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'absent')">{{ $t('investigation.effectProofQuickAbsent') }}</button>
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'inconclusive')">{{ $t('investigation.effectProofQuickInconclusive') }}</button>
              <button type="button" class="btn mini" @click="effectProof.startLinkToProfile(item)">{{ $t('investigation.effectProofLinkButton') }}</button>
              <button type="button" class="btn mini" @click="toggleHistory(item.targetKey)">{{ $t('investigation.effectProofHistoryToggle', { count: item.history.length }) }}</button>
            </div>
            <ul v-if="openHistoryKeys.has(item.targetKey)" class="effect-proof-history">
              <li v-for="record in item.history" :key="record.id">
                <strong>{{ effectProofLevelLabel(record.level) }}</strong>
                <span v-if="record.recordedAt">— {{ record.recordedAt }}</span>
                <span v-if="record.source"> — {{ record.source }}</span>
                <p v-if="record.note">{{ record.note }}</p>
              </li>
              <li v-if="item.history.length === 0" class="muted">{{ $t('investigation.effectProofHistoryEmpty') }}</li>
            </ul>
          </article>
          <p v-if="effectProof.known.length === 0" class="muted">{{ $t('investigation.effectProofEmpty') }}</p>
        </section>
        <section class="notebook-column">
          <div class="notebook-column-head">
            <h3>{{ $t('investigation.effectProofUncertainSection') }}</h3>
            <span>{{ effectProof.uncertain.length }}</span>
          </div>
          <article v-for="item in effectProof.uncertain" :key="item.targetKey" class="hypothesis-card">
            <div class="hypothesis-head">
              <strong>{{ item.targetLabel || item.address }}</strong>
              <span>{{ effectProofLevelLabel(item.bestLevel) }}</span>
            </div>
            <p v-if="item.address && item.targetLabel">{{ item.address }}</p>
            <p class="evidence-preview">{{ item.nextAction }}</p>
            <div class="effect-proof-actions">
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'observed')">{{ $t('investigation.effectProofQuickObserved') }}</button>
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'absent')">{{ $t('investigation.effectProofQuickAbsent') }}</button>
              <button type="button" class="btn mini" @click="effectProof.prefillObservation(item, 'inconclusive')">{{ $t('investigation.effectProofQuickInconclusive') }}</button>
              <button type="button" class="btn mini" @click="effectProof.startLinkToProfile(item)">{{ $t('investigation.effectProofLinkButton') }}</button>
              <button type="button" class="btn mini" @click="toggleHistory(item.targetKey)">{{ $t('investigation.effectProofHistoryToggle', { count: item.history.length }) }}</button>
            </div>
            <ul v-if="openHistoryKeys.has(item.targetKey)" class="effect-proof-history">
              <li v-for="record in item.history" :key="record.id">
                <strong>{{ effectProofLevelLabel(record.level) }}</strong>
                <span v-if="record.recordedAt">— {{ record.recordedAt }}</span>
                <span v-if="record.source"> — {{ record.source }}</span>
                <p v-if="record.note">{{ record.note }}</p>
              </li>
              <li v-if="item.history.length === 0" class="muted">{{ $t('investigation.effectProofHistoryEmpty') }}</li>
            </ul>
          </article>
          <p v-if="effectProof.uncertain.length === 0" class="muted">{{ $t('investigation.effectProofEmpty') }}</p>
        </section>
      </div>

      <form v-if="effectProof.linkingItem" class="notebook-form effect-proof-link-form" @submit.prevent="effectProof.confirmLinkToProfile()">
        <h3>{{ $t('investigation.effectProofLinkTitle') }} — {{ effectProof.linkingItem.targetLabel || effectProof.linkingItem.address }}</h3>
        <p v-if="effectProof.linkedProofVersionMismatch()" class="warning">{{ $t('investigation.effectProofLinkVersionMismatch') }}</p>
        <label>
          <span>{{ $t('investigation.effectProofLinkProfileLabel') }}</span>
          <input
            v-model="effectProof.linkProfileName"
            class="filter-input"
            :placeholder="$t('investigation.effectProofLinkProfilePlaceholder')"
            :disabled="effectProof.linkBusy"
          />
        </label>
        <button
          type="button"
          class="btn mini"
          :disabled="effectProof.linkBusy || !effectProof.linkProfileName.trim()"
          @click="effectProof.loadProfileEntriesForLink()"
        >
          {{ $t('investigation.effectProofLinkLoadEntries') }}
        </button>
        <label>
          <span>{{ $t('investigation.effectProofLinkEntryLabel') }}</span>
          <select
            :value="`${effectProof.linkEntryKind}:${effectProof.linkEntryName}`"
            class="filter-input"
            :disabled="effectProof.linkBusy || effectProof.linkProfileEntries.length === 0"
            @change="onLinkEntrySelected(($event.target as HTMLSelectElement).value)"
          >
            <option value=":" disabled>{{ $t('investigation.effectProofLinkEntryPlaceholder') }}</option>
            <option
              v-for="entry in effectProof.linkProfileEntries"
              :key="`${entry.kind}:${entry.name}`"
              :value="`${entry.kind}:${entry.name}`"
            >
              {{ entry.name }} ({{ entry.kind === 'target' ? $t('investigation.effectProofLinkEntryKindTarget') : $t('investigation.effectProofLinkEntryKindPatch') }})
            </option>
          </select>
        </label>
        <label>
          <span>{{ $t('investigation.effectProofLinkKindLabel') }}</span>
          <select v-model="effectProof.linkNoteKind" class="filter-input" :disabled="effectProof.linkBusy">
            <option v-for="option in linkNoteKindOptions" :key="option.value" :value="option.value">{{ option.label }}</option>
          </select>
        </label>
        <input
          v-model="effectProof.linkDescription"
          class="filter-input effect-proof-link-description"
          :placeholder="$t('investigation.effectProofLinkDescriptionPlaceholder')"
          :disabled="effectProof.linkBusy"
        />
        <p v-if="effectProof.linkError" class="error">{{ effectProof.linkError }}</p>
        <p v-if="effectProof.linkMessage" class="muted">{{ effectProof.linkMessage }}</p>
        <button
          class="btn primary"
          type="submit"
          :disabled="effectProof.linkBusy || !effectProof.linkProfileName.trim() || !effectProof.linkEntryName.trim()"
        >
          {{ $t('investigation.effectProofLinkConfirm') }}
        </button>
        <button class="btn" type="button" :disabled="effectProof.linkBusy" @click="effectProof.cancelLinkToProfile()">
          {{ $t('investigation.effectProofLinkCancel') }}
        </button>
      </form>
    </section>

    <section v-if="run" class="summary">
      <div>
        <span>{{ $t('investigation.objective') }}</span>
        <strong>{{ run.objective }}</strong>
      </div>
      <div>
        <span>{{ $t('investigation.process') }}</span>
        <strong>{{ run.processName || $t('investigation.notAttached') }}</strong>
      </div>
      <div>
        <span>{{ $t('investigation.status') }}</span>
        <strong>{{ statusLabel(run.status) }}</strong>
      </div>
      <div>
        <span>{{ $t('investigation.strategy') }}</span>
        <strong>{{ run.preferredStrategy?.label ?? $t('investigation.notDetermined') }}</strong>
      </div>
    </section>

    <section v-if="autoReport" class="panel report">
      <div class="section-head">
        <h2>{{ $t('investigation.aiReport') }}</h2>
        <span>{{ autoReport.processName || $t('investigation.notAttached') }} · {{ $t('investigation.candidateCount', { count: autoReport.candidateCount }) }}</span>
      </div>
      <p>{{ autoReport.summary || $t('investigation.reportAvailable') }}</p>
      <div class="report-grid">
        <div>
          <span>{{ $t('investigation.strategy') }}</span>
          <strong>{{ autoReport.preferredStrategy?.label ?? $t('investigation.notDetermined') }}</strong>
        </div>
        <div>
          <span>{{ $t('investigation.workflow') }}</span>
          <strong>{{ autoReport.workflow || '-' }}</strong>
        </div>
        <div>
          <span>{{ $t('investigation.value') }}</span>
          <strong>{{ autoReport.initialValue || autoReport.targetValue || '-' }}</strong>
        </div>
      </div>
      <div class="report-lists">
        <div>
          <h3>{{ $t('investigation.recommendations') }}</h3>
          <p v-for="item in reportRecommendations.slice(0, 4)" :key="String(item.id ?? item.label)" class="report-item">
            {{ item.label ?? item.id }} · {{ item.reason ?? '' }}
          </p>
          <p v-if="reportRecommendations.length === 0" class="muted">{{ $t('investigation.noRecommendation') }}</p>
        </div>
        <div>
          <h3>{{ $t('investigation.guardrails') }}</h3>
          <p v-for="item in reportGuardrails.slice(0, 4)" :key="String(item.id ?? item.label)" class="report-item">
            {{ item.label ?? item.id }} · {{ item.reason ?? '' }}
          </p>
          <p v-if="reportGuardrails.length === 0" class="muted">{{ $t('investigation.noGuardrail') }}</p>
        </div>
      </div>
    </section>

    <section v-if="bestNextAction" class="panel next-action">
      <div class="section-head">
        <h2>{{ $t('investigation.bestNextActionTitle') }}</h2>
        <span>{{ bestNextAction.source }}</span>
      </div>
      <div class="next-action-body">
        <strong>{{ bestNextAction.label }}</strong>
        <p>{{ bestNextAction.reason }}</p>
        <div class="meta">
          <span>{{ bestNextAction.risk }}</span>
          <span v-if="bestNextAction.confidence">{{ $t('investigation.confidenceLabel', { score: bestNextAction.confidence }) }}</span>
        </div>
      </div>
    </section>

    <div v-if="!run" class="empty">
      <h2>{{ $t('investigation.noActiveInvestigation') }}</h2>
      <p>{{ $t('investigation.noActiveInvestigationBody') }}</p>
      <p>{{ $t('investigation.emptyBodyPrefix') }} <strong>Assistant</strong>{{ $t('investigation.emptyBodyMiddle') }} <strong>Auto</strong>{{ $t('investigation.emptyBodySuffix') }}</p>
      <button class="btn primary" @click="store.activeView = 'assistant'">{{ $t('investigation.goToAssistant') }}</button>
    </div>

    <div v-else class="grid">
      <section class="panel timeline">
        <div class="section-head">
          <h2>{{ $t('investigation.steps') }}</h2>
          <span>{{ filteredSteps.length }} / {{ steps.length }}</span>
        </div>
        <div class="filters">
          <input v-model="searchFilter" class="filter-input" :placeholder="$t('investigation.searchTimelinePlaceholder')" />
          <select v-model="statusFilter" class="filter-input">
            <option value="all">{{ $t('investigation.allStatuses') }}</option>
            <option value="planned">planned</option>
            <option value="running">running</option>
            <option value="success">success</option>
            <option value="warning">warning</option>
            <option value="error">error</option>
            <option value="checkpoint">checkpoint</option>
          </select>
          <select v-model="riskFilter" class="filter-input">
            <option value="all">{{ $t('investigation.allRisks') }}</option>
            <option value="safe">safe</option>
            <option value="write">write</option>
            <option value="debug">debug</option>
            <option value="patch">patch</option>
            <option value="inject">inject</option>
          </select>
          <select v-model="toolFilter" class="filter-input">
            <option value="all">{{ $t('investigation.allTools') }}</option>
            <option v-for="tool in toolOptions" :key="tool" :value="tool">{{ tool }}</option>
          </select>
        </div>
        <article v-for="step in filteredSteps" :key="step.id" class="step" :class="stepClass(step.status)">
          <div class="step-head">
            <strong>{{ step.title }}</strong>
            <span>{{ step.time }}</span>
          </div>
          <p>{{ step.detail }}</p>
          <p v-if="stepStrategyReason(step)" class="step-reason">{{ $t('investigation.why', { reason: stepStrategyReason(step) }) }}</p>
          <div class="meta">
            <span>{{ step.status }}</span>
            <span v-if="step.tool">{{ step.tool }}</span>
            <span v-if="step.risk">{{ step.risk }}</span>
          </div>
        </article>
        <p v-if="filteredSteps.length === 0" class="muted">{{ $t('investigation.noStepMatches') }}</p>
      </section>

      <aside class="side">
        <section class="panel">
          <h2>{{ $t('investigation.hypothesesTitle') }}</h2>
          <article v-for="item in hypotheses" :key="String(item.id ?? item.label)" class="card">
            <strong>{{ item.label ?? item.id }}</strong>
            <p>{{ item.reason ?? $t('investigation.hypothesisFallbackReason') }}</p>
          </article>
          <p v-if="hypotheses.length === 0" class="muted">{{ $t('investigation.noHypothesisYet') }}</p>
        </section>

        <section class="panel">
          <h2>{{ $t('investigation.checkpointsTitle') }}</h2>
          <article v-for="item in sortedCheckpoints" :key="String(item.id ?? item.address ?? item.label)" class="card checkpoint">
            <strong>{{ checkpointTitle(item) }}</strong>
            <div class="checkpoint-badges">
              <span>{{ checkpointKindLabel(item) }}</span>
              <span v-if="checkpointScoreLabel(item)">{{ checkpointScoreLabel(item) }}</span>
              <span :class="item.requiresConfirmation === true ? 'risk-badge' : 'safe-badge'">{{ checkpointRiskLabel(item) }}</span>
            </div>
            <p>{{ checkpointDetail(item) || $t('investigation.confirmationRequiredBeforeAction') }}</p>
            <p v-if="checkpointStrategyReason(item)" class="step-reason">{{ $t('investigation.why', { reason: checkpointStrategyReason(item) }) }}</p>
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
              <button v-if="checkpointAddress(item)" class="btn mini" :title="checkpointPlanReason(item, 'watch')" @click="watchCheckpoint(item)">{{ $t('investigation.watch') }}</button>
              <button v-if="checkpointCanWrite(item)" class="btn mini" :title="checkpointPlanReason(item, 'write')" @click="store.executeCheckpointWrite(item, false)">{{ $t('investigation.prepareWrite') }}</button>
              <button v-if="checkpointCanWrite(item)" class="btn mini" :title="checkpointPlanReason(item, 'freeze_polling')" @click="store.executeCheckpointWrite(item, true)">{{ $t('investigation.freeze') }}</button>
              <button v-if="checkpointCanDebug(item)" class="btn mini" :title="checkpointPlanReason(item, 'find_writes')" @click="store.executeCheckpointFindWhatWrites(item)">{{ $t('investigation.findWhatWrites') }}</button>
              <button v-if="checkpointCanAob(item)" class="btn mini" :title="checkpointPlanReason(item, 'aob_patch')" @click="store.prepareCheckpointAob(item)">{{ $t('investigation.aobPatch') }}</button>
              <button v-if="checkpointCanForceValue(item)" class="btn mini" :title="checkpointPlanReason(item, 'force_value')" @click="selectForceValueTarget(item)">{{ $t('investigation.forceValueHook') }}</button>
              <button class="btn mini" @click="bookmarkCheckpoint(item)">{{ $t('investigation.bookmark') }}</button>
              <button v-if="checkpointAddress(item)" class="btn mini" :title="checkpointPlanReason(item, 'trainer')" @click="createFromCheckpoint(item)">{{ $t('investigation.createTrainer') }}</button>
            </div>
            <div v-if="forceValueTarget === item" class="checkpoint-actions">
              <input
                v-model="forceValueInput"
                class="input"
                :placeholder="$t('investigation.valuePlaceholder')"
                :disabled="forceValueBusy"
                @keyup.enter="applyForceValue()"
              />
              <button class="btn mini" type="button" :disabled="forceValueBusy || !forceValueInput.trim()" @click="applyForceValue()">{{ $t('investigation.apply') }}</button>
            </div>
            <p v-if="forceValueTarget === item && forceValueResult" :class="forceValueResult.success ? 'hint' : 'error'">
              {{ forceValueResult.success
                ? $t('investigation.valueForcedSuccess', { address: forceValueResult.patchAddress })
                : forceValueResult.error }}
            </p>
          </article>
          <p v-if="checkpoints.length === 0" class="muted">{{ $t('investigation.noActiveCheckpoint') }}</p>
        </section>
      </aside>
    </div>

    <section v-if="exportText" class="panel export">
      <div class="export-head">
        <h2>{{ $t('investigation.exportTitle') }}</h2>
        <button class="btn" @click="exportText = ''">{{ $t('investigation.close') }}</button>
      </div>
      <pre>{{ exportText }}</pre>
    </section>

    <section class="panel">
      <div class="export-head">
        <h2>{{ $t('investigation.uwpStateTitle') }}</h2>
        <span>{{ $t('investigation.beforeAfterCount', { before: store.saveFileSnapshotBefore.length, after: store.saveFileSnapshotAfter.length }) }}</span>
      </div>
      <p class="muted">
        {{ $t('investigation.uwpStateExplanation') }}
      </p>
      <div class="topbar">
        <button
          class="btn"
          :disabled="store.saveFileSnapshotBusy || !store.isAttached"
          @click="store.takeSaveFileSnapshotBefore()"
        >
          {{ $t('investigation.snapshotBefore') }}
        </button>
        <button
          class="btn"
          :disabled="store.saveFileSnapshotBusy || !store.isAttached"
          @click="store.takeSaveFileSnapshotAfter()"
        >
          {{ $t('investigation.snapshotAfter') }}
        </button>
        <button
          class="btn"
          :disabled="store.saveFileSnapshotBusy || store.saveFileSnapshotBefore.length === 0 || store.saveFileSnapshotAfter.length === 0"
          @click="store.compareSaveFileSnapshots()"
        >
          {{ $t('investigation.compare') }}
        </button>
      </div>
      <template v-if="store.saveFileSnapshotDiff">
        <template v-if="store.saveFileSnapshotDiff.success">
          <p class="muted">
            {{ $t('investigation.diffCounts', {
              added: store.saveFileSnapshotDiff.addedCount ?? 0,
              removed: store.saveFileSnapshotDiff.removedCount ?? 0,
              modified: store.saveFileSnapshotDiff.modifiedCount ?? 0,
              unchanged: store.saveFileSnapshotDiff.unchangedCount ?? 0,
            }) }}
          </p>
          <article
            v-for="entry in store.saveFileSnapshotDiff.modified"
            :key="entry.path"
            class="archive-row"
          >
            <div>
              <strong>{{ entry.path }}</strong>
              <span>
                {{ $t('investigation.bytesChange', {
                  before: entry.sizeBytesBefore,
                  after: entry.sizeBytesAfter,
                  sign: (entry.sizeDeltaBytes ?? 0) >= 0 ? '+' : '',
                  delta: entry.sizeDeltaBytes,
                }) }}
              </span>
            </div>
          </article>
          <article
            v-for="entry in store.saveFileSnapshotDiff.added"
            :key="entry.path"
            class="archive-row"
          >
            <div>
              <strong>{{ entry.path }}</strong>
              <span>{{ $t('investigation.newFileBytes', { bytes: entry.sizeBytes }) }}</span>
            </div>
          </article>
          <p
            v-if="(store.saveFileSnapshotDiff.modified?.length ?? 0) === 0 && (store.saveFileSnapshotDiff.added?.length ?? 0) === 0"
            class="muted"
          >
            {{ $t('investigation.noFileChanged') }}
          </p>
        </template>
        <p v-else class="muted">{{ store.saveFileSnapshotDiff.error }}</p>
      </template>
    </section>

    <section class="panel archive">
      <div class="export-head">
        <h2>{{ $t('investigation.archivesTitle') }}</h2>
        <button class="btn" :disabled="store.investigationArchive.length === 0" @click="store.clearInvestigationArchive()">{{ $t('investigation.clearArchive') }}</button>
      </div>
      <article v-for="item in store.investigationArchive" :key="item.id" class="archive-row">
        <div>
          <strong>{{ item.objective }}</strong>
          <span>{{ $t('investigation.archiveStepsCount', { status: statusLabel(item.status), count: item.steps.length }) }}</span>
        </div>
        <button class="btn mini" @click="store.restoreInvestigationFromArchive(item.id)">{{ $t('investigation.restore') }}</button>
      </article>
      <p v-if="store.investigationArchive.length === 0" class="muted">{{ $t('investigation.noArchive') }}</p>
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

.next-action-banner {
  margin-bottom: 14px;
  padding: 10px 14px;
  border-radius: 8px;
  border: 1px solid rgba(122, 162, 247, 0.34);
  background: rgba(122, 162, 247, 0.07);
  color: var(--text-primary);
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

.notebook-plan-form {
  display: grid;
  grid-template-columns: minmax(240px, 1fr) auto;
  gap: 8px;
  align-items: center;
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

.next-test-card {
  border: 1px solid rgba(122, 162, 247, 0.28);
  border-radius: 8px;
  background: var(--bg-primary);
  padding: 10px;
}

.next-test-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
  margin-top: 8px;
}

.next-test-grid div {
  min-width: 0;
}

.next-test-grid h3 {
  margin: 0 0 5px;
  color: var(--text-dim);
  font-size: 11px;
  text-transform: uppercase;
}

.next-test-grid p {
  margin: 4px 0 0;
  font-size: 12px;
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

.effect-proof-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.effect-proof-history {
  margin: 8px 0 0;
  padding-left: 16px;
  font-size: 12px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.effect-proof-history p {
  margin: 2px 0 0;
  opacity: 0.85;
}

.effect-proof-link-form {
  margin-top: 12px;
  border-top: 1px solid rgba(125, 142, 255, 0.16);
  padding-top: 12px;
}

.effect-proof-link-form .warning {
  color: var(--warning);
  font-weight: 600;
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
  .notebook-plan-form,
  .notebook-form,
  .notebook-grid,
  .next-test-grid,
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
