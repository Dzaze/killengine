<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>Watch chaines de pointeurs</h2>
        <InfoDot topic="watchPointerChains" />
        <RiskBadge level="read" />
      </div>
      <span>{{ store.watchedPointerChains.length }} chaine(s)</span>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.refreshWatchedPointerChains()">Rafraichir</button>
      <button class="btn btn-secondary compact" type="button" :disabled="store.watchedPointerChains.length === 0" @click="store.clearWatchedPointerChains()">Vider</button>
    </div>
    <div v-if="store.watchedPointerChains.length === 0" class="hint">Teste une chaine de pointeurs puis clique Watch pour la surveiller en live.</div>
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
