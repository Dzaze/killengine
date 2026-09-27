<script setup lang="ts">
/**
 * UX-PRODUIT-15C -- panneau-coach flottant du guide interactif du tutoriel,
 * monté uniquement dans l'instance enfant tutoriel (App.vue, v-if
 * store.isTutorialMode). Ce composant ne fait JAMAIS de scan/écriture lui-
 * même -- il dirige l'utilisateur vers les vraies vues (Processus, Expert,
 * Profils) avec les vrais formulaires/confirmations, puis vérifie l'état
 * réel sur un clic explicite "Vérifier cette étape".
 *
 * Règle dure : aucune valeur de groundTruth (santé/adresses de la cible
 * démo) n'est jamais interpolée dans ce template -- l'utilisateur doit la
 * lire dans la fenêtre de la cible démo elle-même, exactement comme dans un
 * vrai jeu. Ne pas ajouter de {{ ... }} qui afficherait ces champs.
 */
import { computed, onMounted, ref } from 'vue'
import { useTutorialStore, TUTORIAL_STEP_ORDER, type TutorialStepId } from '@/stores/tutorial'
import { useI18n } from 'vue-i18n'

const { t } = useI18n()
const store = useTutorialStore()

const profileName = ref('')
const targetName = ref('')
const retrieveBusy = ref(false)

onMounted(() => {
  void store.ensureGroundTruth()
})

const stepGoTo: Partial<Record<TutorialStepId, 'process' | 'expert'>> = {
  attach: 'process',
  search: 'expert',
  refine: 'expert',
  write: 'expert',
}

function stepIcon(id: TutorialStepId): string {
  const status = store.stepStatus[id]
  if (status === 'passed') return '✓'
  if (status === 'failed') return '!'
  if (status === 'checking') return '…'
  return String(TUTORIAL_STEP_ORDER.indexOf(id) + 1)
}

function stepClass(id: TutorialStepId): string {
  const idx = TUTORIAL_STEP_ORDER.indexOf(id)
  if (store.stepStatus[id] === 'passed') return 'done'
  if (idx === store.currentStepIndex) return 'active'
  return ''
}

const currentStatus = computed(() => store.stepStatus[store.currentStepId])
const currentError = computed(() => store.stepError[store.currentStepId])
const goToTarget = computed(() => stepGoTo[store.currentStepId])

async function verifyCurrentStep() {
  switch (store.currentStepId) {
    case 'attach':
      await store.verifyAttach()
      break
    case 'search':
      await store.verifySearch()
      break
    case 'refine':
      store.verifyRefine()
      break
    case 'write':
      store.verifyWrite()
      break
    case 'verify':
      await store.verifyEffect()
      break
    case 'save':
      if (!profileName.value.trim() || !targetName.value.trim()) return
      await store.verifySave(profileName.value.trim(), targetName.value.trim())
      break
    default:
      break
  }
}

async function onRestartTarget() {
  await store.restartTarget()
}

async function onVerifyRetrieve() {
  if (!profileName.value.trim() || !targetName.value.trim()) return
  retrieveBusy.value = true
  try {
    await store.verifyRetrieve(profileName.value.trim(), targetName.value.trim())
  } finally {
    retrieveBusy.value = false
  }
}
</script>

