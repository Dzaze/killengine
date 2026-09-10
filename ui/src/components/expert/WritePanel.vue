<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from 'vue'
import { backend, type StableLocatorSuggestion } from '@/services/backend'
import { useI18n } from 'vue-i18n'
import { useExpertPointerChain } from '@/composables/useExpertPointerChain'
import { useExpertWriteSelection } from '@/composables/useExpertWriteSelection'
import { useAppStore } from '@/stores/app'
import { formatNumber } from '@/utils/format'
import { valueTypeOptions } from '@/utils/valueTypes'
import InfoDot from './InfoDot.vue'
import RiskBadge from './RiskBadge.vue'

const store = useAppStore()
const { t } = useI18n()
const {
  selectedCandidateAddresses,
  writePanelRef,
  writePlan,
  writeFailures,
  hasSelectedWriteTargets,
  writeTargetLabel,
  canWriteFromPanel,
  writeButtonLabel,
  clearCandidateSelection,
  writeFromPanel,
} = useExpertWriteSelection()
const { pointerScanValueType, savePointerChain } = useExpertPointerChain()

const freezeIntervalPresets = [16, 33, 50, 100, 250, 500]
const stableLocatorResult = ref<StableLocatorSuggestion | null>(null)
const stableLocatorBusy = ref(false)
const stableLocatorForAddress = ref('')
const breakpointFreezeStats = ref<Record<string, unknown> | null>(null)
let breakpointFreezeStatsTimer: ReturnType<typeof setInterval> | null = null

function stopBreakpointFreezeStatsPolling() {
  if (breakpointFreezeStatsTimer !== null) {
    clearInterval(breakpointFreezeStatsTimer)
    breakpointFreezeStatsTimer = null
  }
}

async function pollBreakpointFreezeStats() {
  const controller = backend.getController()
  if (!controller.getBreakpointFreezeStats) return
  breakpointFreezeStats.value = await controller.getBreakpointFreezeStats()
}

watch(() => store.breakpointFreezeEnabled, (enabled) => {
  stopBreakpointFreezeStatsPolling()
  if (enabled) {
    void pollBreakpointFreezeStats()
    breakpointFreezeStatsTimer = setInterval(() => { void pollBreakpointFreezeStats() }, 1000)
  } else {
    breakpointFreezeStats.value = null
  }
})

onBeforeUnmount(() => {
  stopBreakpointFreezeStatsPolling()
})

async function suggestStableLocator(addressHex: string) {
  if (!addressHex.trim()) return
  stableLocatorBusy.value = true
  stableLocatorResult.value = null
  stableLocatorForAddress.value = addressHex
  try {
    const controller = backend.getController()
    if (controller.suggestStableLocatorForAddress) {
      stableLocatorResult.value = await controller.suggestStableLocatorForAddress(addressHex, {})
    } else {
      stableLocatorResult.value = { success: false, chainCount: 0, error: t('write.stableLocator.backendUnavailable') }
    }
  } catch (e) {
    stableLocatorResult.value = { success: false, chainCount: 0, error: String(e) }
  } finally {
    stableLocatorBusy.value = false
  }
}

function saveStableLocator() {
  if (stableLocatorResult.value?.bestChain) {
    pointerScanValueType.value = store.exactScanType
    void savePointerChain(stableLocatorResult.value.bestChain)
  }
}
</script>

