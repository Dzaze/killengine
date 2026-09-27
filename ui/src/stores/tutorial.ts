/**
 * UX-PRODUIT-15C — store du guide interactif du tutoriel.
 *
 * Ce store ne pilote AUCUN scan/écriture/attache lui-même : il observe l'état
 * déjà réactif de l'app normale (candidats, sélection, résultat d'écriture,
 * pid attaché) et fait uniquement des appels de LECTURE bornés
 * (`getTutorialTargetInfo`, `loadProfile`, `resolveProfileTarget`) sur un
 * clic explicite "Vérifier cette étape" — jamais en tâche de fond. La seule
 * action de cycle de vie déclenchée directement est `restartTutorialTarget`
 * (relance la fenêtre démo), qui n'est ni un scan ni une écriture sur les
 * données du jeu, donc légitime à l'initiative du panneau.
 *
 * Règle dure : `groundTruth`/`getTutorialTargetInfo()` ne sont jamais
 * interpolés tels quels dans un texte affiché (voir tutorialSteps.ts) — la
 * cible démo affiche déjà sa propre valeur dans sa fenêtre
 * (apps/demo/demo_target_main.cpp), c'est là que l'utilisateur doit la lire.
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { backend } from '@/services/backend'
import { i18n } from '@/i18n'
import { useAppStore, type AppView } from './app'
import { useExpertWriteSelection } from '@/composables/useExpertWriteSelection'
import {
  type TutorialGroundTruth,
  candidatesMatchHealthOnly,
  selectionMatchesHealthOnly,
  writeTargetsHealthAddress,
  effectMatchesWrite,
  profileEntrySavedByName,
  profileResolvedAfterRestart,
} from './tutorialSteps'

const { t } = i18n.global

export type TutorialStepId = 'attach' | 'search' | 'refine' | 'write' | 'verify' | 'save'
export type TutorialStepStatus = 'pending' | 'checking' | 'passed' | 'failed'

export const TUTORIAL_STEP_ORDER: TutorialStepId[] = ['attach', 'search', 'refine', 'write', 'verify', 'save']

function emptyRecord<T>(value: T): Record<TutorialStepId, T> {
  return {
    attach: value, search: value, refine: value, write: value, verify: value, save: value,
  }
}

function matchReasonMessage(reason: 'empty' | 'decoyPresent' | 'notFound' | 'tooMany' | undefined): string {
  switch (reason) {
    case 'decoyPresent': return t('tutorialStore.matchReasonDecoyPresent')
    case 'notFound': return t('tutorialStore.matchReasonNotFound')
    case 'tooMany': return t('tutorialStore.matchReasonTooMany')
    case 'empty':
    default:
      return t('tutorialStore.matchReasonEmpty')
  }
}

export const useTutorialStore = defineStore('tutorial', () => {
  const appStore = useAppStore()
  const { selectedCandidateAddresses } = useExpertWriteSelection()

  const guideVisible = ref(true)
  const targetPid = ref(0)
  const groundTruth = ref<TutorialGroundTruth | null>(null)
  const groundTruthError = ref('')
  const currentStepIndex = ref(0)
  const stepStatus = ref<Record<TutorialStepId, TutorialStepStatus>>(emptyRecord('pending'))
  const stepError = ref<Record<TutorialStepId, string>>(emptyRecord(''))
  const expectedWriteValue = ref<number | null>(null)
  const restartBusy = ref(false)
  const resolveAfterRestartAddress = ref('')
  const retrieveVerified = ref(false)

  const currentStepId = computed<TutorialStepId>(() => TUTORIAL_STEP_ORDER[currentStepIndex.value])
  const attachOk = computed(() => targetPid.value > 0 && appStore.attachedPid === targetPid.value)

  async function refreshGroundTruth(): Promise<boolean> {
    try {
      const result = await backend.getController().getTutorialTargetInfo?.()
      if (!result || result.success !== true) {
        groundTruthError.value = String((result as { error?: unknown } | undefined)?.error ?? t('tutorialStore.groundTruthUnavailable'))
        return false
      }
      const raw = result.groundTruth as Record<string, unknown> | undefined
      if (!raw) {
        groundTruthError.value = t('tutorialStore.groundTruthUnavailable')
        return false
      }
      groundTruth.value = {
        healthAddress: String(raw.healthAddress ?? ''),
        decoyAlphaAddress: String(raw.decoyAlphaAddress ?? ''),
        decoyBetaAddress: String(raw.decoyBetaAddress ?? ''),
        health: Number(raw.health ?? Number.NaN),
      }
      targetPid.value = Number(result.pid ?? 0)
      groundTruthError.value = ''
      return true
    } catch (e) {
      groundTruthError.value = String(e)
      return false
    }
  }

  async function ensureGroundTruth(): Promise<boolean> {
    if (groundTruth.value) return true
    return refreshGroundTruth()
  }

  function goToView(view: AppView) {
    appStore.activeView = view
  }

  function retryStep(id: TutorialStepId) {
    stepStatus.value[id] = 'pending'
    stepError.value[id] = ''
    if (id === 'save') {
      retrieveVerified.value = false
      resolveAfterRestartAddress.value = ''
    }
  }

  function quit() {
    guideVisible.value = false
  }

  function reopen() {
    guideVisible.value = true
  }

  function advanceIfPossible(id: TutorialStepId) {
    const idx = TUTORIAL_STEP_ORDER.indexOf(id)
    if (stepStatus.value[id] === 'passed' && idx === currentStepIndex.value && idx < TUTORIAL_STEP_ORDER.length - 1) {
      currentStepIndex.value = idx + 1
    }
  }

  // Étape 1 -- Attacher (purement réactif, pas d'appel réseau hors ground truth initiale).
  async function verifyAttach() {
    stepStatus.value.attach = 'checking'
    const ok = await ensureGroundTruth()
    if (!ok) {
      stepStatus.value.attach = 'failed'
      stepError.value.attach = groundTruthError.value
      return
    }
    if (attachOk.value) {
      stepStatus.value.attach = 'passed'
      stepError.value.attach = ''
      advanceIfPossible('attach')
    } else {
      stepStatus.value.attach = 'failed'
      stepError.value.attach = t('tutorialStore.notAttachedYet')
    }
  }

  // Étape 2 -- Chercher puis varier.
  async function verifySearch() {
    stepStatus.value.search = 'checking'
    const ok = await ensureGroundTruth()
    if (!ok || !groundTruth.value) {
      stepStatus.value.search = 'failed'
      stepError.value.search = groundTruthError.value
      return
    }
    const addresses = (appStore.candidatePage?.candidates ?? []).map((c) => c.address)
    const result = candidatesMatchHealthOnly(addresses, groundTruth.value)
    if (result.ok) {
      stepStatus.value.search = 'passed'
      stepError.value.search = ''
      advanceIfPossible('search')
    } else {
      stepStatus.value.search = 'failed'
      stepError.value.search = matchReasonMessage(result.reason)
    }
  }

  // Étape 3 -- Affiner puis sélectionner.
  function verifyRefine() {
    stepStatus.value.refine = 'checking'
    if (!groundTruth.value) {
      stepStatus.value.refine = 'failed'
      stepError.value.refine = t('tutorialStore.groundTruthUnavailable')
      return
    }
    const result = selectionMatchesHealthOnly(selectedCandidateAddresses.value, groundTruth.value)
    if (result.ok) {
      stepStatus.value.refine = 'passed'
      stepError.value.refine = ''
      advanceIfPossible('refine')
    } else {
      stepStatus.value.refine = 'failed'
      stepError.value.refine = matchReasonMessage(result.reason)
    }
  }

  // Étape 4 -- Écrire (le vrai formulaire + la vraie confirmation RiskGate ont déjà eu lieu).
  function verifyWrite() {
    stepStatus.value.write = 'checking'
    if (!groundTruth.value) {
      stepStatus.value.write = 'failed'
      stepError.value.write = t('tutorialStore.groundTruthUnavailable')
      return
    }
    const ok = writeTargetsHealthAddress(appStore.writeResult, groundTruth.value)
    if (ok) {
      stepStatus.value.write = 'passed'
      stepError.value.write = ''
      const parsed = Number(appStore.writeValue)
      expectedWriteValue.value = Number.isFinite(parsed) ? parsed : null
      advanceIfPossible('write')
    } else {
      stepStatus.value.write = 'failed'
      stepError.value.write = t('tutorialStore.writeNotConfirmedYet')
    }
  }

  // Étape 5 -- Vérifier l'effet (une seule relecture bornée, pas de sondage continu).
  async function verifyEffect() {
    stepStatus.value.verify = 'checking'
    if (expectedWriteValue.value === null) {
      stepStatus.value.verify = 'failed'
      stepError.value.verify = t('tutorialStore.writeStepFirst')
      return
    }
    const ok = await refreshGroundTruth()
    if (!ok || !groundTruth.value) {
      stepStatus.value.verify = 'failed'
      stepError.value.verify = groundTruthError.value
      return
    }
    if (effectMatchesWrite(groundTruth.value.health, expectedWriteValue.value)) {
      stepStatus.value.verify = 'passed'
      stepError.value.verify = ''
      advanceIfPossible('verify')
    } else {
      stepStatus.value.verify = 'failed'
      stepError.value.verify = t('tutorialStore.effectNotObservedYet')
    }
  }

  // Étape 6a -- Sauvegarder (réutilise le vrai formulaire ProfileView -- ce store ne fait que relire).
  async function verifySave(profileName: string, targetName: string) {
    stepStatus.value.save = 'checking'
    try {
      const result = await backend.getController().loadProfile(profileName)
      if (result?.success !== true) {
        stepStatus.value.save = 'failed'
        stepError.value.save = String(result?.error ?? t('tutorialStore.profileNotSavedYet'))
        return
      }
      const entries = (result.targets as Array<{ name: string }>) ?? []
      if (profileEntrySavedByName(entries, targetName)) {
        stepStatus.value.save = 'passed'
        stepError.value.save = ''
        advanceIfPossible('save')
      } else {
        stepStatus.value.save = 'failed'
        stepError.value.save = t('tutorialStore.profileNotSavedYet')
      }
    } catch (e) {
      stepStatus.value.save = 'failed'
      stepError.value.save = String(e)
    }
  }

  // Étape 6b -- Retrouver : redémarre la cible démo (action de cycle de vie, pas un scan/écriture).
  async function restartTarget() {
    restartBusy.value = true
    try {
      const result = await backend.getController().restartTutorialTarget?.()
      if (result && result.success) {
        groundTruth.value = null
        await refreshGroundTruth()
      }
      return result ?? { success: false, error: t('tutorialStore.groundTruthUnavailable') }
    } finally {
      restartBusy.value = false
    }
  }

  async function verifyRetrieve(profileName: string, targetName: string): Promise<boolean> {
    try {
      const result = await backend.getController().resolveProfileTarget(profileName, targetName)
      const address = result?.success === true ? String(result.address ?? '') : ''
      resolveAfterRestartAddress.value = address
      const ok = profileResolvedAfterRestart(address)
      retrieveVerified.value = ok
      return ok
    } catch {
      resolveAfterRestartAddress.value = ''
      retrieveVerified.value = false
      return false
    }
  }

  const tutorialComplete = computed(() => stepStatus.value.save === 'passed' && retrieveVerified.value)

  return {
    guideVisible,
    currentStepId,
    currentStepIndex,
    stepStatus,
    stepError,
    groundTruthError,
    restartBusy,
    resolveAfterRestartAddress,
    retrieveVerified,
    tutorialComplete,
    targetPid,
    attachOk,
    goToView,
    retryStep,
    quit,
    reopen,
    ensureGroundTruth,
    verifyAttach,
    verifySearch,
    verifyRefine,
    verifyWrite,
    verifyEffect,
    verifySave,
    restartTarget,
    verifyRetrieve,
  }
})
