<script setup lang="ts">
// ANALYSE-CLINE-1 — vue dédiée Pattern Learning (03/09/2026, Claude). Le
// backend (core/pattern_learning/*, apps/desktop/pattern_learning_manager.*)
// et les 18 méthodes Q_INVOKABLE existaient déjà depuis c9991c3
// (02/09/2026) mais sans aucune vue. Couverture volontairement partielle
// (statistiques, détection moteur, classification, profils, suggestions) —
// voir docs/PHASE_TRACKER.md "ANALYSE-CLINE-1"/"PATTERN-LEARNING-1" pour ce
// qui reste hors scope (clustering, suivi temps réel).
import { computed, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t } = useI18n()

const PATTERN_TYPE_NAMES = computed(() => [
  t('patternLearning.patternTypes.unknown'),
  t('patternLearning.patternTypes.resourceCounter'),
  t('patternLearning.patternTypes.healthPoints'),
  t('patternLearning.patternTypes.stateFlag'),
  t('patternLearning.patternTypes.timer'),
  t('patternLearning.patternTypes.coordinate'),
  t('patternLearning.patternTypes.uiReference'),
  t('patternLearning.patternTypes.noise'),
])

const statistics = ref<Record<string, unknown>>({})

async function loadStatistics() {
  const result = await store.getPatternLearningStatistics()
  statistics.value = result ?? {}
}

// --- Détection de moteur ---
const detectionResult = ref<Record<string, unknown> | null>(null)
const detecting = ref(false)

async function runDetectEngine() {
  detecting.value = true
  try {
    const moduleNames = store.processModules.map((m) => m.name)
    detectionResult.value = await store.detectGameEngine(moduleNames)
  } finally {
    detecting.value = false
  }
}

// --- Classification de pattern ---
const classifyAddress = ref('')
const classifyValues = ref('')
const classifying = ref(false)
const classification = ref<Record<string, unknown> | null>(null)

async function runClassify() {
  const values = classifyValues.value
    .split(',')
    .map((v) => Number(v.trim()))
    .filter((v) => !Number.isNaN(v))
  if (!classifyAddress.value.trim() || values.length === 0) return
  classifying.value = true
  try {
    const timestamps = values.map((_, i) => i * 100)
    classification.value = await store.classifyMemoryPattern(classifyAddress.value.trim(), values, timestamps)
  } finally {
    classifying.value = false
  }
}

// --- Profils ---
const knownGames = ref<string[]>([])
const selectedGame = ref('')
const loadedProfile = ref<Record<string, unknown> | null>(null)
const newGameName = ref('')
const newExecutableName = ref('')
const profileBusy = ref(false)

async function refreshGames() {
  knownGames.value = await store.listKnownGameProfiles()
}

async function loadSelectedProfile() {
  if (!selectedGame.value) return
  profileBusy.value = true
  try {
    loadedProfile.value = await store.loadGameProfile(selectedGame.value)
  } finally {
    profileBusy.value = false
  }
}

async function deleteSelectedProfile() {
  if (!selectedGame.value) return
  profileBusy.value = true
  try {
    const ok = await store.deleteGameProfile(selectedGame.value)
    if (ok) {
      if (loadedProfile.value?.gameName === selectedGame.value) loadedProfile.value = null
      selectedGame.value = ''
      await refreshGames()
    }
  } finally {
    profileBusy.value = false
  }
}

async function createProfile() {
  if (!newGameName.value.trim()) return
  profileBusy.value = true
  try {
    const ok = await store.saveGameProfile({
      gameName: newGameName.value.trim(),
      executableName: newExecutableName.value.trim(),
    })
    if (ok) {
      newGameName.value = ''
      newExecutableName.value = ''
      await refreshGames()
    }
  } finally {
    profileBusy.value = false
  }
}

// --- Suggestions ---
const suggestGameName = ref('')
const suggestPatternType = ref(1)
const suggestCount = ref(5)
const suggestions = ref<Array<Record<string, unknown>>>([])
const suggesting = ref(false)

