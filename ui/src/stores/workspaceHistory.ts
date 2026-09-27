/**
 * KillEngine — store Historique et récupération du workspace (UX-PRODUIT-13,
 * docs/PHASE_TRACKER.md). Fondation composée au-dessus de `workspaceSession.ts`
 * (déjà une feuille sans dépendance vers `./app`) : capture automatique après
 * inactivité, capture manuelle/avant import/avant restauration, aperçu
 * avant/après par section, restauration sélective.
 *
 * Ne duplique aucune logique de mutation : `buildRecoverableSnapshotJson()`
 * et `importWorkspaceJson(json, 'revision_restore')` (workspaceSession.ts)
 * portent la construction/l'application réelles ; ce store orchestre
 * seulement QUAND capturer et QUOI restaurer, via les fonctions pures de
 * `workspaceHistoryScheduling.ts` (fingerprint/throttle/diff, testées en
 * isolation par scripts/test-workspace-history-scheduling.ps1).
 */
import { defineStore, storeToRefs } from 'pinia'
import { ref, watch } from 'vue'
import { i18n } from '@/i18n'
import { backend, type WorkspaceRevisionSummary } from '@/services/backend'
import { useWorkspaceSessionStore } from './workspaceSession'
import { useTrainerStore } from './trainer'
import {
  computeFingerprint,
  shouldCaptureAutomatic,
  diffRecoverableSnapshots,
  type RecoverableSnapshot,
  type SectionDiffResult,
  type WorkspaceHistorySectionKey,
} from './workspaceHistoryScheduling'

const { t } = i18n.global

export type { WorkspaceHistorySectionKey } from './workspaceHistoryScheduling'

const kAutomaticDebounceMs = 2000

