<script setup lang="ts">
import { computed, nextTick, ref } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const chatInput = ref('')
const chatScroll = ref<HTMLElement | null>(null)

const isAwaitingChange = computed(() => store.workflowStatus === 'awaiting_value_change')
const needsMoreRefinement = computed(() => store.workflowStatus === 'needs_more_refinement')
const isWorkflowActive = computed(() => store.workflowStatus !== 'idle')
const contextItems = computed(() => {
  const context = store.smartSearchContext
  if (!context) return []

  const items: Array<{ label: string, value: string }> = []
  if (context.profileTargets.length > 0) {
    items.push({ label: 'État', value: 'profil actif' })
  } else if (context.chatTargets.length > 0) {
    items.push({ label: 'État', value: 'adresses actives' })
  } else if (context.active || context.candidateCount > 0) {
    items.push({ label: 'État', value: 'recherche active' })
  } else {
    items.push({ label: 'État', value: 'aucun contexte actif' })
  }
  if (context.initialValue) items.push({ label: 'Recherche', value: context.initialValue })
  if (context.targetValue) items.push({ label: 'Cible', value: context.targetValue })
  if (context.valueType) items.push({ label: 'Type', value: context.valueType })
  if (context.candidateCount > 0) items.push({ label: 'Candidats', value: String(context.candidateCount) })
  if (context.chatTargets.length > 0) {
    items.push({
      label: 'Adresses',
      value: context.chatTargets.map((target) => `0x${target.address}`).join(' · '),
    })
  }
  if (context.profileTargets.length > 0) {
    const groups = Array.from(new Set(
      context.profileTargets
        .map((target) => String(target.group ?? '').trim())
        .filter(Boolean),
    ))
    if (groups.length > 0) {
      items.push({ label: 'Groupes profil', value: groups.join(' · ') })
    }
    items.push({
      label: 'Cibles profil',
      value: context.profileTargets.map((target) => `${target.profile}:${target.target}`).join(' · '),
    })
  }
  if (context.hasUndoReduction) items.push({ label: 'Réduction', value: 'restaurable' })
  return items
})

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
    .map((s) => {
      const history = valueHistoryFor(s)
      return `0x${s.address} (${s.type}) → ${s.value}${history ? ` · ${history}` : ''}`
    })
    .join('\n')
}

function valueHistoryFor(record: Record<string, unknown>): string {
  const history = Array.isArray(record.valueHistory) ? record.valueHistory : []
  const values = history
    .map((item) => {
      const entry = item as Record<string, unknown>
      const value = entry.currentNumber
      return typeof value === 'number' && Number.isFinite(value) ? String(value) : ''
    })
    .filter(Boolean)

  const uniqueValues = values.filter((value, index) => index === 0 || value !== values[index - 1])
  if (uniqueValues.length === 0) return ''
  return `observé : ${uniqueValues.slice(-4).join(' -> ')}`
}

