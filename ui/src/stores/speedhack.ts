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
import { i18n } from '@/i18n'
import {
  backend,
  type ApiHookStatus,
  type ProcessNetworkBlockStatus,
  type SpeedhackStatus,
} from '@/services/backend'
import { useActionLogStore } from './actionLog'

const { t } = i18n.global

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
      actionLogStore.addActionLog('speedhack', t('speedhackStore.statusUnavailable'), String(e), 'warning')
      return null
    }
  }

  // startApiHookAsync/stopApiHookAsync ne bloquent plus le thread GUI
  // (injection + attente du handler jusqu'à 6s/3s sur thread séparé côté
  // backend) : elles renvoient juste {started:true} immédiatement, le vrai
  // résultat arrive via onApiHookStartFinished/onApiHookStopFinished
  // (branchés dans app.ts) — busy reste true jusque-là.
  /** Pas de confirmRiskAction ici -- app.ts confirme avant d'appeler. */
  async function startApiHook() {
    const controller = backend.getController()
    if (!controller.startApiHookAsync) {
      actionLogStore.addActionLog('injection', t('speedhackStore.interceptUnavailable'), t('speedhackStore.backendNotExposed'), 'warning')
      return null
    }
    apiHookBusy.value = true
    try {
      const result = await controller.startApiHookAsync(apiHookModuleName.value, apiHookFunctionName.value, apiHookMode.value, apiHookForcedReturn.value)
      if (!result?.started) {
        apiHookBusy.value = false
        actionLogStore.addActionLog('injection', t('speedhackStore.interceptFailed'), result?.error || t('speedhackStore.unknownReason'), 'error')
      }
      return result
    } catch (e) {
      apiHookBusy.value = false
      actionLogStore.addActionLog('injection', t('speedhackStore.interceptFailed'), String(e), 'error')
      return null
    }
  }

  function onApiHookStartFinished(result: ApiHookStatus) {
    apiHookBusy.value = false
    apiHookStatus.value = result
    if (result.success) {
      actionLogStore.addActionLog('injection', t('speedhackStore.interceptActive'), t('speedhackStore.interceptActiveDetail', { module: apiHookModuleName.value, fn: apiHookFunctionName.value, mode: apiHookMode.value === 1 ? t('speedhackStore.forceReturn') : t('speedhackStore.count') }), 'success')
    } else {
      actionLogStore.addActionLog('injection', t('speedhackStore.interceptFailed'), result.error || t('speedhackStore.unknownReason'), 'error')
    }
  }

  async function stopApiHook() {
    const controller = backend.getController()
    if (!controller.stopApiHookAsync) return null
    try {
      const result = await controller.stopApiHookAsync()
      if (!result?.started) {
        actionLogStore.addActionLog('injection', t('speedhackStore.interceptRemoveFailed'), result?.error || t('speedhackStore.unknownReason'), 'error')
      }
      return result
    } catch (e) {
      actionLogStore.addActionLog('injection', t('speedhackStore.interceptRemoveFailed'), String(e), 'error')
      return null
    }
  }

  function onApiHookStopFinished(result: ApiHookStatus) {
    apiHookStatus.value = result
    actionLogStore.addActionLog('injection', t('speedhackStore.interceptRemoved'), result.finalCallCount !== undefined ? t('speedhackStore.interceptCallCount', { count: result.finalCallCount }) : t('speedhackStore.hookRemoved'), 'success')
  }

  async function refreshApiHookStatus() {
    const controller = backend.getController()
    if (!controller.getApiHookStatus) return null
    try {
      apiHookStatus.value = await controller.getApiHookStatus()
      return apiHookStatus.value
    } catch (e) {
      // UX-CHECKUP round 2 (22/09/2026) : seule fonction refresh* du fichier
      // sans addActionLog sur échec (contrairement à refreshSpeedhackStatus/
      // refreshProcessNetworkBlockStatus juste au-dessus) -- oubli, pas un
      // choix. apiHookStatus.value n'est réécrit que dans le try réussi, donc
      // un échec garde la dernière valeur connue plutôt que de l'effacer.
      actionLogStore.addActionLog('injection', t('speedhackStore.apiHookStatusUnavailable'), String(e), 'warning')
      return null
    }
  }

  // startSpeedhackAsync ne bloque plus le thread GUI (injection + attente du
  // handler jusqu'à 2s sur thread séparé côté backend) : elle renvoie juste
  // {started:true} immédiatement, le vrai résultat arrive via
  // onSpeedhackStartFinished (branché dans app.ts) — busy reste true
  // jusque-là.
  /** Pas de confirmRiskAction/logAiAudit ici -- app.ts s'en charge avant/après. */
  async function startSpeedhack(factor: number) {
    const controller = backend.getController()
    if (!controller.startSpeedhackAsync) {
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackUnavailable'), t('speedhackStore.backendNotExposed'), 'warning')
      return null
    }
    speedhackBusy.value = true
    try {
      const result = await controller.startSpeedhackAsync(factor)
      if (!result?.started) {
        speedhackBusy.value = false
        actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackFailed'), result?.error || t('speedhackStore.unknownReason'), 'error')
      }
      return result
    } catch (e) {
      speedhackBusy.value = false
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackFailed'), String(e), 'error')
      return null
    }
  }

  function onSpeedhackStartFinished(result: SpeedhackStatus) {
    speedhackBusy.value = false
    speedhackStatus.value = result
    if (result.success) {
      speedhackFactor.value = result.factor
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackEnabled'), t('speedhackStore.factorDetail', { factor: result.factor }), 'success')
    } else {
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackFailed'), result.error || t('speedhackStore.unknownReason'), 'error')
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
      actionLogStore.addActionLog('speedhack', t('speedhackStore.factorSettingFailed'), String(e), 'error')
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
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackDisabled'), t('speedhackStore.speedRestored'), 'success')
      return result
    } catch (e) {
      actionLogStore.addActionLog('speedhack', t('speedhackStore.speedhackStopFailed'), String(e), 'error')
      return null
    }
  }

  // blockProcessNetworkAsync/unblockProcessNetworkAsync ne bloquent plus le
  // thread GUI (élévation UAC + New-NetFirewallRule/Remove-NetFirewallRule
  // jusqu'à 15s sur thread séparé côté backend) : elles renvoient juste
  // {started:true} immédiatement, le vrai résultat arrive via
  // onBlockProcessNetworkFinished/onUnblockProcessNetworkFinished (branchés
  // dans app.ts) — busy reste true jusque-là.
  let _pendingBlockProcessName = ''

  /** Pas de confirmRiskAction/logAiAudit ici -- app.ts s'en charge avant/après. */
  async function blockProcessNetwork(processNameValue = '') {
    const controller = backend.getController()
    if (!controller.blockProcessNetworkAsync) {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkBlockUnavailable'), t('speedhackStore.backendNotExposed'), 'warning')
      return null
    }
    _pendingBlockProcessName = processNameValue
    networkBlockBusy.value = true
    try {
      const result = await controller.blockProcessNetworkAsync()
      if (!result?.started) {
        networkBlockBusy.value = false
        actionLogStore.addActionLog('network_block', t('speedhackStore.networkBlockFailed'), result?.error || t('speedhackStore.unknownReason'), 'error')
      }
      return result
    } catch (e) {
      networkBlockBusy.value = false
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkBlockFailed'), String(e), 'error')
      return null
    }
  }

  function onBlockProcessNetworkFinished(result: ProcessNetworkBlockStatus) {
    networkBlockBusy.value = false
    networkBlockStatus.value = { ...result, blocked: result.success ? true : networkBlockStatus.value?.blocked ?? false }
    if (result.success) {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkCut'), t('speedhackStore.networkCutDetail', { exe: result.exePath ?? _pendingBlockProcessName }), 'success')
    } else if (result.cancelled) {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkBlockCancelled'), t('speedhackStore.uacRefused'), 'warning')
    } else {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkBlockFailed'), result.error || t('speedhackStore.unknownReason'), 'error')
    }
  }

  async function unblockProcessNetwork() {
    const controller = backend.getController()
    if (!controller.unblockProcessNetworkAsync) return null
    networkBlockBusy.value = true
    try {
      const result = await controller.unblockProcessNetworkAsync()
      if (!result?.started) {
        networkBlockBusy.value = false
        actionLogStore.addActionLog('network_block', t('speedhackStore.networkRestoreFailed'), result?.error || t('speedhackStore.unknownReason'), 'error')
      }
      return result
    } catch (e) {
      networkBlockBusy.value = false
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkRestoreFailed'), String(e), 'error')
      return null
    }
  }

  function onUnblockProcessNetworkFinished(result: ProcessNetworkBlockStatus) {
    networkBlockBusy.value = false
    if (result.success) {
      networkBlockStatus.value = { ...result, blocked: false }
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkRestored'), t('speedhackStore.firewallRuleRemoved'), 'success')
    } else if (result.cancelled) {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkRestoreCancelled'), t('speedhackStore.uacRefused'), 'warning')
    } else {
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkRestoreFailed'), result.error || t('speedhackStore.unknownReason'), 'error')
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
      actionLogStore.addActionLog('network_block', t('speedhackStore.networkStatusUnavailable'), String(e), 'warning')
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
    onApiHookStartFinished,
    stopApiHook,
    onApiHookStopFinished,
    refreshApiHookStatus,
    startSpeedhack,
    onSpeedhackStartFinished,
    setSpeedhackFactor,
    stopSpeedhack,
    blockProcessNetwork,
    onBlockProcessNetworkFinished,
    unblockProcessNetwork,
    onUnblockProcessNetworkFinished,
    refreshProcessNetworkBlockStatus,
  }
})
