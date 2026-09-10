<script setup lang="ts">
/**
 * Vue d'inspection WebView2/CDP
 * Calquée sur ClrInspectorView.vue - liste des targets CDP,
 * connexion/déconnexion, évaluation JS, recherche DOM.
 */
import { onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import { useWebView2InspectorStore } from '@/stores/webView2Inspector'
import type { WebView2GlobalEntry } from '@/services/backend'
import PanelIntro from '@/components/common/PanelIntro.vue'

const { t } = useI18n()
const appStore = useAppStore()
const store = useWebView2InspectorStore()

const findValueInput = ref('')
const findTextInput = ref('')

onMounted(() => {
  // Rafraîchir le statut au montage
  store.refreshStatus()
})

async function handleListTargets() {
  await store.listTargets()
}

async function handleConnect() {
  if (!appStore.isAttached) {
    appStore.addActionLog('webview2', t('webview2.error.notAttached'), '', 'error')
    return
  }

  // RiskGate pour la connexion à un process externe (même niveau que debug)
  const confirmed = await appStore.confirmRiskAction(
    'debug',
    t('webview2.risk.connectTitle'),
    t('webview2.risk.connectMessage'),
  )
  if (!confirmed) return

  const success = await store.connect()
  if (success) {
    appStore.addActionLog('webview2', t('webview2.success.connected'), '', 'success')
  } else {
    appStore.addActionLog('webview2', store.error || t('webview2.error.connectFailed'), '', 'error')
  }
}

async function handleDisconnect() {
  const success = await store.disconnect()
  if (success) {
    appStore.addActionLog('webview2', t('webview2.success.disconnected'), '', 'success')
  } else {
    appStore.addActionLog('webview2', store.error || t('webview2.error.disconnectFailed'), '', 'error')
  }
}

async function handleEvaluate() {
  if (!store.evaluateScript.trim()) return

  // RiskGate pour l'évaluation JS arbitraire (même niveau que injection car exécute du code)
  const confirmed = await appStore.confirmRiskAction(
    'injection',
    t('webview2.risk.evaluateTitle'),
    t('webview2.risk.evaluateMessage'),
  )
  if (!confirmed) return

  const result = await store.evaluateJavaScript()
  if (result?.success) {
    appStore.addActionLog('webview2', t('webview2.success.evaluated'), '', 'success')
  } else {
    appStore.addActionLog('webview2', store.error || t('webview2.error.evaluateFailed'), '', 'error')
  }
}

async function handleProbeGlobalScope() {
  const result = await store.probeGlobalScope()
  if (result?.success) {
    appStore.addActionLog('webview2', t('webview2.probe.success'), '', 'success')
  } else {
    appStore.addActionLog('webview2', store.error || t('webview2.probe.error'), '', 'error')
  }
}

// Construit une expression d'inspection adaptee au type CDP de la globale,
// plutot que de deviner du JS a l'aveugle (objectif de WEBVIEW-F). Utilise
// la notation crochet (window["nom"]) plutot que l'acces par point : certains
// noms observes ne sont pas des identifiants JS valides (ex: "0", "1" pour
// des references de frame, ou des cles Symbol()).
function buildProbeExpression(entry: WebView2GlobalEntry): string {
  const key = JSON.stringify(entry.name)
  if (entry.type === 'function') {
    return `window[${key}].toString().slice(0, 500)`
  }
  if (entry.type === 'object') {
    return `JSON.stringify(Object.keys(window[${key}] || {}))`
  }
  return `window[${key}]`
}

function exploreGlobal(entry: WebView2GlobalEntry) {
  store.evaluateScript = buildProbeExpression(entry)
}

async function handleFindValues() {
  if (!findValueInput.value.trim()) return
  await store.findDisplayedValues(findValueInput.value)
}

async function handleFindText() {
  if (!findTextInput.value.trim()) return
  await store.findDisplayedText(findTextInput.value)
}

function handleReset() {
  store.reset()
}
</script>

<template>
  <div class="webview2-inspector-view">
    <PanelIntro
      :what="$t('webview2.intro.what')"
      :purpose="$t('webview2.intro.purpose')"
      :how="$t('webview2.intro.how')"
    />

    <!-- Statut et contrôles principaux -->
    <div class="section">
      <div class="section-header">
        <h3>{{ $t('webview2.status.title') }}</h3>
        <span
          class="status-badge"
          :class="{ connected: store.isConnected, disconnected: !store.isConnected }"
        >
          {{ store.isConnected ? $t('webview2.status.connected') : $t('webview2.status.disconnected') }}
        </span>
      </div>

      <div class="controls-row">
        <button
          class="btn btn-secondary"
          :disabled="store.isConnecting || !appStore.isAttached"
          @click="handleListTargets"
        >
          {{ $t('webview2.actions.listTargets') }}
        </button>
        <button
          class="btn btn-primary"
          :disabled="!store.canConnect || store.isConnecting || !appStore.isAttached"
          @click="handleConnect"
        >
          {{ store.isConnecting ? $t('webview2.actions.connecting') : $t('webview2.actions.connect') }}
        </button>
        <button
          class="btn btn-secondary"
          :disabled="!store.isConnected"
          @click="handleDisconnect"
        >
          {{ $t('webview2.actions.disconnect') }}
        </button>
        <button
          class="btn btn-ghost"
          @click="handleReset"
        >
          {{ $t('webview2.actions.reset') }}
        </button>
      </div>

      <div
        v-if="!appStore.isAttached"
        class="warning-message"
      >
        {{ $t('webview2.warning.notAttached') }}
      </div>

      <div
        v-if="store.error"
        class="error-message"
      >
        {{ store.error }}
      </div>
    </div>

    <!-- Liste des targets -->
    <div
      v-if="store.targets.length > 0"
      class="section"
    >
      <h3>{{ $t('webview2.targets.title') }}</h3>
      <div class="targets-list">
        <div
          v-for="target in store.targets"
          :key="target.id"
          class="target-item"
          :class="{ selected: store.selectedTargetId === target.id }"
          @click="store.selectTarget(target.id)"
        >
          <div class="target-info">
            <span class="target-id">{{ target.id }}</span>
            <span
              v-if="target.title"
              class="target-title"
            >{{ target.title }}</span>
            <span
              v-if="target.url"
              class="target-url"
            >{{ target.url }}</span>
            <span
              v-if="target.type"
              class="target-type"
            >{{ target.type }}</span>
          </div>
          <input
            type="radio"
            name="webview2-target"
            :checked="store.selectedTargetId === target.id"
            @click.stop="store.selectTarget(target.id)"
          >
        </div>
      </div>
    </div>

    <!-- Reconnaissance du scope global -->
    <div
      v-if="store.isConnected"
      class="section"
    >
      <h3>{{ $t('webview2.probe.title') }}</h3>
      <p class="hint">{{ $t('webview2.probe.hint') }}</p>
      <div class="controls-row">
        <button
          class="btn btn-secondary"
          :disabled="store.isProbingGlobalScope"
          @click="handleProbeGlobalScope"
        >
          {{ store.isProbingGlobalScope ? $t('webview2.probe.running') : $t('webview2.probe.run') }}
        </button>
      </div>
      <div v-if="store.globalScopeResult" class="result-panel">
        <template v-if="store.globalScopeResult.success">
          <p class="hint">
            {{ $t('webview2.probe.summary', {
              count: store.globalScopeResult.customGlobalsCount ?? 0,
              total: store.globalScopeResult.totalGlobalsSeen ?? 0,
              mode: store.globalScopeResult.baselineMode === 'dynamic_about_blank'
                ? $t('webview2.probe.modeDynamic')
                : $t('webview2.probe.modeStatic'),
            }) }}
          </p>
          <div class="results-list">
            <div
              v-for="(entry, index) in store.globalScopeResult.customGlobals"
              :key="index"
              class="result-item"
            >
              <div class="result-header">
                <span class="result-selector">{{ entry.name }}</span>
                <span v-if="entry.type" class="result-tag">{{ entry.type }}</span>
                <button
                  class="btn btn-ghost mini explore-btn"
                  @click="exploreGlobal(entry)"
                >
                  {{ $t('webview2.probe.explore') }}
                </button>
              </div>
            </div>
          </div>
          <p v-if="store.globalScopeResult.media" class="hint">
            {{ $t('webview2.probe.media', {
              video: store.globalScopeResult.media.video ?? 0,
              audio: store.globalScopeResult.media.audio ?? 0,
              iframes: store.globalScopeResult.media.iframes?.length ?? 0,
            }) }}
          </p>
        </template>
        <div v-else class="result-error">
          {{ store.globalScopeResult.error || $t('webview2.probe.error') }}
        </div>
      </div>
    </div>

    <!-- Évaluation JavaScript -->
    <div
      v-if="store.isConnected"
      class="section"
    >
      <h3>{{ $t('webview2.evaluate.title') }}</h3>
      <div class="evaluate-section">
        <textarea
          v-model="store.evaluateScript"
          class="script-input"
          :placeholder="$t('webview2.evaluate.placeholder')"
          rows="4"
        />
        <button
          class="btn btn-primary"
          :disabled="!store.canEvaluate"
          @click="handleEvaluate"
        >
          {{ store.isEvaluating ? $t('webview2.evaluate.running') : $t('webview2.evaluate.run') }}
        </button>
      </div>

      <!-- Résultat de l'évaluation -->
      <div
        v-if="store.evaluateResult"
        class="result-panel"
      >
        <h4>{{ $t('webview2.evaluate.result') }}</h4>
        <div
          v-if="store.evaluateResult.success"
          class="result-success"
        >
          <pre v-if="store.evaluateResult.value !== undefined">{{ JSON.stringify(store.evaluateResult.value, null, 2) }}</pre>
          <span v-else>{{ $t('webview2.evaluate.noResult') }}</span>
        </div>
        <div
          v-else
          class="result-error"
        >
          {{ store.evaluateResult.error || $t('webview2.evaluate.error') }}
        </div>
      </div>
    </div>

    <!-- Recherche DOM -->
    <div
      v-if="store.isConnected"
      class="section"
    >
      <h3>{{ $t('webview2.find.title') }}</h3>

      <!-- Recherche par valeur -->
      <div class="find-section">
        <label>{{ $t('webview2.find.byValue') }}</label>
        <div class="find-row">
          <input
            v-model="findValueInput"
            type="text"
            :placeholder="$t('webview2.find.valuePlaceholder')"
            @keyup.enter="handleFindValues"
          >
          <button
            class="btn btn-secondary"
            :disabled="store.isFinding || !findValueInput.trim()"
            @click="handleFindValues"
          >
            {{ store.isFinding ? $t('webview2.find.searching') : $t('webview2.find.search') }}
          </button>
        </div>
      </div>

      <!-- Recherche par texte -->
      <div class="find-section">
        <label>{{ $t('webview2.find.byText') }}</label>
        <div class="find-row">
          <input
            v-model="findTextInput"
            type="text"
            :placeholder="$t('webview2.find.textPlaceholder')"
            @keyup.enter="handleFindText"
          >
          <button
            class="btn btn-secondary"
            :disabled="store.isFinding || !findTextInput.trim()"
            @click="handleFindText"
          >
            {{ store.isFinding ? $t('webview2.find.searching') : $t('webview2.find.search') }}
          </button>
        </div>
      </div>

      <!-- Résultats de recherche -->
      <div
        v-if="store.findResults.length > 0"
        class="results-section"
      >
        <h4>{{ $t('webview2.find.results', { count: store.findResults.length }) }}</h4>
        <div class="results-list">
          <div
            v-for="(result, index) in store.findResults"
            :key="index"
            class="result-item"
          >
            <div class="result-header">
              <span class="result-selector">{{ result.selector || result.nodeId }}</span>
              <span
                v-if="result.tagName"
                class="result-tag"
              >{{ result.tagName }}</span>
            </div>
            <div
              v-if="result.textContent"
              class="result-content"
            >
              {{ result.textContent.substring(0, 200) }}
              <span v-if="result.textContent.length > 200">...</span>
            </div>
            <div
              v-if="result.value !== undefined"
              class="result-value"
            >
              {{ result.value }}
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.webview2-inspector-view {
  padding: 24px 18px 32px;
  max-width: 1240px;
  margin: 0 auto;
}

.section {
  position: relative;
  overflow: hidden;
  background:
    linear-gradient(180deg, rgba(31, 32, 49, 0.82), rgba(22, 23, 31, 0.96));
  border-radius: 8px;
  padding: 18px;
  margin-bottom: 16px;
  border: 1px solid rgba(122, 162, 247, 0.16);
  box-shadow: 0 14px 34px rgba(0, 0, 0, 0.18);
}

.section-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
}

