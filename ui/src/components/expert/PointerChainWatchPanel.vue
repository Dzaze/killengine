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
        <h2>Watch expressions <span class="hint-inline">(chaines de pointeurs live)</span></h2>
        <InfoDot topic="watchPointerChains" />
        <RiskBadge level="read" />
      </div>
      <span>{{ store.watchedPointerChains.length }} chaine(s)</span>
      <button
        class="btn compact"
        :class="store.watchedPointerChainsLiveEnabled ? 'btn-primary' : 'btn-secondary'"
        type="button"
        :disabled="store.watchedPointerChains.length === 0"
        @click="store.setWatchedPointerChainsLiveEnabled(!store.watchedPointerChainsLiveEnabled)"
      >
        {{ store.watchedPointerChainsLiveEnabled ? 'Live ON' : 'Live OFF' }}
      </button>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.refreshWatchedPointerChains()">Rafraichir</button>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.clearWatchedPointerChains()">Vider</button>
    </div>
    <p class="hint">
      Suit en live une adresse derriere un pointeur (ex: <code>module+0x18 -> +0x4</code>) sans avoir a re-resoudre la chaine
      a la main a chaque fois. Ajoute une chaine testee depuis le panneau Pointer Chains (bouton "Watch"), ou saisis-en une
      directement ci-dessous. Active "Live" pour une re-resolution + relecture automatique toutes les secondes.
    </p>
    <div class="controls watch-expr-controls">
      <input v-model="manualLabel" class="input" placeholder="Libelle (optionnel)" :disabled="manualBusy" />
      <input v-model="manualModule" class="input" placeholder="Module (ex: game.exe)" :disabled="manualBusy" />
      <input v-model="manualBaseOffset" class="input" placeholder="Offset de base (0x...)" :disabled="manualBusy" />
      <input v-model="manualOffsets" class="input" placeholder="Offsets suivants (0x18, 0x4, ...)" :disabled="manualBusy" />
      <select v-model="manualType" class="input select">
        <option v-for="t in valueTypeOptions" :key="t" :value="t">{{ t }}</option>
      </select>
      <button class="btn btn-primary compact" type="button" :disabled="manualBusy || !manualModule.trim() || !manualBaseOffset.trim()" @click="addManualExpression()">
        + Watch
      </button>
    </div>
    <div v-if="store.watchedPointerChains.length === 0" class="hint">Aucune expression surveillee pour l'instant.</div>
    <div v-else class="watch-list">
      <div v-for="chain in store.watchedPointerChains" :key="chain.id" class="watch-row" :class="{ changed: chain.changed }">
        <strong>{{ chain.label }}</strong>
        <code>0x{{ chain.finalAddress }}</code>
        <span>{{ chain.type }}</span>
        <strong>{{ chain.value }}</strong>
        <span v-if="chain.error" class="error">{{ chain.error }}</span>
        <button class="btn btn-secondary compact" type="button" @click="store.removeWatchedPointerChain(chain.id)">x</button>
      </div>
    </div>
  </section>
</template>

<style scoped>
.hint-inline {
  color: var(--text-dim);
  font-size: 11px;
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
