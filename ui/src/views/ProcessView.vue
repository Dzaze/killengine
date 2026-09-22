<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useAppStore } from '@/stores/app'
import { useI18n } from 'vue-i18n'
import InfoDot from '@/components/expert/InfoDot.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t } = useI18n()
const searchFilter = ref('')
const selectedPid = ref<number | null>(null)
const windowOnly = ref(false)

// UX-PIPE-4 : la liste de modules d'un process pouvait dépasser largement 40
// éléments (147 observés en audit live) mais n'en affichait que les 40
// premiers sans aucune indication ni moyen d'accéder au reste.
const MODULE_PAGE_SIZE = 40
const moduleFilter = ref('')
const moduleVisibleCount = ref(MODULE_PAGE_SIZE)
const filteredModules = computed(() => {
  if (!moduleFilter.value) return store.processModules
  const q = moduleFilter.value.toLowerCase()
  return store.processModules.filter((m) => m.name.toLowerCase().includes(q) || m.path.toLowerCase().includes(q))
})
const visibleModules = computed(() => filteredModules.value.slice(0, moduleVisibleCount.value))
watch(moduleFilter, () => {
  moduleVisibleCount.value = MODULE_PAGE_SIZE
})

// UX-PIPE-1 : dérivé réactivement de store.processes (au lieu d'une copie
// locale resynchronisée manuellement) -- sinon un chargement déclenché après
// le montage (connexion backend tardive, voir le watch ci-dessous) ne se
// répercutait jamais sur la liste affichée.
const filteredProcesses = computed(() =>
  store.processes.filter((p) => {
    if (windowOnly.value && !p.hasWindow) return false
    if (!searchFilter.value) return true
    const q = searchFilter.value.toLowerCase()
    return p.name.toLowerCase().includes(q) || p.path.toLowerCase().includes(q)
  }),
)
const selectedProcess = computed(() => store.processes.find((p) => p.pid === selectedPid.value) ?? null)
const kernelStatusText = computed(() => {
  if (!store.kernelDriverStatus) return t('process.kernelStatus.notTested')
  if (store.kernelMemoryReady) return t('process.kernelStatus.ready')
  if (store.kernelDriverStatus.status === 'connected') return t('process.kernelStatus.probeOnly')
  return t('process.kernelStatus.notLoaded')
})

async function selectProcess(pid: number) {
  selectedPid.value = pid
  moduleFilter.value = ''
  moduleVisibleCount.value = MODULE_PAGE_SIZE
  await store.refreshProcessModules(pid)
}

async function attach() {
  if (selectedPid.value !== null) {
    await store.attach(selectedPid.value, store.memoryAccessMode)
  }
}

async function startKernelDriverFromAttach() {
  await store.startKernelDriver()
}

async function detach() {
  await store.detach()
}

async function loadProcesses() {
  await Promise.all([store.refreshProcesses(), store.refreshKernelDriverStatus()])
}

// UX-PIPE-1 : déclenché par la connexion réelle du backend plutôt que par le
// montage du composant -- App.vue lance store.init() (async) au même moment,
// et selon l'ordre de montage Vue, ProcessView pouvait monter avant que le
// backend QWebChannel soit prêt. { immediate: true } couvre les deux cas :
// backend déjà connecté (ex. retour sur cette vue) déclenche le chargement
// tout de suite ; backend pas encore connecté ne fait rien ici et se
// déclenche seul dès que store.isConnected passe à true.
watch(
  () => store.isConnected,
  (connected) => {
    if (connected) void loadProcesses()
  },
  { immediate: true },
)
</script>

