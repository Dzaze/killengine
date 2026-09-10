<script setup lang="ts">
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore, type TrainerFeature } from '@/stores/app'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { t } = useI18n()
const name = ref('')
const action = ref<'write' | 'freeze_polling' | 'freeze_breakpoint' | 'patch' | 'clr_write'>('write')
const address = ref('')
const valueType = ref('Int32')
const value = ref('')
const patchBytes = ref('')
const dependsOn = ref<number[]>([])
const hotkeyDrafts = ref<Record<number, string>>({})
const actionFilter = ref('all')
const statusFilter = ref('all')
const processFilter = ref('all')
const searchFilter = ref('')
const exportText = ref('')
const exportStatus = ref('')
const overlayHotkeyDraft = ref('')
// PHASE 162 : id de la feature dont la generation de chaine de pointeurs est en cours
// (scan lecture seule de quelques secondes) - un seul scan a la fois, comme trainerBusy.
const pointerChainBusyId = ref<number | null>(null)

function overlayHotkeyValue() {
  return overlayHotkeyDraft.value || store.trainerOverlayHotkey || ''
}

const scenarioPresets = computed(() => store.workflowPresets.filter((preset) => preset.id.startsWith('scenario-')))

const checkpoints = computed(() => store.activeInvestigation?.checkpoints ?? [])
const processOptions = computed(() => {
  const processes = new Set<string>()
  for (const feature of store.trainerFeatures) {
    const name = feature.processName.trim()
    if (name) processes.add(name)
  }
  return [...processes].sort((a, b) => a.localeCompare(b))
})
const filteredFeatures = computed(() => {
  const text = searchFilter.value.trim().toLowerCase()
  return store.trainerFeatures.filter((feature) => {
    if (actionFilter.value !== 'all' && feature.action !== actionFilter.value) return false
    if (statusFilter.value !== 'all' && feature.status !== statusFilter.value) return false
    if (processFilter.value !== 'all' && feature.processName !== processFilter.value) return false
    if (!text) return true
    return [
      feature.name,
      feature.processName,
      feature.action,
      feature.address,
      feature.valueType,
      feature.value,
      feature.patchBytes,
      feature.hotkey,
      feature.lastError,
    ].some((value) => String(value ?? '').toLowerCase().includes(text))
  })
})

function fillFromSelection() {
  address.value = store.selectedCandidateAddress
  valueType.value = store.exactScanType
  value.value = store.writeValue
  if (!name.value.trim() && address.value) name.value = `Feature 0x${address.value}`
}

function createFeature() {
  const created = store.createTrainerFeature({
    name: name.value,
    action: action.value,
    address: address.value,
    valueType: valueType.value,
    value: value.value,
    patchBytes: patchBytes.value,
    dependsOn: dependsOn.value.length > 0 ? [...dependsOn.value] : undefined,
  })
  if (created) {
    name.value = ''
    action.value = 'write'
    address.value = ''
    valueType.value = 'Int32'
    value.value = ''
    patchBytes.value = ''
    dependsOn.value = []
  }
}

function dependencyNames(feature: TrainerFeature): string {
  if (!feature.dependsOn?.length) return ''
  return feature.dependsOn
    .map((id) => store.trainerFeatures.find((item) => item.id === id)?.name ?? `#${id}`)
    .join(', ')
}

function dependencyCandidates(feature: TrainerFeature): TrainerFeature[] {
  return store.trainerFeatures.filter((item) => item.id !== feature.id)
}

function updateFeatureDependencies(feature: TrainerFeature, event: Event) {
  const options = Array.from((event.target as HTMLSelectElement).selectedOptions)
  const ids = options.map((option) => Number(option.value)).filter((id) => Number.isFinite(id))
  store.updateTrainerFeatureDependencies(feature.id, ids)
}

function createFromCheckpoint(item: Record<string, unknown>) {
  const feature = store.createTrainerFeatureFromCheckpoint(item)
  if (feature) store.activeView = 'trainer'
}

