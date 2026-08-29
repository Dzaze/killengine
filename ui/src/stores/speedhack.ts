/**
 * KillEngine — store Speedhack / API hooking / blocage réseau (extrait de
 * app.ts, candidat S2 de docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Même règle que clrInspector.ts (S1) : les 3 fonctions à risque
 * (startApiHook/startSpeedhack/blockProcessNetwork) n'ont PAS le gate
 * confirmRiskAction ici (pas encore extrait de app.ts) -- `app.ts` confirme
 * AVANT de déléguer. `logAiAudit` (télémétrie liée à Investigation/
 * searchQuery, hors du périmètre de ce store) reste aussi appelé côté
 * app.ts après délégation, pas ici. `processName` (utilisé uniquement par
 * blockProcessNetwork pour le message de log) est reçu en paramètre.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import {
  backend,
  type ApiHookStatus,
  type ProcessNetworkBlockStatus,
  type SpeedhackStatus,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'

export const useSpeedhackStore = defineStore('speedhack', () => {
  const actionLogStore = useActionLogStore()

  const speedhackStatus = ref<SpeedhackStatus | null>(null)
  const speedhackBusy = ref(false)
  const speedhackFactor = ref(1.0)
  const networkBlockStatus = ref<ProcessNetworkBlockStatus | null>(null)
  const networkBlockBusy = ref(false)
  const apiHookStatus = ref<ApiHookStatus | null>(null)
  const apiHookBusy = ref(false)
  const apiHookModuleName = ref('kernel32.dll')
  const apiHookFunctionName = ref('Sleep')
  const apiHookMode = ref(0)
  const apiHookForcedReturn = ref(0)

  async function refreshSpeedhackStatus() {
    const controller = backend.getController()
    if (!controller.getSpeedhackStatus) return null
    try {
      const status = await controller.getSpeedhackStatus()
      speedhackStatus.value = status
      if (status.active) speedhackFactor.value = status.factor
      return status
    } catch (e) {
      actionLogStore.addActionLog('speedhack', 'Statut speedhack indisponible', String(e), 'warning')
      return null
    }
  }

  /** Pas de confirmRiskAction ici -- app.ts confirme avant d'appeler. */
  async function startApiHook() {
    const controller = backend.getController()
    if (!controller.startApiHook) {
      actionLogStore.addActionLog('injection', 'Interception indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    apiHookBusy.value = true
    try {
      const result = await controller.startApiHook(apiHookModuleName.value, apiHookFunctionName.value, apiHookMode.value, apiHookForcedReturn.value)
      apiHookStatus.value = result
      if (result.success) {
        actionLogStore.addActionLog('injection', 'Interception active', `${apiHookModuleName.value}!${apiHookFunctionName.value}, mode ${apiHookMode.value === 1 ? 'forcer retour' : 'compter'}.`, 'success')
      } else {
        actionLogStore.addActionLog('injection', 'Interception échouée', result.error || 'raison inconnue', 'error')
      }
      return result
    } catch (e) {
      actionLogStore.addActionLog('injection', 'Interception échouée', String(e), 'error')
      return null
    } finally {
      apiHookBusy.value = false
    }
  }

  async function stopApiHook() {
    const controller = backend.getController()
    if (!controller.stopApiHook) return null
    try {
      const result = await controller.stopApiHook()
      apiHookStatus.value = result
      actionLogStore.addActionLog('injection', 'Interception retirée', result.finalCallCount !== undefined ? `${result.finalCallCount} appel(s) intercepté(s) au total.` : 'Hook retiré.', 'success')
      return result
    } catch (e) {
      actionLogStore.addActionLog('injection', 'Retrait de l interception échoué', String(e), 'error')
      return null
    }
  }

  async function refreshApiHookStatus() {
    const controller = backend.getController()
    if (!controller.getApiHookStatus) return null
    try {
      apiHookStatus.value = await controller.getApiHookStatus()
      return apiHookStatus.value
    } catch {
      return null
    }
  }

  /** Pas de confirmRiskAction/logAiAudit ici -- app.ts s'en charge avant/après. */
  async function startSpeedhack(factor: number) {
    const controller = backend.getController()
    if (!controller.startSpeedhack) {
      actionLogStore.addActionLog('speedhack', 'Speedhack indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    speedhackBusy.value = true
    try {
      const result = await controller.startSpeedhack(factor)
      speedhackStatus.value = result
      if (result.success) {
        speedhackFactor.value = factor
        actionLogStore.addActionLog('speedhack', 'Speedhack activé', `Facteur ${factor}x.`, 'success')
      } else {
        actionLogStore.addActionLog('speedhack', 'Speedhack échoué', result.error || 'raison inconnue', 'error')
      }
      return result
    } catch (e) {
      actionLogStore.addActionLog('speedhack', 'Speedhack échoué', String(e), 'error')
      return null
    } finally {
      speedhackBusy.value = false
    }
  }

  async function setSpeedhackFactor(factor: number) {
    const controller = backend.getController()
    if (!controller.setSpeedhackFactor) return null
    try {
      const result = await controller.setSpeedhackFactor(factor)
      speedhackStatus.value = {
        ...(speedhackStatus.value ?? { success: true, active: true, factor }),
        ...result,
        active: result.active ?? speedhackStatus.value?.active ?? true,
        factor: result.factor ?? factor,
      }
      if (result.success) speedhackFactor.value = factor
      return result
    } catch (e) {
      actionLogStore.addActionLog('speedhack', 'Réglage du facteur échoué', String(e), 'error')
      return null
    }
  }

  async function stopSpeedhack() {
    const controller = backend.getController()
    if (!controller.stopSpeedhack) return null
    try {
      const result = await controller.stopSpeedhack()
      speedhackStatus.value = result
      speedhackFactor.value = 1.0
      actionLogStore.addActionLog('speedhack', 'Speedhack désactivé', 'Vitesse remise à la normale.', 'success')
      return result
    } catch (e) {
      actionLogStore.addActionLog('speedhack', 'Arrêt du speedhack échoué', String(e), 'error')
      return null
    }
  }

  /** Pas de confirmRiskAction/logAiAudit ici -- app.ts s'en charge avant/après. */
  async function blockProcessNetwork(processNameValue = '') {
    const controller = backend.getController()
    if (!controller.blockProcessNetwork) {
      actionLogStore.addActionLog('network_block', 'Blocage réseau indisponible', 'Backend non exposé.', 'warning')
      return null
    }
    networkBlockBusy.value = true
    try {
      const result = await controller.blockProcessNetwork()
      networkBlockStatus.value = { ...result, blocked: result.success ? true : networkBlockStatus.value?.blocked ?? false }
      if (result.success) {
        actionLogStore.addActionLog('network_block', 'Réseau coupé', `${result.exePath ?? processNameValue} isolé du réseau.`, 'success')
      } else if (result.cancelled) {
        actionLogStore.addActionLog('network_block', 'Blocage réseau annulé', 'Invite UAC refusée.', 'warning')
      } else {
        actionLogStore.addActionLog('network_block', 'Blocage réseau échoué', result.error || 'raison inconnue', 'error')
      }
      return result
    } catch (e) {
      actionLogStore.addActionLog('network_block', 'Blocage réseau échoué', String(e), 'error')
      return null
    } finally {
      networkBlockBusy.value = false
    }
  }

  async function unblockProcessNetwork() {
    const controller = backend.getController()
    if (!controller.unblockProcessNetwork) return null
    networkBlockBusy.value = true
    try {
      const result = await controller.unblockProcessNetwork()
      if (result.success) {
        networkBlockStatus.value = { ...result, blocked: false }
        actionLogStore.addActionLog('network_block', 'Réseau rétabli', 'Règle pare-feu retirée.', 'success')
      } else if (result.cancelled) {
        actionLogStore.addActionLog('network_block', 'Rétablissement réseau annulé', 'Invite UAC refusée.', 'warning')
      } else {
        actionLogStore.addActionLog('network_block', 'Rétablissement réseau échoué', result.error || 'raison inconnue', 'error')
      }
      return result
    } catch (e) {
      actionLogStore.addActionLog('network_block', 'Rétablissement réseau échoué', String(e), 'error')
      return null
    } finally {
      networkBlockBusy.value = false
    }
  }

  async function refreshProcessNetworkBlockStatus() {
    const controller = backend.getController()
    if (!controller.getProcessNetworkBlockStatus) return null
    try {
      const status = await controller.getProcessNetworkBlockStatus()
      networkBlockStatus.value = status
      return status
    } catch (e) {
      actionLogStore.addActionLog('network_block', 'Statut réseau indisponible', String(e), 'warning')
      return null
    }
  }

  return {
    speedhackStatus,
    speedhackBusy,
    speedhackFactor,
    networkBlockStatus,
    networkBlockBusy,
    apiHookStatus,
    apiHookBusy,
    apiHookModuleName,
    apiHookFunctionName,
    apiHookMode,
    apiHookForcedReturn,
    refreshSpeedhackStatus,
    startApiHook,
    stopApiHook,
    refreshApiHookStatus,
    startSpeedhack,
    setSpeedhackFactor,
    stopSpeedhack,
    blockProcessNetwork,
    unblockProcessNetwork,
    refreshProcessNetworkBlockStatus,
  }
})
