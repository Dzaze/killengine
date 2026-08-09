<script setup lang="ts">
import { computed, nextTick, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const chatInput = ref('')
const chatScroll = ref<HTMLElement | null>(null)

const showExpert = computed(() => store.showExpertPanel)
const isAwaitingChange = computed(() => store.workflowStatus === 'awaiting_value_change')
const needsMoreRefinement = computed(() => store.workflowStatus === 'needs_more_refinement')
const isWorkflowActive = computed(() => store.workflowStatus !== 'idle')

async function scrollToBottom() {
  await nextTick()
  if (chatScroll.value) {
    chatScroll.value.scrollTop = chatScroll.value.scrollHeight
  }
}

async function sendMessage() {
  const value = chatInput.value.trim()
  if (!value) return
  chatInput.value = ''
  store.searchQuery = value
  await store.doSearch()
  await scrollToBottom()
}

async function sendExample(text: string) {
  store.searchQuery = text
  await store.doSearch()
  await scrollToBottom()
}

async function quickChanged() {
  await store.doGuidedChange()
  await scrollToBottom()
}

function workflowLabel(status: string | undefined): string {
  switch (status) {
    case 'awaiting_value_change':
      return 'En attente : change la valeur dans le jeu'
    case 'awaiting_new_value':
      return 'En attente : donne la nouvelle valeur'
    case 'needs_more_refinement':
      return 'Encore trop de candidats — raffine davantage'
    case 'auto_write_done':
      return 'Valeur cible écrite automatiquement'
    case 'auto_write_partial_or_failed':
      return 'Écriture partielle — vérifie manuellement'
    case 'no_candidate':
      return 'Aucun candidat restant'
    default:
      return 'Prêt'
  }
}

function workflowClass(status: string | undefined): string {
  switch (status) {
    case 'auto_write_done':
      return 'wf-success'
    case 'awaiting_value_change':
    case 'awaiting_new_value':
    case 'needs_more_refinement':
      return 'wf-warning'
    case 'auto_write_partial_or_failed':
    case 'no_candidate':
      return 'wf-error'
    default:
      return 'wf-idle'
  }
}

function suggestionsFor(message: typeof store.messages[number]): string {
  if (!message.candidateCount && message.candidateCount !== 0) return ''
  if (!message.suggestions || message.suggestions.length === 0) return ''
  return message.suggestions
    .map((s) => `0x${s.address} (${s.type}) → ${s.value}`)
    .join('\n')
}
</script>

<template>
  <div class="assistant-view">
    <!-- Header -->
    <div class="header">
      <div>
        <h1>{{ $t('search.title') }}</h1>
        <p class="subtitle">Décris ce que tu cherches, KillEngine fait le reste.</p>
      </div>
      <div v-if="isWorkflowActive" class="workflow-badge" :class="workflowClass(store.workflowStatus)">
        <span class="dot"></span>
        {{ workflowLabel(store.workflowStatus) }}
      </div>
    </div>

    <!-- Chat area -->
    <div ref="chatScroll" class="chat-area">
      <!-- Empty state -->
      <div v-if="store.messages.length === 0" class="empty-state">
        <div class="empty-icon">⚡</div>
        <h2>Bienvenue dans l'Assistant</h2>
        <p>Écris par exemple :</p>
        <div class="examples">
          <button class="example-chip" @click="sendExample('Argent : 41250')">
            Argent : 41250
          </button>
          <button class="example-chip" @click="sendExample('Score 1500 → 99999')">
            Score 1500 → 99999
          </button>
          <button class="example-chip" @click="sendExample('41250 99999')">
            41250 99999
          </button>
        </div>
        <p v-if="!store.isAttached" class="warn-text">
          ⚠ Attache d'abord un processus dans l'onglet « Processus ».
        </p>
      </div>

      <!-- Messages -->
      <div
        v-for="msg in store.messages"
        :key="msg.id"
        class="message"
        :class="msg.role === 'user' ? 'message-user' : 'message-assistant'"
      >
        <div class="message-avatar">{{ msg.role === 'user' ? '🧑' : '🤖' }}</div>
        <div class="message-body">
          <div class="message-meta">
            <span class="message-role">{{ msg.role === 'user' ? 'Toi' : 'KillEngine' }}</span>
            <span class="message-time">{{ msg.time }}</span>
          </div>
          <div class="message-text" :class="{ 'is-error': msg.isError }">{{ msg.text }}</div>

          <!-- Candidate badge -->
          <div v-if="msg.candidateCount !== undefined && msg.role === 'assistant'" class="message-badges">
            <span class="badge badge-info">{{ msg.candidateCount }} candidat(s)</span>
            <span v-if="msg.targetValue" class="badge badge-target">cible : {{ msg.targetValue }}</span>
          </div>

          <!-- Auto-write result -->
          <div
            v-if="msg.autoWriteResults && msg.autoWriteResults.length > 0"
            class="auto-write-box"
            :class="msg.autoWriteOk ? 'auto-ok' : 'auto-fail'"
          >
            <div class="auto-write-title">
              {{ msg.autoWriteOk ? '✓ Écriture auto réussie' : '⚠ Écriture auto partielle' }}
            </div>
            <div
              v-for="(r, i) in msg.autoWriteResults"
              :key="i"
              class="auto-write-row"
            >
              <span>0x{{ r.address }}</span>
              <span>{{ r.verified ? '✓ vérifié' : '✗ non vérifié' }}</span>
            </div>
            <p class="rollback-note">
              Tu peux annuler toutes les écritures automatiques ci-dessous.
            </p>
            <button class="btn btn-secondary btn-rollback-batch" @click="store.rollbackLastWriteBatch()">
              ↩ Rollback toutes les écritures
            </button>
          </div>

          <!-- Suggestions -->
          <div v-if="suggestionsFor(msg)" class="suggestions-box">
            <div class="suggestions-title">Adresses suggérées</div>
            <pre class="suggestions-list">{{ suggestionsFor(msg) }}</pre>
          </div>

          <!-- Requires confirmation -->
          <div v-if="msg.requiresConfirmation" class="confirm-box">
            <span class="confirm-icon">🔐</span>
            {{ msg.confirmationReason }}
          </div>
        </div>
      </div>

      <!-- Thinking -->
      <div v-if="store.isSearching" class="message message-assistant">
        <div class="message-avatar">🤖</div>
        <div class="message-body">
          <div class="thinking-dots">
            <span></span><span></span><span></span>
          </div>
        </div>
      </div>
    </div>

    <!-- Quick actions -->
    <div v-if="isAwaitingChange || needsMoreRefinement" class="quick-actions">
      <button class="btn btn-quick" @click="quickChanged">
        ✅ {{ $t('actions.iHaveChanged') }}
      </button>
      <span class="quick-hint">
        Change la valeur dans le jeu, clique ici, puis indique la nouvelle valeur.
      </span>
    </div>

    <!-- Input bar -->
    <div class="input-bar">
      <input
        v-model="chatInput"
        :placeholder="
          isAwaitingChange || needsMoreRefinement
            ? 'Donne la nouvelle valeur observée dans le jeu...'
            : $t('search.placeholder')
        "
        class="chat-input"
        :disabled="store.isSearching"
        @keyup.enter="sendMessage()"
      />
      <button class="btn btn-primary" :disabled="!chatInput.trim() || store.isSearching" @click="sendMessage()">
        <span v-if="store.isSearching">…</span>
        <span v-else>{{ $t('search.button') }}</span>
      </button>
    </div>

    <!-- Expert panel toggle -->
    <button class="expert-toggle" @click="store.showExpertPanel = !store.showExpertPanel">
      {{ showExpert ? '▼' : '▲' }} Mode Expert
    </button>

    <!-- Expert panel (manual controls) -->
    <div v-if="showExpert" class="expert-panel">
      <!-- Ping test -->
      <div class="ping-test">
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
        <span v-if="store.pingResult" class="ping-result">{{ store.pingResult }}</span>
      </div>

      <!-- Exact scan -->
      <div class="expert-section">
        <h2>{{ $t('scan.exact') }}</h2>
        <div class="scan-controls">
          <input
            v-model="store.exactScanValue"
            :placeholder="$t('scan.value')"
            class="scan-input"
            @keyup.enter="store.doExactScan()"
          />
          <select v-model="store.exactScanType" class="scan-select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <button class="btn btn-primary" :disabled="!store.isAttached" @click="store.doExactScan()">
            {{ $t('scan.button') }}
          </button>
        </div>
        <div v-if="store.exactScanResult" class="scan-result">
          <div class="scan-summary">
            <span>{{ $t('scan.matches') }}: {{ store.exactScanResult.matchesFound }}</span>
            <span>{{ $t('scan.regions') }}: {{ store.exactScanResult.regionsScanned }}</span>
            <span>{{ $t('scan.stored') }}: {{ store.exactScanResult.candidateStoreSize }}</span>
            <span v-if="store.exactScanResult.partial">{{ $t('scan.partial') }}</span>
          </div>
          <p v-if="store.exactScanResult.error">{{ store.exactScanResult.error }}</p>
          <div class="next-scan-controls">
            <select v-model="store.nextScanMode" class="scan-select">
              <option value="exact">{{ $t('scan.modeExact') }}</option>
              <option value="changed">{{ $t('scan.modeChanged') }}</option>
              <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
              <option value="increased">{{ $t('scan.modeIncreased') }}</option>
              <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
              <option value="delta">{{ $t('scan.modeDelta') }}</option>
            </select>
            <input
              v-model="store.nextScanValue"
              :disabled="store.nextScanMode !== 'exact' && store.nextScanMode !== 'delta'"
              :placeholder="$t('scan.nextValue')"
              class="scan-input"
              @keyup.enter="store.doNextScan()"
            />
            <button class="btn btn-primary" @click="store.doNextScan()">
              {{ $t('scan.nextScan') }}
            </button>
          </div>
          <div v-if="store.nextScanResult" class="scan-summary next-summary">
            <span>{{ $t('scan.remaining') }}: {{ store.nextScanResult.remaining }}</span>
            <span>{{ $t('scan.checked') }}: {{ store.nextScanResult.checked }}</span>
            <span>{{ $t('scan.unreadable') }}: {{ store.nextScanResult.unreadable }}</span>
          </div>
          <p v-if="store.nextScanResult?.error">{{ store.nextScanResult.error }}</p>
          <div class="candidate-toolbar">
            <input
              v-model="store.candidateFilter"
              :placeholder="$t('scan.filterAddress')"
              class="candidate-filter"
              @input="store.candidatePageIndex = 0; store.refreshCandidates()"
            />
            <button class="btn btn-secondary" :disabled="store.candidatePageIndex === 0" @click="store.previousCandidatePage()">
              {{ $t('scan.previous') }}
            </button>
            <button
              class="btn btn-secondary"
              :disabled="!store.candidatePage || (store.candidatePageIndex + 1) * store.candidatePageSize >= store.candidatePage.totalCount"
              @click="store.nextCandidatePage()"
            >
              {{ $t('scan.next') }}
            </button>
          </div>
          <div v-if="store.candidatePage" class="page-info">
            {{ store.candidatePage.pageIndex + 1 }} /
            {{ Math.max(1, Math.ceil(store.candidatePage.totalCount / store.candidatePage.pageSize)) }}
          </div>
          <div class="match-list">
            <div v-for="match in store.candidatePage?.candidates ?? []" :key="match.address" class="match-row">
              <button class="pick-btn" @click="store.selectCandidate(match.address, match.type)">
                0x{{ match.address }}
              </button>
              <span>{{ match.type }}</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Write / Freeze -->
      <div class="expert-section">
        <h2>{{ $t('write.title') }}</h2>
        <div class="write-controls">
          <input v-model="store.selectedCandidateAddress" class="scan-input" :placeholder="$t('write.address')" />
          <input v-model="store.writeValue" class="scan-input" :placeholder="$t('write.value')" />
          <button class="btn btn-primary" :disabled="!store.selectedCandidateAddress || !store.writeValue" @click="store.writeSelectedValue()">
            {{ $t('write.write') }}
          </button>
          <button class="btn btn-secondary" @click="store.rollbackLastWrite()">
            {{ $t('write.rollback') }}
          </button>
          <button class="btn btn-secondary" :disabled="!store.selectedCandidateAddress || !store.writeValue" @click="store.toggleFreeze()">
            {{ store.freezeEnabled ? $t('write.stopFreeze') : $t('write.freeze') }}
          </button>
        </div>
        <div v-if="store.writeResult" class="scan-summary next-summary">
          <span>{{ store.writeResult.success ? 'OK' : 'FAIL' }}</span>
          <span v-if="store.writeResult.verified">{{ $t('write.verified') }}</span>
          <span v-if="store.writeResult.bytesWritten">{{ store.writeResult.bytesWritten }} B</span>
        </div>
        <p v-if="store.writeResult?.error">{{ store.writeResult.error }}</p>
      </div>

      <!-- Unknown -->
      <div class="expert-section">
        <h2>{{ $t('unknown.title') }}</h2>
        <div class="scan-controls unknown-controls">
          <select v-model="store.unknownScanType" class="scan-select">
            <option>Int32</option>
            <option>Int64</option>
            <option>Float32</option>
            <option>Float64</option>
          </select>
          <select v-model="store.unknownScanMode" class="scan-select">
            <option value="changed">{{ $t('scan.modeChanged') }}</option>
            <option value="unchanged">{{ $t('scan.modeUnchanged') }}</option>
            <option value="increased">{{ $t('scan.modeIncreased') }}</option>
            <option value="decreased">{{ $t('scan.modeDecreased') }}</option>
          </select>
          <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.captureUnknownSnapshot()">
            {{ $t('unknown.capture') }}
          </button>
          <button class="btn btn-primary" :disabled="!store.isAttached" @click="store.doUnknownNextScan()">
            {{ $t('unknown.compare') }}
          </button>
        </div>
        <div v-if="store.unknownSnapshotResult" class="scan-summary next-summary">
          <span>{{ $t('unknown.regions') }}: {{ store.unknownSnapshotResult.regionsCaptured }}</span>
          <span>{{ $t('unknown.bytes') }}: {{ store.unknownSnapshotResult.bytesCaptured }}</span>
          <span v-if="store.unknownSnapshotResult.partial">{{ $t('scan.partial') }}</span>
        </div>
        <p v-if="store.unknownSnapshotResult?.error">{{ store.unknownSnapshotResult.error }}</p>
        <div v-if="store.unknownNextScanResult" class="scan-summary next-summary">
          <span>{{ $t('scan.matches') }}: {{ store.unknownNextScanResult.matchesFound }}</span>
          <span>{{ $t('scan.stored') }}: {{ store.unknownNextScanResult.stored }}</span>
        </div>
        <p v-if="store.unknownNextScanResult?.error">{{ store.unknownNextScanResult.error }}</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
.assistant-view {
  display: flex;
  flex-direction: column;
  height: 100%;
  padding: 16px 24px;
}

.header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin-bottom: 16px;
}

