/**
 * KillEngine — store Centre d'activité permanent (UX-PRODUIT-12,
 * docs/PHASE_TRACKER.md). Miroir frontend du registre backend
 * (core/activity/activity_registry.h + apps/desktop/activity_manager.h) :
 * scans, collecte Timeline, scripts Lua, installations de module,
 * surveillance de fichier de sauvegarde.
 *
 * Fondation sans dépendance vers `./app` (même principe que `actionLog.ts`/
 * `scanning.ts`) : appelé depuis app.ts (connexion du signal + boucle de
 * réconciliation dans init()), pas l'inverse. La navigation "Voir" par kind
 * reste dans le composant consommateur (ActivityPanel.vue), qui a déjà accès
 * à useAppStore() pour changer activeView/pendingExpertAnchor.
 *
 * Le snapshot backend est l'autorité ; la poussée activityUpdated (une seule
 * entrée + revision globale) n'est qu'un indice pour éviter d'attendre la
 * réconciliation périodique -- toute entrée dont la revision n'est pas plus
 * récente que celle déjà connue localement est ignorée (protège contre une
 * poussée en retard/rejouée qui rouvrirait une activité déjà terminée).
 */
import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { backend, type ActivityEntry, type ActivitySnapshot, type ActivityUpdatePayload } from '@/services/backend'

export type { ActivityEntry } from '@/services/backend'

const kReconcileIntervalMs = 2000

function isTerminalState(state: ActivityEntry['state']): boolean {
  return state === 'completed' || state === 'cancelled' || state === 'failed' || state === 'interrupted'
}

export const useActivityStore = defineStore('activity', () => {
  const entries = ref<Map<string, ActivityEntry>>(new Map())
  const globalRevision = ref(0)
  const drawerOpen = ref(false)
  const reconcileInFlight = ref(false)
  let reconcileTimer: ReturnType<typeof setInterval> | undefined

  const sortedEntries = computed<ActivityEntry[]>(() => {
    const running: ActivityEntry[] = []
    const terminal: ActivityEntry[] = []
    for (const entry of entries.value.values()) {
      if (isTerminalState(entry.state)) terminal.push(entry)
      else running.push(entry)
    }
    running.sort((a, b) => a.startedAtMs - b.startedAtMs)
    terminal.sort((a, b) => b.finishedAtMs - a.finishedAtMs)
    return [...running, ...terminal]
  })

  const runningCount = computed(() => {
    let count = 0
    for (const entry of entries.value.values()) {
      if (!isTerminalState(entry.state)) count += 1
    }
    return count
  })

  const unacknowledgedTerminalCount = computed(() => {
    let count = 0
    for (const entry of entries.value.values()) {
      if (isTerminalState(entry.state) && !entry.acknowledged) count += 1
    }
    return count
  })

  function applySnapshot(snapshot: ActivitySnapshot) {
    if (!snapshot || !Array.isArray(snapshot.entries)) return
    const next = new Map<string, ActivityEntry>()
    for (const entry of snapshot.entries) {
      if (entry?.operationId) {
        next.set(entry.operationId, entry)
      }
    }
    entries.value = next
    if (typeof snapshot.globalRevision === 'number' && snapshot.globalRevision > globalRevision.value) {
      globalRevision.value = snapshot.globalRevision
    }
  }

  function applyDelta(payload: ActivityUpdatePayload) {
    if (!payload?.entry?.operationId) return
    const existing = entries.value.get(payload.entry.operationId)
    const isNewerEntry = !existing || payload.entry.revision > existing.revision
    if (isNewerEntry) {
      const next = new Map(entries.value)
      next.set(payload.entry.operationId, payload.entry)
      entries.value = next
    }

    if (typeof payload.globalRevision === 'number') {
      if (payload.globalRevision > globalRevision.value) {
        globalRevision.value = payload.globalRevision
      } else if (payload.globalRevision < globalRevision.value) {
        // Poussée manifestement en retard (révision globale déjà dépassée
        // localement) : re-synchroniser hors cycle plutôt que d'attendre le
        // prochain tick de réconciliation périodique.
        void reconcile()
      }
    }
  }

  async function reconcile() {
    if (reconcileInFlight.value) return
    reconcileInFlight.value = true
    try {
      const controller = backend.getController()
      const snapshot = await controller.getActivitySnapshot?.()
      if (snapshot) {
        applySnapshot(snapshot)
      }
    } catch {
      // Silencieux : le prochain tick de réconciliation (ou la reconnexion)
      // réessaiera ; pas d'état d'erreur dédié pour un simple poll de fond.
    } finally {
      reconcileInFlight.value = false
    }
  }

  function startReconciliationLoop() {
    if (reconcileTimer) return
    reconcileTimer = setInterval(() => { void reconcile() }, kReconcileIntervalMs)
  }

  function stopReconciliationLoop() {
    if (reconcileTimer) {
      clearInterval(reconcileTimer)
      reconcileTimer = undefined
    }
  }

  async function cancelActivity(operationId: string): Promise<{ accepted: boolean, error?: string }> {
    try {
      const controller = backend.getController()
      const result = await controller.cancelActivity?.(operationId)
      return result ?? { accepted: false, error: 'unavailable' }
    } catch (error) {
      return { accepted: false, error: error instanceof Error ? error.message : String(error) }
    }
  }

  function openDrawer() {
    drawerOpen.value = true
  }
  function closeDrawer() {
    drawerOpen.value = false
  }
  function toggleDrawer() {
    drawerOpen.value = !drawerOpen.value
  }

  return {
    entries,
    globalRevision,
    drawerOpen,
    sortedEntries,
    runningCount,
    unacknowledgedTerminalCount,
    applySnapshot,
    applyDelta,
    reconcile,
    startReconciliationLoop,
    stopReconciliationLoop,
    cancelActivity,
    openDrawer,
    closeDrawer,
    toggleDrawer,
  }
})
