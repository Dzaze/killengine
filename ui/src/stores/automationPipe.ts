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
import { i18n } from '@/i18n'
import { backend } from '@/services/backend'
import { useActionLogStore } from './actionLog'

const { t } = i18n.global

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
      actionLogStore.addActionLog('automation', t('automationPipeStore.unavailable'), t('automationPipeStore.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.enableAutomationMode()
      automationPipeStatus.value = result
      actionLogStore.addActionLog('automation', result.success === true ? t('automationPipeStore.enabled') : t('automationPipeStore.enableFailed'), String(result.error ?? ''), result.success === true ? 'success' : 'error')
      return result
    } catch (e) {
      actionLogStore.addActionLog('automation', t('automationPipeStore.enableFailed'), String(e), 'error')
      return null
    }
  }

  async function disableAutomationMode() {
    const controller = backend.getController()
    if (!controller.disableAutomationMode) {
      actionLogStore.addActionLog('automation', t('automationPipeStore.unavailable'), t('automationPipeStore.backendNotExposed'), 'warning')
      return null
    }
    try {
      const result = await controller.disableAutomationMode()
      automationPipeStatus.value = result
      actionLogStore.addActionLog('automation', t('automationPipeStore.disabled'), '', 'success')
      return result
    } catch (e) {
      actionLogStore.addActionLog('automation', t('automationPipeStore.disableFailed'), String(e), 'error')
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