.section h3 {
  margin: 0 0 16px 0;
  font-size: 1.1rem;
  color: var(--text-primary);
  letter-spacing: 0;
}

.section h4 {
  margin: 1rem 0 0.5rem 0;
  font-size: 1rem;
  color: var(--text-secondary);
}

.status-badge {
  display: inline-flex;
  align-items: center;
  min-height: 28px;
  padding: 0.25rem 0.8rem;
  border-radius: 999px;
  font-size: 0.875rem;
  font-weight: 700;
  border: 1px solid currentColor;
  background: rgba(31, 32, 49, 0.72);
}

.status-badge.connected {
  color: var(--success);
  box-shadow: 0 0 18px rgba(158, 206, 106, 0.12);
}

.status-badge.disconnected {
  color: var(--warning);
  box-shadow: 0 0 18px rgba(224, 175, 104, 0.12);
}

.controls-row {
  display: flex;
  gap: 8px;
  flex-wrap: wrap;
  margin-bottom: 14px;
}

.btn {
  min-height: 34px;
  padding: 0.5rem 1rem;
  border-radius: 6px;
  border: 1px solid transparent;
  cursor: pointer;
  font-size: 0.875rem;
  font-weight: 650;
  transition: transform 0.16s ease, border-color 0.16s ease, background-color 0.16s ease, opacity 0.16s ease;
}

