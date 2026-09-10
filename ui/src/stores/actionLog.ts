/**
 * KillEngine — store Action Log (extrait de app.ts, candidat S5 de
 * docs/REFACTOR_ROADMAP.md, 29/08/2026).
 *
 * Volontairement sans dépendance vers `./app`, même principe que
 * `./investigation` (candidat S6) : c'est une fondation appelée depuis
 * ~282 sites répartis dans app.ts, pas l'inverse. `processName` (utilisé
 * uniquement par les exports) est reçu en paramètre explicite plutôt que lu
 * directement sur le store principal.
 */
import { defineStore } from 'pinia'
import { ref } from 'vue'
import { i18n } from '@/i18n'

const { t } = i18n.global

export interface UserActionLogEntry {
  id: number
  time: string
  kind: string
  title: string
  detail: string
  status: 'info' | 'success' | 'warning' | 'error'
}

function nowTime(): string {
  return new Date().toLocaleTimeString('fr-FR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

export const useActionLogStore = defineStore('actionLog', () => {
  const actionLog = ref<UserActionLogEntry[]>([])
  const actionLogIdCounter = ref(0)

  const actionLogStorageKey = 'killengine.action_log.v1'

  function addActionLog(
    kind: string,
    title: string,
    detail = '',
    status: UserActionLogEntry['status'] = 'info',
  ) {
    actionLogIdCounter.value += 1
    actionLog.value.unshift({
      id: actionLogIdCounter.value,
      time: nowTime(),
      kind,
      title,
      detail,
      status,
    })
    actionLog.value = actionLog.value.slice(0, 80)
    saveActionLog()
  }

  function saveActionLog() {
    try {
      window.localStorage.setItem(actionLogStorageKey, JSON.stringify({
        entries: actionLog.value.slice(0, 200),
        id: actionLogIdCounter.value,
      }))
    } catch {
      // Best-effort audit: runtime actions must continue even if local storage is full.
    }
  }

  function loadActionLog() {
    try {
      const raw = window.localStorage.getItem(actionLogStorageKey)
      if (!raw) return
      const parsed = JSON.parse(raw) as { entries?: UserActionLogEntry[], id?: number }
      actionLog.value = Array.isArray(parsed.entries) ? parsed.entries.slice(0, 200) : []
      actionLogIdCounter.value = Number(parsed.id ?? actionLog.value.reduce((max, entry) => Math.max(max, Number(entry.id) || 0), 0))
    } catch {
      actionLog.value = []
      actionLogIdCounter.value = 0
    }
  }

  function clearActionLog() {
    actionLog.value = []
    actionLogIdCounter.value = 0
    saveActionLog()
  }

  function exportActionLogJson(processNameValue = ''): string {
    return JSON.stringify({
      version: 1,
      exportedAt: new Date().toISOString(),
      processName: processNameValue,
      entries: actionLog.value,
    }, null, 2)
  }

  function exportActionLogMarkdown(processNameValue = ''): string {
    const lines = [
      '# KillEngine Audit Log',
      '',
      `${t('actionLogStore.markdown.export')}: ${new Date().toISOString()}`,
      `${t('actionLogStore.markdown.process')}: ${processNameValue || t('actionLogStore.markdown.notAttached')}`,
      `${t('actionLogStore.markdown.entries')}: ${actionLog.value.length}`,
      '',
      ...actionLog.value.slice(0, 200).map((entry) =>
        `- ${entry.time} [${entry.status}] ${entry.kind} - ${entry.title}${entry.detail ? `: ${entry.detail}` : ''}`,
      ),
    ]
    return lines.join('\n')
  }

  return {
    actionLog,
    actionLogIdCounter,
    addActionLog,
    saveActionLog,
    loadActionLog,
    clearActionLog,
    exportActionLogJson,
    exportActionLogMarkdown,
  }
})
