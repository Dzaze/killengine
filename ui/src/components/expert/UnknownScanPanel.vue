<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatBytes, formatNumber } from '@/utils/format'
import { valueTypeOptions } from '@/utils/valueTypes'

const store = useAppStore()
const { t } = useI18n()

const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
const unknownGuideReady = computed(() => Boolean(store.unknownSnapshotResult?.success) || hasCandidateContext.value)

const unknownGuideActions = [
  { mode: 'increased', labelKey: 'unknown.increased' },
  { mode: 'decreased', labelKey: 'unknown.decreased' },
  { mode: 'unchanged', labelKey: 'unknown.stable' },
  { mode: 'changed', labelKey: 'unknown.changed' },
] as const
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096]
const unknownDepthLabel = (mb: number) => (mb === -1 ? t('unknown.auto') : t('unknown.megabytes', { mb }))
</script>

<template>
  <section class="panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('unknown.title') }}</h2>
        <InfoDot topic="unknown" />
        <RiskBadge level="read" />
      </div>
    </div>
    <p class="panel-hint">{{ $t('help.unknown.when') }}</p>
    <div class="controls unknown-controls">
      <select v-model="store.unknownScanType" class="input select" :disabled="store.scanBusy">
        <option value="Auto">{{ $t('unknown.autoMultiType') }}</option>
        <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
      </select>
      <label class="checkbox-label compact-toggle">
        <input v-model="store.unknownWritableOnly" type="checkbox" :disabled="store.scanBusy" />
        <span>{{ $t('unknown.writableOnly') }}</span>
      </label>
      <label class="checkbox-label compact-toggle">
        <input v-model="store.unknownCopyOnWriteOnly" type="checkbox" :disabled="store.scanBusy || !store.unknownWritableOnly" />
        <span>{{ $t('unknown.copyOnWrite') }}</span>
      </label>
      <label class="compact-select">
        <span>{{ $t('unknown.depth') }}</span>
        <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select" :disabled="store.scanBusy">
          <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
        </select>
      </label>
      <button class="btn btn-primary" :disabled="store.scanBusy" @click="store.captureUnknownSnapshot()">
        <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ store.scanBusy ? $t('unknown.capturing') : $t('unknown.capture') }}</span>
      </button>
    </div>
    <p class="hint unknown-guide-warning">
      {{ $t('unknown.warningBeforeClick') }}
    </p>
    <div class="unknown-guide">
      <button
        v-for="action in unknownGuideActions"
        :key="action.mode"
        class="btn btn-secondary compact guide-btn"
        type="button"
        :class="{ active: store.unknownScanMode === action.mode }"
        :disabled="store.scanBusy || !unknownGuideReady"
        @click="store.runUnknownGuideStep(action.mode)"
      >
        {{ $t(action.labelKey) }}
      </button>
    </div>
    <div v-if="store.unknownSnapshotResult || store.unknownNextScanResult" class="metrics">
      <span v-if="store.unknownSnapshotResult">{{ $t('unknown.regions') }}: {{ formatNumber(store.unknownSnapshotResult.regionsCaptured) }}</span>
      <span v-if="store.unknownSnapshotResult">{{ $t('unknown.bytes') }}: {{ formatNumber(store.unknownSnapshotResult.bytesCaptured) }}</span>
      <span v-if="store.unknownSnapshotResult?.captureLimitBytes">{{ $t('unknown.limit', { bytes: formatBytes(store.unknownSnapshotResult.captureLimitBytes) }) }}</span>
      <span v-if="store.unknownSnapshotResult?.captureLimitReached" class="warning-text">{{ $t('unknown.limitReached') }}</span>
      <span v-if="store.unknownSnapshotResult?.compressedBytes !== undefined">{{ $t('unknown.compressed', { bytes: formatBytes(store.unknownSnapshotResult.compressedBytes) }) }}</span>
      <span v-if="store.unknownSnapshotResult?.mappedStorage">{{ $t('unknown.tempFileStorage') }}</span>
      <span v-if="store.unknownSnapshotResult?.writableOnly">{{ $t('unknown.writableOnly') }}</span>
      <span v-if="store.unknownSnapshotResult?.copyOnWriteOnly">{{ $t('unknown.copyOnWrite') }}</span>
      <span v-if="store.unknownNextScanResult">{{ $t('scan.matches') }}: {{ formatNumber(store.unknownNextScanResult.matchesFound) }}</span>
      <span v-if="store.unknownNextScanResult">{{ $t('scan.stored') }}: {{ formatNumber(store.unknownNextScanResult.stored) }}</span>
    </div>
    <div v-if="store.unknownNextScanResult?.typePasses?.length" class="metrics">
      <span v-for="pass in store.unknownNextScanResult.typePasses" :key="pass.type">
        {{ pass.type }}: {{ formatNumber(pass.stored ?? pass.matchesFound ?? 0) }}
      </span>
    </div>
    <div v-if="store.unknownSnapshotResult?.captureLimitReached" class="warning depth-warning">
      <p>
        <strong>{{ $t('unknown.limitedCaptureTitle') }}</strong>{{ $t('unknown.limitedCaptureBody', { captured: formatBytes(store.unknownSnapshotResult.bytesCaptured), limit: formatBytes(store.unknownSnapshotResult.captureLimitBytes) }) }}
      </p>
      <p v-if="(store.unknownSnapshotResult.relevantBytes ?? 0) > (store.unknownSnapshotResult.bytesCaptured ?? 0)">
        {{ $t('unknown.relevantMemoryCoverage', { total: formatBytes(store.unknownSnapshotResult.relevantBytes), covered: (((store.unknownSnapshotResult.bytesCaptured ?? 0) / (store.unknownSnapshotResult.relevantBytes ?? 1)) * 100).toFixed(1), missing: (100 - (((store.unknownSnapshotResult.bytesCaptured ?? 0) / (store.unknownSnapshotResult.relevantBytes ?? 1)) * 100)).toFixed(0) }) }}
      </p>
      <p v-if="(store.unknownSnapshotResult.suggestedDepthMb ?? 0) > 0">
        <strong>{{ $t('unknown.recommendationTitle') }}</strong>{{ $t('unknown.recommendationBody', { depth: store.unknownSnapshotResult.suggestedDepthMb }) }}
      </p>
    </div>
    <div v-else-if="store.unknownSnapshotResult?.autoDepthApplied && (store.unknownSnapshotResult.suggestedDepthMb ?? 0) > 0" class="hint depth-info">
      {{ $t('unknown.autoDepthApplied', { depth: store.unknownSnapshotResult.suggestedDepthMb, bytes: formatBytes(store.unknownSnapshotResult.relevantBytes) }) }}
    </div>
    <div v-if="store.unknownGuideSteps.length > 0" class="unknown-timeline">
      <div
        v-for="step in store.unknownGuideSteps"
        :key="step.id"
        class="unknown-step"
        :class="step.status"
      >
        <span>{{ step.time }}</span>
        <strong>{{ step.label }}</strong>
        <em>{{ step.detail }}</em>
      </div>
    </div>
    <p class="hint">
      {{ $t('unknown.footerHint') }}
    </p>
  </section>
