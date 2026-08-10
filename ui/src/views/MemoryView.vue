<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const filter = ref<'all' | 'committed' | 'readable' | 'writable' | 'executable'>('all')

const regions = computed(() => store.memoryMap?.regions ?? [])
const stats = computed(() => store.memoryMap?.stats)

const filteredRegions = computed(() => {
  return regions.value.filter((region) => {
    if (filter.value === 'committed') return region.state === 'committed'
    if (filter.value === 'readable') return region.readable
    if (filter.value === 'writable') return region.writable
    if (filter.value === 'executable') return region.executable
    return true
  })
})

function previewRegion(address: string) {
  void store.readMemoryPreview(address, 64)
}

function openRegionInExpert(address: string) {
  store.openExpertAtAddress(address)
}

function formatBytes(bytes: number | undefined) {
  if (!bytes) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024
    unit += 1
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`
}

onMounted(() => {
  if (store.isAttached) {
    store.refreshMemoryMap()
  }
})
</script>

<template>
  <div class="memory-view">
    <div class="header">
      <div>
        <h1>{{ $t('memory.title') }}</h1>
        <p v-if="store.isAttached">{{ store.processName }}</p>
      </div>
      <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.refreshMemoryMap()">
        {{ $t('memory.refresh') }}
      </button>
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      {{ $t('memory.attachFirst') }}
    </div>

    <template v-else>
      <div class="stats-grid">
        <div class="stat">
          <span>{{ $t('memory.regions') }}</span>
          <strong>{{ stats?.regionCount ?? 0 }}</strong>
        </div>
        <div class="stat">
          <span>{{ $t('memory.committed') }}</span>
          <strong>{{ stats?.committedCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.committedBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.readable') }}</span>
          <strong>{{ stats?.readableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.readableBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.writable') }}</span>
          <strong>{{ stats?.writableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.writableBytes) }}</small>
        </div>
        <div class="stat">
          <span>{{ $t('memory.executable') }}</span>
          <strong>{{ stats?.executableCount ?? 0 }}</strong>
          <small>{{ formatBytes(stats?.executableBytes) }}</small>
        </div>
      </div>

      <div class="toolbar">
        <button class="chip" :class="{ active: filter === 'all' }" @click="filter = 'all'">
          {{ $t('memory.all') }}
        </button>
        <button class="chip" :class="{ active: filter === 'committed' }" @click="filter = 'committed'">
          {{ $t('memory.committed') }}
        </button>
        <button class="chip" :class="{ active: filter === 'readable' }" @click="filter = 'readable'">
          {{ $t('memory.readable') }}
        </button>
        <button class="chip" :class="{ active: filter === 'writable' }" @click="filter = 'writable'">
          {{ $t('memory.writable') }}
        </button>
        <button class="chip" :class="{ active: filter === 'executable' }" @click="filter = 'executable'">
          {{ $t('memory.executable') }}
        </button>
      </div>

      <div
        v-if="store.memoryPreview || store.memoryPreviewLoading"
        class="preview-panel"
        :class="{ loading: store.memoryPreviewLoading, error: store.memoryPreview && !store.memoryPreview.success }"
      >
        <div class="preview-title">
          <div>
            <strong>{{ $t('memory.preview') }}</strong>
            <code v-if="store.memoryPreviewAddress">0x{{ store.memoryPreviewAddress }}</code>
          </div>
          <div class="preview-actions">
            <span v-if="store.memoryPreview">
              {{ store.memoryPreview.bytesRead }}/{{ store.memoryPreview.requestedBytes }} B
            </span>
            <button
              v-if="store.memoryPreviewAddress"
              class="preview-action-btn"
              type="button"
              @click="openRegionInExpert(store.memoryPreviewAddress)"
            >
              Basculer en expert
            </button>
          </div>
        </div>
        <div v-if="store.memoryPreviewLoading" class="preview-loading">
          Lecture de la mémoire...
        </div>
        <pre v-else-if="store.memoryPreview?.hex">{{ store.memoryPreview.hex }}</pre>
        <p v-else-if="store.memoryPreview?.error">{{ store.memoryPreview.error }}</p>
        <p v-else>Aucune donnée lisible à cette adresse.</p>
      </div>

      <div class="region-list">
        <div v-for="region in filteredRegions.slice(0, 250)" :key="`${region.baseAddress}-${region.size}`" class="region-row">
          <div class="address">0x{{ region.baseAddress }}</div>
          <div class="size">{{ formatBytes(region.size) }}</div>
          <div class="protection">{{ region.protection }}</div>
          <div class="state">{{ region.state }}</div>
          <div class="type">{{ region.type }}</div>
          <div class="region-actions">
            <button
              class="preview-btn"
              :class="{ active: store.memoryPreviewAddress === region.baseAddress }"
              :disabled="!region.readable || (store.memoryPreviewLoading && store.memoryPreviewAddress === region.baseAddress)"
              @click="previewRegion(region.baseAddress)"
            >
              {{ store.memoryPreviewLoading && store.memoryPreviewAddress === region.baseAddress ? 'Lecture...' : $t('memory.preview') }}
            </button>
            <button
              class="preview-btn expert-btn"
              :disabled="!region.readable"
              @click="openRegionInExpert(region.baseAddress)"
            >
              Expert
            </button>
          </div>
        </div>
      </div>
    </template>
  </div>
</template>

<style scoped>
.memory-view {
  max-width: 1100px;
  padding: 24px 32px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 20px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 13px;
}

.empty-state {
  padding: 32px;
  color: var(--text-dim);
  text-align: center;
}

.stats-grid {
  display: grid;
  grid-template-columns: repeat(5, minmax(120px, 1fr));
  gap: 8px;
  margin-bottom: 16px;
}

.stat {
  min-height: 82px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.stat span,
.stat small {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.stat strong {
  display: block;
  margin: 6px 0 2px;
  color: var(--text-primary);
  font-size: 22px;
}

.toolbar {
  display: flex;
  gap: 6px;
  margin-bottom: 10px;
}

.chip,
.btn {
  border: none;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.15s;
}

.chip {
  padding: 7px 10px;
  background: var(--bg-tertiary);
  color: var(--text-dim);
  font-size: 12px;
}

.chip.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.btn {
  padding: 8px 16px;
  font-size: 13px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.region-list {
  display: flex;
  max-height: 510px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.region-row {
  display: grid;
  grid-template-columns: 150px 90px 120px 90px 90px 150px;
  gap: 12px;
  align-items: center;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-dim);
  font-size: 12px;
}

.address,
.protection {
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
}

.preview-btn {
  padding: 5px 8px;
  border: none;
  border-radius: 6px;
  background: var(--bg-accent);
  color: var(--text-secondary);
  cursor: pointer;
  font-size: 12px;
}

.region-actions {
  display: flex;
  gap: 6px;
}

.expert-btn {
  color: var(--accent);
}

.preview-btn:disabled {
  cursor: not-allowed;
  opacity: 0.35;
}

.preview-btn.active {
  color: var(--accent);
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--accent) 45%, transparent);
}

.preview-panel {
  margin: 0 0 12px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.preview-panel.loading {
  border-color: color-mix(in srgb, var(--accent) 35%, var(--border));
}

.preview-panel.error {
  border-color: color-mix(in srgb, #ef4444 35%, var(--border));
}

.preview-title {
  display: flex;
  gap: 12px;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
}

.preview-title div,
.preview-actions {
  display: flex;
  min-width: 0;
  gap: 10px;
  align-items: center;
}

.preview-actions {
  flex-shrink: 0;
}

.preview-action-btn {
  padding: 5px 9px;
  border: none;
  border-radius: 6px;
  background: var(--bg-accent);
  color: var(--accent);
  cursor: pointer;
  font-size: 12px;
}

.preview-title strong {
  color: var(--text-primary);
}

.preview-title code {
  color: var(--accent);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.preview-title span,
.preview-panel p {
  color: var(--text-dim);
  font-size: 12px;
}

.preview-loading {
  color: var(--text-secondary);
  font-size: 13px;
}

.preview-panel pre {
  max-height: 160px;
  overflow: auto;
  overflow-wrap: anywhere;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  white-space: pre-wrap;
}

@media (max-width: 900px) {
  .stats-grid {
    grid-template-columns: repeat(2, minmax(120px, 1fr));
  }

  .region-row {
    grid-template-columns: 1fr;
    gap: 4px;
  }
}
</style>