function filteredCandidatesFor(message: typeof store.messages[number]): string {
  const filtered = message.filteredWriteCandidates ?? []
  if (filtered.length === 0) return ''
  return filtered
    .map((candidate) => {
      const reason = String(candidate.noiseFilterReason ?? 'rejeté')
      return `0x${candidate.address} (${candidate.regionType ?? '?'}) · ${reason}`
    })
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
      <div class="header-actions">
        <div v-if="isWorkflowActive" class="workflow-badge" :class="workflowClass(store.workflowStatus)">
          <span class="dot"></span>
          {{ workflowLabel(store.workflowStatus) }}
        </div>
        <button class="btn btn-secondary btn-small" :disabled="store.isSearching" @click="store.startNewSearchContext()">
          Nouvelle recherche
        </button>
      </div>
    </div>

    <div v-if="store.activeChatMemoryTargets.length > 0" class="active-targets">
      <div>
        <strong>{{ store.activeChatMemoryTargets.length }} adresse(s) mémoire active(s)</strong>
        <span>
          {{ store.activeChatMemoryTargets.map((target) => `0x${target.address}`).join(' · ') }}
        </span>
      </div>
      <button class="btn btn-secondary btn-small" @click="store.clearActiveChatMemoryTargets()">
        Oublier
      </button>
    </div>

    <div v-if="contextItems.length > 0" class="context-strip">
      <div v-for="item in contextItems" :key="item.label" class="context-chip">
        <span>{{ item.label }}</span>
        <strong>{{ item.value }}</strong>
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
          <div v-if="msg.intentRationale" class="decision-line">
            Décision : {{ msg.intentRationale }}
          </div>

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
              <span>{{ r.verified ? '✓ final vérifié' : '✗ non vérifié' }}</span>
              <span v-if="r.confirmationMode" class="confirm-steps">
                {{ r.temporaryVerified ? 'test OK' : 'test KO' }}
                ·
                {{ r.restoredBeforeFinal ? 'restauré' : 'non restauré' }}
              </span>
              <span v-if="valueHistoryFor(r)" class="value-history">{{ valueHistoryFor(r) }}</span>
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

          <div v-if="filteredCandidatesFor(msg)" class="filtered-box">
            <div class="suggestions-title">Filtre anti-bruit</div>
            <pre class="suggestions-list">{{ filteredCandidatesFor(msg) }}</pre>
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
  gap: 12px;
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

.header-actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 8px;
}

.wf-success { background: rgba(158, 206, 106, 0.15); color: var(--success); }
.wf-success .dot { background: var(--success); }
.wf-warning { background: rgba(224, 175, 104, 0.15); color: var(--warning); }
.wf-warning .dot { background: var(--warning); }
.wf-error { background: rgba(247, 118, 142, 0.15); color: var(--error); }
.wf-error .dot { background: var(--error); }
.wf-idle { background: var(--bg-tertiary); color: var(--text-dim); }
.wf-idle .dot { background: var(--text-dim); }

.active-targets {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 10px;
  padding: 10px 12px;
  border: 1px solid rgba(122, 162, 247, 0.35);
  border-radius: 8px;
  background: rgba(122, 162, 247, 0.08);
}

.active-targets strong,
.active-targets span {
  display: block;
}

.active-targets strong {
  color: var(--text-primary);
  font-size: 13px;
}

.active-targets span {
  overflow-wrap: anywhere;
  margin-top: 3px;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.context-strip {
  display: flex;
  gap: 6px;
  overflow-x: auto;
  margin-bottom: 10px;
  padding-bottom: 2px;
}

.context-chip {
  display: inline-flex;
  min-width: 0;
  max-width: 280px;
  align-items: center;
  gap: 6px;
  padding: 6px 9px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-tertiary);
  white-space: nowrap;
}

.context-chip span {
  flex-shrink: 0;
  color: var(--text-dim);
  font-size: 11px;
}

.context-chip strong {
  overflow: hidden;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  font-weight: 600;
  text-overflow: ellipsis;
}

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

.decision-line {
  max-width: 720px;
  color: var(--text-dim);
  font-size: 11px;
  line-height: 1.35;
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
  flex-wrap: wrap;
  gap: 6px 12px;
  justify-content: space-between;
  padding: 3px 0;
  font-family: 'Cascadia Code', monospace;
}

.confirm-steps {
  color: var(--text-dim);
}

.value-history {
  width: 100%;
  color: var(--text-dim);
  font-family: inherit;
  font-size: 11px;
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

.filtered-box {
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid rgba(247, 118, 142, 0.28);
  border-radius: 8px;
  background: rgba(247, 118, 142, 0.08);
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

.btn-small {
  padding: 6px 10px;
  border-radius: 6px;
  font-size: 12px;
}

.btn-rollback-batch {
  margin-top: 6px;
  padding: 6px 12px;
  font-size: 12px;
}
</style>