export const useWorkspaceHistoryStore = defineStore('workspaceHistory', () => {
  const workspaceSessionStore = useWorkspaceSessionStore()
  const trainerStore = useTrainerStore()
  const { trainerFeatures } = storeToRefs(trainerStore)

  const revisions = ref<WorkspaceRevisionSummary[]>([])
  const revisionsPersistenceError = ref<string | null>(null)
  const listBusy = ref(false)

  const previewRevisionId = ref<string | null>(null)
  const previewDiff = ref<SectionDiffResult[] | null>(null)
  const previewBusy = ref(false)
  const previewError = ref<string | null>(null)

  const restoreBusy = ref(false)
  const restoreError = ref<string | null>(null)

  let lastCapturedFingerprint: string | null = null
  let lastAutoSaveAtMs = 0
  let debounceTimer: ReturnType<typeof setTimeout> | undefined
  let stopWatcher: (() => void) | null = null

  function isTrainerBusyOrActive(): boolean {
    return trainerStore.trainerBusy || trainerFeatures.value.some((feature) => feature.enabled)
  }

  async function captureNow(
    reason: 'automatic' | 'before_import' | 'before_restore' | 'manual',
    projectContext = '',
    targetName = '',
  ): Promise<{ success: boolean, error?: string, revision?: WorkspaceRevisionSummary }> {
    const payloadJson = workspaceSessionStore.buildRecoverableSnapshotJson()
    try {
      const controller = backend.getController()
      const result = await controller.createWorkspaceRevision?.(payloadJson, { reason, projectContext, targetName })
      if (!result) {
        revisionsPersistenceError.value = t('workspaceHistoryStore.persistenceFailedGeneric')
        return { success: false, error: revisionsPersistenceError.value }
      }
      if (result.success) {
        // L'empreinte "dernière capturée" avance quelle que soit la raison --
        // une capture manuelle/de protection compte aussi comme référence
        // pour éviter qu'un automatique 2s plus tard ne resauvegarde un
        // contenu identique.
        lastCapturedFingerprint = computeFingerprint(payloadJson)
        if (reason === 'automatic') {
          lastAutoSaveAtMs = Date.now()
        }
        revisionsPersistenceError.value = null
        void refreshList()
      } else {
        revisionsPersistenceError.value = result.error ?? t('workspaceHistoryStore.persistenceFailedGeneric')
      }
      return result
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      revisionsPersistenceError.value = message
      return { success: false, error: message }
    }
  }

  function scheduleAutomaticCaptureCheck() {
    if (debounceTimer) clearTimeout(debounceTimer)
    debounceTimer = setTimeout(() => {
      debounceTimer = undefined
      const payloadJson = workspaceSessionStore.buildRecoverableSnapshotJson()
      const currentFingerprint = computeFingerprint(payloadJson)
      if (shouldCaptureAutomatic({
        lastFingerprint: lastCapturedFingerprint,
        currentFingerprint,
        lastAutoSaveAtMs,
        nowMs: Date.now(),
      })) {
        void captureNow('automatic')
      }
    }, kAutomaticDebounceMs)
  }

  /** Idempotent : un second appel ne double pas l'abonnement (app.ts::init() peut être rappelé). */
  function startWatching() {
    if (stopWatcher) return
    stopWatcher = watch(
      () => workspaceSessionStore.buildRecoverableSnapshotJson(),
      () => scheduleAutomaticCaptureCheck(),
    )
  }

  async function refreshList(offset = 0, limit = 20) {
    listBusy.value = true
    try {
      const controller = backend.getController()
      const result = await controller.listWorkspaceRevisions?.({ offset, limit })
      if (result?.success) {
        revisions.value = result.entries
        revisionsPersistenceError.value = null
      } else {
        revisionsPersistenceError.value = result?.error ?? t('workspaceHistoryStore.persistenceFailedGeneric')
      }
    } catch (error) {
      revisionsPersistenceError.value = error instanceof Error ? error.message : String(error)
    } finally {
      listBusy.value = false
    }
  }

  function clearPreview() {
    previewRevisionId.value = null
    previewDiff.value = null
    previewError.value = null
  }

  async function loadPreview(id: string) {
    previewBusy.value = true
    previewError.value = null
    previewDiff.value = null
    previewRevisionId.value = id
    try {
      const controller = backend.getController()
      const result = await controller.readWorkspaceRevision?.(id)
      if (!result?.success || !result.payloadJson) {
        previewError.value = result?.error ?? t('workspaceHistoryStore.persistenceFailedGeneric')
        return
      }
      const revisionSnapshot = JSON.parse(result.payloadJson) as RecoverableSnapshot
      const currentSnapshot = JSON.parse(workspaceSessionStore.buildRecoverableSnapshotJson()) as RecoverableSnapshot
      // Avant/après : "avant" est l'état COURANT (ce qui serait perdu),
      // "après" est le contenu de la révision (ce qui serait restauré).
      previewDiff.value = diffRecoverableSnapshots(currentSnapshot, revisionSnapshot)
    } catch (error) {
      previewError.value = error instanceof Error ? error.message : String(error)
    } finally {
      previewBusy.value = false
    }
  }

  async function restoreSelectedSections(id: string, sections: Set<WorkspaceHistorySectionKey>) {
    restoreError.value = null
    if (sections.size === 0) {
      restoreError.value = t('workspaceHistoryStore.noSectionSelected')
      return { success: false, error: restoreError.value }
    }
    // Garde-fou dédié à la restauration (plus strict que l'import classique,
    // qui se contente d'ignorer la seule section Trainer) : une feature
    // active bloque TOUTE la sélection tant que Trainer en fait partie --
    // l'utilisateur peut décocher Trainer pour continuer.
    if (sections.has('trainer') && isTrainerBusyOrActive()) {
      restoreError.value = t('workspaceHistoryStore.trainerActiveBlocksRestore')
      return { success: false, error: restoreError.value }
    }

    restoreBusy.value = true
    try {
      const protection = await captureNow('before_restore')
      if (!protection.success) {
        restoreError.value = protection.error ?? t('workspaceHistoryStore.protectionFailed')
        return { success: false, error: restoreError.value }
      }

      const controller = backend.getController()
      const read = await controller.readWorkspaceRevision?.(id)
      if (!read?.success || !read.payloadJson) {
        restoreError.value = read?.error ?? t('workspaceHistoryStore.persistenceFailedGeneric')
        return { success: false, error: restoreError.value }
      }

      const fullPayload = JSON.parse(read.payloadJson) as Record<string, unknown>
      const filtered: Record<string, unknown> = {
        version: fullPayload.version,
        exportedAt: fullPayload.exportedAt,
        lastPresetId: fullPayload.lastPresetId,
      }
      for (const section of sections) {
        if (fullPayload[section] !== undefined) {
          filtered[section] = fullPayload[section]
        }
      }

      const result = await workspaceSessionStore.importWorkspaceJson(JSON.stringify(filtered), 'revision_restore')
      if ('error' in result) {
        restoreError.value = String(result.error)
        return { success: false as const, error: restoreError.value }
      }
      clearPreview()
      return result
    } finally {
      restoreBusy.value = false
    }
  }

  async function deleteRevision(id: string) {
    try {
      const controller = backend.getController()
      const result = await controller.deleteWorkspaceRevision?.(id)
      if (result?.success) {
        if (previewRevisionId.value === id) clearPreview()
        void refreshList()
      }
      return result ?? { success: false, error: 'unavailable' }
    } catch (error) {
      return { success: false, error: error instanceof Error ? error.message : String(error) }
    }
  }

  return {
    revisions,
    revisionsPersistenceError,
    listBusy,
    previewRevisionId,
    previewDiff,
    previewBusy,
    previewError,
    restoreBusy,
    restoreError,
    startWatching,
    captureNow,
    refreshList,
    loadPreview,
    clearPreview,
    restoreSelectedSections,
    deleteRevision,
  }
})
