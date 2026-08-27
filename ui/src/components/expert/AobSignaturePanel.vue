<script setup lang="ts">
import { useExpertAobFlow } from '@/composables/useExpertAobFlow'
import { formatBytes, formatNumber } from '@/utils/format'
import InfoDot from './InfoDot.vue'
import RiskBadge from './RiskBadge.vue'

const {
  aobPattern,
  aobExecutableOnly,
  aobImageOnly,
  aobMaxResults,
  aobBusy,
  aobResult,
  aobStabilizeBusy,
  aobStabilizeResult,
  aobSignatureResult,
  aobAutoScanSkippedReason,
  codePatchAddress,
  codePatchBytes,
  codePatchBusy,
  codePatchResult,
  codePatchSuggestBusy,
  codePatchSuggestionResult,
  valueOverrideSuggestion,
  valueOverrideInput,
  valueOverrideError,
  codePatchProfileName,
  codePatchProfilePatchName,
  codePatchProfileDescription,
  codePatchProfileBusy,
  codePatchProfileResult,
  scanAobSignature,
  stabilizeSelectedAobSignature,
  useAobMatchAddress,
  bookmarkAobMatch,
  bookmarkCurrentCodePatch,
  selectAobPatchAddress,
  useCodePatchSuggestion,
  applyValueOverrideSuggestion,
  suggestSelectedCodePatches,
  applySelectedCodePatch,
  restoreSelectedCodePatch,
  saveSelectedCodePatchProfile,
} = useExpertAobFlow()
</script>

