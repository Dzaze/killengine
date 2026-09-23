<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import {
  assistantTools,
  ASSISTANT_TOOLS_SNAPSHOT,
  type AssistantTool,
  type AssistantToolExecution,
} from '@/services/assistantTools'

const { t } = useI18n()

/** Totaux par mode d'exécution — dérivés de la liste, jamais codés en dur. */
const executionCounts = computed(() => {
  const counts: Record<AssistantToolExecution, number> = { direct: 0, confirm: 0, redirect: 0 }
  for (const tool of assistantTools) counts[tool.execution] += 1
  return counts
})

/** Vrai si la liste statique a dérivé du snapshot documenté (garde-fou agent). */
const snapshotDrift = computed(() => assistantTools.length !== ASSISTANT_TOOLS_SNAPSHOT)

const toolsByCategory = computed(() => {
  const categories: Array<{ label: string, tools: AssistantTool[] }> = []
  for (const tool of assistantTools) {
    let entry = categories.find((c) => c.label === tool.category)
    if (!entry) {
      entry = { label: tool.category, tools: [] }
      categories.push(entry)
    }
    entry.tools.push(tool)
  }
  return categories
})

function categoryLabel(category: string): string {
  return t('assistantToolsPanel.categories.' + category)
}

function toolSummary(tool: AssistantTool): string {
  return t('assistantToolsPanel.tools.' + tool.name + '.summary')
}

function toolNote(tool: AssistantTool): string {
  return t('assistantToolsPanel.tools.' + tool.name + '.note')
}

const executionLabels = computed<Record<AssistantToolExecution, string>>(() => ({
  direct: t('assistantToolsPanel.execution.directLabel'),
  confirm: t('assistantToolsPanel.execution.confirmLabel'),
  redirect: t('assistantToolsPanel.execution.redirectLabel'),
}))

const executionTitles = computed<Record<AssistantToolExecution, string>>(() => ({
  direct: t('assistantToolsPanel.execution.directTitle'),
  confirm: t('assistantToolsPanel.execution.confirmTitle'),
  redirect: t('assistantToolsPanel.execution.redirectTitle'),
}))

const riskLabels: Record<string, string> = {
  safe: 'safe',
  write: 'write',
  debug: 'debug',
  patch: 'patch',
  injection: 'injection',
  script: 'script',
}
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <h2>{{ $t('assistantToolsPanel.title') }}</h2>
      <div class="panel-actions">
        <span class="status-pill" :class="snapshotDrift ? 'warn' : 'ok'">
          {{ $t('assistantToolsPanel.toolCount', { count: assistantTools.length }) }}
        </span>
      </div>
    </div>

    <div class="tools-stats-grid">
      <div class="tool-stat-cell">
        <span>{{ $t('assistantToolsPanel.stats.directTitle') }}</span>
        <strong class="stat-direct">{{ executionCounts.direct }}</strong>
        <em>{{ $t('assistantToolsPanel.stats.directDetail') }}</em>
      </div>
      <div class="tool-stat-cell">
        <span>{{ $t('assistantToolsPanel.stats.confirmTitle') }}</span>
        <strong class="stat-confirm">{{ executionCounts.confirm }}</strong>
        <em>{{ $t('assistantToolsPanel.stats.confirmDetail') }}</em>
      </div>
      <div class="tool-stat-cell">
        <span>{{ $t('assistantToolsPanel.stats.redirectTitle') }}</span>
        <strong class="stat-redirect">{{ executionCounts.redirect }}</strong>
        <em>{{ $t('assistantToolsPanel.stats.redirectDetail') }}</em>
      </div>
    </div>

    <p class="hint">
      {{ $t('assistantToolsPanel.hintPrefix') }}
      <code>find_what_writes</code>, <code>test_candidate_fields</code> {{ $t('assistantToolsPanel.hintAnd') }}
      <code>patch_file_bytes</code> {{ $t('assistantToolsPanel.hintSuffix') }}
    </p>

    <details class="tools-details">
      <summary>{{ $t('assistantToolsPanel.fullListSummary') }}</summary>
      <div v-for="group in toolsByCategory" :key="group.label" class="tool-category">
        <strong>{{ categoryLabel(group.label) }}</strong>
        <div v-for="tool in group.tools" :key="tool.name" class="tool-row">
          <span class="tool-exec-badge" :class="tool.execution" :title="executionTitles[tool.execution]">
            {{ executionLabels[tool.execution] }}
          </span>
          <div class="tool-info">
            <code>{{ tool.name }}</code>
            <span class="tool-risk">{{ $t('assistantToolsPanel.riskPrefix') }}&nbsp;: {{ riskLabels[tool.risk] }}</span>
            <span class="tool-summary">{{ toolSummary(tool) }}</span>
            <span v-if="tool.hasNote" class="tool-note">{{ toolNote(tool) }}</span>
          </div>
        </div>
      </div>
    </details>

    <p class="hint">
      {{ $t('assistantToolsPanel.investigationModeHint') }}
    </p>
  </section>
