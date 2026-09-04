<script setup lang="ts">
import { onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'

const { t } = useI18n()
const store = useAppStore()

const statusLabel = (status: string) => {
  if (status === 'ok') return t('modules.status.ok')
  if (status === 'missing') return t('modules.status.missing')
  return status
}

const statusClass = (status: string) => {
  if (status === 'ok') return 'ok'
  if (status === 'missing') return 'missing'
  return 'unknown'
}

const installLabel = (moduleId: string) => {
  switch (moduleId) {
    case 'lua_runtime': return t('modules.install.lua')
    case 'ai_model': return t('modules.install.model')
    case 'clr_inspector': return t('modules.install.clr')
    case 'kernel_driver': return t('modules.install.kernel')
    default: return t('modules.install.generic')
  }
}

const isInstallTarget = (moduleId: string) => {
  return store.moduleInstallBusy && store.moduleInstallModuleId === moduleId
}

onMounted(() => {
  void store.refreshModuleCatalog()
})
</script>

<template>
  <div class="modules-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.modules') }}</h1>
        <p>{{ $t('modules.intro') }}</p>
      </div>
      <button
        class="btn-secondary refresh-btn"
        :disabled="store.moduleCatalogBusy || store.moduleInstallBusy"
        @click="store.refreshModuleCatalog()"
      >
        {{ store.moduleCatalogBusy ? $t('modules.refreshing') : $t('modules.refresh') }}
      </button>
    </div>

    <div v-if="store.moduleCatalog.length === 0 && !store.moduleCatalogBusy" class="empty">
      {{ $t('modules.empty') }}
    </div>

    <div v-for="mod in store.moduleCatalog" :key="mod.id" class="module-card" :class="{ busy: isInstallTarget(mod.id) }">
      <div class="module-head">
        <div class="module-title">
          <span class="module-name">{{ mod.displayName }}</span>
          <span class="module-status" :class="statusClass(mod.status)">{{ statusLabel(mod.status) }}</span>
        </div>
        <button
          v-if="mod.installable && !store.moduleInstallBusy"
          class="btn-primary install-btn"
          @click="store.installModule(mod.id)"
        >
          {{ installLabel(mod.id) }}
        </button>
      </div>
      <p class="module-desc">{{ mod.description }}</p>
      <p v-if="mod.detail" class="module-detail">{{ mod.detail }}</p>
      <p v-if="mod.path" class="module-path">{{ mod.path }}</p>

      <div v-if="isInstallTarget(mod.id)" class="install-progress">
        <div class="progress-bar">
          <div class="progress-fill" />
        </div>
        <p class="progress-text">{{ store.moduleInstallProgress || $t('modules.progress') }}</p>
        <button class="btn-secondary cancel-btn" @click="store.cancelModuleInstall()">
          {{ $t('modules.cancel') }}
        </button>
      </div>
    </div>

    <div
      v-if="store.moduleInstallResult && !store.moduleInstallBusy"
      class="install-result"
      :class="store.moduleInstallResult.success ? 'success' : 'error'"
    >
      <p>{{ String(store.moduleInstallResult.message ?? store.moduleInstallResult.error ?? '') }}</p>
      <button class="btn-secondary" @click="store.refreshModuleCatalog()">
        {{ $t('modules.refresh') }}
      </button>
    </div>
  </div>
</template>

<style scoped>
.modules-view {
  padding: 24px 32px;
  max-width: 860px;
}

.header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 16px;
  margin-bottom: 20px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
  margin: 0 0 6px;
}

.header p {
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
  max-width: 640px;
}

.refresh-btn {
  flex-shrink: 0;
}

.empty {
  padding: 24px;
  border: 1px dashed var(--border);
  border-radius: 6px;
  color: var(--text-dim);
  text-align: center;
}

.module-card {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  padding: 14px 16px;
  margin-bottom: 12px;
}

.module-card.busy {
  border-color: color-mix(in srgb, var(--accent) 50%, var(--border));
}

.module-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.module-title {
  display: flex;
  align-items: center;
  gap: 10px;
}

.module-name {
  font-weight: 600;
  color: var(--text-primary);
}

.module-status {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 8px;
  border-radius: 10px;
  border: 1px solid var(--border);
}

.module-status.ok {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.module-status.missing {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.module-desc {
  margin: 10px 0 0;
  font-size: 13px;
  color: var(--text-secondary);
}

.module-detail {
  margin: 8px 0 0;
  font-size: 12px;
  color: var(--text-dim);
}

.module-path {
  margin: 6px 0 0;
  font-size: 11px;
  color: var(--text-dim);
  font-family: 'Consolas', monospace;
  word-break: break-all;
}

.install-progress {
  margin-top: 12px;
}

.progress-bar {
  height: 6px;
  border-radius: 3px;
  background: var(--bg-secondary);
  overflow: hidden;
}

.progress-fill {
  height: 100%;
  width: 40%;
  background: var(--accent);
  animation: module-progress 1.4s ease-in-out infinite alternate;
}

@keyframes module-progress {
  from { width: 15%; }
  to { width: 85%; }
}

.progress-text {
  margin: 8px 0;
  font-size: 12px;
  color: var(--text-secondary);
  font-family: 'Consolas', monospace;
  word-break: break-all;
}

.install-result {
  margin-top: 16px;
  padding: 12px 14px;
  border-radius: 6px;
  border: 1px solid var(--border);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.install-result p {
  margin: 0;
  font-size: 13px;
  word-break: break-word;
}

.install-result.success {
  border-color: color-mix(in srgb, var(--success) 40%, var(--border));
  color: var(--success);
}

.install-result.error {
  border-color: color-mix(in srgb, var(--error) 40%, var(--border));
  color: var(--error);
}
</style>
