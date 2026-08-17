<script setup lang="ts">
import { ref, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import type { ProcessInfo } from '@/services/backend'

const store = useAppStore()
const searchFilter = ref('')
const selectedPid = ref<number | null>(null)
const windowOnly = ref(false)

const filteredProcesses = ref<ProcessInfo[]>([])

function updateFiltered() {
  filteredProcesses.value = store.processes.filter((p) => {
    if (windowOnly.value && !p.hasWindow) return false
    if (!searchFilter.value) return true
    const q = searchFilter.value.toLowerCase()
    return p.name.toLowerCase().includes(q) || p.path.toLowerCase().includes(q)
  })
}

async function selectProcess(pid: number) {
  selectedPid.value = pid
  await store.refreshProcessModules(pid)
}

async function attach() {
  if (selectedPid.value !== null) {
    await store.attach(selectedPid.value)
  }
}

async function detach() {
  await store.detach()
}

onMounted(async () => {
  await store.refreshProcesses()
  updateFiltered()
})
</script>

<template>
  <div class="process-view">
    <div class="header">
      <h1>{{ $t('process.select') }}</h1>
      <button class="btn btn-secondary" @click="store.refreshProcesses().then(updateFiltered)">
        {{ $t('process.refresh') }}
      </button>
    </div>

    <!-- Attached status -->
    <div v-if="store.isAttached" class="attached-banner">
      <div class="attached-info">
        <span class="badge badge-success">● {{ $t('app.attached') }}</span>
        <strong>{{ store.processName }}</strong>
      </div>
      <button class="btn btn-danger" @click="detach">
        {{ $t('process.detach') }}
      </button>
    </div>

    <!-- Search filter -->
    <div class="filter-bar">
      <input
        v-model="searchFilter"
        placeholder="Filtrer..."
        class="search-input"
        @input="updateFiltered"
      />
      <label class="window-toggle">
        <input v-model="windowOnly" type="checkbox" @change="updateFiltered" />
        {{ $t('process.windowOnly') }}
      </label>
    </div>

    <!-- Process list -->
    <div class="process-list">
      <div
        v-for="proc in filteredProcesses"
        :key="proc.pid"
        class="process-item"
        :class="{ selected: selectedPid === proc.pid }"
        @click="selectProcess(proc.pid)"
      >
        <div class="proc-icon">🎮</div>
        <div class="proc-info">
          <div class="proc-name">{{ proc.name }}</div>
          <div class="proc-details">
            <span class="proc-pid">PID {{ proc.pid }}</span>
            <span class="proc-arch">{{ proc.arch }}</span>
            <span class="proc-modules">{{ proc.moduleCount }} modules</span>
            <span v-if="proc.hasWindow" class="proc-window">{{ $t('process.hasWindow') }}</span>
          </div>
          <div class="proc-path" :title="proc.path">{{ proc.path }}</div>
        </div>
      </div>

      <div v-if="filteredProcesses.length === 0" class="empty">
        {{ $t('process.noProcesses') }}
      </div>
    </div>

    <!-- Attach button -->
    <div v-if="selectedPid !== null && !store.isAttached" class="attach-bar">
      <span>Sélectionné: PID {{ selectedPid }}</span>
      <button class="btn btn-primary" @click="attach">
        {{ $t('process.attach') }}
      </button>
    </div>

    <div v-if="selectedPid !== null" class="module-panel">
      <div class="module-header">
        <h2>{{ $t('process.modules') }}</h2>
        <span>{{ store.processModules.length }}</span>
      </div>
      <div class="module-list">
        <div v-for="module in store.processModules.slice(0, 40)" :key="`${module.baseAddress}-${module.name}`" class="module-item">
          <div class="module-name">{{ module.name }}</div>
          <div class="module-path" :title="module.path">{{ module.path }}</div>
        </div>
        <div v-if="store.processModules.length === 0" class="empty compact">
          {{ $t('process.noModules') }}
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.process-view {
  padding: 24px 32px;
  max-width: 1120px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
}

.attached-banner {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: rgba(158, 206, 106, 0.1);
  border: 1px solid rgba(158, 206, 106, 0.3);
  border-radius: 8px;
  padding: 12px 16px;
  margin-bottom: 20px;
}

.attached-info {
  display: flex;
  align-items: center;
  gap: 12px;
}

.badge {
  font-size: 13px;
  font-weight: 600;
}

.badge-success {
  color: var(--success);
}

.filter-bar {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
}

.search-input {
  flex: 1;
  padding: 10px 14px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
  font-size: 14px;
  outline: none;
}

.search-input:focus {
  border-color: var(--accent);
}

.window-toggle {
  display: flex;
  align-items: center;
  gap: 8px;
  color: var(--text-dim);
  font-size: 13px;
  white-space: nowrap;
}

.process-list {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: min(620px, calc(100vh - 260px));
  overflow-y: auto;
}

.process-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 14px;
  background: var(--bg-tertiary);
  border: 1px solid transparent;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.15s;
}

.process-item:hover {
  border-color: var(--border);
  background: var(--bg-accent);
}

.process-item.selected {
  border-color: var(--accent);
  background: rgba(122, 162, 247, 0.1);
}

.proc-icon {
  font-size: 20px;
}

.proc-info {
  flex: 1;
  min-width: 0;
}

.proc-name {
  font-size: 14px;
  font-weight: 500;
  color: var(--text-primary);
}

.proc-details {
  display: flex;
  gap: 12px;
  font-size: 12px;
  color: var(--text-dim);
}

.proc-path {
  margin-top: 2px;
  overflow: hidden;
  color: var(--text-dim);
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.empty {
  padding: 40px;
  text-align: center;
  color: var(--text-dim);
}

.attach-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px;
  background: var(--bg-tertiary);
  border: 1px solid var(--accent);
  border-radius: 8px;
  margin-top: 20px;
}

.module-panel {
  margin-top: 20px;
}

.module-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}

.module-header h2 {
  margin: 0;
  color: var(--text-primary);
  font-size: 15px;
}

.module-header span {
  color: var(--text-dim);
  font-size: 12px;
}

.module-list {
  display: flex;
  max-height: 220px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.module-item {
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.module-name {
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 500;
}

.module-path {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.empty.compact {
  padding: 16px;
}

.btn {
  padding: 8px 16px;
  border: none;
  border-radius: 6px;
  font-size: 13px;
  cursor: pointer;
  transition: all 0.15s;
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.btn-danger {
  background: var(--error);
  color: white;
}
</style>
