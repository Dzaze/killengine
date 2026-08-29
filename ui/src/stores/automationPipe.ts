/**
 * KillEngine — store Automation Pipe status (extrait de app.ts, candidat S3
 * de docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Même règle que clrInspector.ts/speedhack.ts : `enableAutomationMode`
 * n'a PAS le gate confirmRiskAction ici -- `app.ts` confirme AVANT de
 * déléguer.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { backend } from '@/services/backend'
import { useActionLogStore } from './actionLog'

export const useAutomationPipeStore = defineStore('automationPipe', () => {
  const actionLogStore = useActionLogStore()

  const automationPipeStatus = ref<Record<string, unknown> | null>(null)

  async function refreshAutomationPipeStatus() {
    const controller = backend.getController()
    if (!controller.getAutomationPipeStatus) return
    try {
      automationPipeStatus.value = await controller.getAutomationPipeStatus()
    } catch (e) {
      console.error('[KillEngine] Failed to refresh automation pipe status:', e)
    }
  }

  /** Pas de confirmRiskAction ici -- app.ts confirme avant d'appeler. */
  async function enableAutomationMode() {
    const controller = backend.getController()
    if (!controller.enableAutomationMode) {
      actionLogStore.addActionLog('automation', 'Mode Automation indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const result = await controller.enableAutomationMode()
      automationPipeStatus.value = result
      actionLogStore.addActionLog('automation', result.success === true ? 'Mode Automation activé' : 'Activation échouée', String(result.error ?? ''), result.success === true ? 'success' : 'error')
      return result
    } catch (e) {
      actionLogStore.addActionLog('automation', 'Activation échouée', String(e), 'error')
      return null
    }
  }

  async function disableAutomationMode() {
    const controller = backend.getController()
    if (!controller.disableAutomationMode) {
      actionLogStore.addActionLog('automation', 'Mode Automation indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    try {
      const result = await controller.disableAutomationMode()
      automationPipeStatus.value = result
      actionLogStore.addActionLog('automation', 'Mode Automation désactivé', '', 'success')
      return result
    } catch (e) {
      actionLogStore.addActionLog('automation', 'Désactivation échouée', String(e), 'error')
      return null
    }
  }

  return {
    automationPipeStatus,
    refreshAutomationPipeStatus,
    enableAutomationMode,
    disableAutomationMode,
  }
})
