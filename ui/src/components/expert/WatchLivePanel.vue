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
        <h2>Watch live</h2>
        <InfoDot topic="watchLive" />
        <RiskBadge level="read" />
      </div>
      <span>{{ store.watchedAddresses.length }} adresse(s) · {{ store.watchLiveReadLimit }}/cycle</span>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.refreshWatchedAddresses()"
      >
        Rafraîchir
      </button>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.setWatchLiveEnabled(!store.watchLiveEnabled)"
      >
        {{ store.watchLiveEnabled ? 'Arrêter' : 'Démarrer' }}
      </button>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.clearWatchedAddresses()"
      >
        Vider
      </button>
    </div>
    <div v-if="store.watchedAddresses.length === 0" class="hint">Sélectionne un candidat ou clique Watch pour surveiller une adresse.</div>
    <div v-else class="watch-list">
      <div v-for="item in store.watchedAddresses" :key="item.address" class="watch-row" :class="{ changed: item.changed }">
        <code>0x{{ item.address }}</code>
        <span>{{ item.type }}</span>
        <strong>{{ item.value || '-' }}</strong>
        <span v-if="item.previousValue">avant: {{ item.previousValue }}</span>
        <span>{{ item.updatedAt }}</span>
        <button class="btn btn-secondary compact" @click="store.removeAddressFromWatch(item.address)">Retirer</button>
      </div>
    </div>
  </section>
</template>