<template>
  <div class="process-view">
    <div class="header">
      <h1>{{ $t('process.select') }}</h1>
      <button class="btn btn-secondary" :disabled="store.processesLoading" @click="loadProcesses">
        {{ $t('process.refresh') }}
      </button>
    </div>

    <PanelIntro
      :what="$t('process.intro.what')"
      :purpose="$t('process.intro.purpose')"
      :how="$t('process.intro.how')"
    />

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
        :placeholder="$t('process.filterPlaceholder')"
        class="search-input"
      />
      <label class="window-toggle">
        <input v-model="windowOnly" type="checkbox" />
        {{ $t('process.windowOnly') }}
      </label>
    </div>

    <!-- Process list -->
    <div v-if="store.processesLoading && store.processes.length === 0" class="empty">
      {{ $t('process.loading') }}
    </div>
    <div v-else-if="store.processesError" class="empty error">
      {{ $t('process.errors.loadFailed', { error: store.processesError }) }}
      <button class="btn btn-secondary" @click="loadProcesses">{{ $t('process.refresh') }}</button>
    </div>
    <div v-else class="process-list">
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
            <span class="proc-modules">{{ $t('process.moduleCount', { count: proc.moduleCount }) }}</span>
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
      <div class="attach-summary">
        <span>{{ $t('process.selectedProcess', { name: selectedProcess?.name ?? $t('process.fallbackProcess'), pid: selectedPid }) }}</span>
        <small>{{ $t('process.accessMode.preAttachHint') }}</small>
      </div>
      <div class="access-mode-panel">
        <div class="access-mode-title">
          <strong>{{ $t('process.accessMode.title') }}</strong>
          <InfoDot topic="processKernelAccess" align="right" />
        </div>
        <div class="access-mode-options">
          <button
            class="mode-option"
            :class="{ active: store.memoryAccessMode === 'standard' }"
            type="button"
            @click="store.setMemoryAccessMode('standard')"
          >
            <strong>{{ $t('process.accessMode.standard') }}</strong>
            <span>{{ $t('process.accessMode.standardDescription') }}</span>
          </button>
          <button
            class="mode-option"
            :class="{ active: store.memoryAccessMode === 'kernel' }"
            type="button"
            @click="store.setMemoryAccessMode('kernel')"
          >
            <strong>{{ $t('process.accessMode.kernel') }}</strong>
            <span>{{ kernelStatusText }}</span>
          </button>
        </div>
        <p v-if="store.memoryAccessMode === 'kernel' && store.kernelMemoryReady" class="access-mode-hint ready">
          {{ $t('process.accessMode.kernelReadyHint') }}
        </p>
        <p v-else-if="store.memoryAccessMode === 'kernel'" class="access-mode-hint warning">
          {{ $t('process.accessMode.kernelWarningHint') }}
        </p>
        <div v-if="store.memoryAccessMode === 'kernel'" class="kernel-driver-actions">
          <button
            class="btn btn-secondary"
            type="button"
            :disabled="store.kernelDriverStatusLoading"
            :title="$t('process.kernelActions.testTitle')"
            @click="store.refreshKernelDriverStatus()"
          >
            {{ store.kernelDriverStatusLoading ? $t('process.kernelActions.testing') : $t('process.kernelActions.test') }}
          </button>
          <button
            class="btn btn-secondary"
            type="button"
            :disabled="store.kernelDriverStartLoading || store.kernelDriverStatusLoading || store.kernelMemoryReady"
            :title="$t('process.kernelActions.startTitle')"
            @click="startKernelDriverFromAttach()"
          >
            {{ store.kernelDriverStartLoading ? $t('process.kernelActions.loading') : (store.kernelMemoryReady ? $t('process.kernelActions.loaded') : $t('process.kernelActions.load')) }}
          </button>
        </div>
        <p v-if="store.memoryAccessMode === 'kernel' && store.kernelDriverStatusError" class="access-mode-hint warning">
          {{ store.kernelDriverStatusError }}
        </p>
      </div>
      <button class="btn btn-primary" @click="attach">
        {{ $t('process.attach') }}
      </button>
    </div>

    <div v-if="selectedPid !== null" class="module-panel">
      <div class="module-header">
        <h2>{{ $t('process.modules') }}</h2>
        <span>
          {{ moduleFilter || visibleModules.length < filteredModules.length
            ? $t('process.moduleCountFiltered', { shown: visibleModules.length, total: filteredModules.length })
            : store.processModules.length }}
        </span>
      </div>
      <input
        v-if="store.processModules.length > MODULE_PAGE_SIZE"
        v-model="moduleFilter"
        :placeholder="$t('process.moduleFilterPlaceholder')"
        class="search-input module-filter-input"
      />
      <div class="module-list">
        <div v-for="module in visibleModules" :key="`${module.baseAddress}-${module.name}`" class="module-item">
          <div class="module-name">{{ module.name }}</div>
          <div class="module-path" :title="module.path">{{ module.path }}</div>
        </div>
        <div v-if="store.processModules.length === 0" class="empty compact">
          {{ $t('process.noModules') }}
        </div>
        <div v-else-if="filteredModules.length === 0" class="empty compact">
          {{ $t('process.noModulesMatch') }}
        </div>
        <button
          v-if="visibleModules.length < filteredModules.length"
          class="btn btn-secondary module-load-more"
          @click="moduleVisibleCount += MODULE_PAGE_SIZE"
        >
          {{ $t('process.showMoreModules', { count: Math.min(MODULE_PAGE_SIZE, filteredModules.length - visibleModules.length) }) }}
        </button>
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

.empty.error {
  color: var(--error);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
}

.attach-bar {
  display: grid;
  grid-template-columns: minmax(180px, 1fr) minmax(320px, 1.4fr) auto;
  align-items: center;
  gap: 14px;
  padding: 16px;
  background: var(--bg-tertiary);
  border: 1px solid var(--accent);
  border-radius: 8px;
  margin-top: 20px;
}

.attach-summary {
  display: grid;
  gap: 3px;
  min-width: 0;
}

.attach-summary span {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 600;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.attach-summary small,
.access-mode-hint {
  color: var(--text-dim);
  font-size: 12px;
}

.access-mode-panel {
  display: grid;
  gap: 8px;
}

.access-mode-title {
  display: flex;
  align-items: center;
  gap: 6px;
  color: var(--text-primary);
  font-size: 12px;
}

.access-mode-options {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 6px;
}

.mode-option {
  display: grid;
  gap: 2px;
  min-height: 54px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-dim);
  text-align: left;
  cursor: pointer;
}

.mode-option.active {
  border-color: var(--accent);
  background: rgba(122, 162, 247, 0.1);
}

.mode-option strong {
  color: var(--text-primary);
  font-size: 12px;
}

.mode-option span {
  font-size: 11px;
}

.access-mode-hint {
  margin: 0;
}

.access-mode-hint.ready {
  color: var(--success);
}

.access-mode-hint.warning {
  color: var(--warning);
}

.kernel-driver-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.kernel-driver-actions .btn {
  padding: 7px 10px;
  font-size: 12px;
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

.module-filter-input {
  width: 100%;
  box-sizing: border-box;
  margin-bottom: 8px;
}

.module-list {
  display: flex;
  max-height: 220px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.module-load-more {
  align-self: center;
  margin: 4px 0;
  font-size: 12px;
  padding: 6px 12px;
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

@media (max-width: 900px) {
  .attach-bar,
  .access-mode-options {
    grid-template-columns: 1fr;
  }
}
</style>
