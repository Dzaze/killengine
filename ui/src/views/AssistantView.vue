<script lang="ts">
// Portee module (persiste tant que l'app tourne, contrairement aux
// variables de <script setup> qui sont recreees a chaque (re)montage du
// composant) : evite de renvoyer la requete de warmup a chaque fois que
// l'utilisateur navigue vers puis hors de l'onglet Assistant dans la meme
// session.
let localAiWarmupRequested = false
</script>

<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { useAppStore, type WorkflowPreset } from '@/stores/app'
import { useWebView2InspectorStore } from '@/stores/webView2Inspector'
import { backend } from '@/services/backend'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const chatInput = ref('')
const chatScroll = ref<HTMLElement | null>(null)

// Indicateur de réflexion dynamique — messages qui changent pendant le traitement.
// Deux jeux de phrases : un pour les requêtes qui ressemblent à un scan concret
// (contiennent un nombre ou une adresse — ce sont les seules que le classifieur
// C++ route vers ActivateMemoryTargets/ExactScan/GuidedScan/... côté
// smart_search_manager.cpp::classifySmartSearchIntent), un autre plus neutre
// pour le texte libre qui tombe dans l'intent Unknown et part côté raisonnement
// IA (ai/ai_engine.cpp) — annoncer "Recherche en mémoire..." pour un message
// comme "il va falloir trouver les xp dans les dll" était trompeur.
const concreteThinkingMessages = [
  'Je vais rechercher ça en mémoire...',
  'Analyse de ta requête...',
  'Recherche en mémoire...',
  'Comparaison des candidats...',
  'Filtrage des faux positifs...',
  'Optimisation des résultats...',
]
const genericThinkingMessages = [
  'Je réfléchis...',
  'Analyse de ta requête...',
  'Je regarde ce que je peux faire...',
]
const thinkingIndex = ref(0)
const activeThinkingMessages = ref(concreteThinkingMessages)
const thinkingText = ref(activeThinkingMessages.value[0])
let thinkingTimer: ReturnType<typeof setInterval> | null = null
let lastSentQuery = ''

function queryLooksLikeConcreteScan(text: string): boolean {
  return /\d/.test(text) || /0x[0-9a-f]+/i.test(text)
}

