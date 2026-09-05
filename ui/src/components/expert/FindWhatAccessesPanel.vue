<script setup lang="ts">
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

defineProps<{
  findWhatAccessesResult: Record<string, unknown> | null
}>()

function formatAddress(hex: string | undefined) {
  if (!hex) return '—'
  return String(hex)
}

function formatNumber(n: unknown) {
  if (typeof n === 'number') return n.toLocaleString()
  return String(n ?? 0)
}
</script>

<template>
  <section class="panel">
    <div class="panel-head">
      <div class="panel-heading">
        <h2>{{ $t('findWhatAccesses.title') }}</h2>
        <InfoDot topic="findWhatAccesses" />
        <RiskBadge level="code" />
      </div>
      <span v-if="findWhatAccessesResult?.hitCount">
        {{ formatNumber(findWhatAccessesResult.hitCount) }} accès capturé(s)
      </span>
    </div>

    <p class="hint">
      {{ $t('findWhatAccesses.intro') }}
    </p>

    <!-- Résultat : table de hits -->
    <div v-if="findWhatAccessesResult?.hitCount" class="accesses-table">
      <table class="data-table">
        <thead>
          <tr>
            <th>#</th>
            <th>{{ $t('findWhatAccesses.instruction') }}</th>
            <th>{{ $t('findWhatAccesses.module') }}</th>
            <th>{{ $t('findWhatAccesses.thread') }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(hit, idx) in (findWhatAccessesResult.hits as Record<string, unknown>[] ?? [])" :key="idx">
            <td class="idx-cell">{{ idx + 1 }}</td>
            <td class="instr-cell">
              <code>{{ formatAddress(hit.instructionPointer as string) }}</code>
            </td>
            <td class="module-cell">
              <span v-if="hit.module">{{ hit.module }}+{{ formatAddress(hit.moduleOffset as string) }}</span>
              <span v-else class="no-module">—</span>
            </td>
            <td class="thread-cell">{{ hit.threadId ?? '—' }}</td>
          </tr>
        </tbody>
      </table>
    </div>

    <!-- Aucun résultat -->
    <div v-else-if="findWhatAccessesResult && !findWhatAccessesResult.cancelled" class="empty-state">
      {{ findWhatAccessesResult.error ?? $t('findWhatAccesses.empty') }}
    </div>

    <!-- Annulé -->
    <div v-if="findWhatAccessesResult?.cancelled" class="cancelled-state">
      {{ $t('findWhatAccesses.cancelled') }}
    </div>
  </section>
</template>

<style scoped>
.panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
}

.panel-heading {
  display: flex;
  align-items: center;
  gap: 8px;
}

.panel-heading h2 {
  font-size: 16px;
  color: var(--text-primary);
  margin: 0;
}

.hint {
  color: var(--text-secondary);
  font-size: 12px;
  margin-bottom: 12px;
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.data-table th {
  text-align: left;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-dim);
  font-weight: 600;
}

.data-table td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
}

.data-table tbody tr:hover {
  background: var(--bg-tertiary);
}

.instr-cell code {
  font-family: 'Cascadia Code', 'Fira Code', monospace;
  font-size: 11px;
  color: var(--accent);
}

.module-cell {
  color: var(--text-secondary);
}

.no-module {
  color: var(--text-dim);
}

.empty-state,
.cancelled-state {
  padding: 16px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  font-size: 12px;
  text-align: center;
}

.cancelled-state {
  color: var(--text-dim);
  font-style: italic;
}
</style>
