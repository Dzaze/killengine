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
        <h2>Pointer Chains <span class="hint-inline">(jeux modernes / applications dynamiques)</span></h2>
        <InfoDot topic="pointerChains" />
        <RiskBadge level="read" />
      </div>
      <span v-if="pointerScanResult">{{ formatNumber(pointerScanResult.chainCount) }} chaine(s)</span>
    </div>
    <p class="panel-hint">{{ $t('help.pointerChains.when') }}</p>
    <p class="hint">
      Pour les jeux modernes et applications avec allocations dynamiques, les ressources changent souvent d'adresse.
      Trouve d'abord l'adresse avec un scan normal, puis utilise le scanner de pointeurs pour
      decouvrir une chaine stable qui survivra aux redemarrages.
    </p>
    <div class="controls pointer-chain-controls">
      <input
        v-model="pointerScanAddress"
        class="input"
        placeholder="Adresse cible (0x...)"
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
        placeholder="Profondeur"
        title="Nombre de niveaux de dereferencement"
      />
      <input
        v-model.number="pointerScanMaxOffset"
        type="number"
        min="0"
        step="16"
        class="input"
        placeholder="Offset max"
        title="Offset maximum entre pointeur et cible"
      />
      <button
        class="btn btn-primary"
        :disabled="!pointerScanAddress.trim() || store.scanBusy || !store.isAttached"
        @click="runPointerScan()"
      >
        <span v-if="pointerScanBusy" class="btn-spinner" aria-hidden="true"></span>
        <span>{{ pointerScanBusy ? 'Scan...' : 'Scanner les pointeurs' }}</span>
      </button>
    </div>
    <div v-if="pointerScanResult" class="metrics">
      <span>Chaines: {{ formatNumber(pointerScanResult.chainCount) }}</span>
      <span>Pointeurs scannes: {{ formatNumber(pointerScanResult.pointersScanned) }}</span>
      <span>Bytes: {{ formatBytes(pointerScanResult.bytesScanned) }}</span>
      <span v-if="pointerScanResult.elapsedMs">Temps: {{ formatNumber(pointerScanResult.elapsedMs) }} ms</span>
      <span v-if="pointerScanResult.partial">Resultat partiel</span>
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
          <span class="chain-depth">profondeur {{ chain.depth }}</span>
        </div>
        <div class="pointer-chain-actions">
          <button class="btn btn-secondary compact" @click="testPointerChain(chain)">
            Tester
          </button>
          <button
            class="btn btn-secondary compact"
            @click="usePointerChainAsCandidate(chain)"
          >
            Utiliser
          </button>
          <button
            class="btn btn-primary compact"
            @click="savePointerChain(chain)"
          >
            Sauver profil
          </button>
          <button
            class="btn btn-secondary compact"
            @click="bookmarkPointerChain(chain)"
          >
            Note
          </button>
          <button
            class="btn btn-secondary compact"
            title="Surveille cette chaine en live (adresse re-resolue a chaque cycle) dans le panneau Watch chaines de pointeurs."
            @click="watchPointerChain(chain)"
          >
            Watch
          </button>
        </div>
      </div>
    </div>
    <p v-if="pointerScanResult?.error" class="error">{{ pointerScanResult.error }}</p>
    <p v-if="pointerResolveResult" class="hint">
      Resolution : {{ pointerResolveResult.success ? 'OK 0x' + pointerResolveResult.finalAddress : 'ECHEC ' + pointerResolveResult.error }}
    </p>
  </section>
</template>
