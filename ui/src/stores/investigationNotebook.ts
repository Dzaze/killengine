/**
 * KillEngine - store du carnet d'hypotheses PHASE 120-F.
 *
 * Interface manuelle uniquement : le backend garde le moteur deterministe de
 * ponderation, ce store ne fait que synchroniser la synthese et l'etat UI.
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

export const useInvestigationNotebookStore = defineStore('investigationNotebook', () => {
  const actionLogStore = useActionLogStore()

  const hypothesisDraft = ref('')
  const baselineScore = ref(50)
  const confirmed = ref<InvestigationHypothesis[]>([])
  const active = ref<InvestigationHypothesis[]>([])
  const refuted = ref<InvestigationHypothesis[]>([])
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
    baselineScore,
    confirmed,
    active,
    refuted,
    evidenceNotes,
    busy,
    error,
    lastResult,
    totalCount,
    refreshNotebook,
    addHypothesis,
    recordTestResult,
    resetNotebook,
  }
})
