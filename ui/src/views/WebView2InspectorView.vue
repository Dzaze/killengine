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
      what="Un explorateur pour l'état JavaScript des apps hybrides natif+web (WebView2, Electron, CEF) : il interroge le moteur JS via le protocole Chrome DevTools plutôt que la mémoire brute."
      purpose="Trouver et modifier les vraies données affichées par une interface web embarquée (score, texte, variables JS) quand le scan mémoire classique ne trouve que du bruit moteur (V8/Blink)."
      how="Liste les targets CDP disponibles pour un process, connecte-toi à la bonne target, puis évalue du JavaScript ou cherche une valeur/un texte affiché."
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
  padding: 1rem;
  max-width: 1200px;
  margin: 0 auto;
}

.section {
  background: var(--surface-1);
  border-radius: 8px;
  padding: 1rem;
  margin-bottom: 1rem;
  border: 1px solid var(--border-color);
}

.section-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 1rem;
}

.section h3 {
  margin: 0 0 1rem 0;
  font-size: 1.1rem;
  color: var(--text-primary);
}

.section h4 {
  margin: 1rem 0 0.5rem 0;
  font-size: 1rem;
  color: var(--text-secondary);
}

.status-badge {
  padding: 0.25rem 0.75rem;
  border-radius: 4px;
  font-size: 0.875rem;
  font-weight: 500;
}

.status-badge.connected {
  background: var(--success-bg);
  color: var(--success-text);
}

.status-badge.disconnected {
  background: var(--warning-bg);
  color: var(--warning-text);
}

.controls-row {
  display: flex;
  gap: 0.5rem;
  flex-wrap: wrap;
  margin-bottom: 1rem;
}

.btn {
  padding: 0.5rem 1rem;
  border-radius: 4px;
  border: none;
  cursor: pointer;
  font-size: 0.875rem;
  transition: opacity 0.2s;
}

.btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.btn-primary {
  background: var(--primary);
  color: white;
}

.btn-secondary {
  background: var(--surface-2);
  color: var(--text-primary);
  border: 1px solid var(--border-color);
}

.btn-ghost {
  background: transparent;
  color: var(--text-secondary);
}

.warning-message {
  padding: 0.75rem;
  background: var(--warning-bg);
  color: var(--warning-text);
  border-radius: 4px;
  margin-top: 0.5rem;
}

.error-message {
  padding: 0.75rem;
  background: var(--error-bg);
  color: var(--error-text);
  border-radius: 4px;
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
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0.75rem;
  border: 1px solid var(--border-color);
  border-radius: 4px;
  cursor: pointer;
  transition: background 0.2s;
}

.target-item:hover {
  background: var(--surface-2);
}

.target-item.selected {
  border-color: var(--primary);
  background: var(--primary-bg, rgba(59, 130, 246, 0.1));
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
}

.target-title {
  font-weight: 500;
  color: var(--text-primary);
}

.target-url {
  font-size: 0.75rem;
  color: var(--text-muted);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.target-type {
  font-size: 0.75rem;
  color: var(--text-muted);
  text-transform: uppercase;
}

.evaluate-section {
  display: flex;
  flex-direction: column;
  gap: 0.75rem;
}

.script-input {
  width: 100%;
  padding: 0.75rem;
  border: 1px solid var(--border-color);
  border-radius: 4px;
  background: var(--surface-2);
  color: var(--text-primary);
  font-family: monospace;
  font-size: 0.875rem;
  resize: vertical;
}

.result-panel {
  margin-top: 1rem;
  padding: 1rem;
  background: var(--surface-2);
  border-radius: 4px;
  border: 1px solid var(--border-color);
}

.result-success pre {
  margin: 0;
  padding: 0.75rem;
  background: var(--surface-1);
  border-radius: 4px;
  overflow-x: auto;
  font-size: 0.875rem;
}

.result-error {
  color: var(--error-text);
  padding: 0.75rem;
  background: var(--error-bg);
  border-radius: 4px;
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
}

.find-row input {
  flex: 1;
  padding: 0.5rem 0.75rem;
  border: 1px solid var(--border-color);
  border-radius: 4px;
  background: var(--surface-2);
  color: var(--text-primary);
}

.results-section {
  margin-top: 1rem;
  padding-top: 1rem;
  border-top: 1px solid var(--border-color);
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
  background: var(--surface-2);
  border-radius: 4px;
  border: 1px solid var(--border-color);
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
  color: var(--primary);
}

.result-tag {
  font-size: 0.75rem;
  padding: 0.125rem 0.375rem;
  background: var(--surface-3);
  border-radius: 3px;
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
</style>
