<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useActivityStore, type ActivityEntry } from '@/stores/activity'
import { useAppStore, type AppView } from '@/stores/app'

/**
 * UX-PRODUIT-12 -- Centre d'activité permanent : tiroir listant les
 * opérations en cours/terminées (scans, Timeline, Lua, installations,
 * surveillance de fichier). Nouveau patron de tiroir slide-in (aucun
 * composant de tiroir existant à réutiliser) ; reprend seulement les
 * conventions ARIA/backdrop de App.vue (.risk-backdrop, role="dialog"
 * aria-modal="true").
 */

const emit = defineEmits<{ close: [] }>()

const activityStore = useActivityStore()
const appStore = useAppStore()

const cancellingIds = ref<Set<string>>(new Set())
const now = ref(Date.now())
let tickTimer: ReturnType<typeof setInterval> | undefined

onMounted(() => {
  document.addEventListener('keydown', onKeydown)
  tickTimer = setInterval(() => { now.value = Date.now() }, 1000)
})

onUnmounted(() => {
  document.removeEventListener('keydown', onKeydown)
  if (tickTimer) clearInterval(tickTimer)
})

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    close()
  }
}

function close() {
  activityStore.closeDrawer()
  emit('close')
}

const kindToView: Partial<Record<string, AppView>> = {
  scan_exact: 'expert',
  scan_auto: 'expert',
  scan_next: 'expert',
  scan_capture_unknown: 'expert',
  scan_unknown_next: 'expert',
  timeline_collection: 'memory-timeline',
  lua_script: 'scripting',
  module_install: 'modules',
  save_file_watch: 'investigation',
  candidate_comparison: 'memory-timeline',
}

function isTerminal(state: ActivityEntry['state']): boolean {
  return state === 'completed' || state === 'cancelled' || state === 'failed' || state === 'interrupted'
}

function isCancellable(entry: ActivityEntry): boolean {
  return entry.canCancel && (entry.state === 'running' || entry.state === 'cancel_requested')
}

function canOpen(entry: ActivityEntry): boolean {
  return Boolean(kindToView[entry.kind])
}

function openEntry(entry: ActivityEntry) {
  const view = kindToView[entry.kind]
  if (view) {
    appStore.activeView = view
  }
  close()
}

async function stopEntry(entry: ActivityEntry) {
  cancellingIds.value.add(entry.operationId)
  try {
    await activityStore.cancelActivity(entry.operationId)
  } finally {
    cancellingIds.value.delete(entry.operationId)
  }
}

function elapsedLabel(entry: ActivityEntry): string {
  const end = isTerminal(entry.state) && entry.finishedAtMs > 0 ? entry.finishedAtMs : now.value
  const seconds = Math.max(0, Math.round((end - entry.startedAtMs) / 1000))
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  const remSeconds = seconds % 60
  return `${minutes}m${String(remSeconds).padStart(2, '0')}s`
}

const runningEntries = computed(() => activityStore.sortedEntries.filter((e) => !isTerminal(e.state)))
const terminalEntries = computed(() => activityStore.sortedEntries.filter((e) => isTerminal(e.state)))
</script>

<template>
  <div class="activity-backdrop" role="presentation" @click.self="close">
    <section class="activity-drawer" role="dialog" aria-modal="true" aria-labelledby="activity-panel-title">
      <header class="activity-header">
        <h2 id="activity-panel-title">{{ $t('activity.title') }}</h2>
        <button type="button" class="activity-close" :aria-label="$t('activity.close')" @click="close">✕</button>
      </header>

      <div class="activity-body">
        <p v-if="runningEntries.length === 0 && terminalEntries.length === 0" class="activity-empty">
          {{ $t('activity.empty') }}
        </p>

        <template v-if="runningEntries.length > 0">
          <h3 class="activity-section-title">{{ $t('activity.sectionActive') }}</h3>
          <ul class="activity-list">
            <li v-for="entry in runningEntries" :key="entry.operationId" class="activity-entry">
              <div class="activity-entry-main">
                <span class="activity-kind">{{ $t(`activity.kind.${entry.kind}`) }}</span>
                <span class="activity-state" :class="`state-${entry.state}`">{{ $t(`activity.state.${entry.state}`) }}</span>
              </div>
              <div v-if="entry.target" class="activity-target">{{ entry.target.processName }} · PID {{ entry.target.pid }}</div>
              <div class="activity-summary">{{ entry.summary }}</div>
              <div class="activity-progress">
                <div v-if="typeof entry.progress === 'number'" class="activity-progress-bar">
                  <div class="activity-progress-fill" :style="{ width: `${Math.min(100, Math.max(0, entry.progress))}%` }" />
                </div>
                <div v-else class="activity-progress-indeterminate" />
                <span class="activity-elapsed">{{ elapsedLabel(entry) }}</span>
              </div>
              <div class="activity-actions">
                <button v-if="canOpen(entry)" type="button" class="activity-btn" @click="openEntry(entry)">{{ $t('activity.view') }}</button>
                <button
                  v-if="isCancellable(entry)"
                  type="button"
                  class="activity-btn danger"
                  :disabled="cancellingIds.has(entry.operationId)"
                  @click="stopEntry(entry)"
                >
                  {{ $t('activity.stop') }}
                </button>
              </div>
            </li>
          </ul>
        </template>

        <template v-if="terminalEntries.length > 0">
          <h3 class="activity-section-title">{{ $t('activity.sectionTerminal') }}</h3>
          <ul class="activity-list">
            <li v-for="entry in terminalEntries" :key="entry.operationId" class="activity-entry">
              <div class="activity-entry-main">
                <span class="activity-kind">{{ $t(`activity.kind.${entry.kind}`) }}</span>
                <span class="activity-state" :class="`state-${entry.state}`">{{ $t(`activity.state.${entry.state}`) }}</span>
              </div>
              <div v-if="entry.target" class="activity-target">{{ entry.target.processName }} · PID {{ entry.target.pid }}</div>
              <div class="activity-summary">{{ entry.summary }}</div>
              <div v-if="entry.errorMessage" class="activity-error">{{ entry.errorMessage }}</div>
              <div class="activity-progress">
                <span class="activity-elapsed">{{ elapsedLabel(entry) }}</span>
              </div>
              <div class="activity-actions">
                <button v-if="canOpen(entry)" type="button" class="activity-btn" @click="openEntry(entry)">{{ $t('activity.view') }}</button>
              </div>
            </li>
          </ul>
        </template>
      </div>
    </section>
  </div>