<template>
  <section ref="writePanelRef" class="panel risk-write">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('write.title') }}</h2>
        <InfoDot topic="write" />
        <RiskBadge level="write" />
      </div>
      <div class="panel-title-actions">
        <span v-if="store.writeResult">{{ store.writeResult.success ? $t('write.statusOk') : $t('write.statusFail') }}</span>
      </div>
    </div>
    <p class="panel-hint">{{ $t('help.write.when') }}</p>
    <div class="controls write-controls">
      <div v-if="hasSelectedWriteTargets" class="input multi-target-summary" :title="selectedCandidateAddresses.map((address) => `0x${address}`).join(', ')">
        <strong>{{ writeTargetLabel }}</strong>
        <button class="inline-clear" type="button" @click="clearCandidateSelection()">{{ $t('write.manual') }}</button>
      </div>
      <input
        v-else
        v-model="store.selectedCandidateAddress"
        class="input"
        :placeholder="$t('write.address')"
        @input="store.updateWriteSafetyWarning()"
        @blur="store.updateWriteSafetyWarning()"
      />
      <select v-model="store.exactScanType" class="input select">
        <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
      </select>
      <input v-model="store.writeValue" class="input" :placeholder="$t('write.value')" @keyup.enter="writeFromPanel()" />
      <button class="btn btn-primary" :disabled="!canWriteFromPanel" @click="writeFromPanel()">
        {{ writeButtonLabel }}
      </button>
      <button class="btn btn-secondary" @click="store.rollbackLastWrite()">
        {{ $t('write.rollback') }}
      </button>
      <label class="freeze-interval-control">
        <span>{{ $t('write.freezeInterval') }}</span>
        <select
          v-model.number="store.freezeIntervalMs"
          class="input select"
          @change="store.setFreezeInterval(store.freezeIntervalMs)"
        >
          <option v-for="ms in freezeIntervalPresets" :key="ms" :value="ms">{{ ms }} ms</option>
        </select>
      </label>
      <button class="btn" :class="store.freezeEnabled ? 'btn-secondary' : 'btn-primary'" :disabled="hasSelectedWriteTargets || (store.freezeEnabled ? !store.selectedCandidateAddress : !store.canWriteSelectedValue)" @click="store.toggleFreeze()">
        {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
      </button>
      <button
        class="btn btn-secondary"
        :disabled="hasSelectedWriteTargets || store.breakpointFreezeEnabled || !store.canWriteSelectedValue"
        :title="$t('write.freezeBpTitle')"
        @click="store.startBreakpointFreeze()"
      >
        {{ $t('write.freezeBp') }}
      </button>
      <InfoDot topic="freezeBp" align="right" />
      <button
        class="btn btn-secondary"
        :disabled="!store.breakpointFreezeEnabled"
        :title="$t('write.stopBpTitle')"
        @click="store.stopBreakpointFreeze()"
      >
        {{ $t('write.stopBp') }}
      </button>
      <span v-if="store.breakpointFreezeEnabled && breakpointFreezeStats" class="bp-live-stats" :class="{ warning: breakpointFreezeStats.healthy === false }">
        {{ $t('write.bpStats', { hits: formatNumber(Number(breakpointFreezeStats.hits ?? 0)), rewrites: formatNumber(Number(breakpointFreezeStats.rewrites ?? 0)) }) }}<template v-if="Number(breakpointFreezeStats.errors ?? 0) > 0"> · {{ $t('write.bpErrors', { count: formatNumber(Number(breakpointFreezeStats.errors ?? 0)) }) }}</template>
      </span>
    </div>
    <div v-if="hasSelectedWriteTargets" class="write-plan">
      <div class="write-plan-title">
        <strong>{{ $t('write.planTitle') }}</strong>
        <span>{{ $t('write.planSummary', { count: writePlan.length, value: store.writeValue.trim() || '-' }) }}</span>
      </div>
      <div class="write-plan-list">
        <div v-for="target in writePlan.slice(0, 12)" :key="`${target.address}:${target.mode}`" class="write-plan-row">
          <code>0x{{ target.address }}</code>
          <span>{{ target.mode }}</span>
          <strong>{{ target.encodedValue }}</strong>
        </div>
      </div>
      <span v-if="writePlan.length > 12" class="muted">{{ $t('write.moreTargets', { count: writePlan.length - 12 }) }}</span>
    </div>
    <div v-if="store.writeSafetyWarning" class="write-safety">
      <p class="warning">{{ store.writeSafetyWarning }}</p>
      <label class="safety-ack">
        <input v-model="store.writeSafetyAcknowledged" type="checkbox" />
        {{ $t('write.confirmMemoryWrite') }}
      </label>
    </div>
    <div v-if="store.writeResult || store.freezeIntervalResult" class="metrics">
      <span v-if="store.writeResult">{{ store.writeResult.bytesWritten }} B</span>
      <span v-if="store.writeResult?.written !== undefined">{{ $t('write.writtenCount', { written: formatNumber(store.writeResult.written), total: formatNumber(store.writeResult.total) }) }}</span>
      <span v-if="store.writeResult?.protectionChanged">VirtualProtectEx{{ store.writeResult.protectionChangedCount ? `: ${formatNumber(store.writeResult.protectionChangedCount)}` : '' }}</span>
      <span v-if="store.writeResult?.verified">{{ $t('write.verified') }}</span>
      <span v-if="store.writeResult?.enabled !== undefined">{{ $t('write.freezeStatus', { status: store.writeResult.enabled ? $t('write.on') : $t('write.off') }) }}</span>
      <span v-if="store.writeResult?.mode === 'breakpoint'">{{ $t('write.bpStatus', { status: store.breakpointFreezeEnabled ? $t('write.on') : $t('write.off') }) }}</span>
      <span v-if="store.writeResult?.rewrites !== undefined">{{ $t('write.rewrites', { count: formatNumber(store.writeResult.rewrites) }) }}</span>
      <span v-if="store.freezeIntervalResult">{{ $t('write.intervalMs', { ms: store.freezeIntervalMs }) }}</span>
      <span v-if="store.writeResult?.suspendedThreadCount !== undefined" :title="$t('write.suspendedThreadsTitle')">
        {{ $t('write.suspendedThreads', { count: formatNumber(store.writeResult.suspendedThreadCount) }) }}
      </span>
    </div>
    <p v-if="store.writeResult?.error" class="error">{{ store.writeResult.error }}</p>
    <p v-if="store.writeResult?.warning" class="hint warning-hint">{{ store.writeResult.warning }}</p>
    <div
      v-if="store.writeResult?.success && !hasSelectedWriteTargets && store.selectedCandidateAddress"
      class="stable-locator"
    >
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="stableLocatorBusy"
        @click="suggestStableLocator(store.selectedCandidateAddress)"
      >
        <span v-if="stableLocatorBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ stableLocatorBusy ? $t('write.stableLocator.searching') : $t('write.stableLocator.button') }}</span>
      </button>
      <InfoDot :text="$t('write.stableLocator.info')" />
      <span v-if="stableLocatorResult && stableLocatorForAddress === store.selectedCandidateAddress" class="stable-locator-result">
        <template v-if="stableLocatorResult.success && stableLocatorResult.bestChain">
          <span class="hint">{{ stableLocatorResult.message }}</span>
          <button class="btn btn-secondary compact" type="button" @click="saveStableLocator()">
            {{ $t('write.stableLocator.saveProfile') }}
          </button>
        </template>
        <template v-else-if="stableLocatorResult.success">
          <span class="hint">{{ stableLocatorResult.message }}</span>
        </template>
        <template v-else>
          <span class="hint">{{ stableLocatorResult.error || $t('write.stableLocator.unavailable') }}</span>
        </template>
      </span>
    </div>
    <div v-if="writeFailures.length" class="write-fail-list">
      <div class="source-list-title">
        <strong>{{ $t('write.failuresTitle') }}</strong>
        <span>{{ $t('write.failureCount', { count: formatNumber(writeFailures.length) }) }}</span>
      </div>
      <div v-for="failure in writeFailures.slice(0, 16)" :key="`${failure.address}:${failure.type}:${failure.variantLabel}`" class="write-fail-row">
        <code>0x{{ failure.address || '-' }}</code>
        <span>{{ failure.variantLabel || failure.type || '-' }}</span>
        <strong>{{ failure.encodedHex || '-' }}</strong>
        <span>{{ failure.error || '-' }}</span>
      </div>
    </div>
  </section>
</template>

<style scoped>
.write-controls {
  grid-template-columns: minmax(160px, 1fr) 120px minmax(120px, 1fr) auto auto minmax(120px, auto) auto auto auto;
  align-items: center;
}

.freeze-interval-control {
  display: grid;
  grid-template-columns: auto minmax(86px, 1fr);
  gap: 6px;
  align-items: center;
  color: var(--text-dim);
  font-size: 12px;
}

.multi-target-summary {
  display: flex;
  min-width: 0;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.multi-target-summary strong {
  overflow: hidden;
  color: var(--text-primary);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-plan {
  display: grid;
  gap: 6px;
  margin-top: 8px;
  padding: 8px;
  border: 1px solid rgba(158, 206, 106, 0.22);
  border-radius: 6px;
  background: rgba(158, 206, 106, 0.06);
}

.write-plan-title,
.write-plan-row {
  display: grid;
  grid-template-columns: minmax(180px, 1fr) minmax(120px, 180px) minmax(100px, 160px);
  gap: 8px;
  align-items: center;
}

.write-plan-title {
  color: var(--text-dim);
  font-size: 12px;
}

.write-plan-title strong {
  color: var(--text-primary);
}

.write-plan-list {
  display: grid;
  gap: 4px;
}

.write-plan-row {
  padding: 5px 6px;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.write-plan-row code {
  color: var(--text-primary);
}

.write-plan-row strong {
  color: var(--success);
}

.stable-locator {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-top: 8px;
}

.bp-live-stats {
  color: var(--success);
  font-size: 12px;
  white-space: nowrap;
}

.bp-live-stats.warning {
  color: var(--warning);
}

.stable-locator-result {
  display: inline-flex;
  gap: 8px;
  align-items: center;
}

.write-fail-list {
  display: grid;
  gap: 5px;
  margin-top: 8px;
}

.write-fail-row {
  display: grid;
  grid-template-columns: minmax(150px, 1fr) minmax(100px, 140px) minmax(130px, 180px) minmax(180px, 1fr);
  gap: 8px;
  align-items: center;
  padding: 6px 8px;
  border: 1px solid rgba(247, 118, 142, 0.2);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.write-fail-row code,
.write-fail-row span,
.write-fail-row strong {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-fail-row code {
  color: var(--text-primary);
}

.write-fail-row strong {
  color: var(--warning);
}

@media (max-width: 860px) {
  .write-plan-title,
  .write-plan-row,
  .write-controls {
    grid-template-columns: 1fr;
  }
}
</style>