.header h1 {
  font-size: 22px;
  color: var(--text-primary);
}

.subtitle {
  font-size: 13px;
  color: var(--text-dim);
  margin-top: 2px;
}

.workflow-badge {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 12px;
  border-radius: 16px;
  font-size: 12px;
  font-weight: 500;
}

.workflow-badge .dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
}

.wf-success { background: rgba(158, 206, 106, 0.15); color: var(--success); }
.wf-success .dot { background: var(--success); }
.wf-warning { background: rgba(224, 175, 104, 0.15); color: var(--warning); }
.wf-warning .dot { background: var(--warning); }
.wf-error { background: rgba(247, 118, 142, 0.15); color: var(--error); }
.wf-error .dot { background: var(--error); }
.wf-idle { background: var(--bg-tertiary); color: var(--text-dim); }
.wf-idle .dot { background: var(--text-dim); }

/* Chat area */
.chat-area {
  flex: 1;
  overflow-y: auto;
  padding: 8px 0;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  text-align: center;
  flex: 1;
  gap: 8px;
}

.empty-icon {
  font-size: 48px;
  margin-bottom: 8px;
}

.empty-state h2 {
  font-size: 20px;
  color: var(--text-primary);
}

.empty-state p {
  color: var(--text-dim);
  font-size: 14px;
}