<template>
  <div v-if="!store.guideVisible" class="tutorial-reopen">
    <button type="button" class="risk-btn secondary" @click="store.reopen()">
      {{ t('tutorial.entry.reopen') }}
    </button>
  </div>
  <aside
    v-else
    class="tutorial-guide"
    role="complementary"
    :aria-label="t('tutorial.title')"
  >
    <header class="tutorial-guide-header">
      <div>
        <h2>{{ t('tutorial.title') }}</h2>
        <p class="tutorial-guide-subtitle">{{ t('tutorial.subtitle') }}</p>
      </div>
      <button
        type="button"
        class="tutorial-guide-quit"
        :title="t('tutorial.actions.quit')"
        :aria-label="t('tutorial.actions.quit')"
        @click="store.quit()"
      >
        ✕
      </button>
    </header>

    <ol class="tutorial-steps">
      <li v-for="id in TUTORIAL_STEP_ORDER" :key="id" :class="stepClass(id)">
        <span class="tutorial-step-icon">{{ stepIcon(id) }}</span>
        <span>{{ t(`tutorial.steps.${id}.label`) }}</span>
      </li>
    </ol>

    <div v-if="!store.tutorialComplete" class="tutorial-step-detail">
      <p class="tutorial-step-hint">{{ t(`tutorial.steps.${store.currentStepId}.hint`) }}</p>
      <p class="tutorial-step-expected">{{ t(`tutorial.steps.${store.currentStepId}.expected`) }}</p>

      <button
        v-if="goToTarget"
        type="button"
        class="risk-btn secondary"
        @click="store.goToView(goToTarget!)"
      >
        {{ t(`tutorial.steps.${store.currentStepId}.goTo`) }}
      </button>

      <div v-if="store.currentStepId === 'save'" class="tutorial-save-fields">
        <label>
          {{ t('tutorial.actions.profileNamePlaceholder') }}
          <input v-model="profileName" type="text" :placeholder="t('tutorial.actions.profileNamePlaceholder')">
        </label>
        <label>
          {{ t('tutorial.actions.targetNamePlaceholder') }}
          <input v-model="targetName" type="text" :placeholder="t('tutorial.actions.targetNamePlaceholder')">
        </label>
      </div>

      <div class="tutorial-step-actions">
        <button
          type="button"
          class="risk-btn primary"
          :disabled="currentStatus === 'checking'"
          @click="verifyCurrentStep"
        >
          {{ t('tutorial.actions.verify') }}
        </button>
        <button
          v-if="currentStatus === 'failed'"
          type="button"
          class="risk-btn secondary"
          @click="store.retryStep(store.currentStepId)"
        >
          {{ t('tutorial.actions.retry') }}
        </button>
      </div>

      <p v-if="currentStatus === 'checking'" class="tutorial-status checking">
        {{ t('tutorial.status.checking') }}
      </p>
      <p v-else-if="currentStatus === 'passed' && store.currentStepId !== 'save'" class="tutorial-status passed">
        {{ t('tutorial.status.passed') }}
      </p>
      <p v-else-if="currentStatus === 'failed'" class="tutorial-status failed">
        {{ t('tutorial.status.failed', { reason: currentError }) }}
      </p>

      <div v-if="store.currentStepId === 'save' && store.stepStatus.save === 'passed'" class="tutorial-retrieve">
        <p class="tutorial-step-hint">{{ t('tutorial.actions.restartTarget') }}</p>
        <button type="button" class="risk-btn secondary" :disabled="store.restartBusy" @click="onRestartTarget">
          {{ t('tutorial.actions.restartTarget') }}
        </button>
        <button
          type="button"
          class="risk-btn primary"
          :disabled="retrieveBusy"
          @click="onVerifyRetrieve"
        >
          {{ t('tutorial.actions.verify') }}
        </button>
        <p v-if="retrieveBusy" class="tutorial-status checking">{{ t('tutorial.status.checking') }}</p>
        <p v-else-if="store.retrieveVerified" class="tutorial-status passed">{{ t('tutorial.status.passed') }}</p>
      </div>
    </div>

    <div v-else class="tutorial-complete">
      <h3>{{ t('tutorial.status.completedTitle') }}</h3>
      <p>{{ t('tutorial.status.completedDetail') }}</p>
      <button type="button" class="risk-btn primary" @click="store.quit()">
        {{ t('tutorial.actions.quit') }}
      </button>
    </div>
  </aside>
</template>

<style scoped>
.tutorial-guide {
  position: fixed;
  right: 16px;
  bottom: 16px;
  z-index: 900;
  width: 340px;
  max-width: calc(100vw - 32px);
  max-height: calc(100vh - 32px);
  overflow-y: auto;
  background: var(--bg-secondary);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 14px;
  box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
}

.tutorial-guide-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 8px;
}

.tutorial-guide-header h2 {
  margin: 0;
  font-size: 15px;
  color: var(--text-primary);
}

.tutorial-guide-subtitle {
  margin: 4px 0 0;
  font-size: 12px;
  color: var(--text-muted);
}

.tutorial-guide-quit {
  background: none;
  border: none;
  color: var(--text-muted);
  cursor: pointer;
  font-size: 14px;
  line-height: 1;
  padding: 4px;
}

.tutorial-steps {
  list-style: none;
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 12px 0;
  padding: 0;
}

.tutorial-steps li {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: var(--text-muted);
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 3px 8px;
}

.tutorial-steps li.active {
  color: var(--text-primary);
  border-color: var(--accent);
}

.tutorial-steps li.done {
  color: var(--success);
  border-color: var(--success);
}

.tutorial-step-icon {
  font-weight: 600;
}

.tutorial-step-detail {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.tutorial-step-hint {
  margin: 0;
  color: var(--text-primary);
  font-size: 13px;
}

.tutorial-step-expected {
  margin: 0;
  color: var(--text-muted);
  font-size: 12px;
  font-style: italic;
}

.tutorial-save-fields {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.tutorial-save-fields label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-size: 11px;
  color: var(--text-muted);
}

.tutorial-save-fields input {
  background: var(--bg-tertiary);
  border: 1px solid var(--border);
  border-radius: 6px;
  color: var(--text-primary);
  padding: 6px 8px;
}

.tutorial-step-actions {
  display: flex;
  gap: 8px;
}

.tutorial-status {
  margin: 0;
  font-size: 12px;
}

.tutorial-status.checking {
  color: var(--text-muted);
}

.tutorial-status.passed {
  color: var(--success);
}

.tutorial-status.failed {
  color: var(--error);
}

.tutorial-retrieve {
  display: flex;
  flex-direction: column;
  gap: 8px;
  border-top: 1px solid var(--border);
  padding-top: 8px;
  margin-top: 4px;
}

.tutorial-complete {
  text-align: center;
}

.tutorial-complete h3 {
  color: var(--success);
  margin: 4px 0;
}

.tutorial-reopen {
  position: fixed;
  right: 16px;
  bottom: 16px;
  z-index: 900;
}

@media (max-width: 480px) {
  .tutorial-guide {
    right: 8px;
    bottom: 8px;
    width: calc(100vw - 16px);
  }
}
</style>
