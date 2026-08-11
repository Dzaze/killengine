<script setup lang="ts">
import { computed, nextTick, onUnmounted, ref, watch } from 'vue'
import { useAppStore } from '@/stores/app'

const store = useAppStore()
const chatInput = ref('')
const chatScroll = ref<HTMLElement | null>(null)

// Indicateur de réflexion dynamique — messages qui changent pendant le traitement
const thinkingMessages = [
  'Réflexion en cours...',
  'Analyse de ta requête...',
  'Recherche en mémoire...',
  'Comparaison des candidats...',
  'Filtrage des faux positifs...',
  'Optimisation des résultats...',
]
const thinkingIndex = ref(0)
const thinkingText = ref(thinkingMessages[0])
let thinkingTimer: ReturnType<typeof setInterval> | null = null

watch(() => store.isSearching, (searching) => {
  if (searching) {
    thinkingIndex.value = 0
    thinkingText.value = thinkingMessages[0]
    thinkingTimer = setInterval(() => {
      thinkingIndex.value = (thinkingIndex.value + 1) % thinkingMessages.length
      thinkingText.value = thinkingMessages[thinkingIndex.value]
    }, 1500)
  } else if (thinkingTimer) {
    clearInterval(thinkingTimer)
    thinkingTimer = null
  }
})

watch(() => store.messages.length, () => {
  void scrollToBottom()
})

onUnmounted(() => {
  if (thinkingTimer) clearInterval(thinkingTimer)
})

const isAwaitingChange = computed(() => store.workflowStatus === 'awaiting_value_change')
const needsMoreRefinement = computed(() => store.workflowStatus === 'needs_more_refinement')
const isWorkflowActive = computed(() => store.workflowStatus !== 'idle')
const hasActiveAddresses = computed(() => store.activeChatMemoryTargets.length > 0)
const searchPlaceholder = computed(() => {
  if (isAwaitingChange.value || needsMoreRefinement.value) return 'Donne la nouvelle valeur observée dans le jeu...'
  if (hasActiveAddresses.value) return 'Ex: mets les à 3000, freeze à 500, ou lance une nouvelle recherche...'
  return 'Ex: j’ai une valeur à 905 je la veux à 10000'
})
const searchStatusText = computed(() => {
  if (store.isSearching) return thinkingText.value
  if (isWorkflowActive.value) return workflowLabel(store.workflowStatus)
  return store.isAttached ? 'Prêt à chercher' : 'Attache un processus avant de scanner'
})
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
  if (context.writeHistory && context.writeHistory.length > 0) {
    items.push({ label: 'Écritures', value: context.writeHistory.join(' -> ') })
  }
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

async function useMessageSuggestions(message: typeof store.messages[number]) {
  if (!message.suggestions || message.suggestions.length === 0) return
  await store.useSuggestedAddresses(message.suggestions)
  await scrollToBottom()
}

async function startNewSearchFromMessage() {
  await store.startNewSearchContext()
  await scrollToBottom()
}

async function searchTargetElsewhere(message: typeof store.messages[number]) {
  const value = message.targetValue || String(message.suggestions?.[0]?.value ?? '')
  await store.searchValueElsewhere(value)
  await scrollToBottom()
}

async function testSingleAddress(suggestion: Record<string, unknown>) {
  await store.testSingleSuggestedAddress(suggestion)
  await scrollToBottom()
}

function watchSuggestion(suggestion: Record<string, unknown>) {
  store.addAddressToWatch(String(suggestion.address ?? ''), String(suggestion.type ?? store.exactScanType))
  store.setWatchLiveEnabled(true)
}

function keepSuggestion(suggestion: Record<string, unknown>) {
  store.keepCandidate(String(suggestion.address ?? ''))
}

function ignoreSuggestion(suggestion: Record<string, unknown>) {
  store.ignoreCandidate(String(suggestion.address ?? ''))
}

async function searchSuggestionAsType(suggestion: Record<string, unknown>, type: string) {
  await store.searchValueAsType(String(suggestion.value ?? ''), type)
  await scrollToBottom()
}

