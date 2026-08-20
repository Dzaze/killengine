<script setup lang="ts">
import { computed } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatNumber, formatRate } from '@/utils/format'

const store = useAppStore()

const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
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
        title="Restaure les candidats d'avant la dernière réduction (utile même si la réduction est tombée à 0)."
        @click="store.undoCandidateScan()"
      >
        Restaurer réduction
      </button>
    </div>
    <p class="hint">{{ $t('scan.nextScanHint') }}</p>
    <p v-if="!hasCandidateContext" class="hint">Lance d'abord une recherche (bouton « Chercher ») pour avoir des candidats à réduire ici.</p>
    <div class="controls next-controls">
      <select
        v-model="store.nextScanMode"
        class="input select"
        :disabled="store.scanBusy || !hasCandidateContext"
        :title="!hasCandidateContext ? 'Lance d\'abord une recherche pour avoir des candidats.' : ''"
      >
        <option value="exact">{{ $t('scan.modeExact') }}</option>
        <option value="changed">{{ $t('scan.modeChanged') }}</option>
        <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
        <option value="increased">{{ $t('scan.modeIncreased') }}</option>
        <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
        <option value="delta">{{ $t('scan.modeDelta') }}</option>
      </select>
      <input
        v-model="store.nextScanValue"
        :disabled="store.scanBusy || !hasCandidateContext || (store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta')"
        :placeholder="$t('scan.nextValue')"
        :title="!hasCandidateContext ? 'Lance d\'abord une recherche pour avoir des candidats.' : ''"
        class="input"
        @keyup.enter="store.doNextScan()"
      />
      <button
        class="btn btn-primary"
        :disabled="store.scanBusy || !hasCandidateContext"
        :title="!hasCandidateContext ? 'Lance d\'abord une recherche pour avoir des candidats à réduire.' : ''"
        @click="store.doNextScan()"
      >
        <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ store.scanBusy ? 'Scan...' : $t('scan.nextScan') }}</span>
      </button>
    </div>
    <div v-if="store.nextScanResult" class="metrics">
      <span>{{ $t('scan.remaining') }}: {{ formatNumber(store.nextScanResult.remaining) }}</span>
      <span>{{ $t('scan.checked') }}: {{ formatNumber(store.nextScanResult.checked) }}</span>
      <span>{{ $t('scan.unreadable') }}: {{ formatNumber(store.nextScanResult.unreadable) }}</span>
      <span v-if="store.nextScanResult.elapsedMs">Temps: {{ formatNumber(store.nextScanResult.elapsedMs) }} ms</span>
      <span v-if="store.nextScanResult.candidatesPerSecond">Débit: {{ formatRate(store.nextScanResult.candidatesPerSecond) }}</span>
    </div>
    <p v-if="store.nextScanResult?.error" class="error">{{ store.nextScanResult.error }}</p>
  </section>
</template>
