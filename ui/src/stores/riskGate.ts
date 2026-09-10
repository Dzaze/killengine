/**
 * KillEngine — store RiskGate (extrait de app.ts, dernière fondation
 * partagée du plan de refactorisation, docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * `confirmRiskAction` est le SEUL mécanisme de confirmation humaine avant
 * une action à risque (write/debug/patch/injection) dans toute l'app —
 * voir docs/PHASE_TRACKER_HISTORY.md (PHASE 153-155, retrait du gating par
 * mode : confirmRiskAction est la seule friction autorisée). Ce store ne
 * doit jamais s'auto-accepter en dehors des deux cas déjà existants avant
 * cette extraction (contournement pipe d'automatisation, mémorisation
 * explicite par l'utilisateur type "ne plus redemander pour le speedhack").
 *
 * Dépend de `./actionLog` et `./investigation` (safe, fondations sans
 * dépendance vers `./app`). La télémétrie `logAiAudit` (liée à
 * activeInvestigation/searchQuery côté app.ts) est injectée en callback
 * optionnel par l'appelant plutôt qu'importée directement, pour ne pas
 * dupliquer cette logique ici.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { i18n } from '@/i18n'
import type { InvestigationStep } from './investigation'
import { useActionLogStore } from './actionLog'
import { useInvestigationStore } from './investigation'

const { t } = i18n.global

export interface RiskDialogState {
  open: boolean
  risk: NonNullable<InvestigationStep['risk']>
  title: string
  detail: string
  rememberKey?: 'speedhack'
  rememberChoice?: boolean
  rememberLabel?: string
}

export const useRiskGateStore = defineStore('riskGate', () => {
  const actionLogStore = useActionLogStore()
  const investigationStore = useInvestigationStore()

  const riskDialog = ref<RiskDialogState | null>(null)
  const mutedRiskConfirmations = ref<Record<string, boolean>>({})
  // Incrémenté/décrémenté par le pont pipe d'automatisation (app.ts,
  // window.__killengineAutomationBridge.dispatch) autour de chaque appel --
  // > 0 pendant qu'un appel piloté par le pipe est en vol, pour que
  // confirmRiskAction s'auto-accepte au lieu d'ouvrir un vrai dialogue
  // (comportement délibéré depuis PHASE 158-159, pas un bug).
  const automationPipeDispatchDepth = ref(0)

  let riskDialogResolver: ((accepted: boolean) => void) | null = null

  async function confirmRiskAction(
    risk: NonNullable<InvestigationStep['risk']>,
    title: string,
    detail: string,
    onAudit?: (event: string, payload: Record<string, unknown>) => void,
    rememberKeyInput?: 'speedhack',
  ): Promise<boolean> {
    if (automationPipeDispatchDepth.value > 0) {
      actionLogStore.addActionLog('risk_gate', t('riskGateStore.pipeAutoConfirmed', { title }), detail, 'success')
      onAudit?.('risk_pipe_bypass', { risk, title, detail })
      investigationStore.addInvestigationStep({
        title: t('riskGateStore.pipeAutoConfirmed', { title }),
        detail,
        status: 'checkpoint',
        risk,
        tool: 'AutomationPipe',
        payload: { accepted: true, title, detail, source: 'automation_pipe' },
      })
      return true
    }
    const rememberKey = rememberKeyInput
    if (rememberKey && mutedRiskConfirmations.value[rememberKey]) {
      actionLogStore.addActionLog('risk_gate', t('riskGateStore.rememberedConfirmation', { title }), detail, 'info')
      onAudit?.('risk_muted_accept', { risk, title, detail, rememberKey })
      return true
    }
    const accepted = await new Promise<boolean>((resolve) => {
      if (riskDialogResolver) {
        riskDialogResolver(false)
      }
      riskDialogResolver = resolve
      riskDialog.value = {
        open: true,
        risk,
        title,
        detail,
        rememberKey,
        rememberChoice: false,
        rememberLabel: rememberKey === 'speedhack' ? t('riskGateStore.rememberSpeedhack') : undefined,
      }
    })
    actionLogStore.addActionLog('risk_gate', accepted ? t('riskGateStore.confirmed', { title }) : t('riskGateStore.refused', { title }), detail, accepted ? 'success' : 'warning')
    onAudit?.(accepted ? 'risk_confirmed' : 'risk_refused', { risk, title, detail })
    investigationStore.addInvestigationStep({
      title: accepted ? t('riskGateStore.riskConfirmed', { title }) : t('riskGateStore.riskRefused', { title }),
      detail,
      status: accepted ? 'checkpoint' : 'warning',
      risk,
      tool: 'RiskGate',
      payload: { accepted, title, detail },
    })
    return accepted
  }

  function resolveRiskDialog(accepted: boolean) {
    const dialog = riskDialog.value
    if (accepted && dialog?.rememberKey && dialog.rememberChoice) {
      mutedRiskConfirmations.value = {
        ...mutedRiskConfirmations.value,
        [dialog.rememberKey]: true,
      }
    }
    const resolver = riskDialogResolver
    riskDialogResolver = null
    riskDialog.value = null
    if (resolver) {
      resolver(accepted)
    }
  }

  return {
    riskDialog,
    mutedRiskConfirmations,
    automationPipeDispatchDepth,
    confirmRiskAction,
    resolveRiskDialog,
  }
})