</template>

<style scoped>
.activity-backdrop {
  position: fixed;
  inset: 0;
  z-index: 1000;
  background: rgba(8, 9, 14, 0.5);
}

.activity-drawer {
  position: absolute;
  top: 0;
  right: 0;
  bottom: 0;
  width: min(380px, 100%);
  background: var(--bg-secondary);
  border-left: 1px solid rgba(125, 142, 255, 0.2);
  box-shadow: -24px 0 60px rgba(0, 0, 0, 0.4);
  display: flex;
  flex-direction: column;
}

.activity-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px;
  border-bottom: 1px solid rgba(125, 142, 255, 0.14);
}

.activity-header h2 {
  font-size: 16px;
  color: var(--text-primary);
}

.activity-close {
  background: transparent;
  border: none;
  color: var(--text-muted);
  font-size: 16px;
  cursor: pointer;
  padding: 4px 8px;
}

.activity-close:hover {
  color: var(--text-primary);
}

.activity-body {
  flex: 1;
  overflow-y: auto;
  padding: 12px 16px 20px;
}

.activity-empty {
  color: var(--text-muted);
  font-size: 13px;
  margin-top: 12px;
}

.activity-section-title {
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  color: var(--text-muted);
  margin: 16px 0 8px;
}

.activity-list {
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.activity-entry {
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-tertiary);
  padding: 10px 12px;
}

.activity-entry-main {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.activity-kind {
  font-size: 13px;
  font-weight: 600;
  color: var(--text-primary);
}

.activity-state {
  font-size: 11px;
  padding: 2px 8px;
  border-radius: 999px;
  color: var(--text-muted);
  border: 1px solid var(--border);
}

.activity-state.state-running,
.activity-state.state-cancel_requested {
  color: var(--accent);
  border-color: var(--accent);
}

.activity-state.state-completed {
  color: var(--success);
  border-color: var(--success);
}

.activity-state.state-failed,
.activity-state.state-interrupted {
  color: var(--error);
  border-color: var(--error);
}

.activity-target,
.activity-summary {
  font-size: 12px;
  color: var(--text-muted);
  margin-top: 4px;
  overflow-wrap: anywhere;
}

.activity-error {
  font-size: 12px;
  color: var(--error);
  margin-top: 4px;
  overflow-wrap: anywhere;
}

.activity-progress {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 8px;
}

.activity-progress-bar {
  flex: 1;
  height: 4px;
  border-radius: 2px;
  background: rgba(125, 142, 255, 0.14);
  overflow: hidden;
}

.activity-progress-fill {
  height: 100%;
  background: var(--accent);
  transition: width 0.2s ease;
}

.activity-progress-indeterminate {
  flex: 1;
  height: 4px;
  border-radius: 2px;
  background: repeating-linear-gradient(90deg, var(--accent) 0 8px, transparent 8px 16px);
  opacity: 0.5;
}

.activity-elapsed {
  font-size: 11px;
  color: var(--text-muted);
  white-space: nowrap;
}

.activity-actions {
  display: flex;
  gap: 8px;
  margin-top: 8px;
}

.activity-btn {
  font-size: 12px;
  padding: 4px 10px;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--bg-accent);
  color: var(--text-primary);
  cursor: pointer;
}

.activity-btn:hover {
  border-color: var(--accent);
}

.activity-btn.danger {
  border-color: color-mix(in srgb, var(--error) 45%, var(--border));
  color: var(--error);
}

.activity-btn:disabled {
  opacity: 0.5;
  cursor: default;
}
</style>
