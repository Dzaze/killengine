/**
 * KillEngine - store du carnet d'hypotheses PHASE 120-F.
 *
 * Le backend garde le moteur deterministe de ponderation ; le modele local
 * propose seulement des hypotheses et un prochain test.
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { backend } from '@/services/backend'
import { useActionLogStore } from './actionLog'

export type InvestigationHypothesisStatus = 'active' | 'confirmed' | 'refuted'

export interface InvestigationHypothesis {
  id: string
  description: string
  confidenceScore: number
  status: InvestigationHypothesisStatus
  evidenceLog: string[]
}

export interface InvestigationNextTest {
  title: string
  tool: string
  risk: string
  preconditions: string[]
  expectedIfTrue: string
  expectedIfFalse: string
  rationale: string
}

function asHypothesis(value: unknown): InvestigationHypothesis | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  const id = String(item.id ?? '').trim()
  const description = String(item.description ?? '').trim()
  const status = String(item.status ?? 'active') as InvestigationHypothesisStatus
  const confidenceScore = Number(item.confidenceScore ?? 0)
  if (!id || !description || !['active', 'confirmed', 'refuted'].includes(status)) return null
  return {
    id,
    description,
    confidenceScore: Number.isFinite(confidenceScore) ? Math.max(0, Math.min(100, confidenceScore)) : 0,
    status,
    evidenceLog: Array.isArray(item.evidenceLog) ? item.evidenceLog.map((entry) => String(entry)) : [],
  }
}

function asHypothesisList(value: unknown): InvestigationHypothesis[] {
  if (!Array.isArray(value)) return []
  return value.map(asHypothesis).filter((item): item is InvestigationHypothesis => item !== null)
}

function asNextTest(value: unknown): InvestigationNextTest | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  const title = String(item.title ?? '').trim()
  if (!title) return null
  return {
    title,
    tool: String(item.tool ?? '').trim(),
    risk: String(item.risk ?? 'safe').trim(),
    preconditions: Array.isArray(item.preconditions) ? item.preconditions.map((entry) => String(entry)) : [],
    expectedIfTrue: String(item.expectedIfTrue ?? '').trim(),
    expectedIfFalse: String(item.expectedIfFalse ?? '').trim(),
    rationale: String(item.rationale ?? '').trim(),
  }
}

export const useInvestigationNotebookStore = defineStore('investigationNotebook', () => {
  const actionLogStore = useActionLogStore()

  const hypothesisDraft = ref('')
  const symptomDraft = ref('')
  const baselineScore = ref(50)
  const confirmed = ref<InvestigationHypothesis[]>([])
  const active = ref<InvestigationHypothesis[]>([])
  const refuted = ref<InvestigationHypothesis[]>([])
  const suggestedNextTest = ref<InvestigationNextTest | null>(null)
  const lastPlanSource = ref('')
  const generationBusy = ref(false)
  const evidenceNotes = ref<Record<string, string>>({})
  const busy = ref(false)
  const error = ref('')
  const lastResult = ref<Record<string, unknown> | null>(null)

  const totalCount = computed(() => confirmed.value.length + active.value.length + refuted.value.length)

  function applySynthesis(result: Record<string, unknown>) {
    confirmed.value = asHypothesisList(result.confirmed)
    active.value = asHypothesisList(result.active)
    refuted.value = asHypothesisList(result.refuted)
  }

  async function refreshNotebook() {
    const controller = backend.getController()
    if (!controller.getInvestigationNotebookSynthesis) {
      error.value = 'Carnet indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.getInvestigationNotebookSynthesis()
      lastResult.value = result
      if (result.success === true) {
        applySynthesis(result)
      } else {
        error.value = String(result.error ?? 'Synthese impossible.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      busy.value = false
    }
  }

  async function addHypothesis() {
    const description = hypothesisDraft.value.trim()
    if (!description) {
      error.value = 'Description vide.'
      return null
    }

    const controller = backend.getController()
    if (!controller.addInvestigationHypothesis) {
      error.value = 'Carnet indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.addInvestigationHypothesis(description, baselineScore.value)
      lastResult.value = result
      if (result.success === true) {
        hypothesisDraft.value = ''
        actionLogStore.addActionLog('investigation', 'Hypothese ajoutee', description, 'success')
        await refreshNotebook()
      } else {
        error.value = String(result.error ?? 'Ajout refuse.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      busy.value = false
    }
  }

  async function proposePlanFromSymptom() {
    const symptom = symptomDraft.value.trim()
    if (!symptom) {
      error.value = 'Symptome vide.'
      return null
    }

    const controller = backend.getController()
    if (!controller.proposeInvestigationNotebookPlan) {
      error.value = 'Generation du carnet indisponible dans ce backend.'
      return null
    }

    generationBusy.value = true
    busy.value = true
    error.value = ''
    try {
      const result = await controller.proposeInvestigationNotebookPlan(symptom, {
        baselineScore: baselineScore.value,
        useModel: true,
      })
      lastResult.value = result
      if (result.success === true) {
        applySynthesis(result)
        suggestedNextTest.value = asNextTest(result.nextTest)
        lastPlanSource.value = String(result.source ?? result.aiBackend ?? '')
        const addedCount = Array.isArray(result.addedHypotheses) ? result.addedHypotheses.length : 0
        actionLogStore.addActionLog(
          'investigation',
          'Plan carnet propose',
          `${addedCount} hypothese(s), prochain test: ${suggestedNextTest.value?.title ?? 'non defini'}`,
          'success',
        )
      } else {
        error.value = String(result.error ?? 'Generation refusee.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      generationBusy.value = false
      busy.value = false
    }
  }

  async function recordTestResult(hypothesisId: string, confirmedResult: boolean) {
    const controller = backend.getController()
    if (!controller.recordInvestigationTestResult) {
      error.value = 'Carnet indisponible dans ce backend.'
      return null
    }

    const note = String(evidenceNotes.value[hypothesisId] ?? '').trim()
    busy.value = true
    error.value = ''
    try {
      const result = await controller.recordInvestigationTestResult(hypothesisId, confirmedResult, note)
      lastResult.value = result
      if (result.success === true) {
        evidenceNotes.value = { ...evidenceNotes.value, [hypothesisId]: '' }
        actionLogStore.addActionLog(
          'investigation',
          confirmedResult ? 'Hypothese confirmee' : 'Hypothese contredite',
          `${hypothesisId}${note ? ` - ${note}` : ''}`,
          confirmedResult ? 'success' : 'warning',
        )
        await refreshNotebook()
      } else {
        error.value = String(result.error ?? 'Resultat refuse.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      busy.value = false
    }
  }

  async function resetNotebook() {
    const controller = backend.getController()
    if (!controller.resetInvestigationNotebook) {
      error.value = 'Carnet indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.resetInvestigationNotebook()
      lastResult.value = result
      if (result.success === true) {
        confirmed.value = []
        active.value = []
        refuted.value = []
        evidenceNotes.value = {}
        suggestedNextTest.value = null
        lastPlanSource.value = ''
        actionLogStore.addActionLog('investigation', 'Nouveau carnet', 'Hypotheses remises a zero.', 'warning')
      } else {
        error.value = String(result.error ?? 'Reset refuse.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      busy.value = false
    }
  }

  return {
    hypothesisDraft,
    symptomDraft,
    baselineScore,
    confirmed,
    active,
    refuted,
    suggestedNextTest,
    lastPlanSource,
    generationBusy,
    evidenceNotes,
    busy,
    error,
    lastResult,
    totalCount,
    refreshNotebook,
    addHypothesis,
    proposePlanFromSymptom,
    recordTestResult,
    resetNotebook,
  }
})
