<script setup lang="ts">
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { backend } from '@/services/backend'
import { useAppStore } from '@/stores/app'
import { useExpertWriteSelection } from '@/composables/useExpertWriteSelection'
import { formatBytes, formatNumber } from '@/utils/format'
import { findWhatWritesSizeForType } from '@/utils/valueTypes'
import InfoDot from './InfoDot.vue'
import RiskBadge from './RiskBadge.vue'

const fieldStabilityVerdictLabelKeys: Record<string, string> = {
  likely_derived_display: 'candidatePanel.stability.likelyDerivedDisplay',
  likely_event_driven: 'candidatePanel.stability.likelyEventDriven',
  no_writes_observed: 'candidatePanel.stability.noWritesObserved',
  insufficient_data: 'candidatePanel.stability.insufficientData',
}

const store = useAppStore()
const { t } = useI18n()
const {
  selectedCandidateAddresses,
  currentPageCandidates,
  displayedCandidates,
  clearCandidateSelection,
  useCandidateInAssistant,
  isCandidateSelected,
  toggleCandidateSelection,
  toggleCurrentPageSelection,
  useSelectedCandidatesInAssistant,
  writeSelectedCandidates,
  writeSelectedCandidatesAtomic,
  writeSelectedCandidateKernel,
  watchOrRefreshCandidate,
  watchSelectedCandidates,
  watchCurrentCandidatePage,
  watchedCandidate,
} = useExpertWriteSelection()

const candidatePageTotal = computed(() => {
  if (!store.candidatePage) return 1
  if (store.candidatePage.displaySuppressed) return 1
  return Math.max(1, Math.ceil(store.candidatePage.totalCount / store.candidatePage.pageSize))
})

const fieldStabilityByAddress = ref<Record<string, { busy: boolean, result?: Record<string, unknown>, error?: string }>>({})

function confidencePercent(confidence: number | undefined): number {
  if (confidence === undefined || confidence === null) return 100
  return Math.round(confidence * 100)
}

function confidenceClass(confidence: number | undefined): string {
  const pct = confidencePercent(confidence)
  if (pct >= 80) return 'conf-high'
  if (pct >= 50) return 'conf-medium'
  return 'conf-low'
}

function candidateCurrentValue(address: string): string {
  return watchedCandidate(address)?.value || '-'
}

function candidateReadError(address: string): string {
  return watchedCandidate(address)?.error || ''
}

function freezeCandidateCurrent(address: string, type: string) {
  void store.freezeCandidateCurrent(address, type)
}

async function analyzeCandidateStability(address: string, type: string) {
  fieldStabilityByAddress.value = { ...fieldStabilityByAddress.value, [address]: { busy: true } }
  try {
    const controller = backend.getController()
    if (!controller.analyzeFieldStability) {
      fieldStabilityByAddress.value = { ...fieldStabilityByAddress.value, [address]: { busy: false, error: t('candidatePanel.stability.backendUnavailable') } }
      return
    }
    const result = await controller.analyzeFieldStability(address, { size: findWhatWritesSizeForType(type) })
    fieldStabilityByAddress.value = { ...fieldStabilityByAddress.value, [address]: { busy: false, result } }
  } catch (e) {
    fieldStabilityByAddress.value = { ...fieldStabilityByAddress.value, [address]: { busy: false, error: String(e) } }
  }
}

function fieldStabilityLabel(address: string): string {
  const entry = fieldStabilityByAddress.value[address]
  if (!entry) return ''
  if (entry.busy) return t('candidatePanel.stability.analyzing')
  if (entry.error) return t('candidatePanel.stability.error', { error: entry.error })
  const result = entry.result
  if (!result?.success) return String(result?.error || t('candidatePanel.stability.failure'))
  const verdict = String(result.verdict || '')
  return fieldStabilityVerdictLabelKeys[verdict] ? t(fieldStabilityVerdictLabelKeys[verdict]) : verdict
}
</script>

