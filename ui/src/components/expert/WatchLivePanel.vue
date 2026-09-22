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
    </div>
    <div v-if="store.watchedAddresses.length === 0" class="hint">{{ $t('watchLivePanel.empty') }}</div>
    <div v-else class="watch-list">
      <div v-for="item in store.watchedAddresses" :key="item.address" class="watch-row" :class="{ changed: item.changed, unreadable: item.error }">
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