async function runRecoveryAction(action: Record<string, unknown> | string) {
  const actionId = typeof action === 'string' ? action : String(action.id ?? '')
  const actionValue = typeof action === 'string' ? '' : String(action.value ?? store.targetValueGuided ?? '')
  const actionTarget = typeof action === 'string' ? '' : String(action.target ?? store.targetValueGuided ?? '')
  if (actionId === 'rollback_batch') {
    await store.rollbackLastWriteBatch()
  } else if (actionId === 'clear_targets') {
    await store.clearActiveChatMemoryTargets()
  } else if (actionId === 'new_search') {
    await store.startNewSearchContext()
  } else if (actionId === 'continue_candidates') {
    store.pushMessage('assistant', 'Garde le jeu ouvert, fais varier la valeur et donne-moi la nouvelle valeur observée pour continuer la réduction.')
  } else if (actionId === 'undo_reduction') {
    await store.undoCandidateScan()
    store.pushMessage('assistant', 'J’ai restauré les candidats précédents. Tu peux maintenant essayer changed, increased, ou une autre représentation.')
  } else if (actionId === 'try_changed') {
    await store.undoCandidateScan()
    store.nextScanMode = 'changed'
    store.nextScanValue = ''
    await store.doNextScan()
  } else if (actionId === 'try_increased') {
    await store.undoCandidateScan()
    store.nextScanMode = 'increased'
    store.nextScanValue = ''
    await store.doNextScan()
  } else if (actionId === 'retry_float32') {
    await store.searchValueAsType(actionValue, 'Float32', actionTarget)
  } else if (actionId === 'retry_int64') {
    await store.searchValueAsType(actionValue, 'Int64', actionTarget)
  } else if (actionId === 'retry_int32_x100') {
    const numeric = Number(actionValue.trim().replace(',', '.'))
    const numericTarget = Number(actionTarget.trim().replace(',', '.'))
    const nextValue = Number.isFinite(numeric) ? String(numeric * 100) : actionValue
    const nextTarget = Number.isFinite(numericTarget) ? String(numericTarget * 100) : actionTarget
    await store.searchValueAsType(nextValue, 'Int32', nextTarget)
  } else if (actionId === 'try_unknown_increased') {
    await store.startNewSearchContext()
    store.activeView = 'expert'
    store.unknownScanMode = 'increased'
    store.pushMessage('assistant', 'Passe en Unknown initial value : capture une première image, fais augmenter la valeur dans le jeu, puis lance increased.')
  }
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
      return 'Adresses actives pour modification'
    case 'freeze_done':
      return 'Freeze actif'
    case 'requires_manual_write':
      return 'Écriture prête à confirmer'
    case 'auto_write_partial_or_failed':
      return 'Écriture partielle — vérifie manuellement'
    case 'auto_write_problem':
      return 'Adresses à vérifier'
    case 'no_candidate':
      return 'Aucun candidat restant'
    default:
      return 'Prêt'
  }
}

function workflowClass(status: string | undefined): string {
  switch (status) {
    case 'auto_write_done':
    case 'freeze_done':
      return 'wf-success'
    case 'awaiting_value_change':
    case 'awaiting_new_value':
    case 'needs_more_refinement':
    case 'requires_manual_write':
      return 'wf-warning'
    case 'auto_write_partial_or_failed':
    case 'auto_write_problem':
    case 'no_candidate':
      return 'wf-error'
    default:
      return 'wf-idle'
  }
}

function suggestionRowsFor(message: typeof store.messages[number]): Array<Record<string, unknown>> {
  return message.suggestions ?? []
}

function confidenceFor(record: Record<string, unknown>): string {
  const label = String(record.confidenceLabel ?? '').trim()
  const reason = String(record.confidenceReason ?? '').trim()
  if (reason) return reason
  if (label) return label
  const confidence = Number(record.confidence ?? Number.NaN)
  if (!Number.isFinite(confidence)) return ''
  if (confidence >= 0.85) return 'fiabilité élevée'
  if (confidence >= 0.65) return 'fiabilité moyenne'
  return 'fiabilité faible'
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
  return `ancienne valeur observée : ${uniqueValues.slice(-4).join(' -> ')}`
}

function writeHistoryFor(message: typeof store.messages[number]): string {
  const history = message.writeHistory ?? []
  return history.length > 0 ? history.join(' -> ') : ''
}

