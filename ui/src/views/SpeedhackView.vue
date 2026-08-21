<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { useAppStore } from '@/stores/app'
import InfoDot from '@/components/expert/InfoDot.vue'

const store = useAppStore()

const presets = [0.25, 0.5, 1, 2, 4, 10]

const isActive = computed(() => store.speedhackStatus?.active === true)
const riskModeBlocksSpeedhack = computed(() => store.settingAutoRiskMode !== 'Trainer')
const statusLabel = computed(() => {
  if (!store.speedhackStatus) return 'Inconnu'
  if (store.speedhackStatus.installError) return 'Échec install'
  return isActive.value ? 'Actif' : 'Inactif'
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

function enableTrainerMode() {
  store.settingAutoRiskMode = 'Trainer'
  store.lastRiskBlockReason = ''
}

onMounted(() => {
  void store.refreshSpeedhackStatus()
})
</script>

<template>
  <div class="speedhack-view">
    <div class="header">
      <div>
        <h1>Speedhack</h1>
        <p>{{ store.isAttached ? store.processName : 'Aucun processus attaché' }}</p>
      </div>
      <InfoDot topic="speedhack" align="right" />
    </div>

    <div v-if="!store.isAttached" class="empty-state">
      <p>Attache d'abord un processus autorisé pour accélérer ou ralentir le temps qu'il perçoit.</p>
      <button class="btn btn-secondary" @click="store.activeView = 'process'">Aller à Processus</button>
    </div>

    <template v-else>
      <section class="status-band">
        <span>Statut : <strong>{{ statusLabel }}</strong></span>
        <span>Facteur actif : <strong>{{ (store.speedhackStatus?.factor ?? 1).toFixed(2) }}x</strong></span>
        <span>Mode Auto : <strong>{{ store.settingAutoRiskMode }}</strong></span>
        <span v-if="store.speedhackStatus?.installError" class="error">
          Aucune fonction de temps n'a pu être hookée dans cette cible.
        </span>
      </section>

      <section v-if="riskModeBlocksSpeedhack && !isActive" class="warning-band">
        <div>
          <strong>Injection bloquée par le mode Auto.</strong>
          <span>Le speedhack injecte un composant dans la cible : passe en Trainer pour autoriser ce palier.</span>
        </div>
        <button class="btn btn-secondary compact" @click="enableTrainerMode">
          Passer en Trainer
        </button>
      </section>

      <section v-if="store.lastRiskBlockReason" class="warning-band muted">
        {{ store.lastRiskBlockReason }}
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
            title="Gèle l'horloge perçue par la cible — aucune animation/minuteur ne progresse tant que c'est actif."
            @click="applyPreset(0)"
          >
            Pause (0x)
          </button>
        </div>

        <div class="actions-row">
          <button
            class="btn"
            :class="isActive ? 'btn-secondary' : 'btn-primary'"
            :disabled="store.speedhackBusy"
            :title="riskModeBlocksSpeedhack && !isActive ? 'Le mode Auto actuel bloque les injections. Passe en Trainer pour lancer le speedhack.' : ''"
            @click="toggle"
          >
            {{ isActive ? 'Désactiver' : 'Activer' }}
          </button>
          <span class="hint">
            Injecte un composant dans la cible qui hooke ses fonctions de temps (QueryPerformanceCounter,
            GetTickCount, timeGetTime) — traité comme une écriture par injection, confirmation requise.
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

.warning-band {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  border: 1px solid rgba(245, 158, 11, 0.45);
  background: rgba(245, 158, 11, 0.12);
  border-radius: 8px;
  padding: 12px 14px;
  margin-bottom: 14px;
  color: var(--text-primary);
}

.warning-band div {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.warning-band span,
.warning-band.muted {
  color: var(--text-dim);
  font-size: 12px;
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
