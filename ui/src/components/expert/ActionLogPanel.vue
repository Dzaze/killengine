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
        <h2>{{ $t('actionLogPanel.title') }}</h2>
        <InfoDot topic="journal" />
        <RiskBadge level="read" />
      </div>
      <span>{{ $t('actionLogPanel.entryCount', { count: store.actionLog.length }) }}</span>
    </div>
    <div class="action-log">
      <div v-for="entry in store.actionLog.slice(0, 18)" :key="entry.id" class="action-entry" :class="entry.status">
        <span>{{ entry.time }}</span>
        <strong>{{ entry.title }}</strong>
        <em>{{ entry.detail }}</em>
      </div>
    </div>
  </section>
</template>
