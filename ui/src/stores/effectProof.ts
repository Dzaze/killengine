/**
 * KillEngine - store du registre de preuves d'effet (PRODUIT-R section R1).
 *
 * Distingue explicitement ecriture confirmee (relecture) / effet confirme
 * (comportement observe) / solution durable (persistance testee) pour qu'une
 * simple relecture reussie ne soit jamais presentee comme un objectif atteint.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { backend } from '@/services/backend'
import { useActionLogStore } from './actionLog'

export type EffectProofLevel = 'unverified' | 'inconclusive' | 'write_confirmed' | 'effect_confirmed' | 'durable_solution'

export interface EffectProofTargetStatus {
  targetKey: string
  targetLabel: string
  address: string
  bestLevel: EffectProofLevel
  nextAction: string
  history: Array<Record<string, unknown>>
}

function asTargetStatus(value: unknown): EffectProofTargetStatus | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  const targetKey = String(item.targetKey ?? '').trim()
  if (!targetKey) return null
  return {
    targetKey,
    targetLabel: String(item.targetLabel ?? '').trim(),
    address: String(item.address ?? '').trim(),
    bestLevel: (String(item.bestLevel ?? 'unverified') as EffectProofLevel),
    nextAction: String(item.nextAction ?? '').trim(),
    history: Array.isArray(item.history) ? (item.history as Array<Record<string, unknown>>) : [],
  }
}

function asTargetStatusList(value: unknown): EffectProofTargetStatus[] {
  if (!Array.isArray(value)) return []
  return value.map(asTargetStatus).filter((item): item is EffectProofTargetStatus => item !== null)
}

export const useEffectProofStore = defineStore('effectProof', () => {
  const actionLogStore = useActionLogStore()

  const targetLabelDraft = ref('')
  const addressDraft = ref('')
  const levelDraft = ref<EffectProofLevel>('effect_confirmed')
  const sourceDraft = ref('')
  const conditionsDraft = ref('')
  const noteDraft = ref('')

  const known = ref<EffectProofTargetStatus[]>([])
  const uncertain = ref<EffectProofTargetStatus[]>([])
  const overallNextAction = ref('')
  const busy = ref(false)
  const error = ref('')

  function applySynthesis(result: Record<string, unknown>) {
    known.value = asTargetStatusList(result.known)
    uncertain.value = asTargetStatusList(result.uncertain)
    overallNextAction.value = String(result.overallNextAction ?? '')
  }

  async function refresh() {
    const controller = backend.getController()
    if (!controller.getEffectProofSynthesis) {
      error.value = 'Registre de preuves indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.getEffectProofSynthesis()
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

  async function recordProof() {
    const targetLabel = targetLabelDraft.value.trim()
    const address = addressDraft.value.trim()
    if (!targetLabel && !address) {
      error.value = 'Renseigner un libelle ou une adresse.'
      return null
    }

    const controller = backend.getController()
    if (!controller.recordEffectProof) {
      error.value = 'Registre de preuves indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.recordEffectProof(
        targetLabel,
        address,
        levelDraft.value,
        sourceDraft.value.trim(),
        conditionsDraft.value.trim(),
        '',
        noteDraft.value.trim(),
      )
      if (result.success === true) {
        actionLogStore.addActionLog('investigation', 'Preuve d\'effet enregistree', `${targetLabel || address} - ${levelDraft.value}`, 'success')
        noteDraft.value = ''
        await refresh()
      } else {
        error.value = String(result.error ?? 'Enregistrement refuse.')
      }
      return result
    } catch (e) {
      error.value = String(e)
      return null
    } finally {
      busy.value = false
    }
  }

  async function resetLedger() {
    const controller = backend.getController()
    if (!controller.resetEffectProofLedger) {
      error.value = 'Registre de preuves indisponible dans ce backend.'
      return null
    }

    busy.value = true
    error.value = ''
    try {
      const result = await controller.resetEffectProofLedger()
      if (result.success === true) {
        known.value = []
        uncertain.value = []
        overallNextAction.value = ''
        actionLogStore.addActionLog('investigation', 'Registre de preuves reinitialise', '', 'warning')
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
    targetLabelDraft,
    addressDraft,
    levelDraft,
    sourceDraft,
    conditionsDraft,
    noteDraft,
    known,
    uncertain,
    overallNextAction,
    busy,
    error,
    refresh,
    recordProof,
    resetLedger,
  }
})
