<script setup lang="ts">
import { computed } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()

const isDegraded = computed(() => store.localAiWarmupStage === 'degraded' || store.localAiWarmupStage === 'error')

const stagePercent = computed(() => {
  switch (store.localAiWarmupStage) {
    case 'initializing': return 15
    case 'loadingModel': return 55
    case 'warmingPrompt': return 85
    case 'ready': return 100
    default: return 100
  }
})
</script>

<template>
  <div class="warmup-backdrop" role="presentation">
    <section class="warmup-card" role="dialog" aria-modal="true" aria-labelledby="warmup-title">
      <div class="warmup-logo">
        <span class="warmup-logo-icon">⚡</span>
        <span class="warmup-logo-text">KillEngine</span>
      </div>

      <template v-if="!isDegraded">
        <h1 id="warmup-title" class="warmup-title">{{ $t('aiWarmup.title') }}</h1>
        <div class="warmup-bar-track">
          <div class="warmup-bar-fill" :style="{ width: stagePercent + '%' }" />
        </div>
        <p class="warmup-stage">{{ $t('aiWarmup.stage.' + store.localAiWarmupStage) }}</p>
      </template>

      <template v-else>
        <h1 id="warmup-title" class="warmup-title warmup-title-degraded">
          {{ store.localAiWarmupStage === 'error' ? $t('aiWarmup.errorTitle') : $t('aiWarmup.degradedTitle') }}
        </h1>
        <p class="warmup-degraded-reason">
          {{ store.localAiWarmupStage === 'error' ? store.localAiWarmupError : $t('aiWarmup.degradedReason.' + store.localAiWarmupDegradedReason) }}
        </p>
        <button type="button" class="warmup-continue-btn" @click="store.localAiWarmupVisible = false">
          {{ $t('aiWarmup.continueWithoutAi') }}
        </button>
      </template>

      <a class="warmup-credit" href="https://benoist-pirolley.web.app/" target="_blank" rel="noopener">Pirolley Benoist</a>
    </section>
  </div>
</template>

<style scoped>
.warmup-backdrop {
  position: fixed;
  inset: 0;
  z-index: 2000;
  display: grid;
  place-items: center;
  padding: 24px;
  background: rgba(8, 9, 14, 0.86);
}

.warmup-card {
  width: min(440px, 100%);
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 18px;
  padding: 48px 40px 32px;
  border: 1px solid rgba(125, 142, 255, 0.2);
  border-radius: 12px;
  background: linear-gradient(180deg, rgba(22, 24, 38, 0.98) 0%, rgba(15, 16, 26, 0.98) 100%);
  box-shadow: 0 32px 90px rgba(0, 0, 0, 0.55);
}

.warmup-logo {
  display: flex;
  align-items: center;
  gap: 12px;
}

.warmup-logo-icon {
  font-size: 34px;
  filter: drop-shadow(0 0 14px rgba(255, 158, 100, 0.45));
}

.warmup-logo-text {
  font-size: 26px;
  font-weight: 700;
  color: #9fbdff;
  text-shadow: 0 0 22px rgba(122, 162, 247, 0.35);
}

.warmup-title {
  font-size: 16px;
  font-weight: 600;
  color: var(--text-primary);
}

.warmup-title-degraded {
  color: var(--warning);
}

.warmup-bar-track {
  width: 100%;
  height: 8px;
  border-radius: 999px;
  background: rgba(125, 142, 255, 0.12);
  overflow: hidden;
}

.warmup-bar-fill {
  height: 100%;
  border-radius: 999px;
  background: linear-gradient(90deg, #ff9e64, #7aa2f7);
  box-shadow: 0 0 16px rgba(122, 162, 247, 0.55);
  transition: width 0.6s ease;
  animation: warmup-pulse 1.8s ease-in-out infinite;
}

@keyframes warmup-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.72; }
}

.warmup-stage {
  font-size: 13px;
  color: var(--text-dim);
}

.warmup-degraded-reason {
  font-size: 13px;
  color: var(--text-dim);
  line-height: 1.5;
}

.warmup-continue-btn {
  padding: 9px 20px;
  border: 1px solid rgba(224, 175, 104, 0.4);
  border-radius: 6px;
  background: rgba(224, 175, 104, 0.12);
  color: var(--warning);
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
}

.warmup-continue-btn:hover {
  background: rgba(224, 175, 104, 0.2);
}

.warmup-credit {
  margin-top: 6px;
  font-size: 11px;
  color: #565f89;
  text-decoration: none;
}

.warmup-credit:hover {
  color: #7aa2f7;
  text-decoration: underline;
}
</style>