<template>
  <section class="panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('candidatePanel.title') }}</h2>
        <InfoDot topic="candidates" />
        <RiskBadge level="read" />
      </div>
      <span>{{ $t('candidatePanel.selectionSummary', { total: formatNumber(store.candidatePage?.totalCount), selected: selectedCandidateAddresses.length }) }}</span>
    </div>
    <p class="panel-hint">{{ $t('help.candidates.when') }}</p>
    <div class="candidate-toolbar">
      <input
        v-model="store.candidateFilter"
        :placeholder="$t('scan.filterAddress')"
        class="input"
        @input="store.candidatePageIndex = 0; store.refreshCandidates()"
      />
      <button class="btn btn-secondary" :disabled="store.candidatePageIndex === 0" @click="store.previousCandidatePage()">
        {{ $t('scan.previous') }}
      </button>
      <button
        class="btn btn-secondary"
        :disabled="!store.candidatePage || store.candidatePage.displaySuppressed || (store.candidatePageIndex + 1) * store.candidatePageSize >= store.candidatePage.totalCount"
        @click="store.nextCandidatePage()"
      >
        {{ $t('scan.next') }}
      </button>
    </div>
    <div class="selection-toolbar">
      <button class="btn btn-secondary compact" :disabled="store.candidatePage?.displaySuppressed || currentPageCandidates.length === 0" @click="toggleCurrentPageSelection()">
        {{ $t('candidatePanel.selectPage') }}
      </button>
      <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="clearCandidateSelection()">
        {{ $t('candidatePanel.clear') }}
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0" @click="useSelectedCandidatesInAssistant()">
        {{ $t('candidatePanel.useSelection') }}
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0 || !store.writeValue.trim()" @click="writeSelectedCandidates()">
        {{ store.kernelMemoryModeActive ? $t('candidatePanel.writeViaKernel') : $t('candidatePanel.writeSelection') }}
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length < 2 || !store.writeValue.trim()" @click="writeSelectedCandidatesAtomic()">
        {{ $t('candidatePanel.writeAtomic') }}
      </button>
      <InfoDot topic="writeAtomic" align="right" />
      <button
        v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess && !store.kernelMemoryModeActive"
        class="btn btn-secondary compact"
        :title="$t('candidatePanel.writeKernelTitle')"
        :disabled="selectedCandidateAddresses.length !== 1 || !store.writeValue.trim()"
        @click="writeSelectedCandidateKernel()"
      >
        {{ $t('candidatePanel.writeViaKernel') }}
      </button>
      <InfoDot topic="writeKernel" align="right" />
      <button class="btn btn-secondary compact" :disabled="store.candidatePage?.displaySuppressed || currentPageCandidates.length === 0" @click="watchCurrentCandidatePage()">
        {{ $t('candidatePanel.watchPage') }}
      </button>
      <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="watchSelectedCandidates()">
        {{ $t('candidatePanel.watchSelection') }}
      </button>
    </div>
    <div class="page-info">
      {{ store.candidatePage ? store.candidatePage.pageIndex + 1 : 1 }} / {{ candidatePageTotal }}
    </div>
    <div v-if="store.candidatePage" class="metrics candidate-storage">
      <span>{{ store.candidatePage.fileBacked ? $t('candidatePanel.fileStorage') : $t('candidatePanel.ramStorage') }}</span>
      <span>{{ $t('candidatePanel.fileBytes', { bytes: formatBytes(store.candidatePage.candidateStoreBytes) }) }}</span>
      <span>{{ $t('candidatePanel.estimatedRam', { bytes: formatBytes(store.candidatePage.candidateStoreMemoryBytes) }) }}</span>
    </div>
    <div
      v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess"
      class="kernel-escalation-guide"
    >
      <strong>{{ $t('candidatePanel.kernelReady') }}</strong>
      <span>{{ $t('candidatePanel.kernelReadyHint') }}</span>
    </div>
    <div v-if="store.candidatePage?.displaySuppressed" class="candidate-suppressed">
      {{ $t('candidatePanel.displaySuppressed', { count: formatNumber(store.candidatePage.totalCount) }) }}
    </div>
    <p v-if="store.nextScanResult?.stableGroupHint" class="hint stable-group-hint">
      {{ store.nextScanResult.stableGroupHint }}
    </p>
    <div class="candidate-list">
      <div
        v-for="match in displayedCandidates"
        :key="match.address"
        class="candidate-row"
        :class="[`candidate-${store.candidateVisualState(match)}`]"
      >
        <label class="candidate-check">
          <input
            type="checkbox"
            :checked="isCandidateSelected(match.address)"
            @change="toggleCandidateSelection(match.address)"
          />
        </label>
        <button class="address-btn" @click="store.selectCandidate(match.address, match.type)">
          0x{{ match.address }}
        </button>
        <div class="candidate-meta">
          <span class="candidate-type">{{ match.type }}</span>
          <span
            v-if="match.confidence !== undefined && match.confidence < 1"
            class="confidence-badge"
            :class="confidenceClass(match.confidence)"
            :title="match.variantLabel"
          >
            {{ confidencePercent(match.confidence) }}%
          </span>
          <span v-if="match.variantLabel" class="variant-label">{{ match.variantLabel }}</span>
          <span
            v-if="match.writeVerified"
            class="write-verified-badge"
            :title="$t('candidatePanel.writeVerifiedTitle')"
          >
            {{ $t('candidatePanel.written') }}
          </span>
          <span class="visual-state">{{ store.candidateVisualStateLabel(match) }}</span>
          <span v-if="store.watchedAddresses.some((item) => item.address === match.address)" class="live-dot">{{ $t('candidatePanel.watchBadge') }}</span>
        </div>
        <div class="candidate-value" :class="{ error: candidateReadError(match.address) }" :title="candidateReadError(match.address) || match.lastValueHex">
          <span>{{ $t('candidatePanel.value') }}</span>
          <strong>{{ candidateCurrentValue(match.address) }}</strong>
        </div>
        <div class="candidate-actions">
          <button class="btn btn-secondary compact" @click="useCandidateInAssistant(match.address, match.type)">
            {{ $t('candidatePanel.use') }}
          </button>
          <button class="btn btn-secondary compact" @click="watchOrRefreshCandidate(match.address, match.type)">
            {{ watchedCandidate(match.address) ? $t('candidatePanel.refresh') : $t('candidatePanel.watch') }}
          </button>
          <button class="btn btn-secondary compact" @click="freezeCandidateCurrent(match.address, match.type)">
            {{ $t('candidatePanel.freezeCurrent') }}
          </button>
          <button
            class="btn btn-secondary compact"
            :disabled="fieldStabilityByAddress[match.address]?.busy"
            :title="$t('candidatePanel.stabilityTitle')"
            @click="analyzeCandidateStability(match.address, match.type)"
          >
            {{ $t('candidatePanel.stabilityButton') }}
          </button>
          <button class="btn btn-secondary compact" @click="store.keepCandidate(match.address)">
            {{ $t('candidatePanel.keep') }}
          </button>
          <button class="btn btn-secondary compact" @click="store.ignoreCandidate(match.address)">
            {{ $t('candidatePanel.ignore') }}
          </button>
        </div>
        <div v-if="fieldStabilityLabel(match.address)" class="candidate-stability-result">
          {{ fieldStabilityLabel(match.address) }}
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.candidate-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 6px;
}