function writeSummaryFor(message: typeof store.messages[number], record: Record<string, unknown>): string {
  const previous = message.previousTargetValue
  const current = String(record.value ?? message.targetValue ?? '').trim()
  if (previous && current && previous !== current) return `dernière écriture : ${previous} · nouvelle écriture : ${current}`
  if (current) return `nouvelle écriture : ${current}`
  return ''
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

    <div class="assistant-status-strip" :class="{ 'is-working': store.isSearching }">
      <div class="top-search-status">
        <span class="status-dot"></span>
        <span>{{ searchStatusText }}</span>
      </div>
      <div v-if="store.isSearching" class="top-search-progress"></div>
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
        :class="[msg.role === 'user' ? 'message-user' : 'message-assistant', { 'thinking-indicator': msg.isThinking }]"
      >
        <div class="message-avatar" :class="{ 'thinking-avatar': msg.isThinking }">
          {{ msg.role === 'user' ? '🧑' : '🤖' }}
        </div>
        <div class="message-body">
          <div class="message-meta">
            <span class="message-role">{{ msg.isThinking ? 'KillEngine réfléchit' : msg.role === 'user' ? 'Toi' : 'KillEngine' }}</span>
            <span class="message-time">{{ msg.time }}</span>
          </div>
          <div v-if="msg.isThinking" class="thinking-card">
            <div class="thinking-dots">
              <span></span><span></span><span></span>
            </div>
            <Transition name="thinking-fade" mode="out-in">
              <span :key="thinkingText" class="thinking-text">{{ thinkingText }}</span>
            </Transition>
          </div>
          <div v-else class="message-text" :class="{ 'is-error': msg.isError }">{{ msg.text }}</div>
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
            <div v-if="writeHistoryFor(msg)" class="write-history-line">
              Historique : {{ writeHistoryFor(msg) }}
            </div>
            <div v-if="msg.activeTargetCount" class="active-write-line">
              {{ msg.activeTargetCount }} adresse(s) gardée(s) actives pour les prochaines modifications.
            </div>
            <div
              v-for="(r, i) in msg.autoWriteResults"
              :key="i"
              class="auto-write-row"
            >
              <span>0x{{ r.address }}</span>
              <span>{{ r.verified ? '✓ final vérifié' : '✗ non vérifié' }}</span>
              <span v-if="writeSummaryFor(msg, r)" class="write-summary">{{ writeSummaryFor(msg, r) }}</span>
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
            <div class="message-actions">
              <button class="btn btn-secondary btn-small" @click="useMessageSuggestions(msg)">
                Réutiliser ces adresses
              </button>
              <button class="btn btn-secondary btn-small" @click="startNewSearchFromMessage()">
                Nouvelle recherche
              </button>
            </div>
          </div>

          <!-- Suggestions -->
          <div v-if="suggestionRowsFor(msg).length > 0" class="suggestions-box">
            <div class="suggestions-title">Adresses suggérées</div>
            <div
              v-for="(suggestion, index) in suggestionRowsFor(msg)"
              :key="`${suggestion.address}-${index}`"
              class="suggestion-row"
            >
              <div class="suggestion-main">
                <code>0x{{ suggestion.address }}</code>
                <span>{{ suggestion.type }} → {{ suggestion.value }}</span>
              </div>
              <div v-if="confidenceFor(suggestion) || valueHistoryFor(suggestion)" class="suggestion-confidence">
                {{ [confidenceFor(suggestion), valueHistoryFor(suggestion)].filter(Boolean).join(' · ') }}
              </div>
              <div class="suggestion-actions">
                <button class="btn btn-secondary btn-small" @click="testSingleAddress(suggestion)">
                  Tester cette adresse
                </button>
                <button class="btn btn-secondary btn-small" @click="watchSuggestion(suggestion)">
                  Watch
                </button>
                <button class="btn btn-secondary btn-small" @click="keepSuggestion(suggestion)">
                  Garder
                </button>
                <button class="btn btn-secondary btn-small" @click="ignoreSuggestion(suggestion)">
                  Ignorer
                </button>
              </div>
            </div>
            <div class="message-actions">
              <button class="btn btn-secondary btn-small" @click="searchTargetElsewhere(msg)">
                Chercher cette valeur ailleurs
              </button>
              <button class="btn btn-secondary btn-small" @click="searchSuggestionAsType(suggestionRowsFor(msg)[0], 'Int32')">
                Chercher en Int32
              </button>
              <button class="btn btn-secondary btn-small" @click="searchSuggestionAsType(suggestionRowsFor(msg)[0], 'Float32')">
                Chercher en Float32
              </button>
            </div>
          </div>

          <div v-if="msg.recoveryActions && msg.recoveryActions.length > 0" class="recovery-box">
            <div class="suggestions-title">Que faire maintenant ?</div>
            <div class="message-actions">
              <button
                v-for="action in msg.recoveryActions"
                :key="String(action.id)"
                class="btn btn-secondary btn-small"
                @click="runRecoveryAction(action)"
              >
                {{ action.label }}
              </button>
            </div>
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
          searchPlaceholder
        "
        class="chat-input"
        :disabled="store.isSearching"
        @keyup.enter="sendMessage()"
      />
      <button class="btn btn-primary" :disabled="!chatInput.trim() || store.isSearching" @click="sendMessage()">
        <span v-if="store.isSearching" class="btn-spinner" aria-hidden="true"></span>
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

.assistant-status-strip {
  position: relative;
  margin-bottom: 12px;
  padding: 7px 10px;
  overflow: hidden;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: rgba(36, 40, 59, 0.58);
}

.top-search-status {
  display: flex;
  align-items: center;
  gap: 7px;
  min-height: 16px;
  color: var(--text-dim);
  font-size: 12px;
}

.status-dot {
  width: 7px;
  height: 7px;
  border-radius: 999px;
  background: var(--text-dim);
}

.assistant-status-strip.is-working {
  border-color: rgba(122, 162, 247, 0.55);
}

.assistant-status-strip.is-working .status-dot {
  background: var(--accent);
  box-shadow: 0 0 0 4px rgba(122, 162, 247, 0.12);
}

.top-search-progress {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  height: 2px;
  overflow: hidden;
  background: rgba(122, 162, 247, 0.15);
}

.top-search-progress::before {
  content: '';
  position: absolute;
  top: 0;
  bottom: 0;
  left: -35%;
  width: 35%;
  background: var(--accent);
  animation: search-progress 1.15s ease-in-out infinite;
}

@keyframes search-progress {
  0% { transform: translateX(0); }
  100% { transform: translateX(390%); }
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

.write-history-line,
.active-write-line {
  margin-bottom: 6px;
  color: var(--text-secondary);
  font-size: 11px;
}

.write-history-line {
  font-family: 'Cascadia Code', monospace;
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

.write-summary {
  width: 100%;
  color: var(--text-secondary);
  font-family: inherit;
  font-size: 11px;
}

.value-history {
  width: 100%;
  color: var(--text-dim);
  font-family: inherit;
  font-size: 11px;
}

.message-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
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

.recovery-box {
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid rgba(224, 175, 104, 0.35);
  border-radius: 8px;
  background: rgba(224, 175, 104, 0.08);
}

.suggestion-row {
  padding: 7px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.suggestion-row:first-of-type {
  border-top: 0;
}

.suggestion-main {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  color: var(--text-secondary);
  font-size: 12px;
}

.suggestion-main code {
  color: var(--accent);
  font-family: 'Cascadia Code', monospace;
}

.suggestion-confidence {
  margin-top: 3px;
  color: var(--text-dim);
  font-size: 11px;
}

.suggestion-actions {
  display: flex;
  margin-top: 6px;
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

/* Thinking indicator */
.thinking-indicator {
  align-self: flex-start;
}

.thinking-avatar {
  animation: thinking-pulse 2s ease-in-out infinite;
}

@keyframes thinking-pulse {
  0%, 100% { transform: scale(1); filter: brightness(1); }
  50% { transform: scale(1.08); filter: brightness(1.15); }
}

.thinking-card {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 16px;
  background: rgba(122, 162, 247, 0.08);
  border: 1px solid rgba(122, 162, 247, 0.2);
  border-radius: 12px;
  border-bottom-left-radius: 4px;
}

.thinking-text {
  color: var(--accent);
  font-size: 13px;
  font-style: italic;
}

/* Thinking dots (inside thinking-card) */
.thinking-dots {
  display: flex;
  flex-shrink: 0;
  gap: 4px;
  padding: 0;
  background: transparent;
  border-radius: 0;
}

.thinking-dots span {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--accent);
  animation: bounce 1.4s infinite ease-in-out both;
}

.thinking-dots span:nth-child(1) { animation-delay: -0.32s; }
.thinking-dots span:nth-child(2) { animation-delay: -0.16s; }

@keyframes bounce {
  0%, 80%, 100% { transform: scale(0.6); opacity: 0.4; }
  40% { transform: scale(1); opacity: 1; }
}

.btn-spinner {
  display: inline-block;
  width: 14px;
  height: 14px;
  margin-right: 6px;
  border: 2px solid currentColor;
  border-right-color: transparent;
  border-radius: 999px;
  animation: spin 0.75s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

/* Transition for thinking text */
.thinking-fade-enter-active,
.thinking-fade-leave-active {
  transition: all 0.3s ease;
}

.thinking-fade-enter-from {
  opacity: 0;
  transform: translateY(4px);
}

.thinking-fade-leave-to {
  opacity: 0;
  transform: translateY(-4px);
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
