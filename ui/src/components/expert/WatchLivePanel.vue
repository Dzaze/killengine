<script setup lang="ts">
import { ref } from 'vue'
import { useAppStore } from '@/stores/app'
import { useCandidateComparisonStore } from '@/stores/candidateComparison'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'

const store = useAppStore()
const candidateComparisonStore = useCandidateComparisonStore()

// UX-PRODUIT-16 : sélection locale (aucune sélection n'existait avant sur ce
// panneau, contrairement à Candidats/Trace UI string) -- pas de facteur
// natif ici (WatchedAddress.type est un type brut, pas un variantLabel avec
// échelle) : facteur toujours 1, limite documentée dans le tracker.
const selectedWatchAddresses = ref<string[]>([])

function isWatchSelected(address: string) {
  return selectedWatchAddresses.value.includes(address)
}

function toggleWatchSelection(address: string) {
  if (selectedWatchAddresses.value.includes(address)) {
    selectedWatchAddresses.value = selectedWatchAddresses.value.filter((a) => a !== address)
    return
  }
  selectedWatchAddresses.value = [...selectedWatchAddresses.value, address]
}

function compareSelectedWatched() {
  const selected = new Set(selectedWatchAddresses.value)
  const chosen = store.watchedAddresses.filter((item) => selected.has(item.address))
  if (chosen.length < 2 || chosen.length > 6) return
  candidateComparisonStore.stageSeriesFromSelection(chosen.map((item) => ({
    address: item.address,
    type: item.type,
    factor: 1,
    label: `${item.type} 0x${item.address}`,
  })))
  store.activeView = 'memory-timeline'
}
</script>

<template>
  <section class="panel">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('watchLivePanel.title') }}</h2>
        <InfoDot topic="watchLive" />
        <RiskBadge level="read" />
      </div>
      <span>{{ $t('watchLivePanel.addressCount', { count: store.watchedAddresses.length, limit: store.watchLiveReadLimit }) }}</span>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.refreshWatchedAddresses()"
      >
        {{ $t('watchLivePanel.refresh') }}
      </button>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.setWatchLiveEnabled(!store.watchLiveEnabled)"
      >
        {{ store.watchLiveEnabled ? $t('watchLivePanel.stop') : $t('watchLivePanel.start') }}
      </button>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="store.watchedAddresses.length === 0"
        @click="store.clearWatchedAddresses()"
      >
        {{ $t('watchLivePanel.clear') }}
      </button>
      <button
        class="btn btn-secondary compact"
        type="button"
        :disabled="selectedWatchAddresses.length < 2 || selectedWatchAddresses.length > 6"
        :title="$t('watchLivePanel.compareHint')"
        @click="compareSelectedWatched()"
      >
        {{ $t('watchLivePanel.compare', { count: selectedWatchAddresses.length }) }}
      </button>
    </div>
    <div v-if="store.watchedAddresses.length === 0" class="hint">{{ $t('watchLivePanel.empty') }}</div>
    <div v-else class="watch-list">
      <div v-for="item in store.watchedAddresses" :key="item.address" class="watch-row" :class="{ changed: item.changed, unreadable: item.error }">
        <label class="candidate-check">
          <input type="checkbox" :checked="isWatchSelected(item.address)" @change="toggleWatchSelection(item.address)" />
        </label>
        <code>0x{{ item.address }}</code>
        <span>{{ item.type }}</span>
        <strong v-if="item.error" class="watch-error" :title="item.error">{{ $t('watchLivePanel.unreadable') }}</strong>
        <strong v-else>{{ item.value || '-' }}</strong>
        <span v-if="item.error && item.previousValue">{{ $t('watchLivePanel.lastKnownValue', { value: item.previousValue }) }}</span>
        <span v-else-if="item.previousValue">{{ $t('watchLivePanel.previousValue', { value: item.previousValue }) }}</span>
        <span>{{ item.updatedAt }}</span>
        <button class="btn btn-secondary compact" @click="store.removeAddressFromWatch(item.address)">{{ $t('watchLivePanel.remove') }}</button>
      </div>
    </div>
  </section>
</template>

<style scoped>
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
</style>
