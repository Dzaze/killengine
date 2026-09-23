<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatBytes, formatNumber, formatRate } from '@/utils/format'
import { valueTypeOptions } from '@/utils/valueTypes'

const props = defineProps<{
  onStartNewScan: () => void | Promise<void>
}>()

const store = useAppStore()
const { t } = useI18n()

const hasCandidateContext = computed(() => (store.candidatePage?.totalCount ?? 0) > 0)
const exactScanButtonLabel = computed(() => hasCandidateContext.value ? t('exactScan.newScan') : t('exactScan.firstScan'))
</script>

<template>
  <section class="panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('scan.exact') }}</h2>
        <InfoDot topic="scanExact" />
        <RiskBadge level="read" />
      </div>
      <div class="panel-actions">
        <span v-if="store.exactScanResult?.partial">{{ $t('scan.partial') }}</span>
        <button class="btn btn-secondary compact" type="button" :disabled="store.scanBusy" @click="props.onStartNewScan()">
          {{ $t('exactScan.newScan') }}
        </button>
      </div>
    </div>
    <p class="panel-hint">{{ $t('help.scanExact.when') }}</p>
    <div class="controls exact-controls">
      <input
        v-model="store.exactScanValue"
        :placeholder="$t('scan.value')"
        class="input"
        :disabled="store.scanBusy"
        @keyup.enter="store.doExactScan()"
      />
      <select v-model="store.exactScanType" class="input select" :disabled="store.scanBusy">
        <option value="Auto">{{ $t('exactScan.autoMultiType') }}</option>
        <option v-for="type in valueTypeOptions" :key="type">{{ type }}</option>
      </select>
      <button class="btn btn-primary" :disabled="!store.exactScanValue.trim() || store.scanBusy" @click="store.doExactScan()">
        <span v-if="store.scanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ store.scanBusy ? $t('exactScan.scanning') : exactScanButtonLabel }}</span>
      </button>
    </div>
    <div v-if="store.inferredExactTypes.length > 0" class="type-suggestions">
      <button
        v-for="item in store.inferredExactTypes"
        :key="String(item.type)"
        class="type-chip"
        type="button"
        :class="{ active: store.exactScanType === String(item.type).replace(/\s+x\d+$/i, '') }"
        :title="`${item.confidence} · ${item.reason}`"
        @click="store.useInferredType(String(item.type))"
      >
        <strong>{{ item.type }}</strong>
        <span>{{ item.confidence }}</span>
      </button>
    </div>

    <div class="expert-toggle">
      <label class="checkbox-label">
        <input type="checkbox" v-model="store.expertModeEnabled" />
        {{ $t('exactScan.expertMode') }}
      </label>
    </div>
    <div v-if="store.expertModeEnabled" class="expert-filters">
      <div class="controls expert-address-controls">
        <input
          v-model="store.expertStartAddress"
          class="input"
          :placeholder="$t('exactScan.startAddressPlaceholder')"
        />
        <input
          v-model="store.expertStopAddress"
          class="input"
          :placeholder="$t('exactScan.endAddressPlaceholder')"
        />
        <input
          v-model.number="store.expertAlignment"
          type="number"
          min="0"
          step="1"
          class="input"
          :placeholder="$t('exactScan.alignmentPlaceholder')"
        />
      </div>
      <div class="expert-flags">
        <label class="checkbox-label">
          <input type="checkbox" v-model="store.expertWritableOnly" />
          {{ $t('exactScan.writableOnly') }}
        </label>
        <label class="checkbox-label">
          <input type="checkbox" v-model="store.expertExecutableOnly" />
          {{ $t('exactScan.executableOnly') }}
        </label>
        <label class="checkbox-label">
          <input type="checkbox" v-model="store.expertCopyOnWriteOnly" />
          {{ $t('exactScan.copyOnWriteOnly') }}
        </label>
      </div>
      <p class="hint">
        {{ $t('exactScan.expertHint') }}
      </p>
    </div>

    <div v-if="store.exactScanResult" class="metrics">
      <span>{{ $t('scan.matches') }}: {{ formatNumber(store.exactScanResult.matchesFound) }}</span>
      <span>{{ $t('scan.regions') }}: {{ formatNumber(store.exactScanResult.regionsScanned) }}</span>
      <span>{{ $t('scan.stored') }}: {{ formatNumber(store.exactScanResult.candidateStoreSize) }}</span>
      <span v-if="store.exactScanResult.elapsedMs">{{ $t('exactScan.elapsedMs', { ms: formatNumber(store.exactScanResult.elapsedMs) }) }}</span>
      <span v-if="store.exactScanResult.bytesPerSecond">{{ $t('exactScan.throughput', { rate: formatRate(store.exactScanResult.bytesPerSecond) }) }}</span>
    </div>
    <p v-if="store.exactScanResult?.error" class="error">{{ store.exactScanResult.error }}</p>
    <div class="encrypted-scan-panel">
      <div class="source-list-title">
        <strong>{{ $t('exactScan.encryptedScan') }}</strong>
        <span>{{ $t('exactScan.encryptedHint') }}</span>
      </div>
      <div class="controls encrypted-controls">
        <select v-model="store.encryptedScanMode" class="input select compact-input" :disabled="store.scanBusy">
          <option value="xor">XOR</option>
          <option value="add">Add</option>
          <option value="sub">Sub</option>
          <option value="not">NOT</option>
        </select>
        <input v-model="store.encryptedScanKey" class="input compact-input" :placeholder="$t('exactScan.keyPlaceholder')" :disabled="store.scanBusy || store.encryptedScanMode === 'not'" />
        <select v-model.number="store.encryptedScanKeySearchBits" class="input select compact-input" :disabled="store.scanBusy || store.encryptedScanMode === 'not'">
          <option :value="0">{{ $t('exactScan.fixedKey') }}</option>
          <option :value="8">{{ $t('exactScan.brute8') }}</option>
          <option :value="16">{{ $t('exactScan.brute16') }}</option>
        </select>
        <button class="btn btn-primary compact" type="button" :disabled="!store.exactScanValue.trim() || store.scanBusy" @click="store.doEncryptedScan()">
          {{ $t('exactScan.scanEncrypted') }}
        </button>
      </div>
      <div v-if="store.encryptedScanResult" class="metrics">
        <span>{{ $t('exactScan.matches', { count: formatNumber(store.encryptedScanResult.matchesFound) }) }}</span>
        <span>{{ $t('exactScan.regions', { count: formatNumber(store.encryptedScanResult.regionsScanned) }) }}</span>
        <span>{{ $t('exactScan.readBytes', { bytes: formatBytes(store.encryptedScanResult.bytesScanned) }) }}</span>
        <span v-if="store.encryptedScanResult.partial">{{ $t('exactScan.partial') }}</span>
      </div>
      <p v-if="store.encryptedScanResult?.error" class="error">{{ store.encryptedScanResult.error }}</p>
      <div v-if="store.encryptedScanResult?.matches.length" class="encrypted-result-list">
        <div v-for="match in store.encryptedScanResult.matches.slice(0, 12)" :key="`${match.address}:${match.variantLabel}`" class="source-row">
          <code>0x{{ match.address }}</code>
          <span>{{ match.type }}</span>
          <span>{{ match.variantLabel || '-' }}</span>
          <button class="btn btn-secondary compact" type="button" @click="store.selectCandidate(match.address, match.type)">{{ $t('exactScan.use') }}</button>
          <button class="btn btn-secondary compact" type="button" @click="store.addAddressToWatch(match.address, match.type)">{{ $t('exactScan.watch') }}</button>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.exact-controls {
  grid-template-columns: 1fr 120px auto;
}

.expert-toggle {
  margin-top: 10px;
}

.expert-filters {
  margin-top: 8px;
  padding: 8px;
  border: 1px dashed var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.expert-address-controls {
  grid-template-columns: 1fr 1fr 140px;
  margin-bottom: 6px;
}

.expert-flags {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}

.type-suggestions {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
  margin-top: 7px;
}

.type-chip {
  display: inline-flex;
  min-width: 0;
  align-items: center;
  gap: 6px;
  min-height: 26px;
  padding: 4px 8px;
  border: 1px solid var(--border);
  border-radius: 999px;
  background: rgba(36, 40, 59, 0.72);
  color: var(--text-secondary);
  cursor: pointer;
  font-size: 11px;
}

.type-chip strong {
  color: var(--text-primary);
  font-size: 11px;
  line-height: 1;
}

.type-chip span {
  overflow: hidden;
  max-width: 58px;
  color: var(--text-muted);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.type-chip:hover,
.type-chip.active {
  border-color: rgba(122, 162, 247, 0.55);
  background: rgba(122, 162, 247, 0.12);
  color: var(--accent);
}

.type-chip.active strong {
  color: var(--accent);
}
</style>
