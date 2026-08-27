<script setup lang="ts">
import { computed, ref } from 'vue'
import { backend } from '@/services/backend'
import { useAppStore } from '@/stores/app'
import { useExpertWriteSelection } from '@/composables/useExpertWriteSelection'
import { formatBytes, formatNumber } from '@/utils/format'
import { findWhatWritesSizeForType } from '@/utils/valueTypes'
import InfoDot from './InfoDot.vue'
import RiskBadge from './RiskBadge.vue'

const fieldStabilityVerdictLabels: Record<string, string> = {
  likely_derived_display: 'Probablement affiché/recalculé',
  likely_event_driven: 'Probablement source événementielle',
  no_writes_observed: 'Aucune écriture observée',
  insufficient_data: 'Données insuffisantes',
}

const store = useAppStore()
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
      fieldStabilityByAddress.value = { ...fieldStabilityByAddress.value, [address]: { busy: false, error: 'analyzeFieldStability non exposé par ce backend.' } }
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
  if (entry.busy) return 'Analyse...'
  if (entry.error) return `Erreur: ${entry.error}`
  const result = entry.result
  if (!result?.success) return String(result?.error || 'Échec')
  const verdict = String(result.verdict || '')
  return fieldStabilityVerdictLabels[verdict] || verdict
}
</script>

<template>
  <section class="panel risk-read">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Candidats</h2>
        <InfoDot topic="candidates" />
        <RiskBadge level="read" />
      </div>
      <span>{{ formatNumber(store.candidatePage?.totalCount) }} · {{ selectedCandidateAddresses.length }} sélectionné(s)</span>
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
        Sélection page
      </button>
      <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="clearCandidateSelection()">
        Effacer
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0" @click="useSelectedCandidatesInAssistant()">
        Utiliser sélection
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length === 0 || !store.writeValue.trim()" @click="writeSelectedCandidates()">
        {{ store.kernelMemoryModeActive ? 'Écrire via kernel' : 'Écrire sur sélection' }}
      </button>
      <button class="btn btn-primary compact" :disabled="selectedCandidateAddresses.length < 2 || !store.writeValue.trim()" @click="writeSelectedCandidatesAtomic()">
        Écrire ensemble (atomique)
      </button>
      <InfoDot topic="writeAtomic" align="right" />
      <button
        v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess && !store.kernelMemoryModeActive"
        class="btn btn-secondary compact"
        :title="`Contourne les protections mémoire usermode — pour une adresse qui refuse de tenir une écriture normale (ex: instabilité/compteur animé).`"
        :disabled="selectedCandidateAddresses.length !== 1 || !store.writeValue.trim()"
        @click="writeSelectedCandidateKernel()"
      >
        Écrire via kernel
      </button>
      <InfoDot topic="writeKernel" align="right" />
      <button class="btn btn-secondary compact" :disabled="store.candidatePage?.displaySuppressed || currentPageCandidates.length === 0" @click="watchCurrentCandidatePage()">
        Watch page
      </button>
      <button class="btn btn-secondary compact" :disabled="selectedCandidateAddresses.length === 0" @click="watchSelectedCandidates()">
        Watch sélection
      </button>
    </div>
    <div class="page-info">
      {{ store.candidatePage ? store.candidatePage.pageIndex + 1 : 1 }} / {{ candidatePageTotal }}
    </div>
    <div v-if="store.candidatePage" class="metrics candidate-storage">
      <span>{{ store.candidatePage.fileBacked ? 'Stockage fichier' : 'Stockage RAM' }}</span>
      <span>Fichier: {{ formatBytes(store.candidatePage.candidateStoreBytes) }}</span>
      <span>RAM estimée: {{ formatBytes(store.candidatePage.candidateStoreMemoryBytes) }}</span>
    </div>
    <div
      v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess"
      class="kernel-escalation-guide"
    >
      <strong>Kernel prêt</strong>
      <span>Sélectionne 1 candidat, lis/écris via kernel, puis relis. Si la valeur revient, ce n’est probablement pas un blocage d’écriture : lance Écrit par puis Tester automatiquement.</span>
    </div>
    <div v-if="store.candidatePage?.displaySuppressed" class="candidate-suppressed">
      {{ formatNumber(store.candidatePage.totalCount) }} candidats trouvés. Réduis avec un next scan ou filtre une adresse pour afficher une page.
    </div>
    <p v-if="store.nextScanResult?.stableGroupHint" class="hint stable-group-hint">
      {{ store.nextScanResult.stableGroupHint }}
    </p>
    <div class="candidate-list">
      <div
        v-for="match in displayedCandidates"
        :key="match.address"
        class="candidate-row"
        :class="[`candidate-${store.candidateVisualState(match).replace(' ', '-')}`]"
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
            title="Cette adresse a déjà reçu une écriture confirmée avec succès — contrairement à un candidat juste stable au scan, celui-ci a été prouvé écrivable."
          >
            ✓ écrit
          </span>
          <span class="visual-state">{{ store.candidateVisualState(match) }}</span>
          <span v-if="store.watchedAddresses.some((item) => item.address === match.address)" class="live-dot">watch</span>
        </div>
        <div class="candidate-value" :class="{ error: candidateReadError(match.address) }" :title="candidateReadError(match.address) || match.lastValueHex">
          <span>Valeur</span>
          <strong>{{ candidateCurrentValue(match.address) }}</strong>
        </div>
        <div class="candidate-actions">
          <button class="btn btn-secondary compact" @click="useCandidateInAssistant(match.address, match.type)">
            Utiliser
          </button>
          <button class="btn btn-secondary compact" @click="watchOrRefreshCandidate(match.address, match.type)">
            {{ watchedCandidate(match.address) ? 'Rafraîchir' : 'Watch' }}
          </button>
          <button class="btn btn-secondary compact" @click="freezeCandidateCurrent(match.address, match.type)">
            Freeze actuel
          </button>
          <button
            class="btn btn-secondary compact"
            :disabled="fieldStabilityByAddress[match.address]?.busy"
            title="Observe brièvement (lecture seule) le rythme des écritures pour juger si ce champ est probablement affiché/recalculé ou une source événementielle — utile avant de figer/patcher."
            @click="analyzeCandidateStability(match.address, match.type)"
          >
            Stabilité
          </button>
          <button class="btn btn-secondary compact" @click="store.keepCandidate(match.address)">
            Garder
          </button>
          <button class="btn btn-secondary compact" @click="store.ignoreCandidate(match.address)">
            Ignorer
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

.candidate-très-probable {
  border-color: color-mix(in srgb, var(--success) 35%, var(--border));
}

.candidate-à-vérifier {
  border-color: color-mix(in srgb, var(--warning) 35%, var(--border));
}

.candidate-faible,
.candidate-ignoré {
  opacity: 0.65;
}

.candidate-gardé {
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
