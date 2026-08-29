/**
 * KillEngine — store Investigation (extrait de app.ts, candidat S6 de
 * docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Volontairement sans dépendance vers `./app` : c'est une fondation partagée
 * appelée par ~35 sites répartis dans app.ts (scan, checkpoints, chat,
 * trainer...), pas l'inverse — un import circulaire ici casserait cette
 * direction de dépendance. `processName`/`searchQuery`/l'objectif de repli
 * sont donc reçus en paramètres optionnels plutôt que lus directement sur
 * le store principal ; le journal d'action (`addActionLog`, pas encore
 * extrait) reste du ressort de l'appelant — voir `restoreInvestigationFromArchive`.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'

export interface InvestigationStep {
  id: number
  time: string
  title: string
  detail: string
  status: 'planned' | 'running' | 'success' | 'warning' | 'error' | 'checkpoint'
  tool?: string
  risk?: 'safe' | 'write' | 'debug' | 'patch' | 'injection'
  confidence?: number
  payload?: Record<string, unknown>
}

export interface InvestigationRun {
  id: number
  title: string
  objective: string
  processName: string
  startedAt: string
  updatedAt: string
  status: 'active' | 'checkpoint' | 'done' | 'blocked'
  preferredStrategy?: Record<string, unknown>
  summary: string
  steps: InvestigationStep[]
  hypotheses: Array<Record<string, unknown>>
  checkpoints: Array<Record<string, unknown>>
}

function nowTime(): string {
  return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export const useInvestigationStore = defineStore('investigation', () => {
  const activeInvestigation = ref<InvestigationRun | null>(null)
  const investigationArchive = ref<InvestigationRun[]>([])
  const investigationStepIdCounter = ref(0)
  const investigationRunIdCounter = ref(0)

  const investigationStorageKey = 'killengine.investigation.v1'

  function saveInvestigations() {
    try {
      window.localStorage.setItem(investigationStorageKey, JSON.stringify({
        active: activeInvestigation.value,
        archive: investigationArchive.value.slice(0, 20),
        stepId: investigationStepIdCounter.value,
        runId: investigationRunIdCounter.value,
      }))
    } catch {
      // Best-effort persistence: analysis must keep working even if storage is unavailable.
    }
  }

  function loadInvestigations() {
    try {
      const raw = window.localStorage.getItem(investigationStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as {
        active?: InvestigationRun | null
        archive?: InvestigationRun[]
        stepId?: number
        runId?: number
      }
      activeInvestigation.value = parsed.active ?? null
      investigationArchive.value = Array.isArray(parsed.archive) ? parsed.archive.slice(0, 20) : []
      investigationStepIdCounter.value = Number(parsed.stepId ?? 0)
      investigationRunIdCounter.value = Number(parsed.runId ?? 0)
    } catch {
      activeInvestigation.value = null
      investigationArchive.value = []
    }
  }

  function startInvestigation(objective: string, title = 'Investigation Auto', processNameValue = '') {
    if (activeInvestigation.value) {
      investigationArchive.value.unshift({
        ...activeInvestigation.value,
        status: activeInvestigation.value.status === 'active' ? 'blocked' : activeInvestigation.value.status,
      })
      investigationArchive.value = investigationArchive.value.slice(0, 20)
    }

    investigationRunIdCounter.value += 1
    const now = new Date().toISOString()
    activeInvestigation.value = {
      id: investigationRunIdCounter.value,
      title,
      objective,
      processName: processNameValue,
      startedAt: now,
      updatedAt: now,
      status: 'active',
      summary: '',
      steps: [],
      hypotheses: [],
      checkpoints: [],
    }
    saveInvestigations()
  }

  function addInvestigationStep(
    step: Omit<InvestigationStep, 'id' | 'time'>,
    fallbackObjective = 'Investigation manuelle',
    processNameValue = '',
  ) {
    if (!activeInvestigation.value) {
      startInvestigation(fallbackObjective, undefined, processNameValue)
    }
    if (!activeInvestigation.value) return

    investigationStepIdCounter.value += 1
    activeInvestigation.value.steps.unshift({
      id: investigationStepIdCounter.value,
      time: nowTime(),
      ...step,
    })
    activeInvestigation.value.updatedAt = new Date().toISOString()
    if (step.status === 'checkpoint') activeInvestigation.value.status = 'checkpoint'
    if (step.status === 'error') activeInvestigation.value.status = 'blocked'
    saveInvestigations()
  }

  function updateInvestigationFromAutoResult(result: Record<string, unknown>) {
    if (!activeInvestigation.value) return

    const contextReport = typeof result.contextReport === 'object' && result.contextReport !== null
      ? result.contextReport as Record<string, unknown>
      : {}
    const preferredStrategy = typeof contextReport.preferredStrategy === 'object' && contextReport.preferredStrategy !== null
      ? contextReport.preferredStrategy as Record<string, unknown>
      : undefined
    const recommendations = Array.isArray(contextReport.recommendations)
      ? contextReport.recommendations as Array<Record<string, unknown>>
      : []
    const nextBestAction = typeof contextReport.nextBestAction === 'object' && contextReport.nextBestAction !== null
      ? contextReport.nextBestAction as Record<string, unknown>
      : null
    const telemetryInsights = Array.isArray(contextReport.telemetryInsights)
      ? contextReport.telemetryInsights as Array<Record<string, unknown>>
      : []
    const displayValueReport = typeof contextReport.displayValueReport === 'object' && contextReport.displayValueReport !== null
      ? contextReport.displayValueReport as Record<string, unknown>
      : {}
    const displayValueHypothesis = displayValueReport.enabled === true
      ? [{
          id: 'display_value_report',
          label: 'Rapport valeurs affichees',
          reason: String(displayValueReport.recommendation ?? 'Trace UI string et sources numeriques avant debugger.'),
          safe: true,
          traceUiSourceCount: displayValueReport.traceUiSourceCount,
          globalValueHits: displayValueReport.globalValueHits,
        }]
      : []
    const suggestedWrites = Array.isArray(result.suggestedWrites)
      ? result.suggestedWrites as Array<Record<string, unknown>>
      : []
    const plan = Array.isArray(result.plan)
      ? result.plan as Array<Record<string, unknown>>
      : []

    activeInvestigation.value.preferredStrategy = preferredStrategy
    activeInvestigation.value.summary = String(result.message ?? result.error ?? '').trim()
    activeInvestigation.value.hypotheses = [
      ...(nextBestAction ? [{ ...nextBestAction, id: 'next_best_action', label: `Priorité: ${String(nextBestAction.label ?? nextBestAction.id ?? 'action')}` }] : []),
      ...displayValueHypothesis,
      ...telemetryInsights,
      ...recommendations,
    ].slice(0, 8)
    activeInvestigation.value.checkpoints = [
      ...suggestedWrites.map((item) => ({ ...item, kind: 'suggested_write', requiresConfirmation: true })),
      ...telemetryInsights.filter((item) => item.safe === false || item.requiresConfirmation === true),
      ...recommendations.filter((item) => item.safe === false || item.requiresConfirmation === true),
    ].slice(0, 12)

    if (plan.length > 0 && activeInvestigation.value.steps.length === 0) {
      for (const item of plan.slice().reverse()) {
        addInvestigationStep({
          title: String(item.description ?? item.type ?? 'Etape planifiee'),
          detail: String(item.type ?? 'planned'),
          status: 'planned',
          tool: String(item.type ?? ''),
          risk: 'safe',
          payload: item,
        })
      }
    }

    saveInvestigations()
  }

  function finishInvestigation(status: InvestigationRun['status'] = 'done') {
    if (!activeInvestigation.value) return
    activeInvestigation.value.status = status
    activeInvestigation.value.updatedAt = new Date().toISOString()
    investigationArchive.value.unshift(activeInvestigation.value)
    investigationArchive.value = investigationArchive.value.slice(0, 20)
    activeInvestigation.value = null
    saveInvestigations()
  }

  function clearInvestigation() {
    activeInvestigation.value = null
    saveInvestigations()
  }

  function clearInvestigationArchive() {
    investigationArchive.value = []
    saveInvestigations()
  }

  /** Ne journalise PAS dans addActionLog (pas encore extrait de app.ts) —
   * l'appelant (app.ts) journalise après un retour `true`. */
  function restoreInvestigationFromArchive(id: number): boolean {
    const index = investigationArchive.value.findIndex((item) => item.id === id)
    if (index < 0) return false

    if (activeInvestigation.value) {
      investigationArchive.value.unshift({
        ...activeInvestigation.value,
        status: activeInvestigation.value.status === 'active' ? 'blocked' : activeInvestigation.value.status,
        updatedAt: new Date().toISOString(),
      })
    }

    const [restored] = investigationArchive.value.splice(index + (activeInvestigation.value ? 1 : 0), 1)
    if (!restored) return false

    activeInvestigation.value = {
      ...restored,
      status: restored.status === 'done' ? 'checkpoint' : restored.status,
      updatedAt: new Date().toISOString(),
    }
    investigationArchive.value = investigationArchive.value.slice(0, 20)
    saveInvestigations()
    return true
  }

  function exportInvestigationJson(): string {
    return JSON.stringify(activeInvestigation.value ?? {}, null, 2)
  }

  return {
    activeInvestigation,
    investigationArchive,
    investigationStepIdCounter,
    investigationRunIdCounter,
    saveInvestigations,
    loadInvestigations,
    startInvestigation,
    addInvestigationStep,
    updateInvestigationFromAutoResult,
    finishInvestigation,
    clearInvestigation,
    clearInvestigationArchive,
    restoreInvestigationFromArchive,
    exportInvestigationJson,
  }
})