watch(() => store.isSearching, (searching) => {
  if (searching) {
    activeThinkingMessages.value = queryLooksLikeConcreteScan(lastSentQuery)
      ? concreteThinkingMessages
      : genericThinkingMessages
    thinkingIndex.value = 0
    thinkingText.value = activeThinkingMessages.value[0]
    thinkingTimer = setInterval(() => {
      thinkingIndex.value = (thinkingIndex.value + 1) % activeThinkingMessages.value.length
      thinkingText.value = activeThinkingMessages.value[thinkingIndex.value]
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

// PHASE (08/09/2026, goulot d'etranglement) : amorce le serveur llama.cpp
// local et son cache_prompt des l'ouverture du panneau Assistant, pas au
// boot de l'app (l'init reste volontairement paresseuse cote backend, voir
// ai/ai_engine.cpp) -- evite que le tout premier vrai message utilisateur
// paie le cout de demarrage a froid (~90s au pire). Ignore si le backend
// externe (Claude) est actif : rien a rechauffer localement dans ce cas.
async function triggerLocalAiWarmup() {
  if (localAiWarmupRequested) return
  localAiWarmupRequested = true
  try {
    await store.refreshExternalAiStatus()
    if (store.externalAiActiveBackend !== 'claude') {
      await backend.getController().warmupLocalAiModel?.()
    }
  } catch {
    // Best-effort: le warmup n'est qu'une optimisation de latence, un echec reste silencieux.
  }
}

onMounted(() => {
  // AssistantView est la vue par defaut (store.activeView initial = 'assistant',
  // voir stores/app.ts) : elle monte AVANT que backend.connect() (QWebChannel,
  // lance depuis App.vue) ait fini -- backend.getController() jetterait donc
  // une exception silencieuse ici si on l'appelait tout de suite. On attend
  // l'evenement de connexion effective plutot que de deviner un delai.
  if (backend.isConnected) {
    void triggerLocalAiWarmup()
    return
  }
  const unsubscribe = backend.onConnectionChange(() => {
    unsubscribe()
    void triggerLocalAiWarmup()
  })
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
  if (store.investigationReport?.numericSources?.top?.length) {
    const best = store.investigationReport.numericSources.top[0]
    items.push({
      label: 'Enquête',
      value: `top 0x${best.address ?? '?'} · ${Math.round(Number(best.score ?? 0) * 100)}%`,
    })
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
  lastSentQuery = value
  store.searchQuery = value
  await store.doSearch()
  await scrollToBottom()
}

async function sendAutoResolve() {
  const value = chatInput.value.trim()
  if (!value) return
  chatInput.value = ''
  lastSentQuery = value
  store.searchQuery = value
  await store.doAutoResolve()
  await scrollToBottom()
}

async function sendExample(text: string) {
  lastSentQuery = text
  store.searchQuery = text
  await store.doSearch()
  await scrollToBottom()
}

async function useWorkflowPreset(preset: WorkflowPreset) {
  const applied = store.applyWorkflowPreset(preset.id)
  if (!applied) return
  if (applied.mode === 'auto' && store.isAttached) {
    await store.doAutoResolve()
  }
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
    store.activeView = 'expert'
    store.unknownScanMode = 'increased'
    await store.runAutoUnknownObservation(actionValue || store.targetValueGuided || 'capture')
  } else if (actionId === 'try_unknown_changed' || actionId === 'unknown_capture') {
    store.activeView = 'expert'
    store.unknownScanMode = 'changed'
    await store.runAutoUnknownObservation(actionValue || store.targetValueGuided || 'capture')
  } else if (actionId === 'continue_unknown_observation') {
    store.pushMessage('assistant', 'Fais varier la valeur dans le processus, puis tape la nouvelle observation ici. Je reprendrai Unknown automatiquement.')
  } else if (actionId === 'try_encrypted_scan' || actionId === 'encrypted_scan') {
    store.pendingAssistantAction = ''
    const value = actionValue || store.targetValueGuided || store.smartSearchContext?.initialValue || ''
    if (value.trim()) {
      await store.runAutoEncryptedScan(value)
    } else {
      store.activeView = 'expert'
      store.pushMessage('assistant', 'Donne-moi une valeur affichée, puis je lancerai le scan chiffré borné.')
    }
  } else if (actionId === 'trace_ui_string' || actionId === 'trace_ui_sources') {
    await store.acknowledgePendingSmartSearchRecovery()
    const value = actionValue || store.targetValueGuided || store.smartSearchContext?.initialValue || ''
    if (value.trim()) {
      await store.runAutoTraceUiString(value)
    } else {
      store.activeView = 'expert'
      store.pushMessage('assistant', 'Donne-moi la valeur affichée à l’écran, puis je lancerai Trace UI string.')
    }
  } else if (actionId === 'start_changed_pages_diff' || actionId === 'start_changed_pages_session') {
    await store.acknowledgePendingSmartSearchRecovery()
    const controller = backend.getController()
    const start = actionId === 'start_changed_pages_session'
      ? controller.startChangedPagesSession
      : controller.startChangedPagesDiff
    if (!start) {
      store.activeView = 'expert'
      store.pushMessage('assistant', 'Changed Pages non exposé par ce backend.', { isError: true })
    } else {
      const result = await start({
        maxBytesMb: 64,
        blockSize: 64 * 1024,
        privateOnly: true,
        writableOnly: true,
      })
      const payload = result as Record<string, unknown>
      const blocks = Number(payload.blocksCaptured ?? 0)
      store.pushMessage(
        'assistant',
        payload.success
          ? `Changed Pages démarré (${blocks} bloc(s), 64 Mo max). Fais varier la valeur affichée, puis donne-moi l'ancienne et la nouvelle valeur.`
          : `Changed Pages n'a pas pu démarrer : ${String(payload.error ?? 'raison inconnue')}.`,
        { isError: payload.success !== true },
      )
    }
  } else if (actionId === 'review_encrypted_hits') {
    store.activeView = 'expert'
    store.pushMessage('assistant', 'J’ai ouvert Expert : inspecte les hits chiffrés, Watch les meilleurs, puis transforme seulement une piste confirmée en write/freeze.')
  } else if (actionId === 'reduce_again' || actionId === 'reduce_with_new_value') {
    store.pushMessage('assistant', 'Fais varier la valeur dans le jeu, tape la nouvelle valeur observée, puis appuie sur Auto pour réduire les candidats.')
  } else if (actionId === 'run_exact' || actionId === 'exact_or_multitype') {
    store.searchQuery = actionValue || store.targetValueGuided || ''
    if (store.searchQuery.trim()) await store.doAutoResolve()
  } else if (actionId === 'attach_process') {
    store.activeView = 'process'
    store.pushMessage('assistant', 'Va dans Process, attache une application autorisée, puis reviens ici : je reprendrai le plan.')
  } else if (actionId === 'open_expert') {
    const expertStep = typeof action === 'string' ? '' : String(action.expertStep ?? '')
    if (expertStep === 'find' || expertStep === 'inspect' || expertStep === 'act' || expertStep === 'persist') {
      store.pendingExpertStep = expertStep
    }
    const expertAnchor = typeof action === 'string' ? '' : String(action.expertAnchor ?? '')
    if (expertAnchor) {
      store.pendingExpertAnchor = expertAnchor
    }
    store.activeView = 'expert'
    store.pushMessage('assistant', 'Expert ouvert. Je garde le contexte Assistant pour continuer la chaîne dès que tu valides une piste.')
  } else if (actionId === 'confirm_test_write' || actionId === 'guarded_write' || actionId === 'review_top_candidates') {
    store.pushMessage('assistant', 'Checkpoint écriture : je peux préparer le test, mais confirme explicitement la valeur à écrire et les candidats à utiliser.')
  } else if (actionId === 'escalate_freeze_bp') {
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    await store.escalateFreezeToBreakpoint(address)
  } else if (actionId === 'confirm_breakpoint_freeze' || actionId === 'guarded_freeze') {
    store.pushMessage('assistant', 'Checkpoint freeze BP : confirme l’adresse, le type et la valeur figée avant que je lance un breakpoint hardware.')
  } else if (actionId === 'trainer_checkpoint') {
    store.activeView = 'expert'
    store.pushMessage('assistant', 'Checkpoint trainer : vérifie la signature AOB et les matches avant tout patch ou hook.')
  } else if (actionId === 'find_what_writes_targets') {
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    const type = typeof action === 'string' ? 'Int32' : String(action.type ?? 'Int32')
    if (!address) {
      store.pushMessage('assistant', "Aucune adresse récente à capturer.", { isError: true })
    } else {
      store.pushMessage('assistant', 'Fais varier la valeur dans le jeu maintenant : je capture qui écrit sur cette adresse pendant quelques secondes...')
      await scrollToBottom()
      const result = await store.executeCheckpointFindWhatWrites({ address, type })
      if (result === null) {
        store.pushMessage('assistant', "Capture bloquée par ton mode Auto actuel (Safe). Passe en Expert ou Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
      } else {
        const hits = Array.isArray((result as Record<string, unknown>).hits) ? (result as Record<string, unknown>).hits as Array<Record<string, unknown>> : []
        const firstRip = hits.length > 0 ? String(hits[0].instructionPointer ?? '').trim() : ''
        store.pushMessage(
          'assistant',
          hits.length > 0
            ? `Capturé : ${hits.length} instruction(s) écrivent sur 0x${address}. Ajoutées aux checkpoints d'Investigation — tu peux y valider une piste avant d'écrire.`
            : `Aucune écriture capturée sur 0x${address} pendant la fenêtre. Soit la valeur n'a pas changé pendant la capture (réessaie en faisant varier plus vite), soit cette adresse n'est plus la bonne.`,
          {
            isError: hits.length === 0,
            recoveryActions: firstRip
              ? [
                  {
                    id: 'disassemble_backward_targets',
                    label: 'Chercher la vraie source',
                    address: firstRip,
                    watchedAddress: address,
                    reason: "Si l'écriture ne tient jamais, l'adresse ciblée est souvent un compteur animé recalculé à chaque frame — remonter le désassemblage trouve le vrai champ source.",
                  },
                  { id: 'open_expert', label: 'Ouvrir Expert' },
                ]
              : undefined,
          },
        )
      }
    }
  } else if (actionId === 'disassemble_backward_targets') {
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    const watchedAddress = typeof action === 'string' ? '' : String(action.watchedAddress ?? '')
    if (!address) {
      store.pushMessage('assistant', "Aucun RIP capturé à désassembler.", { isError: true })
    } else {
      const result = await store.executeCheckpointDisassembleBackward({ address })
      if (result === null) {
        store.pushMessage('assistant', "Désassemblage bloqué par ton mode Auto actuel (Safe). Passe en Expert ou Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
      } else {
        const candidates = Array.isArray(result.candidateFields) ? result.candidateFields : []
        if (candidates.length === 0) {
          store.pushMessage('assistant', `Aucun champ candidat trouvé avant 0x${address}. L'instruction capturée n'est peut-être pas la fin d'une chaîne de calcul exploitable — essaie une autre adresse ou vérifie le désassemblage dans Expert.`, { isError: true })
        } else {
          const list = candidates
            .map((field) => `[${field.memBaseRegister}+0x${Number(field.memDisplacement ?? 0).toString(16)}]`)
            .join(', ')
          store.pushMessage(
            'assistant',
            `${candidates.length} champ(s) candidat(s) trouvé(s) avant 0x${address} : ${list}. Ajoutés aux checkpoints d'Investigation. Ce sont souvent les vraies sources ("actuel"/"cible") d'un compteur animé, mais deviner lequel à l'œil demande de lire de l'assembleur.`,
            {
              recoveryActions: watchedAddress
                ? [
                    {
                      id: 'test_candidate_fields',
                      label: 'Tester automatiquement lequel tient',
                      address,
                      watchedAddress,
                      reason: 'Écrit une valeur test sur chaque champ, attend quelques secondes, puis vérifie lequel tient — pas besoin de lire l\'assembleur toi-même.',
                    },
                  ]
                : undefined,
            },
          )
        }
      }
    }
  } else if (actionId === 'test_candidate_fields') {
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    const watchedAddress = typeof action === 'string' ? '' : String(action.watchedAddress ?? '')
    if (!address || !watchedAddress) {
      store.pushMessage('assistant', "Adresse RIP ou adresse écrite manquante pour le test.", { isError: true })
    } else {
      store.pushMessage('assistant', 'Test en cours : écriture d\'une valeur test sur chaque champ candidat, puis vérification dans quelques secondes...')
      await scrollToBottom()
      const result = await store.executeCandidateFieldTest(address, watchedAddress)
      if (result === null) {
        store.pushMessage('assistant', "Test bloqué par ton mode Auto actuel (Safe) ou refusé à la confirmation. Passe en Expert ou Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
      } else {
        const outcomes = Array.isArray(result.results) ? result.results : []
        const holding = outcomes.filter((o) => o.verdict === 'holds')
        if (outcomes.length === 0) {
          store.pushMessage('assistant', `Test échoué : ${result.error ?? 'raison inconnue'}.`, { isError: true })
        } else {
          const summary = outcomes
            .map((o) => `[${o.memBaseRegister}+0x${Number(o.memDisplacement ?? 0).toString(16)}] : ${o.verdict === 'holds' ? 'tient' : o.verdict === 'reverts' ? 'repart' : 'erreur'}`)
            .join(', ')
          const best = holding[0]
          store.pushMessage(
            'assistant',
            holding.length > 0
              ? `Résultat : ${summary}. 0x${best.address} tient — c'est probablement la vraie source. Dis-moi la valeur à y écrire (ex: "mets 3000 à 0x${best.address}"), pas sur l'adresse affichée d'origine.`
              : `Résultat : ${summary}. Aucun champ ne tient — essaie un autre RIP capturé, ou vérifie le désassemblage dans Expert.`,
            {
              isError: holding.length === 0,
              recoveryActions: holding.length === 0 ? [{ id: 'open_expert', label: 'Ouvrir Expert' }] : undefined,
            },
          )
        }
      }
    }
  } else if (actionId === 'kernel_write_targets') {
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    const value = typeof action === 'string' ? '' : String(action.value ?? '')
    const valueType = typeof action === 'string' ? 'Int32' : String(action.valueType ?? 'Int32')
    if (!address || !value) {
      store.pushMessage('assistant', "Adresse ou valeur manquante pour l'écriture kernel.", { isError: true })
    } else {
      const result = await store.executeCheckpointKernelWrite({ address, value, type: valueType })
      if (result === null) {
        store.pushMessage('assistant', "Écriture kernel bloquée par ton mode Auto actuel (Safe) ou refusée à la confirmation. Passe en Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
      } else {
        store.pushMessage(
          'assistant',
          result.success
            ? `Écrit ${value} à 0x${address} via le driver kernel (${result.bytesWritten ?? 0} octet(s), contourne les protections usermode).`
            : `Écriture kernel échouée : ${result.error ?? 'raison inconnue'}. Vérifie que le driver KillEngineKernel est chargé (Paramètres > Driver kernel).`,
          { isError: !result.success },
        )
      }
    }
  } else if (actionId === 'speedhack_apply') {
    const mode = typeof action === 'string' ? 'set' : String(action.mode ?? 'set')
    const factor = typeof action === 'string' ? 1 : Number(action.factor ?? 1)
    if (mode === 'off') {
      const result = await store.stopSpeedhack()
      if (result === null) {
        store.pushMessage('assistant', "Impossible de désactiver le speedhack (backend indisponible).", { isError: true })
      } else {
        store.pushMessage(
          'assistant',
          result.success ? 'Speedhack désactivé, vitesse remise à la normale.' : `Échec : ${result.error ?? 'raison inconnue'}`,
          { isError: !result.success },
        )
      }
    } else {
      // setSpeedhackFactor reste synchrone (pas d'injection, juste un
      // réglage sur une session déjà active) ; startSpeedhack (première
      // activation) est non bloquant depuis son passage en Async — le
      // résultat définitif n'est plus dispo ici, seulement l'accusé de
      // démarrage. Le vrai résultat arrive via onSpeedhackStartFinished,
      // visible dans le panneau Speedhack.
      const alreadyActive = store.speedhackStatus?.active === true
      if (alreadyActive) {
        const result = await store.setSpeedhackFactor(factor)
        if (result === null) {
          store.pushMessage('assistant', "Speedhack bloqué par ton mode Auto actuel (Safe) ou refusé à la confirmation. Passe en Expert ou Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
        } else {
          store.pushMessage(
            'assistant',
            result.success ? `Speedhack ajusté à ${factor}x sur ${store.processName || 'la cible'}.` : `Speedhack échoué : ${result.error ?? 'raison inconnue'}.`,
            { isError: !result.success, recoveryActions: [{ id: 'open_speedhack', label: 'Ouvrir Speedhack' }] },
          )
        }
      } else {
        const result = await store.startSpeedhack(factor)
        if (result === null) {
          store.pushMessage('assistant', "Speedhack bloqué par ton mode Auto actuel (Safe) ou refusé à la confirmation. Passe en Expert ou Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
        } else if (!result.started) {
          store.pushMessage('assistant', `Speedhack échoué : ${result.error ?? 'raison inconnue'}.`, { isError: true })
        } else {
          store.pushMessage(
            'assistant',
            `Activation du speedhack à ${factor}x en cours sur ${store.processName || 'la cible'}...`,
            { recoveryActions: [{ id: 'open_speedhack', label: 'Ouvrir Speedhack' }] },
          )
        }
      }
    }
  } else if (actionId === 'network_block_apply') {
    // blockProcessNetworkAsync/unblockProcessNetworkAsync ne bloquent plus le
    // thread GUI (élévation UAC sur thread séparé) : le résultat définitif
    // n'est plus disponible ici, seulement l'accusé de démarrage. Le vrai
    // résultat (succès, exePath...) arrive plus tard via
    // onBlockProcessNetworkFinished/onUnblockProcessNetworkFinished, visible
    // dans le panneau Réseau / le journal d'actions.
    const off = typeof action === 'string' ? false : action.mode === 'off'
    const result = off ? await store.unblockProcessNetwork() : await store.blockProcessNetwork()
    if (result === null) {
      store.pushMessage('assistant', "Blocage réseau bloqué par ton mode Auto actuel (Safe/Expert) ou refusé à la confirmation. Passe en Trainer dans Paramètres pour autoriser ce type d'action.", { isError: true })
    } else if (!result.started) {
      store.pushMessage('assistant', `Blocage réseau échoué : ${result.error ?? 'raison inconnue'}.`, { isError: true })
    } else {
      store.pushMessage(
        'assistant',
        off
          ? "Retrait de la règle pare-feu en cours (invite UAC)..."
          : "Coupure réseau en cours (invite UAC)... Le résultat apparaîtra dans le journal d'actions.",
        { recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }] },
      )
    }
  } else if (actionId === 'dns_spoof_apply') {
    // Couverture chat modèle local (07/09/2026, meme patron que
    // network_block_apply ci-dessus) : reutilise networkStore.addDnsSpoofEntry/
    // removeDnsSpoofEntry (deja branches vers dnsSpoofFinished/dnsRestoreFinished
    // dans app.ts), en poussant d'abord les valeurs fournies par le modele
    // dans les refs que ces fonctions parametree-less lisent.
    const restore = typeof action === 'string' ? false : action.mode === 'restore'
    const domain = typeof action === 'string' ? '' : String(action.domain ?? '')
    const targetIp = typeof action === 'string' ? '' : String(action.targetIp ?? '')
    if (!domain) {
      store.pushMessage('assistant', "Domaine manquant pour l'action DNS.", { isError: true })
    } else if (restore) {
      await store.removeDnsSpoofEntry(domain)
      store.pushMessage('assistant', `Restauration DNS en cours pour ${domain}... Le résultat apparaîtra dans le journal d'actions.`, {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    } else {
      store.dnsSpoofDomain = domain
      store.dnsSpoofTargetIp = targetIp || '127.0.0.1'
      await store.addDnsSpoofEntry()
      store.pushMessage('assistant', `Redirection DNS en cours : ${domain} -> ${store.dnsSpoofTargetIp}... Le résultat apparaîtra dans le journal d'actions.`, {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    }
  } else if (actionId === 'http_proxy_apply') {
    const stop = typeof action === 'string' ? false : action.mode === 'stop'
    if (stop) {
      await store.stopHttpProxy()
      store.pushMessage('assistant', "Arrêt du proxy HTTP en cours... Le résultat apparaîtra dans le journal d'actions.", {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    } else {
      const port = typeof action === 'string' ? 8080 : Number(action.port ?? 8080)
      const interceptHttps = typeof action === 'string' ? true : action.interceptHttps !== false
      store.httpProxyPort = port
      store.httpProxyInterceptHttps = interceptHttps
      await store.startHttpProxy()
      store.pushMessage('assistant', `Démarrage du proxy HTTP en cours (port ${port})... Le résultat apparaîtra dans le journal d'actions.`, {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    }
  } else if (actionId === 'modify_http_request_apply') {
    const requestId = typeof action === 'string' ? '' : String(action.requestId ?? '')
    const newBody = typeof action === 'string' ? '' : String(action.newBody ?? '')
    if (!requestId) {
      store.pushMessage('assistant', "Identifiant de requête HTTP manquant.", { isError: true })
    } else {
      store.selectedHttpRequest = requestId
      await store.modifySelectedHttpRequest(newBody)
      store.pushMessage('assistant', `Requête ${requestId} modifiée.`, {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    }
  } else if (actionId === 'lag_switch_apply') {
    const enabled = typeof action === 'string' ? true : action.enabled !== false
    const delayMs = typeof action === 'string' ? 1000 : Number(action.delayMs ?? 1000)
    if (store.lagSwitchActive === enabled) {
      store.pushMessage('assistant', enabled ? 'Lag switch déjà actif.' : 'Lag switch déjà désactivé.')
    } else {
      store.lagSwitchDelayMs = delayMs
      await store.toggleLagSwitch()
      store.pushMessage('assistant', `${enabled ? 'Activation' : 'Désactivation'} du lag switch en cours... Le résultat apparaîtra dans le journal d'actions.`, {
        recoveryActions: [{ id: 'open_network', label: 'Ouvrir Réseau' }],
      })
    }
  } else if (actionId === 'stealth_mode_apply') {
    // applyStealthMode/restoreStealthMode gardent leur propre confirmRiskAction
    // interne (panneau Stealth des Réglages) -- reutilises tel quel, pas de
    // second mecanisme de confirmation invente ici.
    const restore = typeof action === 'string' ? false : action.mode === 'restore'
    const profile = typeof action === 'string' ? 'default' : String(action.profile ?? 'default')
    const result = restore ? await store.restoreStealthMode() : await store.applyStealthMode(profile)
    if (result === null) {
      store.pushMessage('assistant', "Mode discret refusé à la confirmation.", { isError: true })
    } else {
      store.pushMessage(
        'assistant',
        result.success
          ? (restore ? 'Mode discret désactivé.' : `Mode discret activé (profil ${profile}).`)
          : `Échec : ${result.error ?? 'raison inconnue'}`,
        { isError: !result.success },
      )
    }
  } else if (actionId === 'webview2_evaluate_apply') {
    const expression = typeof action === 'string' ? '' : String(action.expression ?? '')
    if (!expression.trim()) {
      store.pushMessage('assistant', "Expression JavaScript manquante.", { isError: true })
    } else {
      const webView2Store = useWebView2InspectorStore()
      if (!webView2Store.isConnected) {
        store.pushMessage('assistant', "Aucune target WebView2 connectée — ouvre l'onglet WebView2 pour t'y connecter d'abord.", {
          isError: true,
          recoveryActions: [{ id: 'open_webview2_inspector', label: 'Ouvrir WebView2' }],
        })
      } else {
        const result = await webView2Store.evaluateJavaScript(expression)
        store.pushMessage(
          'assistant',
          result?.success
            ? `Résultat : ${JSON.stringify(result.value ?? null)}`
            : `Échec : ${webView2Store.error ?? result?.error ?? 'raison inconnue'}`,
          { isError: !result?.success },
        )
      }
    }
  } else if (actionId === 'write_value_confirm' || actionId === 'freeze_value_confirm') {
    // PHASE 120-D : meme patron que kernel_write_targets ci-dessus, reutilise
    // executeCheckpointWrite (deja gate par confirmRiskAction) plutot que
    // d'inventer une nouvelle logique d'ecriture.
    const isFreeze = actionId === 'freeze_value_confirm'
    const address = typeof action === 'string' ? '' : String(action.address ?? '')
    const value = typeof action === 'string' ? '' : String(action.value ?? '')
    const valueType = typeof action === 'string' ? 'Int32' : String(action.valueType ?? 'Int32')
    if (!address || !value) {
      store.pushMessage('assistant', isFreeze ? "Adresse ou valeur manquante pour le freeze." : "Adresse ou valeur manquante pour l'écriture.", { isError: true })
    } else {
      const result = await store.executeCheckpointWrite({ address, type: valueType, value }, isFreeze)
      if (result === null) {
        store.pushMessage('assistant', isFreeze ? "Freeze refusé à la confirmation ou adresse/valeur invalide." : "Écriture refusée à la confirmation ou adresse/valeur invalide.", { isError: true })
      } else {
        store.pushMessage(
          'assistant',
          result.success
            ? (isFreeze ? `Figé ${value} (${valueType}) à 0x${address}.` : `Écrit ${value} (${valueType}) à 0x${address}.`)
            : `${isFreeze ? 'Freeze échoué' : 'Écriture échouée'} : ${result.error ?? 'raison inconnue'}.`,
          { isError: !result.success },
        )
      }
    }
  } else if (actionId === 'chat_memory_write_confirm' || actionId === 'chat_memory_freeze_confirm' || actionId === 'rewrite_last_auto_write_confirm') {
    // RiskGate chat (29/08/2026) : avant ce correctif, taper une adresse puis
    // une valeur dans le chat ecrivait/figeait reellement la memoire sans
    // AUCUNE interaction utilisateur (constate en direct pendant PHASE 120-D).
    // Le clic sur ce recoveryAction EST la confirmation (pas de second modal
    // confirmRiskAction, retire a la demande du proprietaire -- juge redondant
    // avec cette carte qui affiche deja l'avertissement de risque et le
    // libelle exact de l'action juste avant le bouton). Les adresses cibles
    // restent server-side (m_chatMemoryTargets), un seul recoveryAction
    // suffit quel que soit leur nombre.
    const value = typeof action === 'string' ? '' : String(action.value ?? '')
    if (!value) {
      store.pushMessage('assistant', "Valeur manquante.", { isError: true })
    } else {
      const result = actionId === 'chat_memory_write_confirm'
        ? await store.confirmChatMemoryWrite(value)
        : actionId === 'chat_memory_freeze_confirm'
          ? await store.confirmChatMemoryFreeze(value)
          : await store.confirmRewriteLastAutoWrite(value)
      if (result === null) {
        store.pushMessage('assistant', "Action refusée à la confirmation ou indisponible.", { isError: true })
      } else {
        store.pushMessage(
          'assistant',
          String(result.message ?? (result.success ? 'Fait.' : 'Échoué.')),
          { isError: result.success !== true },
        )
      }
    }
  } else if (actionId === 'trainer_apply_confirm' || actionId === 'trainer_restore_confirm') {
    // PHASE 120-D : reutilise applyTrainerFeature/restoreTrainerFeature (et
    // leurs variantes "all"), qui gerent deja l'ordre des dependances et la
    // confirmation RiskGate en interne (doApplyTrainerFeature/
    // doRestoreTrainerFeature) -- zero nouvelle logique d'execution.
    const restore = actionId === 'trainer_restore_confirm'
    const all = typeof action === 'string' ? false : action.all === true
    const idTarget = typeof action === 'string' ? '' : String(action.id_target ?? '')
    if (all) {
      if (restore) await store.restoreAllTrainerFeatures()
      else await store.applyAllTrainerFeatures()
      store.pushMessage('assistant', restore ? 'Restauration de toutes les features Trainer actives terminée.' : 'Activation de toutes les features Trainer terminée.')
    } else {
      const id = Number(idTarget)
      if (!id || id <= 0) {
        store.pushMessage('assistant', "Id de feature Trainer invalide.", { isError: true })
      } else {
        if (restore) await store.restoreTrainerFeature(id)
        else await store.applyTrainerFeature(id)
        const feature = store.trainerFeatures.find((f) => f.id === id)
        const ok = restore ? feature?.enabled === false : feature?.enabled === true
        store.pushMessage(
          'assistant',
          ok
            ? `Feature Trainer #${id} ${restore ? 'restaurée' : 'activée'}.`
            : `Feature Trainer #${id} : ${feature?.lastError || 'pas de changement (refusé à la confirmation ou déjà dans cet état).'}`,
          { isError: !ok },
        )
      }
    }
  } else if (actionId === 'open_network') {
    store.activeView = 'network'
    store.pushMessage('assistant', "Réseau ouvert : coupe ou rétablis l'accès réseau de la cible depuis là.")
  } else if (actionId === 'open_speedhack') {
    store.activeView = 'speedhack'
    store.pushMessage('assistant', "Speedhack ouvert : le slider et les presets sont là, réglables en direct.")
  } else if (actionId === 'open_pointer_scan') {
    store.pendingExpertStep = 'inspect'
    store.activeView = 'expert'
    store.pushMessage('assistant', "Expert ouvert, section Pointeurs : lance un scan de pointeur stable vers la dernière adresse. Ça permet de la retrouver même si elle change d'une partie à l'autre.")
  } else if (actionId === 'open_clr_inspector') {
    store.activeView = 'clr'
    store.pushMessage('assistant', "CLR Inspector ouvert : cherche l'objet par type ou par valeur de champ plutôt que par adresse brute — c'est l'équivalent d'une chaîne de pointeurs pour ce genre de cible.")
  } else if (actionId === 'open_webview2_inspector') {
    store.activeView = 'webview2'
    store.pushMessage('assistant', "WebView2 Inspector ouvert : inspecte les pages web embarquées via le protocole Chrome DevTools (CDP) — utile pour les jeux Electron ou les UI web.")
  }
  await scrollToBottom()
}

function recoveryActionClass(action: Record<string, unknown>): string {
  const requiresConfirmation = action.requiresConfirmation === true || action.safe === false
  return requiresConfirmation ? 'btn-risk' : 'btn-safe'
}

function recoveryActionLabel(action: Record<string, unknown>): string {
  const label = String(action.label ?? action.id ?? 'Action')
  const requiresConfirmation = action.requiresConfirmation === true || action.safe === false
  return requiresConfirmation ? `Confirmer: ${label}` : label
}

function recoveryActionTitle(action: Record<string, unknown>): string {
  const reason = String(action.reason ?? '').trim()
  const requiresConfirmation = action.requiresConfirmation === true || action.safe === false
  if (requiresConfirmation) return reason || 'Action risquée : confirmation explicite requise avant exécution.'
  return reason || 'Action sûre : lecture, scan ou navigation.'
}

function safeStepClass(step: Record<string, unknown>): string {
  const status = String(step.status ?? '').toLowerCase()
  if (status === 'error' || status === 'failed') return 'safe-step-error'
  if (status === 'warning' || status === 'partial') return 'safe-step-warning'
  return 'safe-step-success'
}

function safeStepLabel(step: Record<string, unknown>): string {
  const tool = String(step.tool ?? 'outil')
  if (tool === 'exact_scan_multi_type') return 'Scan multi-type'
  if (tool === 'next_scan') return 'Réduction'
  if (tool === 'scan_encrypted_value') return 'Scan chiffré'
  if (tool === 'scan_ui_strings') return 'Trace UI string'
  return tool
}

function safeStepMetric(step: Record<string, unknown>): string {
  const payload = typeof step.payload === 'object' && step.payload !== null
    ? step.payload as Record<string, unknown>
    : {}
  const count = payload.matchesFound ?? payload.matchesReturned ?? payload.candidateStoreSize ?? payload.remaining
  const partial = payload.partial === true ? ' · partiel' : ''
  if (count === undefined || count === null || count === '') return partial.trim()
  return `${Number(count).toLocaleString('fr-FR')} résultat(s)${partial}`
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
    case 'awaiting_write_confirmation':
      return 'Auto : écriture à confirmer'
    case 'auto_resolve_no_candidate':
      return 'Auto : aucune piste restante'
    case 'awaiting_unknown_observation':
      return 'Auto : snapshot Unknown prêt'
    case 'auto_resolve_planned':
      return 'Auto : plan prêt'
    case 'auto_write_partial_or_failed':
      return 'Écriture partielle — vérifie manuellement'
    case 'auto_write_problem':
      return 'Adresses à vérifier'
    case 'no_candidate':
      return 'Aucun candidat restant'
    case 'trace_ui_string_found':
      return 'En attente : filtrer dans Expert'
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
    case 'awaiting_write_confirmation':
    case 'auto_resolve_planned':
    case 'trace_ui_string_found':
      return 'wf-warning'
    case 'auto_write_partial_or_failed':
    case 'auto_write_problem':
    case 'no_candidate':
    case 'auto_resolve_no_candidate':
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

    <PanelIntro
      what="Le chat principal pour trouver et modifier des valeurs en mémoire sans connaître les termes techniques."
      purpose="Décrire ce que tu cherches en langage naturel (une valeur affichée, un freeze, un trainer...) et laisser KillEngine choisir le bon outil."
      how="Attache un processus, puis écris ta demande dans le champ de recherche en bas — une valeur affichée à l'écran suffit pour démarrer un scan."
    />

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
        <div class="workflow-presets">
          <button
            v-for="preset in store.workflowPresets"
            :key="preset.id"
            class="workflow-preset"
            :class="{ selected: store.lastWorkflowPresetId === preset.id }"
            type="button"
            @click="useWorkflowPreset(preset)"
          >
            <strong>{{ preset.title }}</strong>
            <span>{{ preset.description }}</span>
            <em>{{ preset.mode === 'auto' ? 'Auto' : 'Guide' }} · {{ preset.risk }}</em>
          </button>
        </div>
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
          <button class="example-chip" @click="sendExample('quelle piste tester maintenant ?')">
            Quelle piste tester ?
          </button>
          <button class="example-chip" @click="sendExample('pourquoi le trainer est bloqué ?')">
            Pourquoi bloqué ?
          </button>
        </div>
        <div v-if="!store.isAttached" class="warn-text">
          <p>⚠ Attache d'abord un processus pour pouvoir chercher une valeur.</p>
          <button class="btn btn-secondary compact" @click="store.activeView = 'process'">Aller à Processus</button>
        </div>
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

          <div v-if="msg.executedSafeSteps && msg.executedSafeSteps.length > 0" class="safe-steps-box">
            <div class="suggestions-title">Actions sûres exécutées</div>
            <div
              v-for="(step, index) in msg.executedSafeSteps"
              :key="`${step.tool}-${index}`"
              class="safe-step-row"
              :class="safeStepClass(step)"
            >
              <div class="safe-step-main">
                <span class="safe-step-index">{{ index + 1 }}</span>
                <strong>{{ safeStepLabel(step) }}</strong>
                <span v-if="safeStepMetric(step)" class="safe-step-metric">{{ safeStepMetric(step) }}</span>
              </div>
              <div v-if="step.detail" class="safe-step-detail">{{ step.detail }}</div>
            </div>
          </div>

          <!-- Auto-write result -->
          <div
            v-if="msg.autoWriteResults && msg.autoWriteResults.length > 0"
            class="auto-write-box"
            :class="msg.invalidated ? 'auto-invalidated' : (msg.autoWriteOk ? 'auto-ok' : 'auto-fail')"
          >
            <div class="auto-write-title">
              {{ msg.invalidated ? '✗ Signalé non fonctionnel ensuite' : (msg.autoWriteOk ? '✓ Écriture auto réussie' : '⚠ Écriture auto partielle') }}
            </div>
            <p v-if="msg.invalidated" class="invalidated-note">
              L'écriture a bien été appliquée en mémoire, mais tu as indiqué que ça n'a pas changé le comportement du jeu — ce n'est probablement pas la bonne adresse.
            </p>
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
              <span>{{ msg.invalidated ? '✗ invalidé' : (r.verified ? '✓ final vérifié' : '✗ non vérifié') }}</span>
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
                :class="recoveryActionClass(action)"
                :title="recoveryActionTitle(action)"
                @click="runRecoveryAction(action)"
              >
                {{ recoveryActionLabel(action) }}
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
            {{ msg.confirmationReason || 'Confirmation nécessaire avant de continuer — vérifie les actions proposées ci-dessous.' }}
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
      <button class="btn btn-secondary" :disabled="!chatInput.trim() || store.isSearching" title="Planifie et lance seulement les actions sûres." @click="sendAutoResolve()">
        Auto
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

.workflow-presets {
  display: grid;
  width: min(760px, 100%);
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 8px;
  margin: 10px 0 4px;
}

.workflow-preset {
  min-height: 92px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-tertiary);
  color: var(--text-primary);
  cursor: pointer;
  text-align: left;
  transition: border-color 0.15s, background 0.15s;
}

.workflow-preset:hover,
.workflow-preset.selected {
  border-color: var(--accent);
  background: var(--bg-accent);
}

.workflow-preset strong,
.workflow-preset span,
.workflow-preset em {
  display: block;
}

.workflow-preset strong {
  font-size: 13px;
}

.workflow-preset span {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 12px;
  line-height: 1.35;
}

.workflow-preset em {
  margin-top: 8px;
  color: var(--accent);
  font-size: 11px;
  font-style: normal;
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

@media (max-width: 760px) {
  .workflow-presets {
    grid-template-columns: 1fr;
  }
}

.warn-text {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 8px;
  margin-top: 16px;
  color: var(--warning);
  font-size: 12px;
}

.warn-text .btn {
  font-size: 12px;
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

.safe-steps-box {
  margin-top: 6px;
  padding: 8px 10px;
  border: 1px solid rgba(122, 162, 247, 0.28);
  border-radius: 8px;
  background: rgba(122, 162, 247, 0.07);
}

.safe-step-row {
  padding: 7px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.safe-step-row:first-of-type {
  border-top: 0;
}

.safe-step-main {
  display: flex;
  flex-wrap: wrap;
  gap: 7px;
  align-items: center;
  color: var(--text-secondary);
  font-size: 12px;
}

.safe-step-index {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: rgba(122, 162, 247, 0.16);
  color: var(--accent);
  font-size: 10px;
  font-weight: 700;
}

.safe-step-success strong {
  color: var(--success);
}

.safe-step-warning strong {
  color: var(--warning);
}

.safe-step-error strong {
  color: var(--error);
}

.safe-step-metric {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.safe-step-detail {
  margin-top: 3px;
  color: var(--text-dim);
  font-size: 11px;
  line-height: 1.35;
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

.auto-write-box.auto-invalidated {
  background: var(--bg-tertiary);
  border-color: var(--border);
  opacity: 0.7;
}

.auto-write-box.auto-invalidated .auto-write-title {
  color: var(--text-dim);
}

.invalidated-note {
  margin-bottom: 6px;
  color: var(--text-dim);
  font-style: italic;
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

.recovery-box .btn-safe {
  border-color: rgba(122, 162, 247, 0.35);
}

.recovery-box .btn-risk {
  border-color: rgba(224, 175, 104, 0.55);
  background: rgba(224, 175, 104, 0.12);
  color: var(--warning);
}

.recovery-box .btn-risk:hover:not(:disabled) {
  background: rgba(224, 175, 104, 0.2);
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
