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
import type { ProfileKnowledgeNoteKind } from '@/services/backend'
import { useActionLogStore } from './actionLog'
import { i18n } from '@/i18n'

const { t } = i18n.global

export type EffectProofLevel = 'unverified' | 'inconclusive' | 'write_confirmed' | 'effect_confirmed' | 'durable_solution'

/** AM-5 (docs/PHASE_TRACKER.md, 16/09/2026) : un enregistrement individuel de
 * l'historique d'une cible -- porte maintenant executableHash/recordedAt en
 * plus des champs déjà connus, utilisés pour la provenance affichée lors de
 * l'enregistrement dans un profil (AM-5c). */
export interface EffectProofRecordEntry {
  id: string
  level: EffectProofLevel
  source: string
  conditions: string
  sessionId: string
  executableHash: string
  recordedAt: string
  note: string
}

export interface EffectProofTargetStatus {
  targetKey: string
  targetLabel: string
  address: string
  bestLevel: EffectProofLevel
  nextAction: string
  history: EffectProofRecordEntry[]
}

function asRecordEntry(value: unknown): EffectProofRecordEntry | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Record<string, unknown>
  return {
    id: String(item.id ?? ''),
    level: (String(item.level ?? 'unverified') as EffectProofLevel),
    source: String(item.source ?? ''),
    conditions: String(item.conditions ?? ''),
    sessionId: String(item.sessionId ?? ''),
    executableHash: String(item.executableHash ?? ''),
    recordedAt: String(item.recordedAt ?? ''),
    note: String(item.note ?? ''),
  }
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
    history: Array.isArray(item.history) ? item.history.map(asRecordEntry).filter((entry): entry is EffectProofRecordEntry => entry !== null) : [],
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
  // AM-5 : identité (executableHash) du process actuellement attaché, telle
  // que calculée côté backend -- sert à avertir si une preuve qu'on tente de
  // relier à un profil (AM-5c) a été observée sur un autre exécutable.
  const currentExecutableHash = ref('')

  function applySynthesis(result: Record<string, unknown>) {
    known.value = asTargetStatusList(result.known)
    uncertain.value = asTargetStatusList(result.uncertain)
    overallNextAction.value = String(result.overallNextAction ?? '')
    currentExecutableHash.value = String(result.currentExecutableHash ?? '')
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

  // ---------------------------------------------------------------------
  // AM-5b (docs/PHASE_TRACKER.md, 16/09/2026) : observation guidée. Préremplit
  // le formulaire existant (cible + niveau) à partir d'une cible déjà connue
  // (une preuve write_confirmed créée automatiquement par AM-5a, ou toute
  // autre entrée) plutôt que de forcer une ressaisie manuelle -- la
  // soumission reste le bouton "Enregistrer" existant : jamais de promotion
  // automatique vers effect_confirmed/durable_solution sans que l'utilisateur
  // ne confirme explicitement conditions + observation.
  // ---------------------------------------------------------------------
  type ObservationOutcome = 'observed' | 'absent' | 'inconclusive'
  const guidedOutcome = ref<ObservationOutcome | null>(null)

  function prefillObservation(item: EffectProofTargetStatus, outcome: ObservationOutcome) {
    targetLabelDraft.value = item.targetLabel
    addressDraft.value = item.address
    levelDraft.value = outcome === 'observed' ? 'effect_confirmed' : 'inconclusive'
    sourceDraft.value = t('investigation.effectProofGuidedSource')
    noteDraft.value = outcome === 'absent' ? t('investigation.effectProofGuidedAbsentNote') : ''
    conditionsDraft.value = ''
    guidedOutcome.value = outcome
  }

  // ---------------------------------------------------------------------
  // AM-5c : enregistrement explicite dans une entrée de profil choisie par
  // l'utilisateur (jamais devinée depuis la seule adresse). Réutilise
  // addProfileKnowledgeNote (R4) tel quel -- pas de nouveau stockage. La
  // provenance (id/niveau/date/session de la preuve d'origine) est conservée
  // dans le texte de la note plutôt que de réattribuer silencieusement la
  // note à la session/version courante.
  // ---------------------------------------------------------------------
  const linkingItem = ref<EffectProofTargetStatus | null>(null)
  const linkProfileName = ref('')
  const linkProfileEntries = ref<Array<{ name: string; kind: 'target' | 'patch' }>>([])
  const linkEntryKind = ref<'target' | 'patch'>('target')
  const linkEntryName = ref('')
  const linkDescription = ref('')
  const linkNoteKind = ref<ProfileKnowledgeNoteKind>('recheck')
  const linkBusy = ref(false)
  const linkError = ref('')
  const linkMessage = ref('')

  function startLinkToProfile(item: EffectProofTargetStatus) {
    linkingItem.value = item
    linkProfileName.value = ''
    linkProfileEntries.value = []
    linkEntryKind.value = 'target'
    linkEntryName.value = ''
    linkDescription.value = ''
    linkNoteKind.value = (item.bestLevel === 'effect_confirmed' || item.bestLevel === 'durable_solution')
      ? 'success_condition'
      : 'recheck'
    linkError.value = ''
    linkMessage.value = ''
  }

  function cancelLinkToProfile() {
    linkingItem.value = null
  }

  /** Preuve d'origine (executableHash) : diffère silencieusement du process
   * actuellement attaché ? Utilisé pour avertir avant d'enregistrer, jamais
   * pour bloquer sans recours (l'utilisateur peut confirmer en connaissance
   * de cause -- ex. un profil qu'on documente hors ligne). */
  function linkedProofVersionMismatch(): boolean {
    const item = linkingItem.value
    if (!item || item.history.length === 0) return false
    const recordHash = item.history[0].executableHash
    return Boolean(recordHash && currentExecutableHash.value && recordHash !== currentExecutableHash.value)
  }

  async function loadProfileEntriesForLink() {
    const name = linkProfileName.value.trim()
    if (!name) return
    const controller = backend.getController()
    linkBusy.value = true
    linkError.value = ''
    linkEntryName.value = ''
    try {
      const result = await controller.loadProfile(name)
      if (result.success !== true) {
        linkError.value = String(result.error ?? t('investigation.effectProofLinkProfileNotFound'))
        linkProfileEntries.value = []
        return
      }
      const targets = Array.isArray(result.targets) ? (result.targets as Array<Record<string, unknown>>) : []
      const patches = Array.isArray(result.patches) ? (result.patches as Array<Record<string, unknown>>) : []
      linkProfileEntries.value = [
        ...targets.map((entry) => ({ name: String(entry.name ?? ''), kind: 'target' as const })),
        ...patches.map((entry) => ({ name: String(entry.name ?? ''), kind: 'patch' as const })),
      ].filter((entry) => entry.name)
    } catch (e) {
      linkError.value = String(e)
      linkProfileEntries.value = []
    } finally {
      linkBusy.value = false
    }
  }

  async function confirmLinkToProfile() {
    const item = linkingItem.value
    const profileName = linkProfileName.value.trim()
    const entryName = linkEntryName.value.trim()
    if (!item || !profileName || !entryName) {
      linkError.value = t('investigation.effectProofLinkMissingSelection')
      return null
    }

    const controller = backend.getController()
    if (!controller.addProfileKnowledgeNote) {
      linkError.value = 'Registre de connaissances indisponible dans ce backend.'
      return null
    }

    const record = item.history[0] as EffectProofRecordEntry | undefined
    const provenanceParts = [
      t('investigation.effectProofLinkProvenance', {
        id: record?.id ?? '',
        level: item.bestLevel,
        date: record?.recordedAt ?? '',
        session: record?.sessionId ?? '',
      }),
    ]
    if (linkedProofVersionMismatch()) {
      provenanceParts.push(t('investigation.effectProofLinkVersionMismatch'))
    }
    if (record?.note) {
      provenanceParts.push(record.note)
    }

    linkBusy.value = true
    linkError.value = ''
    linkMessage.value = ''
    try {
      const result = await controller.addProfileKnowledgeNote(profileName, linkEntryKind.value, entryName, {
        kind: linkNoteKind.value,
        description: linkDescription.value.trim() || item.nextAction || item.targetLabel || item.address,
        evidenceNote: provenanceParts.filter(Boolean).join(' '),
      })
      if (result.success === true) {
        linkMessage.value = t('investigation.effectProofLinkSaved')
        actionLogStore.addActionLog(
          'investigation',
          t('investigation.effectProofLinkSaved'),
          `${profileName} / ${entryName}`,
          'success',
        )
        linkingItem.value = null
      } else {
        linkError.value = String(result.errorCode ?? 'invalid_note')
      }
      return result
    } catch (e) {
      linkError.value = String(e)
      return null
    } finally {
      linkBusy.value = false
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
    currentExecutableHash,
    busy,
    error,
    refresh,
    recordProof,
    resetLedger,
    guidedOutcome,
    prefillObservation,
    linkingItem,
    linkProfileName,
    linkProfileEntries,
    linkEntryKind,
    linkEntryName,
    linkDescription,
    linkNoteKind,
    linkBusy,
    linkError,
    linkMessage,
    startLinkToProfile,
    cancelLinkToProfile,
    loadProfileEntriesForLink,
    confirmLinkToProfile,
    linkedProofVersionMismatch,
  }
})