async function runSuggestions() {
  if (!suggestGameName.value.trim()) return
  suggesting.value = true
  try {
    suggestions.value = await store.getTopPatternSuggestions(
      suggestGameName.value.trim(),
      suggestPatternType.value,
      suggestCount.value,
    )
  } finally {
    suggesting.value = false
  }
}

const classificationReasoning = computed(() => (classification.value?.reasoning as string[] | undefined) ?? [])
const profileKnownOffsets = computed(
  () => (loadedProfile.value?.knownOffsets as Array<Record<string, unknown>> | undefined) ?? [],
)

onMounted(() => {
  void loadStatistics()
  void refreshGames()
})
</script>

<template>
  <div class="pattern-learning-view">
    <div class="header">
      <div>
        <h1>{{ $t('patternLearning.title') }}</h1>
        <p>{{ $t('patternLearning.subtitle') }}</p>
      </div>
    </div>

    <PanelIntro
      :what="$t('patternLearning.intro.what')"
      :purpose="$t('patternLearning.intro.purpose')"
      :how="$t('patternLearning.intro.how')"
    />

    <section class="panel">
      <h3>{{ $t('patternLearning.stats.title') }}</h3>
      <div class="stats-row">
        <div class="stat"><span class="stat-label">{{ $t('patternLearning.stats.knownGames') }}</span><span class="stat-value">{{ statistics.knownGames ?? '—' }}</span></div>
        <div class="stat"><span class="stat-label">{{ $t('patternLearning.stats.engineSignatures') }}</span><span class="stat-value">{{ statistics.engineSignaturesLoaded ?? '—' }}</span></div>
        <div class="stat"><span class="stat-label">{{ $t('patternLearning.stats.patternRules') }}</span><span class="stat-value">{{ statistics.patternRulesLoaded ?? '—' }}</span></div>
      </div>
    </section>

    <section class="panel">
      <h3>{{ $t('patternLearning.engine.title') }}</h3>
      <p class="hint">
        {{ store.isAttached ? $t('patternLearning.engine.attachedModules', { count: store.processModules.length, process: store.processName }) : $t('patternLearning.engine.noProcess') }}
      </p>
      <button class="btn btn-secondary" :disabled="detecting" @click="runDetectEngine">
        {{ detecting ? $t('patternLearning.engine.detecting') : $t('patternLearning.engine.detect') }}
      </button>
      <div v-if="detectionResult" class="result-box">
        <div><span class="k">{{ $t('patternLearning.labels.engine') }}</span><span class="v">{{ detectionResult.typeName }}</span></div>
        <div v-if="detectionResult.version"><span class="k">{{ $t('patternLearning.labels.version') }}</span><span class="v">{{ detectionResult.version }}</span></div>
        <div><span class="k">{{ $t('patternLearning.labels.confidence') }}</span><span class="v">{{ (Number(detectionResult.confidence) * 100).toFixed(0) }}%</span></div>
        <div v-if="detectionResult.signature"><span class="k">{{ $t('patternLearning.labels.signature') }}</span><span class="v mono">{{ detectionResult.signature }}</span></div>
      </div>
    </section>

    <section class="panel">
      <h3>{{ $t('patternLearning.classification.title') }}</h3>
      <div class="field-row">
        <label>{{ $t('patternLearning.labels.address') }}</label>
        <input v-model="classifyAddress" :placeholder="$t('patternLearning.classification.addressPlaceholder')" />
      </div>
      <div class="field-row">
        <label>{{ $t('patternLearning.classification.valuesLabel') }}</label>
        <input v-model="classifyValues" :placeholder="$t('patternLearning.classification.valuesPlaceholder')" />
      </div>
      <button class="btn btn-secondary" :disabled="classifying" @click="runClassify">
        {{ classifying ? $t('patternLearning.classification.classifying') : $t('patternLearning.classification.classify') }}
      </button>
      <div v-if="classification" class="result-box">
        <div><span class="k">{{ $t('patternLearning.labels.type') }}</span><span class="v">{{ classification.typeName }}</span></div>
        <div><span class="k">{{ $t('patternLearning.labels.confidence') }}</span><span class="v">{{ (Number(classification.confidence) * 100).toFixed(0) }}%</span></div>
        <div v-if="classification.suggestedValueType"><span class="k">{{ $t('patternLearning.classification.suggestedType') }}</span><span class="v">{{ $t('patternLearning.classification.suggestedScale', { type: classification.suggestedValueType, scale: classification.suggestedScale }) }}</span></div>
        <ul v-if="classificationReasoning.length > 0" class="reasoning">
          <li v-for="(reason, i) in classificationReasoning" :key="i">{{ reason }}</li>
        </ul>
      </div>
      <div v-else-if="classifying === false && classifyAddress" class="hint">{{ $t('patternLearning.classification.noResult') }}</div>
    </section>

    <section class="panel">
      <h3>{{ $t('patternLearning.profiles.title', { count: knownGames.length }) }}</h3>
      <div class="profile-list">
        <button
          v-for="game in knownGames"
          :key="game"
          class="btn btn-secondary compact"
          :class="{ active: selectedGame === game }"
          @click="selectedGame = game"
        >
          {{ game }}
        </button>
        <span v-if="knownGames.length === 0" class="hint">{{ $t('patternLearning.profiles.empty') }}</span>
      </div>
      <div class="actions-row">
        <button class="btn btn-secondary" :disabled="!selectedGame || profileBusy" @click="loadSelectedProfile">{{ $t('patternLearning.actions.load') }}</button>
        <button class="btn btn-secondary" :disabled="!selectedGame || profileBusy" @click="deleteSelectedProfile">{{ $t('patternLearning.actions.delete') }}</button>
        <button class="btn btn-secondary compact" @click="refreshGames">{{ $t('patternLearning.actions.refresh') }}</button>
      </div>

      <div v-if="loadedProfile" class="result-box">
        <div><span class="k">{{ $t('patternLearning.labels.game') }}</span><span class="v">{{ loadedProfile.gameName }}</span></div>
        <div v-if="loadedProfile.executableName"><span class="k">{{ $t('patternLearning.labels.executable') }}</span><span class="v">{{ loadedProfile.executableName }}</span></div>
        <div><span class="k">{{ $t('patternLearning.labels.engine') }}</span><span class="v">{{ loadedProfile.engineTypeName }} {{ loadedProfile.engineVersion }}</span></div>
        <div><span class="k">{{ $t('patternLearning.labels.sessions') }}</span><span class="v">{{ loadedProfile.sessionCount ?? 0 }}</span></div>
        <div v-if="profileKnownOffsets.length > 0" class="table-wrap">
          <table>
            <thead><tr><th>{{ $t('patternLearning.table.name') }}</th><th>{{ $t('patternLearning.table.offset') }}</th><th>{{ $t('patternLearning.table.type') }}</th><th>{{ $t('patternLearning.table.scale') }}</th><th>{{ $t('patternLearning.table.stability') }}</th></tr></thead>
            <tbody>
              <tr v-for="(offset, i) in profileKnownOffsets" :key="i">
                <td>{{ offset.name }}</td>
                <td class="mono">{{ offset.offset }}</td>
                <td>{{ offset.valueType }}</td>
                <td>{{ offset.scale }}</td>
                <td>{{ (Number(offset.stabilityScore) * 100).toFixed(0) }}%</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>

      <div class="create-row">
        <input v-model="newGameName" :placeholder="$t('patternLearning.profiles.gameNamePlaceholder')" />
        <input v-model="newExecutableName" :placeholder="$t('patternLearning.profiles.executablePlaceholder')" />
        <button class="btn btn-secondary compact" :disabled="!newGameName.trim() || profileBusy" @click="createProfile">{{ $t('patternLearning.profiles.createEmpty') }}</button>
      </div>
    </section>

    <section class="panel">
      <h3>{{ $t('patternLearning.suggestions.title') }}</h3>
      <div class="field-row">
        <label>{{ $t('patternLearning.labels.game') }}</label>
        <input v-model="suggestGameName" :placeholder="$t('patternLearning.profiles.gameNamePlaceholder')" />
      </div>
      <div class="field-row">
        <label>{{ $t('patternLearning.suggestions.patternType') }}</label>
        <select v-model.number="suggestPatternType">
          <option v-for="(name, idx) in PATTERN_TYPE_NAMES" :key="idx" :value="idx">{{ name }}</option>
        </select>
      </div>
      <div class="field-row">
        <label>{{ $t('patternLearning.suggestions.count') }}</label>
        <input v-model.number="suggestCount" type="number" min="1" max="20" />
      </div>
      <button class="btn btn-secondary" :disabled="!suggestGameName.trim() || suggesting" @click="runSuggestions">
        {{ suggesting ? $t('patternLearning.suggestions.searching') : $t('patternLearning.suggestions.run') }}
      </button>
      <div v-if="suggestions.length > 0" class="table-wrap">
        <table>
          <thead><tr><th>{{ $t('patternLearning.table.name') }}</th><th>{{ $t('patternLearning.table.address') }}</th><th>{{ $t('patternLearning.table.type') }}</th><th>{{ $t('patternLearning.table.scale') }}</th><th>{{ $t('patternLearning.table.stability') }}</th></tr></thead>
          <tbody>
            <tr v-for="(s, i) in suggestions" :key="i">
              <td>{{ s.name }}</td>
              <td class="mono">{{ s.address }}</td>
              <td>{{ s.valueType }}</td>
              <td>{{ s.scale }}</td>
              <td>{{ (Number(s.stabilityScore) * 100).toFixed(0) }}%</td>
            </tr>
          </tbody>
        </table>
      </div>
      <div v-else-if="suggesting === false && suggestGameName" class="hint">{{ $t('patternLearning.suggestions.none') }}</div>
    </section>
  </div>