.examples {
  display: flex;
  gap: 8px;
  margin: 12px 0;
  flex-wrap: wrap;
  justify-content: center;
}

.example-chip {
  padding: 8px 16px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 20px;
  color: var(--accent);
  font-size: 13px;
  cursor: pointer;
  transition: all 0.15s;
  font-family: 'Cascadia Code', monospace;
}

.example-chip:hover {
  background: var(--bg-accent);
  border-color: var(--accent);
}

.warn-text {
  color: var(--warning);
  font-size: 12px;
  margin-top: 16px;
}

/* Messages */
.message {
  display: flex;
  gap: 10px;
  max-width: 85%;
  animation: fade-in 0.2s ease;
}

@keyframes fade-in {
  from { opacity: 0; transform: translateY(6px); }
  to { opacity: 1; transform: translateY(0); }
}

.message-user {
  align-self: flex-end;
  flex-direction: row-reverse;
}

.message-avatar {
  width: 32px;
  height: 32px;
  border-radius: 50%;
  background: var(--bg-tertiary);
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 18px;
  flex-shrink: 0;
}

.message-body {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.message-user .message-body {
  align-items: flex-end;
}

.message-meta {
  display: flex;
  gap: 8px;
  font-size: 11px;
  color: var(--text-dim);
}

.message-role {
  font-weight: 600;
}

.message-text {
  padding: 10px 14px;
  border-radius: 12px;
  font-size: 14px;
  line-height: 1.5;
  white-space: pre-wrap;
  word-break: break-word;
}

.message-assistant .message-text {
  background: var(--bg-tertiary);
  color: var(--text-primary);
  border-bottom-left-radius: 4px;
}

.message-user .message-text {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 500;
  border-bottom-right-radius: 4px;
}

.message-text.is-error {
  color: var(--error);
}

.message-badges {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}

.badge {
  padding: 3px 8px;
  border-radius: 10px;
  font-size: 11px;
  font-weight: 500;
}

.badge-info {
  background: rgba(122, 162, 247, 0.15);
  color: var(--accent);
}

.badge-target {
  background: rgba(158, 206, 106, 0.15);
  color: var(--success);
}

.auto-write-box {
  margin-top: 6px;
  padding: 10px;
  border-radius: 8px;
  border: 1px solid var(--border);
  font-size: 12px;
}

.auto-write-box.auto-ok {
  background: rgba(158, 206, 106, 0.08);
  border-color: rgba(158, 206, 106, 0.3);
}

.auto-write-box.auto-fail {
  background: rgba(247, 118, 142, 0.08);
  border-color: rgba(247, 118, 142, 0.3);
}

.auto-write-title {
  font-weight: 600;
  margin-bottom: 6px;
}

.auto-write-row {
  display: flex;
  justify-content: space-between;
  padding: 3px 0;
  font-family: 'Cascadia Code', monospace;
}

.rollback-note {
  margin-top: 6px;
  color: var(--text-dim);
  font-size: 11px;
  font-style: italic;
}

.suggestions-box {
  margin-top: 6px;
  padding: 8px 10px;
  background: var(--bg-primary);
  border: 1px solid var(--border);
  border-radius: 8px;
}

.suggestions-title {
  font-size: 11px;
  color: var(--text-dim);
  margin-bottom: 4px;
}

.suggestions-list {
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  color: var(--text-secondary);
  white-space: pre-wrap;
}

.confirm-box {
  margin-top: 6px;
  padding: 8px 12px;
  background: rgba(224, 175, 104, 0.1);
  border: 1px solid rgba(224, 175, 104, 0.3);
  border-radius: 8px;
  font-size: 12px;
  color: var(--warning);
  display: flex;
  gap: 8px;
  align-items: center;
}

/* Thinking dots */
.thinking-dots {
  display: flex;
  gap: 4px;
  padding: 12px 16px;
  background: var(--bg-tertiary);
  border-radius: 12px;
  border-bottom-left-radius: 4px;
}

.thinking-dots span {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--text-dim);
  animation: bounce 1.4s infinite ease-in-out both;
}