<template>
  <section class="panel aob-panel risk-code">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>AOB signatures</h2>
        <InfoDot topic="aob" />
        <RiskBadge level="code" />
      </div>
      <span v-if="aobResult">{{ formatNumber(aobResult.matchesFound) }} match(es)</span>
    </div>
    <p class="panel-hint">{{ $t('help.aob.when') }}</p>
    <div class="controls aob-controls">
      <input
        v-model="aobPattern"
        class="input"
        placeholder="Pattern: 48 8B ?? ?? 89"
        :disabled="aobBusy"
        @keyup.enter="scanAobSignature()"
      />
      <input v-model.number="aobMaxResults" class="input" type="number" min="1" max="10000" />
      <button class="btn btn-primary" :disabled="aobBusy || !aobPattern.trim()" @click="scanAobSignature()">
        <span v-if="aobBusy" class="btn-spinner" aria-hidden="true"></span>
        Scanner AOB
      </button>
    </div>
    <div class="expert-flags aob-flags">
      <label class="checkbox-label">
        <input v-model="aobExecutableOnly" type="checkbox" :disabled="aobBusy" />
        Code exécutable
      </label>
      <label class="checkbox-label">
        <input v-model="aobImageOnly" type="checkbox" :disabled="aobBusy" />
        Module image
      </label>
    </div>
    <div v-if="aobResult" class="metrics">
      <span>Régions: {{ formatNumber(aobResult.regionsScanned) }}</span>
      <span>Lu: {{ formatBytes(aobResult.bytesScanned) }}</span>
      <span v-if="aobResult.patternBytes">Pattern: {{ formatNumber(aobResult.patternBytes) }} o</span>
      <span v-if="aobResult.signatureQuality" :class="`quality-${aobResult.signatureQuality.level}`">
        Qualité: {{ aobResult.signatureQuality.level }} · {{ aobResult.signatureQuality.score }}/100
      </span>
      <span v-if="aobResult.signatureQuality">
        Fixes: {{ formatNumber(aobResult.signatureQuality.fixedBytes) }} / Wildcards: {{ formatNumber(aobResult.signatureQuality.wildcardBytes) }}
      </span>
      <span v-if="aobResult.partial" class="warning-text">résultats limités</span>
    </div>
    <div v-if="aobSignatureResult" class="metrics">
      <span>Signature: {{ aobSignatureResult.success ? 'OK' : 'FAIL' }}</span>
      <span v-if="aobSignatureResult.module">{{ aobSignatureResult.module }} +0x{{ aobSignatureResult.moduleOffset }}</span>
      <span v-if="aobSignatureResult.patternBytes">{{ formatNumber(aobSignatureResult.patternBytes) }} o</span>
      <span v-if="aobSignatureResult.signatureQuality" :class="`quality-${aobSignatureResult.signatureQuality.level}`">
        Qualité: {{ aobSignatureResult.signatureQuality.level }} · {{ aobSignatureResult.signatureQuality.score }}/100
      </span>
    </div>
    <p v-if="aobAutoScanSkippedReason" class="warning-text">{{ aobAutoScanSkippedReason }}</p>
    <p v-if="aobResult?.signatureWarning" class="hint">{{ aobResult.signatureWarning }}</p>
    <p v-if="aobSignatureResult?.warning" class="hint">{{ aobSignatureResult.warning }}</p>
    <p v-if="aobSignatureResult?.error" class="error">{{ aobSignatureResult.error }}</p>
    <p v-if="aobResult?.error" class="error">{{ aobResult.error }}</p>
    <div class="metrics patch-relay-status">
      <span
        class="quality-medium"
        title="Fallback PHASE 122 : si le patch code direct échoue en ERROR_ACCESS_DENIED sur la bascule RWX, KillEngine tente le relais PowerShell borné aux patchs code."
      >
        Relais patch prêt
      </span>
      <span title="Le relais ne s'applique pas aux écritures mémoire DATA génériques.">
        code uniquement
      </span>
    </div>
    <div class="controls code-patch-controls">
      <input
        v-model="codePatchAddress"
        class="input"
        placeholder="Adresse patch (0x...)"
        :disabled="codePatchBusy"
      />
      <input
        v-model="codePatchBytes"
        class="input"
        placeholder="Bytes exacts: 90 90"
        :disabled="codePatchBusy"
        @keyup.enter="applySelectedCodePatch()"
      />
      <button class="btn btn-primary" :disabled="codePatchBusy || !codePatchAddress.trim() || !codePatchBytes.trim()" @click="applySelectedCodePatch()">
        <span v-if="codePatchBusy" class="btn-spinner" aria-hidden="true"></span>
        Appliquer
      </button>
      <button class="btn btn-primary" :disabled="codePatchSuggestBusy || !codePatchAddress.trim()" @click="suggestSelectedCodePatches()">
        <span v-if="codePatchSuggestBusy" class="btn-spinner" aria-hidden="true"></span>
        Analyser
      </button>
      <button class="btn btn-secondary" :disabled="aobStabilizeBusy || !codePatchAddress.trim()" @click="stabilizeSelectedAobSignature()">
        <span v-if="aobStabilizeBusy" class="btn-spinner" aria-hidden="true"></span>
        Stabiliser AOB
      </button>
      <button class="btn btn-secondary" :disabled="codePatchBusy || !codePatchAddress.trim()" @click="restoreSelectedCodePatch()">
        Restaurer
      </button>
      <button class="btn btn-secondary" :disabled="!codePatchAddress.trim()" @click="bookmarkCurrentCodePatch()">
        Bookmark
      </button>
    </div>
    <div v-if="aobStabilizeResult" class="metrics">
      <span>Auto AOB: {{ aobStabilizeResult.success ? 'unique' : 'à ajuster' }}</span>
      <span v-if="aobStabilizeResult.matchesFound !== undefined">{{ formatNumber(Number(aobStabilizeResult.matchesFound)) }} match(es)</span>
      <span v-if="Array.isArray(aobStabilizeResult.tested)">{{ formatNumber(aobStabilizeResult.tested.length) }} pattern(s)</span>
    </div>
    <p v-if="aobStabilizeResult?.error" class="error">{{ aobStabilizeResult.error }}</p>
    <div v-if="codePatchSuggestionResult" class="metrics">
      <span>Instruction: {{ codePatchSuggestionResult.success ? 'OK' : 'FAIL' }}</span>
      <span v-if="codePatchSuggestionResult.instructionLength">{{ formatNumber(codePatchSuggestionResult.instructionLength) }} o</span>
      <span v-if="codePatchSuggestionResult.mnemonicHint">{{ codePatchSuggestionResult.mnemonicHint }}</span>
      <span v-if="codePatchSuggestionResult.category">{{ codePatchSuggestionResult.category }}</span>
      <span v-if="codePatchSuggestionResult.decoder">{{ codePatchSuggestionResult.decoder }}</span>
      <span v-if="codePatchSuggestionResult.signatureQuality" :class="`quality-${codePatchSuggestionResult.signatureQuality.level}`">
        AOB {{ codePatchSuggestionResult.signatureQuality.level }} · {{ codePatchSuggestionResult.signatureQuality.score }}/100
      </span>
    </div>
    <p v-if="codePatchSuggestionResult?.disassembly" class="hint">{{ codePatchSuggestionResult.disassembly }}</p>
    <p v-if="codePatchSuggestionResult?.stableAobPattern" class="hint">AOB stable: {{ codePatchSuggestionResult.stableAobPattern }}</p>
    <p v-if="codePatchSuggestionResult?.bytes" class="hint">Instruction: {{ codePatchSuggestionResult.bytes }}</p>
    <p v-if="codePatchSuggestionResult?.warning" class="hint">{{ codePatchSuggestionResult.warning }}</p>
    <div v-if="codePatchSuggestionResult?.suggestions?.length" class="patch-suggestion-list">
      <button
        v-for="suggestion in codePatchSuggestionResult.suggestions"
        :key="suggestion.label"
        class="btn compact"
        :class="suggestion.riskLevel === 'low' ? 'btn-primary' : 'btn-secondary'"
        type="button"
        :title="suggestion.description"
        @click="useCodePatchSuggestion(suggestion)"
      >
        {{ suggestion.label }}{{ suggestion.riskLevel ? ` · ${suggestion.riskLevel}` : '' }}
      </button>
    </div>
    <div v-if="valueOverrideSuggestion" class="controls value-override-controls">
      <span class="hint">
        {{ valueOverrideSuggestion.description }} ({{ valueOverrideSuggestion.valueSize }} octet(s), à l'offset {{ valueOverrideSuggestion.valueOffset }} de l'instruction).
      </span>
      <input
        v-model="valueOverrideInput"
        class="input"
        placeholder="Valeur : 999 ou 0x3E7"
        @keyup.enter="applyValueOverrideSuggestion()"
      />
      <button class="btn btn-primary compact" type="button" :disabled="!valueOverrideInput.trim()" @click="applyValueOverrideSuggestion()">
        Appliquer valeur
      </button>
    </div>
    <p v-if="valueOverrideError" class="warning-text">{{ valueOverrideError }}</p>
    <p v-if="codePatchSuggestionResult?.error" class="error">{{ codePatchSuggestionResult.error }}</p>
    <div v-if="codePatchResult" class="metrics">
      <span>Patch: {{ codePatchResult.success ? 'OK' : 'FAIL' }}</span>
      <span v-if="codePatchResult.bytesWritten">{{ formatNumber(codePatchResult.bytesWritten) }} o</span>
      <span v-if="codePatchResult.verified">vérifié</span>
      <span v-if="codePatchResult.protectionChanged" title="Bascule de protection directe ou fallback relais PowerShell selon le blocage runtime.">VirtualProtectEx/relais</span>
      <span v-if="codePatchResult.active">actif</span>
    </div>
    <p v-if="codePatchResult?.originalBytes" class="hint">Originaux: {{ codePatchResult.originalBytes }}</p>
    <p v-if="codePatchResult?.restoredBytes" class="hint">Restaurés: {{ codePatchResult.restoredBytes }}</p>
    <p v-if="codePatchResult?.error" class="error">{{ codePatchResult.error }}</p>
    <div class="controls code-patch-profile-controls">
      <input
        v-model="codePatchProfileName"
        class="input"
        placeholder="Profil trainer"
        :disabled="codePatchProfileBusy"
      />
      <input
        v-model="codePatchProfilePatchName"
        class="input"
        placeholder="Nom patch"
        :disabled="codePatchProfileBusy"
      />
      <input
        v-model="codePatchProfileDescription"
        class="input"
        placeholder="Description"
        :disabled="codePatchProfileBusy"
      />
      <button
        class="btn btn-primary"
        :disabled="codePatchProfileBusy || !codePatchProfileName.trim() || !codePatchProfilePatchName.trim() || !codePatchAddress.trim() || !codePatchBytes.trim()"
        @click="saveSelectedCodePatchProfile()"
      >
        <span v-if="codePatchProfileBusy" class="btn-spinner" aria-hidden="true"></span>
        Sauver trainer
      </button>
    </div>
    <div v-if="codePatchProfileResult" class="metrics">
      <span>Profil: {{ codePatchProfileResult.success ? 'OK' : 'FAIL' }}</span>
      <span v-if="codePatchProfileResult.profileName">{{ codePatchProfileResult.profileName }}</span>
      <span v-if="codePatchProfileResult.patchName">{{ codePatchProfileResult.patchName }}</span>
    </div>
    <p v-if="codePatchProfileResult?.error" class="error">{{ codePatchProfileResult.error }}</p>
    <div v-if="aobResult?.matches?.length" class="aob-list">
      <div v-for="match in aobResult.matches.slice(0, 80)" :key="match.address" class="aob-row">
        <code>0x{{ match.address }}</code>
        <span>{{ match.module || match.memoryType || '-' }}</span>
        <span>{{ match.moduleOffset ? `+0x${match.moduleOffset}` : match.protection || '-' }}</span>
        <button class="btn btn-secondary compact" type="button" @click="useAobMatchAddress(match.address)">Lire</button>
        <button class="btn btn-primary compact" type="button" @click="selectAobPatchAddress(match.address)">Patch</button>
        <button class="btn btn-secondary compact" type="button" @click="bookmarkAobMatch(match)">Note</button>
      </div>
    </div>
  </section>
