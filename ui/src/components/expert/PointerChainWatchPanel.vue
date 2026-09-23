<script setup lang="ts">
import { ref } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { valueTypeOptions } from '@/utils/valueTypes'

const store = useAppStore()

// Ajout manuel d'une watch expression (module + offset de base + offsets de
// dereferencement), sans passer par un scan de pointeurs complet — pour une
// chaine deja connue (ex: retrouvee dans Cheat Engine ou une session passee).
const manualLabel = ref('')
const manualModule = ref('')
const manualBaseOffset = ref('')
const manualOffsets = ref('')
const manualType = ref('Int32')
const manualBusy = ref(false)

async function addManualExpression() {
  if (!manualModule.value.trim() || !manualBaseOffset.value.trim()) return
  manualBusy.value = true
  try {
    const offsets = manualOffsets.value
      .split(/[,\s]+/)
      .map((o) => o.trim())
      .filter(Boolean)
    await store.addWatchedPointerChain(
      { module: manualModule.value.trim(), baseOffset: manualBaseOffset.value.trim(), offsets },
      manualType.value,
      manualLabel.value.trim(),
    )
    manualLabel.value = ''
    manualBaseOffset.value = ''
    manualOffsets.value = ''
  } finally {
    manualBusy.value = false
  }
}
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('pointerChainWatchPanel.title') }} <span class="hint-inline">{{ $t('pointerChainWatchPanel.subtitle') }}</span></h2>
        <InfoDot topic="watchPointerChains" />
        <RiskBadge level="read" />
      </div>
      <span>{{ $t('pointerChainWatchPanel.chainCount', { count: store.watchedPointerChains.length }) }}</span>
      <button
        class="btn compact"
        :class="store.watchedPointerChainsLiveEnabled ? 'btn-primary' : 'btn-secondary'"
        type="button"
        :disabled="store.watchedPointerChains.length === 0"
        @click="store.setWatchedPointerChainsLiveEnabled(!store.watchedPointerChainsLiveEnabled)"
      >
        {{ store.watchedPointerChainsLiveEnabled ? $t('pointerChainWatchPanel.liveOn') : $t('pointerChainWatchPanel.liveOff') }}
      </button>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.refreshWatchedPointerChains()">{{ $t('pointerChainWatchPanel.refresh') }}</button>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.clearWatchedPointerChains()">{{ $t('pointerChainWatchPanel.clear') }}</button>
    </div>
    <p class="hint">
      {{ $t('pointerChainWatchPanel.hintPrefix') }} <code>module+0x18 -> +0x4</code> {{ $t('pointerChainWatchPanel.hintSuffix') }}
    </p>
    <div class="controls watch-expr-controls">
      <input v-model="manualLabel" class="input" :placeholder="$t('pointerChainWatchPanel.labelPlaceholder')" :disabled="manualBusy" />
      <input v-model="manualModule" class="input" :placeholder="$t('pointerChainWatchPanel.modulePlaceholder')" :disabled="manualBusy" />
      <input v-model="manualBaseOffset" class="input" :placeholder="$t('pointerChainWatchPanel.baseOffsetPlaceholder')" :disabled="manualBusy" />
      <input v-model="manualOffsets" class="input" :placeholder="$t('pointerChainWatchPanel.offsetsPlaceholder')" :disabled="manualBusy" />
      <select v-model="manualType" class="input select">
        <option v-for="t in valueTypeOptions" :key="t" :value="t">{{ t }}</option>
      </select>
      <button class="btn btn-primary compact" type="button" :disabled="manualBusy || !manualModule.trim() || !manualBaseOffset.trim()" @click="addManualExpression()">
        {{ $t('pointerChainWatchPanel.addWatch') }}
      </button>
    </div>
    <div v-if="store.watchedPointerChains.length === 0" class="hint">{{ $t('pointerChainWatchPanel.empty') }}</div>
    <div v-else class="watch-list">
      <div v-for="chain in store.watchedPointerChains" :key="chain.id" class="watch-row" :class="{ changed: chain.changed }">
        <strong>{{ chain.label }}</strong>
        <code>0x{{ chain.finalAddress }}</code>
        <span>{{ chain.type }}</span>
        <strong>{{ chain.value }}</strong>
        <span v-if="chain.error" class="error">{{ chain.error }}</span>
        <button class="btn btn-secondary compact" type="button" @click="store.removeWatchedPointerChain(chain.id)">{{ $t('pointerChainWatchPanel.remove') }}</button>
      </div>
    </div>
  </section>
</template>

<style scoped>
.hint-inline {
  color: var(--text-muted);
  font-size: 12px;
  font-weight: normal;
}

.watch-expr-controls {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 8px 0;
}

.watch-expr-controls .input {
  flex: 1 1 140px;
  min-width: 0;
}
</style>