</template>

<style scoped>
/* Mêmes patterns visuels que SettingsView.vue (panel-actions / status-pill y sont scoped). */
.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.status-pill {
  border: 1px solid var(--border);
  border-radius: 999px;
  font-size: 11px;
  padding: 4px 8px;
}

.status-pill.ok {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.status-pill.warn {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.tools-stats-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
  margin-top: 12px;
}

.tool-stat-cell {
  min-height: 76px;
  padding: 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.tool-stat-cell span {
  display: block;
  color: var(--text-muted);
  font-size: 12px;
}

.tool-stat-cell strong {
  display: block;
  margin-top: 4px;
  font-size: 20px;
}

.tool-stat-cell em {
  display: block;
  margin-top: 4px;
  color: var(--text-muted);
  font-size: 11px;
  font-style: normal;
  line-height: 1.4;
}

.stat-direct {
  color: var(--success);
}

.stat-confirm {
  color: var(--warning);
}

.stat-redirect {
  color: var(--accent);
}

.tools-details {
  margin-top: 10px;
  color: var(--text-muted);
  font-size: 12px;
}

.tools-details summary {
  cursor: pointer;
}

.tool-category {
  margin-top: 10px;
}

.tool-category > strong {
  display: block;
  margin-bottom: 4px;
  color: var(--text-primary);
  font-size: 12px;
}

.tool-row {
  display: grid;
  grid-template-columns: 110px minmax(0, 1fr);
  gap: 8px;
  align-items: start;
  padding: 5px 0;
  border-top: 1px solid var(--border);
}

.tool-exec-badge {
  display: inline-block;
  justify-self: start;
  padding: 2px 8px;
  border: 1px solid var(--border);
  border-radius: 10px;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.02em;
  text-transform: uppercase;
  white-space: nowrap;
  cursor: help;
}

.tool-exec-badge.direct {
  border-color: rgba(158, 206, 106, 0.4);
  background: rgba(158, 206, 106, 0.1);
  color: var(--success);
}

.tool-exec-badge.confirm {
  border-color: rgba(224, 175, 104, 0.45);
  background: rgba(224, 175, 104, 0.12);
  color: var(--warning);
}

.tool-exec-badge.redirect {
  border-color: rgba(122, 162, 247, 0.35);
  background: rgba(122, 162, 247, 0.1);
  color: var(--accent);
}

.tool-info {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.tool-info code {
  color: var(--text-primary);
  font-size: 12px;
}

.tool-risk {
  color: var(--text-muted);
  font-size: 11px;
}

.tool-summary {
  color: var(--text-secondary);
  font-size: 12px;
  line-height: 1.4;
}

.tool-note {
  color: var(--text-muted);
  font-size: 12px;
  font-style: italic;
  line-height: 1.4;
}
</style>