</template>

<style scoped>
.unknown-controls {
  grid-template-columns: 120px minmax(110px, auto) minmax(120px, auto) minmax(150px, auto) auto;
  align-items: center;
}

.compact-toggle {
  min-height: 32px;
}

.compact-select {
  display: grid;
  grid-template-columns: auto minmax(90px, 1fr);
  gap: 6px;
  align-items: center;
  color: var(--text-muted);
  font-size: 12px;
}

.warning-text {
  color: var(--warning);
}

.unknown-guide {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.guide-btn.active {
  background: rgba(122, 162, 247, 0.18);
  color: var(--accent);
}

.unknown-timeline {
  display: grid;
  gap: 6px;
  margin-top: 9px;
}

.unknown-step {
  display: grid;
  grid-template-columns: 70px 96px 1fr;
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-muted);
  font-size: 12px;
}

.unknown-step strong {
  color: var(--text-primary);
}

.unknown-step em {
  overflow: hidden;
  color: var(--text-secondary);
  font-style: normal;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.unknown-step.capture {
  border-color: rgba(122, 162, 247, 0.42);
}

.unknown-step.compare,
.unknown-step.refine {
  border-color: rgba(158, 206, 106, 0.38);
}

.unknown-step.error {
  border-color: rgba(247, 118, 142, 0.45);
}
</style>