.btn:hover:not(:disabled) {
  transform: translateY(-1px);
}

.btn:disabled {
  opacity: 0.48;
  cursor: not-allowed;
}

.btn-primary {
  background: linear-gradient(180deg, #7aa2f7, #5d7fca);
  color: #101219;
  box-shadow: 0 10px 22px rgba(122, 162, 247, 0.18);
}

.btn-secondary {
  background: rgba(31, 32, 49, 0.9);
  color: var(--text-primary);
  border-color: rgba(122, 162, 247, 0.18);
}

.btn-secondary:hover:not(:disabled) {
  border-color: rgba(122, 162, 247, 0.42);
  background: rgba(42, 43, 61, 0.94);
}

.btn-ghost {
  background: transparent;
  color: var(--text-secondary);
  border-color: transparent;
}

.btn-ghost:hover:not(:disabled) {
  background: rgba(122, 162, 247, 0.1);
  border-color: rgba(122, 162, 247, 0.2);
}

.btn.mini {
  padding: 0.15rem 0.5rem;
  font-size: 0.75rem;
}

.explore-btn {
  margin-left: auto;
  flex-shrink: 0;
}

.warning-message {
  padding: 0.8rem 0.9rem;
  background: rgba(224, 175, 104, 0.1);
  color: var(--warning);
  border: 1px solid rgba(224, 175, 104, 0.22);
  border-radius: 6px;
  margin-top: 0.5rem;
}

.error-message {
  padding: 0.8rem 0.9rem;
  background: rgba(247, 118, 142, 0.1);
  color: var(--error);
  border: 1px solid rgba(247, 118, 142, 0.24);
  border-radius: 6px;
  margin-top: 0.5rem;
}

.targets-list {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  max-height: 300px;
  overflow-y: auto;
}

.target-item {
  position: relative;
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 14px;
  padding: 0.85rem 0.9rem;
  border: 1px solid rgba(122, 162, 247, 0.14);
  border-radius: 7px;
  background: rgba(26, 27, 38, 0.7);
  cursor: pointer;
  transition: background 0.16s ease, border-color 0.16s ease, transform 0.16s ease, box-shadow 0.16s ease;
}

.target-item:hover {
  background: rgba(31, 32, 49, 0.95);
  border-color: rgba(122, 162, 247, 0.34);
  transform: translateX(2px);
}

.target-item.selected {
  border-color: rgba(122, 162, 247, 0.58);
  background: linear-gradient(90deg, rgba(122, 162, 247, 0.18), rgba(31, 32, 49, 0.88));
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.18);
}