function checkpointPlan(item: Record<string, unknown>) {
  return store.buildCheckpointActionPlan(item)
}

function checkpointPlanReason(item: Record<string, unknown>, actionId: string): string {
  return checkpointPlan(item).actions.find((action) => action.id === actionId)?.reason ?? ''
}

function bookmarkCheckpoint(item: Record<string, unknown>) {
  store.createWorkspaceBookmarkFromCheckpoint(item)
}

function statusClass(status: string) {
  if (status === 'active') return 'ok'
  if (status === 'error') return 'bad'
  if (status === 'ambiguous') return 'warn'
  return 'idle'
}

function featureSignatureQuality(feature: TrainerFeature) {
  if (feature.signatureQuality) return feature.signatureQuality
  if (feature.signatureScore === undefined && !feature.signatureLevel) return null
  return {
    score: feature.signatureScore ?? 0,
    level: feature.signatureLevel || 'unknown',
    warning: feature.signatureWarning || '',
    fixedBytes: feature.signatureFixedBytes ?? 0,
    wildcardBytes: feature.signatureWildcardBytes ?? 0,
    uniqueFixedBytes: feature.signatureUniqueFixedBytes ?? 0,
    fixedRatio: feature.signatureFixedRatio ?? 0,
    trainerSafe: feature.trainerSafe ?? false,
  }
}

function featureQualityClass(feature: TrainerFeature) {
  const quality = featureSignatureQuality(feature)
  return quality ? `quality-${quality.level}` : ''
}

function featureQualityWarning(feature: TrainerFeature): string {
  if (feature.action !== 'patch' || feature.locatorKind !== 'aob') return ''
  const quality = featureSignatureQuality(feature)
  if (!quality) return ''
  const score = Number(quality.score ?? 0)
  const fixedBytes = Number(quality.fixedBytes ?? 0)
  if (fixedBytes < 3 || score < 35) {
    return t('trainer.warnings.weakAob', { score, fixedBytes })
  }
  return String(quality.warning ?? '')
}

function featureWarning(feature: TrainerFeature): string {
  if (feature.status === 'ambiguous') return t('trainer.warnings.ambiguousFeature')
  if (feature.status === 'error') return feature.lastError || t('trainer.warnings.errorFeature')
  if (feature.action === 'patch' && !feature.patchBytes?.trim()) return t('trainer.warnings.incompletePatch')
  if (feature.action === 'patch' && feature.locatorKind === 'aob' && !feature.aobPattern?.trim()) return t('trainer.warnings.missingAob')
  if (feature.action === 'clr_write' && (!feature.clrTypeSubstring || !feature.clrIdentityField || !feature.clrIdentityValue || !feature.clrFieldName)) return t('trainer.warnings.incompleteClrLocator')
  const qualityWarning = featureQualityWarning(feature)
  if (qualityWarning) return qualityWarning
  return ''
}

function featureBlocked(feature: TrainerFeature): boolean {
  return Boolean(featureWarning(feature))
}

function hotkeyDraft(feature: TrainerFeature): string {
  return hotkeyDrafts.value[feature.id] ?? feature.hotkey ?? ''
}

async function saveHotkey(feature: TrainerFeature) {
  await store.registerTrainerFeatureHotkey(feature.id, hotkeyDraft(feature))
}

// PHASE 162 : declencheur UI de generateTrainerFeaturePointerChain (plomberie store deja
// livree et testee via pipe). Les logs/erreurs sont deja geres par la fonction store
// (addActionLog/addTrainerFeatureHistory) - ici on ne gere que l'etat "en cours" du bouton.
async function generatePointerChain(feature: TrainerFeature) {
  if (pointerChainBusyId.value !== null) return
  pointerChainBusyId.value = feature.id
  try {
    await store.generateTrainerFeaturePointerChain(feature.id)
  } finally {
    pointerChainBusyId.value = null
  }
}

function pointerChainButtonLabel(feature: TrainerFeature): string {
  if (pointerChainBusyId.value === feature.id) return t('trainer.pointerChain.scanInProgress')
  return feature.locatorKind === 'pointer_chain' ? t('trainer.pointerChain.regenerate') : t('trainer.pointerChain.generate')
}

