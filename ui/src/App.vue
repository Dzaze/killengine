<script setup lang="ts">
import { computed, onMounted, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import AiWarmupSplash from '@/components/common/AiWarmupSplash.vue'
import AssistantView from '@/views/AssistantView.vue'
import ClrInspectorView from '@/views/ClrInspectorView.vue'
import ExpertView from '@/views/ExpertView.vue'
import InvestigationView from '@/views/InvestigationView.vue'
import LexiconView from '@/views/LexiconView.vue'
import MemoryView from '@/views/MemoryView.vue'
import ModulesView from '@/views/ModulesView.vue'
import MemoryTimelineView from '@/views/MemoryTimelineView.vue'
import MemoryHeatmapView from '@/views/MemoryHeatmapView.vue'
import PatternLearningView from '@/views/PatternLearningView.vue'
import NetworkView from '@/views/NetworkView.vue'
import ProfileView from '@/views/ProfileView.vue'
import ProcessView from '@/views/ProcessView.vue'
import ProjectView from '@/views/ProjectView.vue'
import ScriptingView from '@/views/ScriptingView.vue'
import SettingsView from '@/views/SettingsView.vue'
import SpeedhackView from '@/views/SpeedhackView.vue'
import TrainerView from '@/views/TrainerView.vue'
import WebView2InspectorView from '@/views/WebView2InspectorView.vue'

const store = useAppStore()
const { locale } = useI18n()

const currentView = computed(() => {
  if (store.activeView === 'assistant') return AssistantView
  if (store.activeView === 'process') return ProcessView
  if (store.activeView === 'investigation') return InvestigationView
  if (store.activeView === 'memory') return MemoryView
  if (store.activeView === 'memory-timeline') return MemoryTimelineView
  if (store.activeView === 'memory-heatmap') return MemoryHeatmapView
  if (store.activeView === 'pattern-learning') return PatternLearningView
  if (store.activeView === 'clr') return ClrInspectorView
  if (store.activeView === 'webview2') return WebView2InspectorView
  if (store.activeView === 'scripting') return ScriptingView
  if (store.activeView === 'speedhack') return SpeedhackView
  if (store.activeView === 'network') return NetworkView
  if (store.activeView === 'profiles') return ProfileView
  if (store.activeView === 'trainer') return TrainerView
  if (store.activeView === 'project') return ProjectView
  if (store.activeView === 'expert') return ExpertView
  if (store.activeView === 'lexicon') return LexiconView
  if (store.activeView === 'modules') return ModulesView
  if (store.activeView === 'settings') return SettingsView
  return ProcessView
})

// Sépare une éventuelle mise en garde "⚠ ..." de la fin du detail du risk-modal
// pour la mettre en évidence dans une couleur distincte (var(--warning)) —
// convention déjà utilisée ailleurs dans l'app (ex: ModulesView manualWarning).
const riskDialogDetailParts = computed(() => {
  const detail = store.riskDialog?.detail ?? ''
  const markerIndex = detail.indexOf('⚠')
  if (markerIndex === -1) return { text: detail, warning: '' }
  return {
    text: detail.slice(0, markerIndex).trim(),
    warning: detail.slice(markerIndex).trim(),
  }
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

      <div class="lang-switch">
        <div class="lang-switch-group" role="group" :aria-label="$t('nav.languageSwitchLabel')">
          <button :class="{ active: store.appLanguage === 'fr' }" @click="store.switchLanguage('fr')">FR</button>
          <button :class="{ active: store.appLanguage === 'en' }" @click="store.switchLanguage('en')">EN</button>
        </div>
        <button class="help-btn" :title="$t('nav.helpTitle')" :aria-label="$t('nav.helpTitle')" @click="store.openUserGuide()">
          {{ $t('nav.help') }}
        </button>
      </div>
      <!-- UX-PIPE-5 (docs/PHASE_TRACKER.md, 18/09/2026) : le switch rapide FR/EN
           change la langue Vue immédiatement (effectif pour la session) mais
           peut échouer à persister côté backend -- afficher l'échec ici, là où
           l'utilisateur vient d'agir, pas seulement tout en bas de Paramètres
           (page potentiellement pas même ouverte). -->
      <p v-if="store.languageSwitchError" class="lang-switch-error">
        {{ $t('nav.languageSaveFailed', { error: store.languageSwitchError }) }}
      </p>

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
          :class="{ active: store.activeView === 'modules' }"
          @click="store.activeView = 'modules'"
        >
          {{ $t('nav.modules') }}
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
          :class="{ active: store.activeView === 'memory-timeline' }"
          @click="store.activeView = 'memory-timeline'"
        >
          Timeline
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'memory-heatmap' }"
          @click="store.activeView = 'memory-heatmap'"
        >
          Heatmap
        </button>
        <button
          class="nav-item"
          :class="{ active: store.activeView === 'pattern-learning' }"
          @click="store.activeView = 'pattern-learning'"
        >
          Pattern Learning
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
          :class="{ active: store.activeView === 'webview2' }"
          @click="store.activeView = 'webview2'"
        >
          WebView2
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
          :class="{ active: store.activeView === 'project' }"
          @click="store.activeView = 'project'"
        >
          {{ $t('nav.project') }}
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
          :class="{ active: store.activeView === 'network' }"
          @click="store.activeView = 'network'"
        >
          {{ $t('nav.network') }}
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
          :class="{ active: store.activeView === 'lexicon' }"
          @click="store.activeView = 'lexicon'"
        >
          {{ $t('nav.lexicon') }}
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

    <AiWarmupSplash v-if="store.localAiWarmupVisible" />

    <div v-if="store.showOnboarding" class="risk-backdrop" role="presentation">
      <section class="onboarding-modal" role="dialog" aria-modal="true" aria-labelledby="onboarding-title">
        <h2 id="onboarding-title">{{ $t('app.onboarding.title') }}</h2>
        <p class="onboarding-intro">{{ $t('app.onboarding.intro') }}</p>
        <ol class="onboarding-steps">
          <li>
            <strong>{{ $t('app.onboarding.step1Title') }}</strong>
            {{ $t('app.onboarding.step1Detail') }}
          </li>
          <li>
            <strong>{{ $t('app.onboarding.step2Title') }}</strong>
            {{ $t('app.onboarding.step2Detail') }}
          </li>
          <li>
            <strong>{{ $t('app.onboarding.step3Title') }}</strong>
            {{ $t('app.onboarding.step3Detail') }}
          </li>
        </ol>
        <div class="onboarding-actions">
          <button type="button" class="risk-btn secondary" @click="store.openUserGuide()">
            {{ $t('app.onboarding.fullGuide') }}
          </button>
          <button type="button" class="risk-btn primary" @click="store.dismissOnboarding()">
            {{ $t('app.onboarding.start') }}
          </button>
        </div>
      </section>
    </div>

    <div v-if="store.riskDialog?.open" class="risk-backdrop" role="presentation">
      <section class="risk-modal" role="dialog" aria-modal="true" aria-labelledby="risk-title">
        <div class="risk-head">
          <span class="risk-pill">{{ store.riskDialog.risk }}</span>
        </div>
        <h2 id="risk-title">{{ store.riskDialog.title }}</h2>
        <p class="risk-detail">{{ riskDialogDetailParts.text }}</p>
        <p v-if="riskDialogDetailParts.warning" class="risk-detail-highlight">{{ riskDialogDetailParts.warning }}</p>
        <p class="risk-warning">
          {{ $t('app.riskGate.genericWarning') }}
        </p>
        <label v-if="store.riskDialog.rememberKey" class="risk-remember">
          <input v-model="store.riskDialog.rememberChoice" type="checkbox" />
          <span>{{ store.riskDialog.rememberLabel }}</span>
        </label>
        <div class="risk-actions">
          <button type="button" class="risk-btn secondary" @click="store.resolveRiskDialog(false)">
            {{ $t('app.riskGate.cancel') }}
          </button>
          <button type="button" class="risk-btn primary" @click="store.resolveRiskDialog(true)">
            {{ $t('app.riskGate.confirm') }}
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
  background:
    linear-gradient(180deg, rgba(18, 20, 32, 0.98) 0%, rgba(13, 15, 24, 0.98) 100%);
  border-right: 1px solid rgba(125, 142, 255, 0.16);
  box-shadow: 12px 0 28px rgba(0, 0, 0, 0.22);
  display: flex;
  flex-direction: column;
  flex-shrink: 0;
}

.logo {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 22px 16px 20px;
  border-bottom: 1px solid rgba(125, 142, 255, 0.14);
  background:
    linear-gradient(135deg, rgba(122, 162, 247, 0.1), rgba(255, 158, 100, 0.08) 54%, transparent);
}

.logo-icon {
  font-size: 24px;
  filter: drop-shadow(0 0 10px rgba(255, 158, 100, 0.35));
}

.logo-text {
  font-size: 18px;
  font-weight: 700;
  color: #9fbdff;
  text-shadow: 0 0 18px rgba(122, 162, 247, 0.32);
}

.lang-switch {
  display: flex;
  gap: 4px;
  padding: 10px 16px 14px;
  border-bottom: 1px solid rgba(125, 142, 255, 0.14);
}

.lang-switch-error {
  margin: -6px 16px 10px;
  padding: 6px 8px;
  border-radius: 6px;
  background: color-mix(in srgb, var(--error) 12%, transparent);
  color: var(--error);
  font-size: 11px;
  line-height: 1.3;
}

.lang-switch-group {
  display: flex;
  gap: 4px;
  flex: 1;
}

.lang-switch button {
  flex: 1;
  padding: 6px 0;
  border: 1px solid rgba(125, 142, 255, 0.24);
  border-radius: 6px;
  background: transparent;
  color: #7580b3;
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
  transition: background 0.15s, color 0.15s, border-color 0.15s;
}

.lang-switch button.help-btn {
  flex: 0 0 auto;
  padding: 6px 10px;
}

.lang-switch button:hover {
  color: #c7d2ff;
  border-color: rgba(125, 142, 255, 0.42);
}

.lang-switch button.active {
  background: rgba(122, 162, 247, 0.18);
  color: #9fbdff;
  border-color: rgba(122, 162, 247, 0.5);
}

.nav {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: 14px 8px;
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.nav-item {
  position: relative;
  background: transparent;
  border: 1px solid transparent;
  color: #8793c8;
  padding: 10px 14px 10px 16px;
  text-align: left;
  border-radius: 6px;
  cursor: pointer;
  font-size: 14px;
  font-weight: 500;
  transition: background-color 0.16s ease, border-color 0.16s ease, color 0.16s ease, transform 0.16s ease;
}

.nav-item:hover:not(:disabled) {
  background: rgba(122, 162, 247, 0.1);
  border-color: rgba(122, 162, 247, 0.2);
  color: #d7e1ff;
  transform: translateX(2px);
}

.nav-item.active {
  background:
    linear-gradient(90deg, rgba(122, 162, 247, 0.24), rgba(122, 162, 247, 0.1));
  border-color: rgba(122, 162, 247, 0.28);
  color: #8fb6ff;
  box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.025), 0 8px 22px rgba(0, 0, 0, 0.16);
}

.nav-item.active::before {
  content: '';
  position: absolute;
  left: 7px;
  top: 9px;
  bottom: 9px;
  width: 3px;
  border-radius: 999px;
  background: linear-gradient(180deg, #ff9e64, #7aa2f7);
  box-shadow: 0 0 12px rgba(122, 162, 247, 0.6);
}

.nav-item:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.sidebar-footer {
  padding: 12px 16px;
  border-top: 1px solid rgba(125, 142, 255, 0.14);
  background: rgba(10, 12, 20, 0.52);
}

.version {
  font-size: 11px;
  color: #7580b3;
  margin-bottom: 4px;
}

.credits {
  font-size: 11px;
  color: #a9b8ff;
  margin-bottom: 4px;
}

.status {
  font-size: 12px;
  color: #8793c8;
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

.risk-detail-highlight {
  margin-top: 8px;
  padding: 8px 10px;
  border-radius: 6px;
  border: 1px solid color-mix(in srgb, var(--warning) 40%, var(--border));
  background: color-mix(in srgb, var(--warning) 10%, var(--bg-tertiary));
  color: var(--warning);
  font-weight: 600;
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
