<script setup lang="ts">
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'
import type { RiskLevel } from './risk'

const props = defineProps<{ level: RiskLevel }>()

const { t } = useI18n()

const label = computed(() => t(`help.risk.${props.level}.label`))
const detail = computed(() => t(`help.risk.${props.level}.detail`))
</script>

<template>
  <span class="risk-badge" :class="level" :title="detail">
    <span class="risk-dot" aria-hidden="true"></span>
    {{ label }}
  </span>
</template>

<style scoped>
.risk-badge {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 2px 8px;
  border: 1px solid var(--border);
  border-radius: 10px;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.02em;
  text-transform: uppercase;
  white-space: nowrap;
  cursor: help;
}

.risk-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: currentColor;
}

.risk-badge.read {
  border-color: rgba(158, 206, 106, 0.4);
  background: rgba(158, 206, 106, 0.1);
  color: var(--success);
}

.risk-badge.write {
  border-color: rgba(224, 175, 104, 0.45);
  background: rgba(224, 175, 104, 0.12);
  color: var(--warning);
}

.risk-badge.code {
  border-color: rgba(247, 118, 142, 0.5);
  background: rgba(247, 118, 142, 0.13);
  color: var(--error);
}
</style>