</template>

<style scoped>
.pattern-learning-view {
  padding: 24px 32px;
  max-width: 1100px;
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
  color: var(--text-muted);
  margin-top: 4px;
}

.panel {
  border: 1px solid var(--border);
  background: var(--bg-secondary);
  border-radius: 8px;
  padding: 18px;
  margin-bottom: 14px;
}

.panel h3 {
  margin: 0 0 12px 0;
  font-size: 15px;
  color: var(--text-primary);
}

.hint {
  color: var(--text-muted);
  font-size: 12px;
  margin: 0 0 10px 0;
}

.stats-row {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
  gap: 16px;
}

.stat {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.stat-label {
  font-size: 12px;
  color: var(--text-muted);
}

.stat-value {
  font-size: 20px;
  font-weight: 600;
  color: var(--text-primary);
}

.field-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-bottom: 10px;
  max-width: 420px;
}

.field-row label {
  font-size: 12px;
  color: var(--text-muted);
}

.field-row input,
.field-row select {
  padding: 7px 10px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-primary);
}

.result-box {
  margin-top: 12px;
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  font-size: 13px;
}

.result-box > div {
  display: flex;
  gap: 8px;
  padding: 3px 0;
}

.result-box .k {
  color: var(--text-muted);
  min-width: 110px;
}

.result-box .v {
  color: var(--text-primary);
}

.result-box .v.mono {
  font-family: 'Cascadia Code', 'Consolas', monospace;
}

.reasoning {
  margin: 8px 0 0 0;
  padding-left: 18px;
  color: var(--text-secondary);
}

.profile-list {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: 12px;
}

.profile-list .btn.active {
  border-color: var(--accent);
  color: var(--accent);
}

.actions-row {
  display: flex;
  gap: 10px;
  margin-bottom: 10px;
}

.create-row {
  display: flex;
  gap: 8px;
  margin-top: 12px;
  flex-wrap: wrap;
}

.create-row input {
  padding: 7px 10px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg-primary);
  color: var(--text-primary);
  flex: 1;
  min-width: 160px;
}

.table-wrap {
  overflow-x: auto;
  margin-top: 10px;
}

table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

th {
  text-align: left;
  color: var(--text-muted);
  font-weight: 500;
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
}

td {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  color: var(--text-secondary);
}

.mono {
  font-family: 'Cascadia Code', 'Consolas', monospace;
  color: var(--text-primary);
}
</style>
