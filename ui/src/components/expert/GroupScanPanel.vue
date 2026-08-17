<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { valueTypeOptions } from '@/utils/valueTypes'

const store = useAppStore()

// Rendu ici pour préserver le regroupement visuel d'origine, mais la donnée
// est possédée/écrite par le panel Trace UI string (findWhatAccessesForSource,
// pas encore extrait) — passée en prop plutôt que dupliquée dans un ref local.
defineProps<{
  findWhatAccessesResult: Record<string, unknown> | null
}>()
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Scan groupe</h2>
        <InfoDot topic="groupScan" />
        <RiskBadge level="read" />
      </div>
      <span>{{ store.groupScanResult ? store.groupScanResult.matchesFound + ' structure(s)' : 'valeurs voisines' }}</span>
    </div>
    <p class="hint">Cherche N valeurs avec offsets fixes connus (ex: HP=100 a +0, Mana=50 a +4). Trouve la base de la structure.</p>
    <div class="group-scan-entries">
      <div v-for="(entry, index) in store.groupScanEntries" :key="index" class="group-scan-row">
        <input v-model="entry.offset" class="input-mini" type="text" placeholder="0" spellcheck="false" />
        <select v-model="entry.type" class="input-mini">
          <option v-for="t in valueTypeOptions" :key="t" :value="t">{{ t }}</option>
        </select>
        <input v-model="entry.value" class="input-mini grow" type="text" placeholder="valeur" spellcheck="false" />
        <button class="btn btn-secondary compact" type="button" :disabled="store.groupScanEntries.length <= 2" @click="store.removeGroupScanEntry(index)">x</button>
      </div>
    </div>
    <div class="row-actions">
      <button class="btn btn-secondary compact" type="button" @click="store.addGroupScanEntry()">+ valeur</button>
      <button class="btn btn-secondary compact" type="button" @click="store.clearGroupScanEntries()">Reset</button>
      <label class="hint">distance max</label>
      <input v-model.number="store.groupScanMaxDistance" class="input-mini" type="number" min="4" max="4096" />
      <button
        class="btn btn-primary compact"
        type="button"
        :disabled="store.groupScanBusy || store.scanBusy || !store.isAttached"
        @click="store.doGroupScan()"
      >
        <span v-if="store.groupScanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ store.groupScanBusy ? 'Scan...' : 'Scanner groupe' }}</span>
      </button>
    </div>
    <div v-if="store.groupScanResult" class="metrics">
      <span>Trouves: {{ store.groupScanResult.matchesFound }}</span>
      <span v-if="store.groupScanResult.partial">Partiel</span>
      <span v-if="store.groupScanResult.elapsedMs">Temps: {{ store.groupScanResult.elapsedMs }} ms</span>
    </div>
    <div v-if="store.groupScanResult?.matches?.length" class="group-scan-results">
      <div v-for="match in store.groupScanResult.matches.slice(0, 20)" :key="match.address" class="group-scan-result-row">
        <code>0x{{ match.address }}</code>
        <span>{{ match.variantLabel }}</span>
        <span v-if="match.confidence" class="hint">{{ Math.round(match.confidence * 100) }}%</span>
        <button class="btn btn-secondary compact" type="button" @click="store.addAddressToWatch(match.address, match.type)">Watch</button>
      </div>
    </div>
    <p v-if="store.groupScanResult?.error" class="error">{{ store.groupScanResult.error }}</p>
    <p v-if="findWhatAccessesResult" :class="findWhatAccessesResult.success ? 'hint' : 'error'">
      Lu par : {{ findWhatAccessesResult.hitCount ?? 0 }} acces - {{ findWhatAccessesResult.error ?? '' }}
    </p>
  </section>
</template>

<style scoped>
.group-scan-entries {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin: 6px 0;
}

.group-scan-row {
  display: flex;
  gap: 6px;
  align-items: center;
}

.group-scan-row .input-mini,
.row-actions .input-mini {
  width: 90px;
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 12px;
}

.group-scan-row .grow {
  flex: 1;
  min-width: 0;
}

.group-scan-results {
  display: flex;
  flex-direction: column;
  gap: 4px;
  max-height: 220px;
  overflow-y: auto;
  margin-top: 6px;
}

.group-scan-result-row {
  display: flex;
  gap: 8px;
  align-items: center;
  font-size: 12px;
}

.row-actions {
  display: flex;
  gap: 6px;
  align-items: center;
  margin-top: 6px;
}

.row-actions .input-mini {
  width: 70px;
}
</style>