.thinking-dots span:nth-child(1) { animation-delay: -0.32s; }
.thinking-dots span:nth-child(2) { animation-delay: -0.16s; }

@keyframes bounce {
  0%, 80%, 100% { transform: scale(0.6); opacity: 0.4; }
  40% { transform: scale(1); opacity: 1; }
}

/* Quick actions */
.quick-actions {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 10px 0;
  flex-shrink: 0;
}

.btn-quick {
  background: var(--success);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-quick:hover {
  filter: brightness(1.1);
}

.quick-hint {
  font-size: 12px;
  color: var(--text-dim);
}

/* Input bar */
.input-bar {
  display: flex;
  gap: 10px;
  padding: 10px 0;
  flex-shrink: 0;
}

.chat-input {
  flex: 1;
  padding: 12px 16px;
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 10px;
  color: var(--text-primary);
  font-size: 14px;
  outline: none;
  transition: border-color 0.15s;
}

.chat-input:focus {
  border-color: var(--accent);
}

.chat-input::placeholder {
  color: var(--text-dim);
}

.btn {
  padding: 10px 20px;
  border: none;
  border-radius: 10px;
  font-size: 14px;
  cursor: pointer;
  transition: all 0.15s;
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
  min-width: 100px;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.btn-secondary:hover:not(:disabled) {
  background: var(--border);
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.4;
}

/* Expert panel */
.expert-toggle {
  background: none;
  border: none;
  color: var(--text-dim);
  font-size: 12px;
  cursor: pointer;
  padding: 8px 0;
  text-align: left;
}

.expert-toggle:hover {
  color: var(--accent);
}

.expert-panel {
  max-height: 320px;
  overflow-y: auto;
  border-top: 1px solid var(--border);
  padding-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.expert-section {
  background: var(--bg-secondary);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 12px;
}

.expert-section h2 {
  margin-bottom: 10px;
  color: var(--text-primary);
  font-size: 14px;
}

.ping-test {
  display: flex;
  align-items: center;
  gap: 10px;
}

.ping-result {
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  color: var(--text-dim);
}

.scan-controls {
  display: grid;
  grid-template-columns: 1fr 110px auto;
  gap: 8px;
}

.scan-input,
.scan-select {
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  outline: none;
  font-size: 13px;
}

.scan-result {
  margin-top: 10px;
}

.scan-summary {
  display: flex;
  gap: 12px;
  color: var(--text-dim);
  font-size: 11px;
}

.next-scan-controls {
  display: grid;
  grid-template-columns: 130px 1fr auto;
  gap: 6px;
  margin-top: 10px;
}

.next-summary {
  margin-top: 8px;
}

.unknown-controls {
  grid-template-columns: 100px 130px auto auto;
}

.candidate-toolbar {
  display: grid;
  grid-template-columns: 1fr auto auto;
  gap: 6px;
  margin-top: 10px;
}

.candidate-filter {
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  outline: none;
  font-size: 12px;
}

.page-info {
  margin-top: 6px;
  color: var(--text-dim);
  font-size: 11px;
  text-align: right;
}

.scan-result p {
  margin-top: 6px;
  color: var(--error);
  font-size: 11px;
}

.match-list {
  display: flex;
  max-height: 120px;
  flex-direction: column;
  gap: 3px;
  margin-top: 8px;
  overflow-y: auto;
}

.match-row {
  display: flex;
  justify-content: space-between;
  padding: 4px 8px;
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.match-row span {
  color: var(--text-dim);
}

.pick-btn {
  border: none;
  background: transparent;
  color: var(--text-primary);
  cursor: pointer;
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  text-align: left;
}

.pick-btn:hover {
  color: var(--accent);
}

.write-controls {
  display: grid;
  grid-template-columns: 1fr 1fr auto auto auto;
  gap: 6px;
}

.btn-rollback-batch {
  margin-top: 6px;
  padding: 6px 12px;
  font-size: 12px;
}
</style>