.selection-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 6px;
}

.page-info {
  margin-bottom: 6px;
  text-align: right;
  color: var(--text-dim);
  font-size: 12px;
}

.candidate-storage {
  margin-top: 0;
  margin-bottom: 8px;
}

.candidate-suppressed {
  margin-bottom: 8px;
  padding: 10px 12px;
  border: 1px solid rgba(122, 162, 247, 0.28);
  border-radius: 6px;
  background: rgba(122, 162, 247, 0.08);
  color: var(--text-secondary);
  font-size: 12px;
}

.kernel-escalation-guide {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  gap: 10px;
  align-items: center;
  margin-bottom: 8px;
  padding: 8px 10px;
  border: 1px solid color-mix(in srgb, var(--accent) 30%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-secondary));
  color: var(--text-dim);
  font-size: 12px;
}

.kernel-escalation-guide strong {
  color: var(--text-primary);
}

.stable-group-hint {
  margin-bottom: 8px;
  padding: 10px 12px;
  border: 1px solid rgba(224, 175, 104, 0.35);
  border-radius: 6px;
  background: rgba(224, 175, 104, 0.08);
}

.candidate-list {
  display: flex;
  max-height: 260px;
  flex-direction: column;
  gap: 4px;
  overflow-y: auto;
}

.candidate-row {
  display: grid;
  grid-template-columns: 28px minmax(160px, 1fr) minmax(210px, 1fr) minmax(120px, 0.6fr) minmax(390px, auto);
  gap: 8px;
  align-items: center;
  padding: 7px 8px;
  border: 1px solid transparent;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.candidate-meta,
.candidate-value,
.candidate-actions {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: 6px;
}

.candidate-meta {
  overflow: hidden;
}

.candidate-actions {
  justify-content: flex-end;
}

.candidate-value {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 11px;
}

.candidate-value strong {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.candidate-value.error strong {
  color: var(--error);
}

.candidate-actions .btn {
  flex: 0 0 auto;
  white-space: nowrap;
}

.candidate-stability-result {
  grid-column: 1 / -1;
  color: var(--text-secondary);
  font-size: 11px;
  padding-left: 36px;
}

.candidate-very-likely {
  border-color: color-mix(in srgb, var(--success) 35%, var(--border));
}

.candidate-to-verify {
  border-color: color-mix(in srgb, var(--warning) 35%, var(--border));
}

.candidate-weak,
.candidate-ignored {
  opacity: 0.65;
}

.candidate-kept {
  border-color: color-mix(in srgb, var(--accent) 55%, var(--border));
}

.candidate-check {
  display: flex;
  align-items: center;
  justify-content: center;
}

.candidate-check input {
  width: 16px;
  height: 16px;
  accent-color: var(--accent);
}

.address-btn {
  overflow: hidden;
  border: none;
  background: transparent;
  color: var(--text-primary);
  cursor: pointer;
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  text-align: left;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.address-btn:hover {
  color: var(--accent);
}

.candidate-type {
  color: var(--text-dim);
  font-size: 11px;
}

.confidence-badge {
  padding: 2px 6px;
  border-radius: 4px;
  font-size: 10px;
  font-weight: 600;
  font-family: 'Segoe UI', sans-serif;
  white-space: nowrap;
}

.confidence-badge.conf-high {
  background: rgba(76, 175, 80, 0.18);
  color: #66bb6a;
}

.confidence-badge.conf-medium {
  background: rgba(255, 193, 7, 0.18);
  color: #ffa726;
}

.confidence-badge.conf-low {
  background: rgba(244, 67, 54, 0.18);
  color: #ef5350;
}

.variant-label {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 10px;
  font-style: italic;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.write-verified-badge {
  border: 1px solid rgba(158, 206, 106, 0.35);
  border-radius: 999px;
  color: var(--success);
  font-size: 10px;
  padding: 1px 6px;
  white-space: nowrap;
}

.visual-state,
.live-dot {
  color: var(--text-dim);
  font-size: 11px;
}

.live-dot {
  color: var(--success);
}

@media (max-width: 860px) {
  .candidate-toolbar,
  .candidate-row,
  .kernel-escalation-guide {
    grid-template-columns: 1fr;
  }
}
</style>
