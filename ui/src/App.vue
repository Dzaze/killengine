<script setup lang="ts">
import { computed, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import AssistantView from '@/views/AssistantView.vue'
import ClrInspectorView from '@/views/ClrInspectorView.vue'
import ExpertView from '@/views/ExpertView.vue'
import InvestigationView from '@/views/InvestigationView.vue'
import MemoryView from '@/views/MemoryView.vue'
import ProfileView from '@/views/ProfileView.vue'
import ProcessView from '@/views/ProcessView.vue'
import ScriptingView from '@/views/ScriptingView.vue'
import SettingsView from '@/views/SettingsView.vue'
import SpeedhackView from '@/views/SpeedhackView.vue'
import TrainerView from '@/views/TrainerView.vue'

const store = useAppStore()
const { locale } = useI18n()

const currentView = computed(() => {
  if (store.activeView === 'process') return ProcessView
  if (store.activeView === 'investigation') return InvestigationView
  if (store.activeView === 'memory') return MemoryView
  if (store.activeView === 'clr') return ClrInspectorView
  if (store.activeView === 'scripting') return ScriptingView
  if (store.activeView === 'speedhack') return SpeedhackView
  if (store.activeView === 'profiles') return ProfileView
  if (store.activeView === 'trainer') return TrainerView
  if (store.activeView === 'expert') return ExpertView
  if (store.activeView === 'settings') return SettingsView
  return AssistantView
})

onMounted(() => {
  store.init()
})

watch(
  () => store.appLanguage,
  (language) => {
    locale.value = language
  },
)
</script>

<template>
  <div class="app">
    <!-- Sidebar -->
    <aside class="sidebar">
      <div class="logo">
        <span class="logo-icon">⚡</span>
        <span class="logo-text">KillEngine</span>
      </div>

      <nav class="nav">
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'assistant' }"
          @click="store.activeView = 'assistant'"
        >
          {{ $t('nav.assistant') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'process' }"
          @click="store.activeView = 'process'"
        >
          {{ $t('nav.process') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'investigation' }"
          @click="store.activeView = 'investigation'"
        >
          {{ $t('nav.investigation') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'memory' }"
          @click="store.activeView = 'memory'"
        >
          {{ $t('nav.memory') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'clr' }"
          @click="store.activeView = 'clr'"
        >
          CLR
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'scripting' }"
          @click="store.activeView = 'scripting'"
        >
          {{ $t('nav.scripting') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'profiles' }"
          @click="store.activeView = 'profiles'"
        >
          {{ $t('nav.profiles') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'trainer' }"
          @click="store.activeView = 'trainer'"
        >
          {{ $t('nav.trainer') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'speedhack' }"
          @click="store.activeView = 'speedhack'"
        >
          {{ $t('nav.speedhack') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'expert' }"
          @click="store.activeView = 'expert'"
        >
          {{ $t('nav.expert') }}
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'settings' }"
          @click="store.activeView = 'settings'"
        >
          {{ $t('nav.settings') }}
        </button>
      </nav>

      <div class="sidebar-footer">
        <div class="mode-switch" role="group" aria-label="Mode interface">
          <button
            type="button"
            :class="{ active: store.uiMode === 'beginner' }"
            @click="store.uiMode = 'beginner'"
          >
            Débutant
          </button>
          <button
            type="button"
            :class="{ active: store.uiMode === 'expert' }"
            @click="store.uiMode = 'expert'"
          >
            Expert
          </button>
        </div>
        <div class="credits">Pirolley Benoist</div>
        <div class="version">v{{ store.version }}</div>
        <div class="status" :class="{ connected: store.isConnected }">
          ● {{ store.statusText }}
        </div>
      </div>
    </aside>

    <!-- Main content -->
    <main class="main">
      <component :is="currentView" />
    </main>

    <div v-if="store.showOnboarding" class="risk-backdrop" role="presentation">
      <section class="onboarding-modal" role="dialog" aria-modal="true" aria-labelledby="onboarding-title">
        <h2 id="onboarding-title">Bienvenue dans KillEngine</h2>
        <p class="onboarding-intro">Trois choses à savoir pour démarrer :</p>
        <ol class="onboarding-steps">
          <li>
            <strong>Attache un processus.</strong>
            Onglet Processus, choisis l'application que tu veux analyser.
          </li>
          <li>
            <strong>Décris ce que tu cherches à l'Assistant.</strong>
            Une valeur affichée à l'écran suffit — pas besoin de connaître les types de données ou les scans.
          </li>
          <li>
            <strong>Transforme une trouvaille en Trainer.</strong>
            Une fois une valeur confirmée, sauvegarde-la comme feature réutilisable avec un raccourci clavier.
          </li>
        </ol>
        <div class="onboarding-actions">
          <button type="button" class="risk-btn secondary" @click="store.openUserGuide()">
            Guide complet
          </button>
          <button type="button" class="risk-btn primary" @click="store.dismissOnboarding()">
            Commencer
          </button>
        </div>
      </section>
    </div>

    <div v-if="store.riskDialog?.open" class="risk-backdrop" role="presentation">
      <section class="risk-modal" role="dialog" aria-modal="true" aria-labelledby="risk-title">
        <div class="risk-head">
          <span class="risk-pill">{{ store.riskDialog.risk }}</span>
          <span>Mode {{ store.riskDialog.mode }}</span>
        </div>
        <h2 id="risk-title">{{ store.riskDialog.title }}</h2>
        <p class="risk-detail">{{ store.riskDialog.detail }}</p>
        <p class="risk-warning">
          Cette action modifie ou observe activement un processus local. Confirme uniquement si tu contrôles ce processus et acceptes le risque.
        </p>
        <label v-if="store.riskDialog.rememberKey" class="risk-remember">
          <input v-model="store.riskDialog.rememberChoice" type="checkbox" />
          <span>{{ store.riskDialog.rememberLabel }}</span>
        </label>
        <div class="risk-actions">
          <button type="button" class="risk-btn secondary" @click="store.resolveRiskDialog(false)">
            Annuler
          </button>
          <button type="button" class="risk-btn primary" @click="store.resolveRiskDialog(true)">
            Confirmer
          </button>
        </div>
      </section>
    </div>
  </div>
</template>

<style>
:root {
  --bg-primary: #1a1b26;
  --bg-secondary: #16171f;
  --bg-tertiary: #1f2031;
  --bg-accent: #2a2b3d;
  --text-primary: #c0caf5;
  --text-secondary: #7aa2f7;
  --text-dim: #565f89;
  --accent: #7aa2f7;
  --accent-hover: #89b4fa;
  --success: #9ece6a;
  --warning: #e0af68;
  --error: #f7768e;
  --border: #2a2b3d;
}

* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

html, body, #app {
  height: 100%;
  overflow: hidden;
}

body {
  font-family: 'Segoe UI', 'Cascadia Code', sans-serif;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
}

.app {
  display: flex;
  height: 100vh;
}

.sidebar {
  width: 220px;
  background: var(--bg-secondary);
  border-right: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
}

.logo {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 20px 16px;
  border-bottom: 1px solid var(--border);
}

.logo-icon {
  font-size: 24px;
}

.logo-text {
  font-size: 18px;
  font-weight: 700;
  color: var(--accent);
}

.nav {
  flex: 1;
  padding: 12px 8px;
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.nav-item {
  background: transparent;
  border: none;
  color: var(--text-dim);
  padding: 10px 16px;
  text-align: left;
  border-radius: 6px;
  cursor: pointer;
  font-size: 14px;
  transition: all 0.15s;
}

.nav-item:hover:not(:disabled) {
  background: var(--bg-accent);
  color: var(--text-primary);
}

.nav-item.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.nav-item:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sidebar-footer {
  padding: 12px 16px;
  border-top: 1px solid var(--border);
}

.mode-switch {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 4px;
  margin-bottom: 10px;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.mode-switch button {
  min-width: 0;
  padding: 5px 6px;
  border: none;
  border-radius: 4px;
  background: transparent;
  color: var(--text-dim);
  cursor: pointer;
  font-size: 11px;
}

.mode-switch button.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.version {
  font-size: 11px;
  color: var(--text-dim);
  margin-bottom: 4px;
}

.credits {
  font-size: 11px;
  color: var(--text-secondary);
  margin-bottom: 4px;
}

.status {
  font-size: 12px;
  color: var(--text-dim);
}

.status.connected {
  color: var(--success);
}

.main {
  flex: 1;
  overflow-y: auto;
}

.risk-backdrop {
  position: fixed;
  inset: 0;
  z-index: 1000;
  display: grid;
  place-items: center;
  padding: 24px;
  background: rgba(8, 9, 14, 0.72);
}

.risk-modal {
  width: min(520px, 100%);
  border: 1px solid rgba(224, 175, 104, 0.36);
  border-radius: 8px;
  background: var(--bg-secondary);
  box-shadow: 0 24px 70px rgba(0, 0, 0, 0.46);
  padding: 18px;
}

.risk-head {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.risk-pill {
  border: 1px solid rgba(224, 175, 104, 0.45);
  border-radius: 999px;
  color: var(--warning);
  padding: 3px 8px;
}

.risk-modal h2 {
  color: var(--text-primary);
  font-size: 18px;
}

.risk-detail {
  margin-top: 10px;
  color: var(--text-secondary);
  overflow-wrap: anywhere;
}

.risk-warning {
  margin-top: 12px;
  color: var(--text-dim);
  line-height: 1.5;
}

.risk-remember {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 14px;
  color: var(--text-secondary);
  font-size: 13px;
  cursor: pointer;
}

.risk-remember input {
  width: 16px;
  height: 16px;
  accent-color: var(--warning);
}

.onboarding-modal {
  width: min(520px, 100%);
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
  box-shadow: 0 24px 70px rgba(0, 0, 0, 0.46);
  padding: 20px;
}

.onboarding-modal h2 {
  color: var(--text-primary);
  font-size: 18px;
}

.onboarding-intro {
  margin-top: 8px;
  color: var(--text-dim);
}

.onboarding-steps {
  margin: 14px 0 4px;
  padding-left: 20px;
  color: var(--text-secondary);
  line-height: 1.55;
}

.onboarding-steps li + li {
  margin-top: 10px;
}

.onboarding-steps strong {
  display: block;
  color: var(--text-primary);
}

.onboarding-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 18px;
}

.risk-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 18px;
}

.risk-btn {
  border: 1px solid var(--border);
  border-radius: 6px;
  cursor: pointer;
  padding: 8px 12px;
}

.risk-btn.secondary {
  background: var(--bg-tertiary);
  color: var(--text-secondary);
}

.risk-btn.primary {
  border-color: rgba(224, 175, 104, 0.55);
  background: var(--warning);
  color: #101219;
}
</style>