</template>

<style scoped>
.warning-text {
  color: var(--warning);
}

.quality-medium {
  color: var(--warning);
}

.quality-weak,
.quality-invalid {
  color: var(--error);
}

.metrics {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  margin-top: 9px;
  color: var(--text-dim);
  font-size: 12px;
}

.patch-relay-status span {
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 3px 8px;
}

.aob-controls {
  grid-template-columns: minmax(220px, 1fr) 120px auto;
  align-items: center;
}

.code-patch-controls {
  grid-template-columns: minmax(170px, 1fr) minmax(180px, 1fr) auto auto auto;
  align-items: center;
  margin-top: 10px;
}

.code-patch-profile-controls {
  grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) minmax(160px, 1fr) auto;
  align-items: center;
  margin-top: 10px;
}

.patch-suggestion-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}

.aob-flags {
  margin-top: 8px;
}

.aob-list {
  display: grid;
  gap: 5px;
  margin-top: 10px;
}

.aob-row {
  display: grid;
  grid-template-columns: minmax(150px, 1fr) minmax(120px, 180px) minmax(90px, 140px) auto auto;
  gap: 8px;
  align-items: center;
  min-height: 34px;
  padding: 6px 8px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-dim);
  font-size: 12px;
}

.aob-row code,
.aob-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.aob-row code {
  color: var(--text-primary);
}

@media (max-width: 860px) {
  .aob-controls,
  .code-patch-controls,
  .code-patch-profile-controls,
  .aob-row {
    grid-template-columns: 1fr;
  }
}
</style>