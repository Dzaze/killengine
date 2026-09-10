<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import { useI18n } from 'vue-i18n'
import InfoDot from '@/components/expert/InfoDot.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t } = useI18n()

const presets = [0.25, 0.5, 1, 2, 4, 10]

const isActive = computed(() => store.speedhackStatus?.active === true)
const statusLabel = computed(() => {
  if (!store.speedhackStatus) return t('speedhack.status.unknown')
  if (store.speedhackStatus.installError) return t('speedhack.status.installFailed')
  return isActive.value ? t('speedhack.status.active') : t('speedhack.status.inactive')
})

function onSliderInput(event: Event) {
  const value = Number((event.target as HTMLInputElement).value)
  store.speedhackFactor = value
  if (isActive.value) {
    void store.setSpeedhackFactor(value)
  }
}

function applyPreset(factor: number) {
  store.speedhackFactor = factor
  if (isActive.value) {
    void store.setSpeedhackFactor(factor)
  } else {
    void store.startSpeedhack(factor)
  }
}

function toggle() {
  if (isActive.value) {
    void store.stopSpeedhack()
  } else {
    void store.startSpeedhack(store.speedhackFactor)
  }
}

onMounted(() => {
  void store.refreshSpeedhackStatus()
})
</script>

<template>
  <div class="speedhack-view">
    <div class="header">
      <div>
        <h1>{{ $t('speedhack.title') }}</h1>
        <p>{{ store.isAttached ? store.processName : $t('speedhack.noProcess') }}</p>
      </div>
      <InfoDot topic="speedhack" align="right" />
    </div>

    <PanelIntro
      :what="$t('speedhack.intro.what')"
      :purpose="$t('speedhack.intro.purpose')"
      :how="$t('speedhack.intro.how')"
    />

    <div v-if="!store.isAttached" class="empty-state">
      <p>{{ $t('speedhack.attachPrompt') }}</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">{{ $t('speedhack.goToProcess') }}</button>
    </div>

    <template v-else>
      <section class="status-band">
        <span>{{ $t('speedhack.statusLabel') }} <strong>{{ statusLabel }}</strong></span>
        <span>{{ $t('speedhack.activeFactor') }} <strong>{{ (store.speedhackStatus?.factor ?? 1).toFixed(2) }}x</strong></span>
        <span v-if="store.speedhackStatus?.installError" class="error">
          {{ $t('speedhack.installError') }}
        </span>
      </section>

      <section class="panel">
        <div class="slider-row">
          <span class="slider-label">0.1x</span>
          <input
            type="range"
            min="0.1"
            max="10"
            step="0.1"
            :value="store.speedhackFactor"
            class="speed-slider"
            @input="onSliderInput"
          />
          <span class="slider-label">10x</span>
          <span class="slider-value">{{ store.speedhackFactor.toFixed(1) }}x</span>
        </div>

        <div class="presets-row">
          <button
            v-for="preset in presets"
            :key="preset"
            class="btn btn-secondary compact"
            :class="{ active: store.speedhackFactor === preset }"
            @click="applyPreset(preset)"
          >
            {{ preset }}x
          </button>
          <button
            class="btn btn-secondary compact"
            :class="{ active: store.speedhackFactor === 0 }"
            :title="$t('speedhack.pauseTitle')"
            @click="applyPreset(0)"
          >
            {{ $t('speedhack.pause') }}
          </button>
        </div>

        <div class="actions-row">
          <button
            class="btn"
            :class="isActive ? 'btn-secondary' : 'btn-primary'"
            :disabled="store.speedhackBusy"
            @click="toggle"
          >
            {{ isActive ? $t('speedhack.disable') : $t('speedhack.enable') }}
          </button>
          <span class="hint">
            {{ $t('speedhack.injectionHintStart') }}
            GetTickCount, timeGetTime) {{ $t('speedhack.injectionHintEnd') }}
          </span>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.speedhack-view {
  padding: 24px 32px;
  max-width: 900px;
}

.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  color: var(--text-dim);
  margin-top: 4px;
}

.empty-state,
.status-band,
.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
}

.empty-state {
  padding: 32px;
  color: var(--text-secondary);
}

.status-band {
  display: flex;
  gap: 20px;
  padding: 14px;
  margin-bottom: 14px;
  color: var(--text-dim);
}

.status-band .error {
  color: var(--danger, #e5484d);
}

.panel {
  padding: 18px;
}

.slider-row {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 14px;
}

.slider-label {
  color: var(--text-dim);
  font-size: 12px;
  width: 32px;
}

.slider-value {
  color: var(--text-primary);
  font-weight: 600;
  width: 48px;
  text-align: right;
}

.speed-slider {
  flex: 1;
  accent-color: var(--accent);
}

.presets-row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 18px;
}

.presets-row .btn.active {
  border-color: var(--accent);
  color: var(--accent);
}

.actions-row {
  display: flex;
  align-items: center;
  gap: 14px;
}

.hint {
  color: var(--text-dim);
  font-size: 12px;
}
</style>
