<script setup lang="ts">
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'
import RiskBadge from '@/components/expert/RiskBadge.vue'
import { formatBytes } from '@/utils/format'

const store = useAppStore()
</script>

<template>
  <section class="panel region-context">
    <div class="panel-title">
      <div class="panel-heading">
        <h2>{{ $t('regionPanel.title') }}</h2>
        <InfoDot topic="regionContext" />
        <RiskBadge level="read" />
      </div>
      <span>{{ formatBytes(store.expertRegionSize) }}</span>
    </div>
    <div class="metrics">
      <span>{{ $t('regionPanel.start') }}: {{ store.expertStartAddress || '-' }}</span>
      <span>{{ $t('regionPanel.end') }}: {{ store.expertStopAddress || '-' }}</span>
      <span>{{ $t('regionPanel.protection') }}: {{ store.expertRegionProtection || '-' }}</span>
      <span>{{ $t('regionPanel.state') }}: {{ store.expertRegionState || '-' }}</span>
      <span>Type: {{ store.expertRegionType || '-' }}</span>
    </div>
  </section>
</template>
