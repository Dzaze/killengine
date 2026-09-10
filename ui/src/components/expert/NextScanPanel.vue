<script setup lang="ts">
import { computed } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatNumber, formatRate } from '@/utils/format'

const store = useAppStore()

const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)

// Mode "between" : deux champs min/max composent la valeur "min,max" attendue
// par ApplicationController::nextScan côté backend.
const isBetweenMode = computed(() => store.nextScanMode === 'between')
const rangeMin = computed({
  get: () => store.nextScanValue.split(',')[0] ?? '',
  set: (min: string) => {
    const max = store.nextScanValue.split(',')[1] ?? ''
    store.nextScanValue = `${min},${max}`
  },
})
const rangeMax = computed({
  get: () => store.nextScanValue.split(',')[1] ?? '',
  set: (max: string) => {
    const min = store.nextScanValue.split(',')[0] ?? ''
    store.nextScanValue = `${min},${max}`
  },
})
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('scan.nextScan') }}</h2>
        <InfoDot topic="nextScan" />
        <RiskBadge level="read" />
      </div>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.scanBusy"
        :title="$t('nextScanPanel.restoreTitle')"
        @click="store.undoCandidateScan()"
      >
        {{ $t('nextScanPanel.restoreReduction') }}
      </button>
    </div>
    <p class="hint">{{ $t('scan.nextScanHint') }}</p>
    <p v-if="!hasCandidateContext" class="hint">{{ $t('nextScanPanel.noCandidateContext') }}</p>
    <div class="controls next-controls">
      <select
        v-model="store.nextScanMode"
        class="input select"
        :disabled="store.scanBusy || !hasCandidateContext"
        :title="!hasCandidateContext ? $t('nextScanPanel.needCandidatesTitle') : ''"
      >
        <option value="exact">{{ $t('scan.modeExact') }}</option>
        <option value="changed">{{ $t('scan.modeChanged') }}</option>
        <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
        <option value="increased">{{ $t('scan.modeIncreased') }}</option>
        <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
        <option value="delta">{{ $t('scan.modeDelta') }}</option>
        <option value="between">{{ $t('scan.modeBetween') }}</option>
      </select>
      <template v-if="isBetweenMode">
        <input
          v-model="rangeMin"
          :disabled="store.scanBusy || !hasCandidateContext"
          :placeholder="$t('scan.rangeMin')"
          class="input"
          @keyup.enter="store.doNextScan()"
        />
        <input
          v-model="rangeMax"
          :disabled="store.scanBusy || !hasCandidateContext"
          :placeholder="$t('scan.rangeMax')"
          class="input"
          @keyup.enter="store.doNextScan()"
        />
      </template>
      <input
        v-else
        v-model="store.nextScanValue"
        :disabled="store.scanBusy || !hasCandidateContext || (store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta')"
        :placeholder="$t('scan.nextValue')"
        :title="!hasCandidateContext ? $t('nextScanPanel.needCandidatesTitle') : ''"
        class="input"
        @keyup.enter="store.doNextScan()"
      />
      <button
        class="btn btn-primary"
        :disabled="store.scanBusy || !hasCandidateContext"
        :title="!hasCandidateContext ? $t('nextScanPanel.needCandidatesToReduceTitle') : ''"
        @click="store.doNextScan()"
      >
        <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ store.scanBusy ? $t('nextScanPanel.scanning') : $t('scan.nextScan') }}</span>
      </button>
    </div>
    <div v-if="store.nextScanResult" class="metrics">
      <span>{{ $t('scan.remaining') }}: {{ formatNumber(store.nextScanResult.remaining) }}</span>
      <span>{{ $t('scan.checked') }}: {{ formatNumber(store.nextScanResult.checked) }}</span>
      <span>{{ $t('scan.unreadable') }}: {{ formatNumber(store.nextScanResult.unreadable) }}</span>
      <span v-if="store.nextScanResult.elapsedMs">{{ $t('nextScanPanel.elapsedMs', { ms: formatNumber(store.nextScanResult.elapsedMs) }) }}</span>
      <span v-if="store.nextScanResult.candidatesPerSecond">{{ $t('nextScanPanel.throughput', { rate: formatRate(store.nextScanResult.candidatesPerSecond) }) }}</span>
    </div>
    <p v-if="store.nextScanResult?.error" class="error">{{ store.nextScanResult.error }}</p>
  </section>
</template>
