<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import { useExpertPointerChain } from '@/composables/useExpertPointerChain'
import { formatBytes, formatNumber } from '@/utils/format'
import { valueTypeOptions } from '@/utils/valueTypes'
import InfoDot from './InfoDot.vue'
import RiskBadge from './RiskBadge.vue'

const store = useAppStore()
const {
  pointerScanAddress,
  pointerScanValueType,
  pointerScanMaxDepth,
  pointerScanMaxOffset,
  pointerScanResult,
  pointerResolveResult,
  pointerScanBusy,
  selectedPointerChainIndex,
  runPointerScan,
  testPointerChain,
  usePointerChainAsCandidate,
  savePointerChain,
  watchPointerChain,
  bookmarkPointerChain,
} = useExpertPointerChain()
</script>

<template>
  <section class="panel pointer-chain-panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('pointerChainScan.title') }} <span class="hint-inline">{{ $t('pointerChainScan.subtitle') }}</span></h2>
        <InfoDot topic="pointerChains" />
        <RiskBadge level="read" />
      </div>
      <span v-if="pointerScanResult">{{ $t('pointerChainScan.chainCount', { count: formatNumber(pointerScanResult.chainCount) }) }}</span>
    </div>
    <p class="panel-hint">{{ $t('help.pointerChains.when') }}</p>
    <p class="hint">
      {{ $t('pointerChainScan.hint') }}
    </p>
    <div class="controls pointer-chain-controls">
      <input
        v-model="pointerScanAddress"
        class="input"
        :placeholder="$t('pointerChainScan.targetAddressPlaceholder')"
        :disabled="store.scanBusy || !store.isAttached"
      />
      <select v-model="pointerScanValueType" class="input select">
        <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
      </select>
      <input
        v-model.number="pointerScanMaxDepth"
        type="number"
        min="1"
        max="5"
        class="input"
        :placeholder="$t('pointerChainScan.depthPlaceholder')"
        :title="$t('pointerChainScan.depthTitle')"
      />
      <input
        v-model.number="pointerScanMaxOffset"
        type="number"
        min="0"
        step="16"
        class="input"
        :placeholder="$t('pointerChainScan.maxOffsetPlaceholder')"
        :title="$t('pointerChainScan.maxOffsetTitle')"
      />
      <button
        class="btn btn-primary"
        :disabled="!pointerScanAddress.trim() || store.scanBusy || !store.isAttached"
        @click="runPointerScan()"
      >
        <span v-if="pointerScanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ pointerScanBusy ? $t('pointerChainScan.scanning') : $t('pointerChainScan.scanPointers') }}</span>
      </button>
    </div>
    <div v-if="pointerScanResult" class="metrics">
      <span>{{ $t('pointerChainScan.chains', { count: formatNumber(pointerScanResult.chainCount) }) }}</span>
      <span>{{ $t('pointerChainScan.pointersScanned', { count: formatNumber(pointerScanResult.pointersScanned) }) }}</span>
      <span>{{ $t('pointerChainScan.bytes', { bytes: formatBytes(pointerScanResult.bytesScanned) }) }}</span>
      <span v-if="pointerScanResult.elapsedMs">{{ $t('pointerChainScan.elapsedMs', { ms: formatNumber(pointerScanResult.elapsedMs) }) }}</span>
      <span v-if="pointerScanResult.partial">{{ $t('pointerChainScan.partialResult') }}</span>
    </div>
    <div v-if="pointerScanResult?.chains?.length" class="pointer-chain-list">
      <div
        v-for="(chain, index) in pointerScanResult.chains.slice(0, 20)"
        :key="index"
        class="pointer-chain-row"
        :class="{ selected: selectedPointerChainIndex === index }"
      >
        <label class="candidate-check">
          <input
            type="radio"
            :value="index"
            v-model.number="selectedPointerChainIndex"
          />
        </label>
        <div class="pointer-chain-info">
          <strong>{{ chain.label }}</strong>
          <span class="chain-depth">{{ $t('pointerChainScan.depthValue', { depth: chain.depth }) }}</span>
        </div>
        <div class="pointer-chain-actions">
          <button class="btn btn-secondary compact" @click="testPointerChain(chain)">
            {{ $t('pointerChainScan.test') }}
          </button>
          <button
            class="btn btn-secondary compact"
            @click="usePointerChainAsCandidate(chain)"
          >
            {{ $t('pointerChainScan.use') }}
          </button>
          <button
            class="btn btn-primary compact"
            @click="savePointerChain(chain)"
          >
            {{ $t('pointerChainScan.saveProfile') }}
          </button>
          <button
            class="btn btn-secondary compact"
            @click="bookmarkPointerChain(chain)"
          >
            {{ $t('pointerChainScan.note') }}
          </button>
          <button
            class="btn btn-secondary compact"
            :title="$t('pointerChainScan.watchTitle')"
            @click="watchPointerChain(chain)"
          >
            {{ $t('pointerChainScan.watch') }}
          </button>
        </div>
      </div>
    </div>
    <p v-if="pointerScanResult?.error" class="error">{{ pointerScanResult.error }}</p>
    <p v-if="pointerResolveResult" class="hint">
      {{ pointerResolveResult.success ? $t('pointerChainScan.resolutionOk', { address: pointerResolveResult.finalAddress }) : $t('pointerChainScan.resolutionFail', { error: pointerResolveResult.error }) }}
    </p>
  </section>
</template>