function pointerChainTitle(feature: TrainerFeature): string {
  if (feature.locatorKind === 'absolute') {
    return t('trainer.pointerChain.absoluteTitle')
  }
  return t('trainer.pointerChain.refreshTitle')
}

function showTrainerExport() {
  exportText.value = store.exportTrainerFeaturesJson()
}

function showTrainerMarkdownExport() {
  exportText.value = store.exportTrainerFeaturesMarkdown()
}

async function copyTrainerExport() {
  if (!exportText.value) return
  await navigator.clipboard?.writeText(exportText.value)
  exportStatus.value = t('trainer.export.copied')
}
</script>

<template>
  <div class="trainer-view">
    <header class="topbar">
      <div>
        <h1>{{ $t('trainer.title') }}</h1>
        <p>{{ $t('trainer.subtitle') }}</p>
      </div>
      <div class="actions">
        <button class="btn" @click="store.setTrainerOverlay(!store.trainerOverlayVisible)">
          {{ store.trainerOverlayVisible ? $t('trainer.actions.overlayOff') : $t('trainer.actions.overlayOn') }}
        </button>
        <button class="btn" :disabled="!store.trainerOverlayVisible" @click="store.refreshTrainerOverlay()">{{ $t('trainer.actions.refreshOverlay') }}</button>
        <button class="btn" :disabled="store.trainerBusy || store.trainerFeatures.length === 0" @click="store.applyAllTrainerFeatures()">{{ $t('trainer.actions.applyAll') }}</button>
        <button class="btn" :disabled="store.trainerBusy || store.trainerFeatures.length === 0" @click="store.restoreAllTrainerFeatures()">{{ $t('trainer.actions.restoreAll') }}</button>
        <button class="btn" :disabled="store.trainerFeatures.length === 0" @click="store.saveCurrentWorkspaceProject()">{{ $t('trainer.actions.saveProject') }}</button>
        <button class="btn" :disabled="store.trainerFeatures.length === 0" @click="showTrainerExport()">{{ $t('trainer.actions.exportJson') }}</button>
        <button class="btn" :disabled="store.trainerFeatures.length === 0" @click="showTrainerMarkdownExport()">{{ $t('trainer.actions.exportMd') }}</button>
      </div>
    </header>

    <PanelIntro
      :what="$t('trainer.intro.what')"
      :purpose="$t('trainer.intro.purpose')"
      :how="$t('trainer.intro.how')"
    />

    <p v-if="store.trainerOverlayStatus" class="hotkey-status">{{ store.trainerOverlayStatus }}</p>
    <div class="overlay-hotkey-row">
      <span class="hint">{{ $t('trainer.overlay.hotkeyHint') }}</span>
      <input
        :value="overlayHotkeyValue()"
        class="input hotkey-input"
        :placeholder="$t('trainer.overlay.hotkeyPlaceholder')"
        @input="overlayHotkeyDraft = ($event.target as HTMLInputElement).value"
      />
      <button class="btn" :disabled="!overlayHotkeyValue().trim()" @click="store.registerOverlayHotkey(overlayHotkeyValue())">{{ $t('trainer.overlay.registerHotkey') }}</button>
      <button class="btn" :disabled="!store.trainerOverlayHotkeyId" @click="store.unregisterOverlayHotkey()">{{ $t('trainer.actions.remove') }}</button>
    </div>

    <div class="preset-row">
      <span class="hint preset-row-label">{{ $t('trainer.presets.prompt') }}</span>
      <button
        v-for="preset in scenarioPresets"
        :key="preset.id"
        class="btn"
        type="button"
        @click="store.applyWorkflowPreset(preset.id)"
      >
        {{ preset.title }}
      </button>
    </div>

    <section class="builder panel">
      <h2>{{ $t('trainer.builder.title') }}</h2>
      <div class="form">
        <input v-model="name" class="input" :placeholder="$t('trainer.builder.namePlaceholder')" />
        <select v-model="action" class="input">
          <option value="write">{{ $t('trainer.actionTypes.write') }}</option>
          <option value="freeze_polling">{{ $t('trainer.actionTypes.freezePolling') }}</option>
          <option value="freeze_breakpoint">{{ $t('trainer.actionTypes.freezeBreakpoint') }}</option>
          <option value="patch">{{ $t('trainer.actionTypes.patch') }}</option>
        </select>
        <input v-model="address" class="input mono" :placeholder="$t('trainer.builder.addressPlaceholder')" />
        <input v-model="valueType" class="input" :placeholder="$t('trainer.builder.typePlaceholder')" />
        <input v-if="action !== 'patch'" v-model="value" class="input" :placeholder="$t('trainer.builder.valuePlaceholder')" />
        <input v-else v-model="patchBytes" class="input mono" :placeholder="$t('trainer.builder.patchBytesPlaceholder')" />
        <button class="btn" @click="fillFromSelection()">{{ $t('trainer.builder.fromSelection') }}</button>
        <button class="btn primary" :disabled="!address.trim()" @click="createFeature()">{{ $t('trainer.builder.create') }}</button>
      </div>
      <div v-if="action === 'patch'" class="patch-relay-note" :title="$t('trainer.builder.patchRelayTitle')">
        {{ $t('trainer.builder.patchRelayAvailable') }}
      </div>
      <div v-if="store.trainerFeatures.length > 0" class="depends-on-row">
        <span class="hint">{{ $t('trainer.builder.dependsOnHint') }}</span>
        <select v-model="dependsOn" class="input depends-on-select" multiple>
          <option v-for="feature in store.trainerFeatures" :key="feature.id" :value="feature.id">{{ feature.name }}</option>
        </select>
      </div>
    </section>

    <section v-if="checkpoints.length > 0" class="panel">
      <h2>{{ $t('trainer.checkpoints.title') }}</h2>
      <div class="checkpoint-list">
        <article v-for="item in checkpoints" :key="String(item.id ?? item.address ?? item.label)" class="checkpoint">
          <strong>{{ item.label ?? item.address ?? item.id }}</strong>
          <span>{{ item.type ?? '' }} {{ item.value ?? '' }}</span>
          <div class="checkpoint-plan">
            <span
              v-for="actionItem in checkpointPlan(item).actions.filter((entry) => entry.enabled)"
              :key="actionItem.id"
              :class="`risk-${actionItem.risk}`"
              :title="actionItem.reason"
            >
              {{ actionItem.label }}
            </span>
          </div>
          <div class="checkpoint-actions">
            <button class="btn" :title="checkpointPlanReason(item, 'bookmark')" @click="bookmarkCheckpoint(item)">{{ $t('trainer.checkpoints.bookmark') }}</button>
            <button class="btn" :disabled="!checkpointPlan(item).actions.find((entry) => entry.id === 'trainer')?.enabled" :title="checkpointPlanReason(item, 'trainer')" @click="createFromCheckpoint(item)">{{ $t('trainer.checkpoints.createFeature') }}</button>
          </div>
        </article>
      </div>
    </section>

    <section class="panel">
      <div class="section-head">
        <h2>{{ $t('trainer.features.title') }}</h2>
        <span>{{ $t('trainer.features.count', { shown: filteredFeatures.length, count: store.trainerFeatures.length }) }}</span>
      </div>
      <div class="filters">
        <input v-model="searchFilter" class="input filter-input" :placeholder="$t('trainer.features.searchPlaceholder')" />
        <select v-model="actionFilter" class="input filter-input">
          <option value="all">{{ $t('trainer.filters.allActions') }}</option>
          <option value="write">{{ $t('trainer.actionTypes.write') }}</option>
          <option value="freeze_polling">{{ $t('trainer.actionTypes.freezePolling') }}</option>
          <option value="freeze_breakpoint">{{ $t('trainer.actionTypes.freezeBreakpoint') }}</option>
          <option value="patch">{{ $t('trainer.actionTypes.patch') }}</option>
          <option value="clr_write">{{ $t('trainer.actionTypes.clrWrite') }}</option>
        </select>
        <select v-model="statusFilter" class="input filter-input">
          <option value="all">{{ $t('trainer.filters.allStatuses') }}</option>
          <option value="idle">{{ $t('trainer.statuses.idle') }}</option>
          <option value="active">{{ $t('trainer.statuses.active') }}</option>
          <option value="error">{{ $t('trainer.statuses.error') }}</option>
          <option value="ambiguous">{{ $t('trainer.statuses.ambiguous') }}</option>
        </select>
        <select v-model="processFilter" class="input filter-input">
          <option value="all">{{ $t('trainer.filters.allProcesses') }}</option>
          <option v-for="process in processOptions" :key="process" :value="process">{{ process }}</option>
        </select>
      </div>
      <div v-if="store.trainerFeatures.length === 0" class="empty">{{ $t('trainer.features.empty') }}</div>
      <article v-for="feature in filteredFeatures" :key="feature.id" class="feature">
        <div class="feature-main">
          <strong>{{ feature.name }}</strong>
          <span v-if="feature.processName">{{ feature.processName }}</span>
          <span v-if="feature.locatorKind === 'clr_field'" class="mono">
            {{ feature.clrTypeSubstring }}.{{ feature.clrFieldName }}
          </span>
          <span v-else class="mono">0x{{ feature.address }}</span>
          <span v-if="feature.locatorKind === 'clr_field'">
            {{ $t('trainer.features.viaIdentity', { field: feature.clrIdentityField, value: feature.clrIdentityValue }) }}
          </span>
          <span>{{ feature.action }} · {{ feature.valueType }} {{ feature.value || feature.patchBytes }}</span>
          <span v-if="feature.hotkey" class="hotkey-chip">{{ feature.hotkey }}</span>
          <span v-if="dependencyNames(feature)" class="depends-on-chip" :title="dependencyNames(feature)">{{ $t('trainer.features.dependsOn', { names: dependencyNames(feature) }) }}</span>
          <span
            v-if="feature.pointerChain"
            class="pointer-chain-chip mono"
            :title="$t('trainer.pointerChain.locatorTitle', { module: feature.pointerChain.module, baseOffset: feature.pointerChain.baseOffset, offsets: feature.pointerChain.offsets.join(', ') })"
          >
            {{ feature.pointerChain.module }}+{{ feature.pointerChain.baseOffset }} → [{{ feature.pointerChain.offsets.join(', ') }}]
          </span>
          <span class="status" :class="statusClass(feature.status)">{{ feature.status }}</span>
          <span v-if="featureSignatureQuality(feature)" class="quality-chip" :class="featureQualityClass(feature)">
            AOB {{ featureSignatureQuality(feature)?.level }} · {{ featureSignatureQuality(feature)?.score }}/100
          </span>
          <span v-if="featureSignatureQuality(feature)" class="quality-detail">
            {{ $t('trainer.features.signatureDetails', { fixedBytes: featureSignatureQuality(feature)?.fixedBytes ?? 0, wildcardBytes: featureSignatureQuality(feature)?.wildcardBytes ?? 0 }) }}
          </span>
          <p v-if="featureWarning(feature)" class="feature-warning">{{ featureWarning(feature) }}</p>
          <p v-if="feature.lastError">{{ feature.lastError }}</p>
          <div v-if="feature.history?.length" class="history">
            <span v-for="item in feature.history.slice(0, 5)" :key="`${item.time}-${item.action}`" :class="`history-${item.status}`">
              {{ item.action }} · {{ item.detail || item.status }}
            </span>
          </div>
          <label v-if="dependencyCandidates(feature).length > 0" class="feature-dependencies-editor">
            <span>{{ $t('trainer.features.dependenciesLabel') }}</span>
            <select
              class="input depends-on-select"
              multiple
              :value="feature.dependsOn ?? []"
              @change="updateFeatureDependencies(feature, $event)"
            >
              <option v-for="candidate in dependencyCandidates(feature)" :key="candidate.id" :value="candidate.id">
                {{ candidate.name }}
              </option>
            </select>
          </label>
        </div>
        <div class="feature-actions">
          <button class="btn" :disabled="store.trainerBusy || feature.enabled || featureBlocked(feature)" @click="store.applyTrainerFeature(feature.id)">{{ $t('trainer.actions.on') }}</button>
          <button class="btn" :disabled="store.trainerBusy || !feature.enabled" @click="store.restoreTrainerFeature(feature.id)">{{ $t('trainer.actions.off') }}</button>
          <button class="btn" :disabled="store.trainerBusy || featureBlocked(feature)" @click="store.saveTrainerFeatureToProfile(feature.id)">{{ $t('trainer.actions.saveProfile') }}</button>
          <button
            v-if="feature.locatorKind !== 'clr_field'"
            class="btn"
            :disabled="store.trainerBusy || pointerChainBusyId !== null"
            :title="pointerChainTitle(feature)"
            @click="generatePointerChain(feature)"
          >{{ pointerChainButtonLabel(feature) }}</button>
          <input
            :value="hotkeyDraft(feature)"
            class="input hotkey-input"
            :placeholder="$t('trainer.features.hotkeyPlaceholder')"
            @input="hotkeyDrafts[feature.id] = ($event.target as HTMLInputElement).value"
          />
          <button class="btn" :disabled="store.trainerBusy || !hotkeyDraft(feature).trim()" @click="saveHotkey(feature)">{{ $t('trainer.actions.hotkey') }}</button>
          <button class="btn" :disabled="store.trainerBusy || !feature.hotkeyId" @click="store.unregisterTrainerFeatureHotkey(feature.id)">{{ $t('trainer.actions.removeHotkey') }}</button>
          <button class="btn danger" :disabled="store.trainerBusy" @click="store.deleteTrainerFeature(feature.id)">{{ $t('trainer.actions.delete') }}</button>
        </div>
      </article>
      <p v-if="store.trainerFeatures.length > 0 && filteredFeatures.length === 0" class="empty">{{ $t('trainer.features.noFilterMatch') }}</p>
      <p v-if="store.trainerHotkeyStatus" class="hotkey-status">{{ store.trainerHotkeyStatus }}</p>
    </section>

    <section v-if="exportText" class="panel export">
      <div class="section-head">
        <h2>{{ $t('trainer.export.title') }}</h2>
        <div class="actions">
          <button class="btn" @click="copyTrainerExport()">{{ $t('trainer.actions.copy') }}</button>
          <button class="btn" @click="exportText = ''">{{ $t('trainer.actions.close') }}</button>
        </div>
      </div>
      <p v-if="exportStatus" class="hotkey-status">{{ exportStatus }}</p>
      <pre>{{ exportText }}</pre>
    </section>
  </div>