.target-item.selected::before {
  content: '';
  position: absolute;
  left: 0;
  top: 8px;
  bottom: 8px;
  width: 3px;
  border-radius: 0 999px 999px 0;
  background: linear-gradient(180deg, #ff9e64, #7aa2f7);
}

.target-info {
  display: flex;
  flex-direction: column;
  gap: 0.25rem;
  flex: 1;
  min-width: 0;
}

.target-id {
  font-family: monospace;
  font-size: 0.875rem;
  color: var(--text-secondary);
  overflow-wrap: anywhere;
}

.target-title {
  font-weight: 500;
  color: var(--text-primary);
}

.target-url {
  font-size: 0.75rem;
  color: var(--text-dim);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.target-type {
  width: fit-content;
  font-size: 0.75rem;
  color: var(--text-dim);
  text-transform: uppercase;
  padding: 0.1rem 0.4rem;
  border-radius: 999px;
  background: rgba(122, 162, 247, 0.08);
}

.evaluate-section {
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
}

.script-input {
  width: 100%;
  padding: 0.75rem;
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 7px;
  background: rgba(26, 27, 38, 0.86);
  color: var(--text-primary);
  font-family: monospace;
  font-size: 0.875rem;
  line-height: 1.55;
  resize: vertical;
  outline: none;
}

.script-input:focus,
.find-row input:focus {
  border-color: rgba(122, 162, 247, 0.65);
  box-shadow: 0 0 0 3px rgba(122, 162, 247, 0.12);
}

.result-panel {
  margin-top: 1rem;
  padding: 1rem;
  background: rgba(31, 32, 49, 0.82);
  border-radius: 7px;
  border: 1px solid rgba(122, 162, 247, 0.15);
}

.result-success pre {
  margin: 0;
  padding: 0.75rem;
  background: rgba(16, 18, 28, 0.72);
  border: 1px solid rgba(122, 162, 247, 0.12);
  border-radius: 6px;
  overflow-x: auto;
  font-size: 0.875rem;
  color: #d7e1ff;
}

.result-error {
  color: var(--error);
  padding: 0.75rem;
  background: rgba(247, 118, 142, 0.1);
  border: 1px solid rgba(247, 118, 142, 0.24);
  border-radius: 6px;
}

.find-section {
  margin-bottom: 1rem;
}

.find-section label {
  display: block;
  margin-bottom: 0.5rem;
  font-size: 0.875rem;
  color: var(--text-secondary);
}

.find-row {
  display: flex;
  gap: 0.5rem;
  align-items: stretch;
}

.find-row input {
  flex: 1;
  padding: 0.5rem 0.75rem;
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 6px;
  background: rgba(26, 27, 38, 0.86);
  color: var(--text-primary);
  outline: none;
}

.results-section {
  margin-top: 1rem;
  padding-top: 1rem;
  border-top: 1px solid var(--border);
}

.results-list {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  max-height: 400px;
  overflow-y: auto;
}

.result-item {
  padding: 0.75rem;
  background: rgba(26, 27, 38, 0.74);
  border-radius: 7px;
  border: 1px solid rgba(122, 162, 247, 0.14);
}

.result-header {
  display: flex;
  gap: 0.5rem;
  align-items: center;
  margin-bottom: 0.5rem;
}

.result-selector {
  font-family: monospace;
  font-size: 0.875rem;
  color: var(--accent);
  overflow-wrap: anywhere;
}

.result-tag {
  font-size: 0.75rem;
  padding: 0.125rem 0.375rem;
  background: rgba(122, 162, 247, 0.12);
  border: 1px solid rgba(122, 162, 247, 0.18);
  border-radius: 999px;
  color: var(--text-secondary);
  text-transform: uppercase;
}

.result-content {
  font-size: 0.875rem;
  color: var(--text-secondary);
  white-space: pre-wrap;
  word-break: break-word;
}

.result-value {
  font-family: monospace;
  font-size: 0.875rem;
  color: var(--text-primary);
  margin-top: 0.25rem;
}

@media (max-width: 760px) {
  .webview2-inspector-view {
    padding: 14px 10px 24px;
  }

  .section {
    padding: 14px;
  }

  .section-header,
  .find-row {
    flex-direction: column;
    align-items: stretch;
  }
}
</style>