</template>

<style scoped>
.trainer-view {
  height: 100%;
  overflow-y: auto;
  padding: 18px 24px;
}

.topbar,
.section-head,
.feature,
.checkpoint {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
}

h1 {
  color: var(--text-primary);
  font-size: 24px;
}

h2 {
  color: var(--text-primary);
  font-size: 15px;
}

p,
.empty {
  color: var(--text-dim);
}

.panel {
  margin-top: 14px;
  border: 1px solid var(--border);
  border-radius: 8px;
  background: var(--bg-secondary);
  padding: 12px;
}

.actions,
.feature-actions,
.checkpoint-actions,
.form {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.form {
  margin-top: 10px;
}

.filters {
  display: grid;
  grid-template-columns: minmax(190px, 1fr) repeat(3, minmax(135px, 0.45fr));
  gap: 8px;
  margin-top: 10px;
}

.input {
  min-width: 130px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  padding: 8px 10px;
}

.filter-input {
  min-width: 0;
}

.mono {
  font-family: 'Cascadia Code', monospace;
}

.btn {
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  color: var(--text-secondary);
  cursor: pointer;
  padding: 8px 10px;
}

.btn.primary {
  border-color: var(--accent);
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn.primary:hover:not(:disabled) {
  background: var(--accent-hover);
  border-color: var(--accent-hover);
}

.btn.danger {
  border-color: rgba(247, 118, 142, 0.45);
  color: var(--error);
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.checkpoint-list,
.feature-main {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.checkpoint,
.feature {
  margin-top: 8px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 8px;
  background: var(--bg-primary);
  padding: 10px;
}

.checkpoint-plan {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
}

.checkpoint-plan span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-secondary);
  font-size: 10px;
  padding: 3px 6px;
}

.checkpoint-plan .risk-safe {
  border-color: rgba(158, 206, 106, 0.28);
}

.checkpoint-plan .risk-write,
.checkpoint-plan .risk-debug,
.checkpoint-plan .risk-patch {
  border-color: rgba(224, 175, 104, 0.35);
}

.feature-main {
  align-items: flex-start;
  min-width: 0;
}

.feature-dependencies-editor {
  display: grid;
  width: min(420px, 100%);
  gap: 4px;
  color: var(--text-secondary);
  font-size: 12px;
}

.status {
  border: 1px solid var(--border);
  border-radius: 999px;
  padding: 3px 8px;
  font-size: 11px;
}

.status.ok {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.status.bad {
  border-color: rgba(247, 118, 142, 0.45);
  color: var(--error);
}

.status.warn {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.quality-chip {
  border: 1px solid var(--border);
  border-radius: 999px;
  font-size: 11px;
  padding: 3px 8px;
}

.quality-strong {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.quality-medium {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.quality-weak,
.quality-invalid {
  border-color: rgba(247, 118, 142, 0.45);
  color: var(--error);
}

.quality-detail {
  color: var(--text-dim);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
}

.feature-warning {
  border: 1px solid rgba(224, 175, 104, 0.32);
  border-radius: 6px;
  color: var(--warning);
  padding: 6px 8px;
}

.patch-relay-note {
  border: 1px solid rgba(224, 175, 104, 0.35);
  border-radius: 999px;
  color: var(--warning);
  display: inline-flex;
  font-size: 11px;
  margin-top: 8px;
  padding: 3px 8px;
}

.hotkey-chip {
  border: 1px solid rgba(122, 162, 247, 0.35);
  border-radius: 999px;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  padding: 3px 8px;
}

.pointer-chain-chip {
  overflow: hidden;
  max-width: 380px;
  border: 1px solid rgba(158, 206, 106, 0.35);
  border-radius: 999px;
  color: var(--text-secondary);
  font-size: 11px;
  padding: 3px 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.depends-on-chip {
  overflow: hidden;
  max-width: 320px;
  border: 1px solid rgba(224, 175, 104, 0.35);
  border-radius: 999px;
  color: var(--text-secondary);
  font-size: 11px;
  padding: 3px 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.depends-on-row {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin-top: 8px;
  width: 100%;
}

.depends-on-select {
  min-height: 60px;
}

.history {
  display: flex;
  flex-wrap: wrap;
  gap: 5px;
}

.history span {
  overflow: hidden;
  max-width: 260px;
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
  padding: 3px 7px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.history-success {
  border-color: rgba(158, 206, 106, 0.35) !important;
}

.history-warning {
  border-color: rgba(224, 175, 104, 0.35) !important;
}

.history-error {
  border-color: rgba(247, 118, 142, 0.35) !important;
}

.hotkey-input {
  width: 130px;
  min-width: 120px;
}

.hotkey-status {
  margin-top: 10px;
  color: var(--text-secondary);
}

.overlay-hotkey-row {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-items: center;
  margin-top: 8px;
}

.export pre {
  max-height: 320px;
  overflow: auto;
  margin-top: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
  line-height: 1.45;
  padding: 10px;
  white-space: pre-wrap;
}

@media (max-width: 900px) {
  .feature,
  .checkpoint,
  .topbar {
    align-items: flex-start;
    flex-direction: column;
  }

  .filters {
    grid-template-columns: 1fr;
  }
}
</style>
