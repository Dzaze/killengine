<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { useAppStore } from '@/stores/app'
import AssistantToolsPanel from '@/components/settings/AssistantToolsPanel.vue'
import PanelIntro from '@/components/common/PanelIntro.vue'

const store = useAppStore()
const { locale, t } = useI18n()

const runtimeRows = computed(() => [
  { label: t('settings.backend'), value: store.isConnected ? t('settings.connected') : t('settings.disconnected') },
  { label: t('investigation.process'), value: store.isAttached ? store.processName : t('settings.none') },
  { label: 'Version', value: store.version },
  { label: 'Workflow', value: store.workflowStatus },
])

const stealthThreats = computed(
  () => (store.stealthRiskAnalysis?.threats as Array<Record<string, unknown>> | undefined) ?? [],
)
const stealthRecommendations = computed(
  () => (store.stealthRiskAnalysis?.recommendations as string[] | undefined) ?? [],
)

const debugEvents = computed(() => [...store.smartSearchDebugEvents].reverse())
const valueTypes = ['Int8', 'UInt8', 'Int16', 'UInt16', 'Int32', 'UInt32', 'Int64', 'UInt64', 'Float32', 'Float64']
const performanceModes = ['Auto', 'Eco', 'Normal', 'Performance', 'Max']
const unknownSnapshotPresets = [-1, 128, 512, 1024, 2048, 4096, 8192]
const unknownDepthLabel = (mb: number) => (mb === -1 ? t('settings.autoLabel') : t('settings.mbLabel', { mb }))
const externalAiApiKeyInput = ref('')
async function saveExternalAiApiKey() {
  const result = await store.setExternalAiApiKey(externalAiApiKeyInput.value)
  if (result?.success) {
    externalAiApiKeyInput.value = ''
  }
}
const kernelReadAddress = ref('')
const kernelReadSize = ref(16)
const kernelWriteAddress = ref('')
const kernelWriteBytes = ref('')
const kernelCapabilityLabel = computed(() => {
  const status = store.kernelDriverStatus
  if (!status) return t('settings.kernelNotTested')
  if (status.capabilities.processMemoryAccess) return t('settings.kernelReadWriteReady')
  if (status.status === 'connected') return t('settings.kernelProbeOnly')
  return t('settings.kernelUnavailable')
})
const kernelLearningSteps = computed(() => [
  { label: t('settings.kernelStep1Label'), detail: t('settings.kernelStep1Detail') },
  { label: t('settings.kernelStep2Label'), detail: t('settings.kernelStep2Detail') },
  { label: t('settings.kernelStep3Label'), detail: t('settings.kernelStep3Detail') },
  { label: t('settings.kernelStep4Label'), detail: t('settings.kernelStep4Detail') },
])
const visibleModelCandidates = computed(() =>
  (store.aiModelStatus?.modelCandidates ?? []).slice(0, 6),
)
const visibleExecutableCandidates = computed(() =>
  (store.aiModelStatus?.executableCandidates ?? []).slice(0, 6),
)
const visibleEmbeddedAgents = computed(() =>
  (store.aiModelStatus?.embeddedAgents ?? []).slice(0, 8).map((agent) => ({
    id: String(agent.id ?? ''),
    displayName: String(agent.displayName ?? agent.id ?? 'Agent IA'),
    role: String(agent.role ?? 'agent'),
    modelPath: String(agent.modelPath ?? ''),
    modelFound: agent.modelFound === true,
    valid: agent.valid !== false,
  })),
)

function formatBytes(value: number | undefined) {
  const bytes = value ?? 0
  if (bytes >= 1024 * 1024 * 1024) return t('settings.gbUnit', { value: (bytes / (1024 * 1024 * 1024)).toFixed(2) })
  if (bytes >= 1024 * 1024) return t('settings.mbUnit', { value: (bytes / (1024 * 1024)).toFixed(1) })
  if (bytes >= 1024) return t('settings.kbUnit', { value: (bytes / 1024).toFixed(1) })
  return t('settings.bUnit', { value: bytes })
}

function eventSummary(event: Record<string, unknown>) {
  const parts = [
    event.intent ? `intent=${String(event.intent)}` : '',
    event.tool ? `tool=${String(event.tool)}` : '',
    event.workflowStatus ? `workflow=${String(event.workflowStatus)}` : '',
    event.actionStatus ? `action=${String(event.actionStatus)}` : '',
    event.candidateCount !== undefined ? `${t('settings.eventCandidates')}=${String(event.candidateCount)}` : '',
    event.targetValue ? `${t('settings.eventTarget')}=${String(event.targetValue)}` : '',
  ].filter(Boolean)
  return parts.join('  ')
}

async function refreshAll() {
  await store.doPing()
  await store.loadSettings()
  await store.refreshKernelDriverStatus()
  await store.refreshAutomationPipeStatus()
  await store.refreshDiagnostics()
}

onMounted(() => {
  void refreshAll()
})

watch(
  () => store.appLanguage,
  (language) => {
    locale.value = language
  },
)

watch(
  () => locale.value,
  (language) => {
    store.appLanguage = language === 'en' ? 'en' : 'fr'
  },
  { immediate: true },
)

async function saveAll() {
  locale.value = store.appLanguage
  await store.saveSettings()
}

</script>

<template>
  <div class="settings-view">
    <div class="header">
      <div>
        <h1>{{ $t('nav.settings') }}</h1>
        <p>{{ store.statusText }}</p>
      </div>
      <button class="btn btn-secondary" @click="refreshAll">
        {{ $t('settings.refresh') }}
      </button>
    </div>

    <PanelIntro
      :what="$t('settings.intro.what')"
      :purpose="$t('settings.intro.purpose')"
      :how="$t('settings.intro.how')"
    />

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.interfaceTitle') }}</h2>
      </div>
      <div class="setting-row">
        <div>
          <strong>{{ $t('settings.language') }}</strong>
          <span>{{ store.appLanguage === 'fr' ? $t('settings.french') : $t('settings.english') }}</span>
        </div>
        <div class="segmented">
          <button :class="{ active: store.appLanguage === 'fr' }" @click="store.switchLanguage('fr')">FR</button>
          <button :class="{ active: store.appLanguage === 'en' }" @click="store.switchLanguage('en')">EN</button>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.scanTitle') }}</h2>
      </div>
      <div class="settings-grid">
        <label>
          <span>{{ $t('settings.defaultType') }}</span>
          <select v-model="store.settingDefaultValueType" class="input select">
            <option v-for="type in valueTypes" :key="type">{{ type }}</option>
          </select>
        </label>
        <label>
          <span>{{ $t('settings.maxResults') }}</span>
          <input v-model.number="store.settingScanMaxResults" class="input" type="number" min="1000" max="10000000" step="1000" />
        </label>
        <label>
          <span>{{ $t('settings.performanceMode') }}</span>
          <select v-model="store.settingPerformanceMode" class="input select">
            <option v-for="mode in performanceModes" :key="mode">{{ mode }}</option>
          </select>
        </label>
        <label>
          <span>{{ $t('settings.scanChunk') }}</span>
          <input v-model.number="store.settingScanChunkSizeMb" class="input" type="number" min="0" max="64" step="1" />
        </label>
        <label>
          <span>{{ $t('settings.maxThreads') }}</span>
          <input v-model.number="store.settingScanMaxWorkerThreads" class="input" type="number" min="0" max="128" step="1" />
        </label>
        <label>
          <span>{{ $t('settings.scanInFlightMemory') }}</span>
          <input v-model.number="store.settingScanMaxInFlightMb" class="input" type="number" min="0" max="32768" step="64" />
        </label>
        <label>
          <span>{{ $t('settings.candidateFileThreshold') }}</span>
          <input v-model.number="store.settingCandidateFileBackedThreshold" class="input" type="number" min="1" max="5000000" step="1000" />
        </label>
        <label>
          <span>{{ $t('settings.unknownSnapshotMax') }}</span>
          <select v-model.number="store.settingUnknownSnapshotMaxMb" class="input select">
            <option v-for="mb in unknownSnapshotPresets" :key="mb" :value="mb">{{ unknownDepthLabel(mb) }}</option>
          </select>
        </label>
        <label class="toggle-row">
          <input v-model="store.settingFastScan" type="checkbox" />
          <span>{{ $t('settings.fastScanDefault') }}</span>
        </label>
      </div>
      <p class="hint">
        {{ $t('settings.scanHint') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.tempStorageTitle') }}</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" :disabled="store.scanBusy" @click="store.refreshTemporaryStorageStatus()">
            {{ $t('settings.refreshAction') }}
          </button>
          <button class="btn btn-primary compact" :disabled="store.scanBusy" @click="store.clearTemporaryStorage()">
            {{ $t('settings.cleanupNow') }}
          </button>
        </div>
      </div>
      <div class="runtime-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.total') }}</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.totalBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.active') }}</span>
          <strong>{{ formatBytes(store.temporaryStorageStatus?.activeBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.orphans') }}</span>
          <strong>{{ store.temporaryStorageStatus?.orphanFileCount ?? 0 }} · {{ formatBytes(store.temporaryStorageStatus?.orphanBytes) }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.activeFiles') }}</span>
          <strong>{{ store.temporaryStorageStatus?.activeFileCount ?? 0 }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.tempFolder') }}</span>
        <code>{{ store.temporaryStorageStatus?.tempPath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.candidates') }}</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.candidateBytes) }} · {{ store.temporaryStorageStatus?.candidateFileBacked ? $t('settings.fileBacked') : $t('settings.ram') }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.undoSnapshot') }}</span>
        <code>{{ formatBytes(store.temporaryStorageStatus?.undoBytes) }} / {{ formatBytes(store.temporaryStorageStatus?.snapshotBytes) }}</code>
      </div>
      <p v-if="store.temporaryStorageCleanupResult" class="status-line">
        {{ store.temporaryStorageCleanupResult.message || (store.temporaryStorageCleanupResult.success ? $t('settings.cleanupDone') : store.temporaryStorageCleanupResult.error) }}
      </p>
      <p v-if="store.temporaryStorageError" class="error">{{ store.temporaryStorageError }}</p>
      <p class="hint">
        {{ $t('settings.tempStorageHint') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.localAiTitle') }}</h2>
        <div class="panel-actions">
          <!-- UX-PIPE-2 : "installé" (available) et "prêt à répondre" (ready) sont
               deux états distincts -- une IA désactivée (session ou réglage) ne
               doit jamais s'afficher comme prête, ni comme fichiers manquants. -->
          <span class="status-pill" :class="store.aiModelStatus?.ready ? 'ok' : 'warn'">
            {{ store.aiModelStatus?.ready
              ? $t('settings.aiReady')
              : store.aiModelStatus?.available
                ? $t('settings.aiDisabled')
                : $t('settings.aiUnavailable') }}
          </span>
          <button class="btn btn-secondary compact" :disabled="store.aiModelStatusLoading" @click="store.refreshAiModelStatus()">
            {{ store.aiModelStatusLoading ? $t('settings.checking') : $t('settings.verify') }}
          </button>
        </div>
      </div>
      <div class="settings-grid">
        <label class="wide">
          <span>{{ $t('settings.ggufCustomPath') }}</span>
          <div class="model-path-row">
            <input v-model="store.settingModelPath" class="input" :placeholder="$t('settings.ggufPathPlaceholder')" />
            <button class="btn btn-secondary compact" type="button" @click="store.browseForModel()">{{ $t('settings.browse') }}</button>
          </div>
        </label>
        <label>
          <span>{{ $t('settings.modelThreads') }}</span>
          <input v-model.number="store.settingModelThreads" class="input" type="number" min="1" max="32" step="1" />
        </label>
      </div>
      <div class="model-status-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.backend') }}</span>
          <strong>{{ store.aiModelStatus?.backend || '-' }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.product') }}</span>
          <strong>{{ $t('settings.embeddedAiRequired') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.aiAgents') }}</span>
          <strong>{{ store.aiModelStatus?.embeddedAgentCount ?? visibleEmbeddedAgents.length }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.model') }}</span>
          <strong>{{ store.aiModelStatus?.modelFound ? $t('settings.found') : $t('settings.notFound') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>llama-cli</span>
          <strong>{{ store.aiModelStatus?.executableFound ? $t('settings.found') : $t('settings.notFound') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.threads') }}</span>
          <strong>{{ store.aiModelStatus?.threads ?? store.settingModelThreads }}</strong>
        </div>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.modelDetected') }}</span>
        <code>{{ store.aiModelStatus?.modelPath || store.settingModelPath || '-' }}</code>
      </div>
      <p v-if="store.aiModelStatus?.configuredModelMissing" class="warning">
        {{ store.aiModelStatus.configuredModelWarning }}
      </p>
      <div class="path-row">
        <span>{{ $t('settings.activeRuntime') }}</span>
        <code>{{ store.aiModelStatus?.executablePath || '-' }}</code>
      </div>
      <div v-if="visibleEmbeddedAgents.length" class="embedded-agent-list">
        <strong>{{ $t('settings.embeddedAgents') }}</strong>
        <div v-for="agent in visibleEmbeddedAgents" :key="agent.id" class="candidate-path">
          <span :class="agent.modelFound && agent.valid ? 'ok-text' : 'dim-text'">{{ agent.modelFound && agent.valid ? $t('settings.ok') : '--' }}</span>
          <code>{{ agent.displayName }} · {{ agent.role }} · {{ agent.modelPath || '-' }}</code>
        </div>
      </div>
      <p v-if="store.aiModelStatus?.message" class="status-line">{{ store.aiModelStatus.message }}</p>
      <p v-if="store.aiModelStatus?.modelError && !store.aiModelStatus?.modelFound" class="warning">{{ store.aiModelStatus.modelError }}</p>
      <p v-if="store.aiModelStatusError" class="error">{{ store.aiModelStatusError }}</p>
      <p class="hint">
        {{ $t('settings.localAiHint1') }}
      </p>
      <details class="model-candidates">
        <summary>{{ $t('settings.inspectedPaths') }}</summary>
        <div class="candidate-columns">
          <div>
            <strong>{{ $t('settings.ggufModels') }}</strong>
            <div v-for="candidate in visibleModelCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? $t('settings.ok') : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
          <div>
            <strong>llama-cli</strong>
            <div v-for="candidate in visibleExecutableCandidates" :key="String(candidate.path)" class="candidate-path">
              <span :class="candidate.exists ? 'ok-text' : 'dim-text'">{{ candidate.exists ? $t('settings.ok') : '--' }}</span>
              <code>{{ candidate.path }}</code>
            </div>
          </div>
        </div>
      </details>
      <p class="hint">
        {{ $t('settings.localAiHint2') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.externalAiTitle') }}</h2>
        <div class="panel-actions">
          <span class="status-pill" :class="store.externalAiActiveBackend === 'claude' ? 'ok' : 'warn'">
            {{ store.externalAiActiveBackend === 'claude' ? $t('settings.claudeActive') : $t('settings.localActive') }}
          </span>
        </div>
      </div>
      <p class="hint">
        {{ $t('settings.externalAiHint1') }}
      </p>
      <p class="hint">
        {{ $t('settings.externalAiHint2Prefix') }} <strong>{{ $t('settings.automationModeStrong') }}</strong> {{ $t('settings.externalAiHint2Suffix') }}
      </p>
      <div class="settings-grid">
        <label>
          <span>{{ $t('settings.activeBackend') }}</span>
          <select
            class="input"
            :value="store.externalAiActiveBackend"
            :disabled="store.externalAiBusy"
            @change="store.setActiveAiBackend(($event.target as HTMLSelectElement).value as 'local' | 'claude')"
          >
            <option value="local">{{ $t('settings.localEmbeddedOption') }}</option>
            <option value="claude">{{ $t('settings.claudeApiOption') }}</option>
          </select>
        </label>
      </div>
      <div class="model-path-row">
        <input
          v-model="externalAiApiKeyInput"
          class="input"
          type="password"
          autocomplete="off"
          :placeholder="$t('settings.claudeApiKeyPlaceholder')"
        />
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.externalAiBusy || !externalAiApiKeyInput.trim()"
          @click="saveExternalAiApiKey()"
        >
          {{ $t('settings.save') }}
        </button>
        <button
          class="btn btn-secondary compact"
          type="button"
          :disabled="store.externalAiBusy || !store.externalAiHasApiKey"
          @click="store.clearExternalAiApiKey()"
        >
          {{ $t('settings.remove') }}
        </button>
      </div>
      <div class="model-status-grid">
        <div class="runtime-cell">
          <span>{{ $t('settings.keySaved') }}</span>
          <strong>{{ store.externalAiHasApiKey ? $t('settings.yes') : $t('settings.no') }}</strong>
        </div>
        <div class="runtime-cell">
          <span>{{ $t('settings.requestsThisSession') }}</span>
          <strong>{{ store.externalAiRequestCount }}</strong>
        </div>
      </div>
      <p v-if="store.externalAiError" class="error">{{ store.externalAiError }}</p>
      <p class="hint">
        {{ $t('settings.externalAiHint3') }}
      </p>
    </section>

    <AssistantToolsPanel />

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.workspaceTitle') }}</h2>
      </div>
      <p class="hint">{{ $t('settings.workspaceMovedHint') }}</p>
      <button class="btn btn-primary" type="button" @click="store.activeView = 'project'">
        {{ $t('settings.openProjectView') }}
      </button>
    </section>


    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.stateTitle') }}</h2>
      </div>
      <div class="runtime-grid">
        <div v-for="row in runtimeRows" :key="row.label" class="runtime-cell">
          <span>{{ row.label }}</span>
          <strong>{{ row.value }}</strong>
        </div>
      </div>
      <div class="ping-line">
        <button class="btn btn-secondary" @click="store.doPing()">
          {{ $t('actions.ping') }}
        </button>
        <code>{{ store.pingResult || '-' }}</code>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.diagnosticTitle') }}</h2>
        <div class="panel-actions">
          <button class="btn btn-secondary compact" @click="store.refreshLogTail()">
            {{ $t('settings.logs') }}
          </button>
          <button class="btn btn-secondary compact" @click="store.exportDiagnostics()">
            {{ $t('settings.exportAction') }}
          </button>
        </div>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.log') }}</span>
        <code>{{ store.logFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.smartSearchJson') }}</span>
        <code>{{ store.smartSearchDebugFilePath || '-' }}</code>
      </div>
      <div class="path-row">
        <span>{{ $t('settings.scanTelemetryJson') }}</span>
        <code>{{ store.scanTelemetryFilePath || '-' }}</code>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>{{ $t('settings.debugSmartSearch') }}</strong>
          <span>{{ store.settingSmartSearchDebugEnabled ? $t('settings.enabled') : $t('settings.disabled') }}</span>
        </div>
        <label class="toggle-row">
          <input v-model="store.settingSmartSearchDebugEnabled" type="checkbox" />
          <span>{{ $t('settings.writeJsonl') }}</span>
        </label>
      </div>
      <div class="setting-row inline-setting">
        <div>
          <strong>{{ $t('settings.eventsDisplayed') }}</strong>
          <span>{{ store.settingSmartSearchDebugMaxEvents }}</span>
        </div>
        <input v-model.number="store.settingSmartSearchDebugMaxEvents" class="input short-input" type="number" min="5" max="200" step="5" />
      </div>
      <p v-if="store.diagnosticExportPath" class="status-line">
        {{ $t('settings.diagnosticExported') }} <code>{{ store.diagnosticExportPath }}</code>
      </p>
      <p v-if="store.diagnosticExportError" class="error">{{ store.diagnosticExportError }}</p>
      <p v-if="store.diagnosticOpenFolderError" class="warning">{{ store.diagnosticOpenFolderError }}</p>
      <p v-if="store.logError" class="error">{{ store.logError }}</p>
      <p v-if="store.smartSearchDebugError" class="error">{{ store.smartSearchDebugError }}</p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.portabilityTitle') }}</h2>
      </div>
      <p class="hint">
        {{ $t('settings.portabilityHint') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityApiKeyLabel') }}</strong> — {{ $t('settings.portabilityApiKeyText') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityKernelDriverLabel') }}</strong> — {{ $t('settings.portabilityKernelDriverText') }}
      </p>
      <p class="hint">
        <strong>{{ $t('settings.portabilityDefenderLabel') }}</strong> — {{ $t('settings.portabilityDefenderText') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.antivirusCompatTitle') }}</h2>
      </div>
      <p class="hint">
        {{ $t('settings.antivirusHint') }}
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.defenderExclusionBusy"
          @click="store.requestWindowsDefenderExclusion()"
        >
          {{ store.defenderExclusionBusy ? $t('settings.addingInProgress') : $t('settings.addDefenderExclusion') }}
        </button>
      </div>
      <p v-if="store.defenderExclusionResult?.success" class="status-line">
        {{ $t('settings.exclusionAddedSuccess') }}
      </p>
      <p v-else-if="store.defenderExclusionResult?.cancelled" class="warning">
        {{ $t('settings.elevationRefused') }}
      </p>
      <p v-else-if="store.defenderExclusionResult?.error" class="error">
        {{ store.defenderExclusionResult.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.kernelDriverTitle') }}</h2>
        <span>{{ store.kernelDriverStatus?.status ?? $t('settings.unknownStatus') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.kernelDriverHint') }}
      </p>
      <div class="kernel-learning">
        <div class="kernel-learning-head">
          <strong>{{ $t('settings.kernelJourney') }}</strong>
          <span>{{ kernelCapabilityLabel }}</span>
        </div>
        <div class="kernel-learning-steps">
          <div v-for="step in kernelLearningSteps" :key="step.label" class="kernel-learning-step">
            <strong>{{ step.label }}</strong>
            <span>{{ step.detail }}</span>
          </div>
        </div>
      </div>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.kernelDriverStatusLoading"
          @click="store.refreshKernelDriverStatus()"
        >
          {{ store.kernelDriverStatusLoading ? $t('settings.probing') : $t('settings.testDriver') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.kernelDriverStartLoading || store.kernelDriverStatusLoading"
          :title="$t('settings.restartDriverTitle')"
          @click="store.startKernelDriver()"
        >
          {{ store.kernelDriverStartLoading ? $t('settings.starting') : $t('settings.restartDriver') }}
        </button>
      </div>
      <div v-if="store.kernelDriverStatus" class="settings-grid compact-grid">
        <div>
          <strong>{{ $t('settings.device') }}</strong>
          <span>{{ store.kernelDriverStatus.devicePath }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.message') }}</strong>
          <span>{{ store.kernelDriverStatus.message }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.protocol') }}</strong>
          <span>{{ store.kernelDriverStatus.capabilities.protocolVersion }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.healthProbe') }}</strong>
          <span>{{ store.kernelDriverStatus.capabilities.healthProbe ? $t('settings.yes') : $t('settings.no') }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.kernelMemoryAccess') }}</strong>
          <span>{{ store.kernelDriverStatus.capabilities.processMemoryAccess ? $t('settings.yes') : $t('settings.no') }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.privilegedInstrumentation') }}</strong>
          <span>{{ store.kernelDriverStatus.capabilities.privilegedInstrumentation ? $t('settings.yes') : $t('settings.no') }}</span>
        </div>
      </div>
      <p v-if="store.kernelDriverStatusError" class="error">
        {{ store.kernelDriverStatusError }}
      </p>

      <template v-if="store.kernelDriverStatus?.capabilities.processMemoryAccess">
        <h3>{{ $t('settings.kernelReadTitle') }}</h3>
        <p class="hint">
          {{ $t('settings.kernelReadHint', { process: store.isAttached ? store.processName : $t('settings.none') }) }}
        </p>
        <div class="panel-actions">
          <input v-model="kernelReadAddress" class="input" :placeholder="$t('settings.addressHexPlaceholder')" />
          <input v-model.number="kernelReadSize" class="input short-input" type="number" min="1" max="4096" step="1" />
          <button
            class="btn btn-secondary compact"
            :disabled="store.kernelMemoryReadBusy || !store.isAttached"
            @click="store.readMemoryKernel(kernelReadAddress, kernelReadSize)"
          >
            {{ store.kernelMemoryReadBusy ? $t('settings.reading') : $t('settings.readKernel') }}
          </button>
        </div>
        <p v-if="store.kernelMemoryReadResult?.success" class="status-line">
          {{ $t('settings.bytesRead', { count: store.kernelMemoryReadResult.bytesRead, hex: store.kernelMemoryReadResult.hex }) }}
        </p>
        <p v-else-if="store.kernelMemoryReadResult?.error" class="error">
          {{ store.kernelMemoryReadResult.error }}
        </p>

        <h3>{{ $t('settings.kernelWriteTitle') }}</h3>
        <p class="hint">
          {{ $t('settings.kernelWriteHint') }}
        </p>
        <div class="panel-actions">
          <input v-model="kernelWriteAddress" class="input" :placeholder="$t('settings.addressHexPlaceholder')" />
          <input v-model="kernelWriteBytes" class="input" :placeholder="$t('settings.bytesHexPlaceholder')" />
          <button
            class="btn btn-secondary compact"
            :disabled="store.kernelMemoryWriteBusy || !store.isAttached"
            @click="store.writeMemoryKernel(kernelWriteAddress, kernelWriteBytes)"
          >
            {{ store.kernelMemoryWriteBusy ? $t('settings.writing') : $t('settings.writeKernel') }}
          </button>
        </div>
        <p v-if="store.kernelMemoryWriteResult?.success" class="status-line">
          {{ $t('settings.bytesWritten', { count: store.kernelMemoryWriteResult.bytesWritten }) }}
        </p>
        <p v-else-if="store.kernelMemoryWriteResult?.error" class="error">
          {{ store.kernelMemoryWriteResult.error }}
        </p>
      </template>
      <p v-else class="hint">
        {{ $t('settings.kernelUnavailableHint') }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.automationModeTitle') }}</h2>
        <span>{{ store.automationPipeStatus?.running ? $t('settings.activeState') : $t('settings.inactive') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.automationHint') }}
      </p>
      <div class="panel-actions">
        <button
          v-if="!store.automationPipeStatus?.running"
          class="btn btn-secondary compact"
          @click="store.enableAutomationMode()"
        >
          {{ $t('settings.enableAutomation') }}
        </button>
        <button
          v-else
          class="btn btn-secondary compact"
          @click="store.disableAutomationMode()"
        >
          {{ $t('settings.disableAutomation') }}
        </button>
        <button class="btn btn-secondary compact" @click="store.refreshAutomationPipeStatus()">
          {{ $t('settings.refreshStatus') }}
        </button>
      </div>
      <div v-if="store.automationPipeStatus" class="settings-grid compact-grid">
        <div>
          <strong>{{ $t('settings.pipe') }}</strong>
          <span>{{ store.automationPipeStatus.pipeName }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.callsReceived') }}</strong>
          <span>{{ store.automationPipeStatus.callCount ?? 0 }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.lastCall') }}</strong>
          <span>{{ store.automationPipeStatus.lastMethod || '—' }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.atLabel') }}</strong>
          <span>{{ store.automationPipeStatus.lastCallAt || '—' }}</span>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.stealthModeTitle') }}</h2>
        <span>{{ store.stealthStatus?.active ? $t('settings.stealthActiveProfile', { profile: store.stealthStatus.profile }) : $t('settings.inactive') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.stealthHint') }}
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.stealthBusy || !store.isAttached"
          @click="store.applyStealthMode('sc2')"
        >
          {{ $t('settings.enableSc2') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.stealthBusy || !store.isAttached"
          @click="store.applyStealthMode('default')"
        >
          {{ $t('settings.enableDefault') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.stealthBusy || !store.isAttached"
          @click="store.applyStealthMode('minimal')"
        >
          {{ $t('settings.enableMinimal') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.stealthBusy || !store.stealthStatus?.active"
          @click="store.restoreStealthMode()"
        >
          {{ $t('settings.restoreDisable') }}
        </button>
        <button class="btn btn-secondary compact" :disabled="store.stealthBusy" @click="store.refreshStealthStatus()">
          {{ $t('settings.refreshStatus') }}
        </button>
      </div>
      <div v-if="store.stealthStatus?.modules" class="settings-grid compact-grid">
        <div>
          <strong>antiDebug</strong>
          <span>{{ store.stealthStatus.modules.antiDebug ? $t('settings.activeState') : $t('settings.inactiveState') }}</span>
        </div>
        <div>
          <strong>processMask</strong>
          <span>{{ store.stealthStatus.modules.processMask ? $t('settings.activeState') : $t('settings.inactiveState') }}</span>
        </div>
        <div>
          <strong>dllMask</strong>
          <span>{{ store.stealthStatus.modules.dllMask ? $t('settings.activeState') : $t('settings.inactiveState') }}</span>
        </div>
      </div>

      <div class="panel-actions" style="margin-top: 12px">
        <button
          class="btn btn-secondary compact"
          :disabled="store.stealthBusy || !store.isAttached"
          @click="store.analyzeStealthRisk()"
        >
          {{ $t('settings.analyzeDetectability') }}
        </button>
      </div>
      <p v-if="!store.isAttached" class="hint">{{ $t('settings.attachToAnalyze') }}</p>

      <div v-if="store.stealthRiskAnalysis?.success" class="stealth-analysis">
        <div class="stealth-risk-line">
          <span class="risk-badge" :class="`risk-${store.stealthRiskAnalysis.riskLevel}`">
            {{ store.stealthRiskAnalysis.riskLevel }} — {{ store.stealthRiskAnalysis.riskScore }}/100
          </span>
          <span class="hint">{{ $t('settings.moduleScannedCount', { count: store.stealthRiskAnalysis.moduleCount }) }}</span>
        </div>
        <ul v-if="stealthThreats.length" class="stealth-threat-list">
          <li v-for="(threat, idx) in stealthThreats" :key="idx">
            <strong>{{ threat.name }}</strong> ({{ threat.source }}) — {{ threat.detail }}
          </li>
        </ul>
        <ul v-if="stealthRecommendations.length" class="stealth-recommendation-list">
          <li v-for="(rec, idx) in stealthRecommendations" :key="idx">{{ rec }}</li>
        </ul>
      </div>
      <p v-else-if="store.stealthRiskAnalysis?.error" class="error">{{ store.stealthRiskAnalysis.error }}</p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.webview2CdpTitle') }}</h2>
        <span>{{ store.webView2CdpDebugFlagStatus?.enabled ? $t('settings.activeState') : $t('settings.inactive') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.webview2CdpHintPrefix') }} <strong>{{ $t('settings.webview2CdpHintStrong') }}</strong> {{ $t('settings.webview2CdpHintSuffix') }}
      </p>
      <div class="panel-actions">
        <button
          v-if="!store.webView2CdpDebugFlagStatus?.enabled"
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.enableWebView2CdpDebugFlag()"
        >
          {{ $t('settings.enableCdpDebug') }}
        </button>
        <button
          v-else
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.disableWebView2CdpDebugFlag()"
        >
          {{ $t('settings.disableCdpDebug') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2CdpDebugFlagBusy"
          @click="store.refreshWebView2CdpDebugFlagStatus()"
        >
          {{ $t('settings.refreshStatus') }}
        </button>
      </div>
      <p v-if="store.webView2CdpDebugFlagStatus?.value" class="status-line">
        {{ $t('settings.variableSetLabel') }} <code>{{ store.webView2CdpDebugFlagStatus.value }}</code>
      </p>
      <p v-if="store.webView2CdpDebugFlagStatus?.error" class="error">
        {{ store.webView2CdpDebugFlagStatus.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.webview2PrepTitle') }}</h2>
        <span>{{ store.webView2SystemPrepStatus?.capabilityInstalled ? $t('settings.ready') : $t('settings.toPrepare') }}</span>
      </div>
      <p class="hint">
        {{ $t('settings.webview2PrepHint') }}
      </p>
      <div class="panel-actions">
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy"
          @click="store.refreshWebView2SystemPrepStatus()"
        >
          {{ store.webView2SystemPrepBusy ? $t('settings.diagnosing') : $t('settings.runDiagnostic') }}
        </button>
        <button
          class="btn btn-secondary compact"
          :disabled="store.webView2SystemPrepBusy || store.webView2SystemPrepStatus?.capabilityInstalled"
          @click="store.installWebView2DeveloperModeCapability()"
        >
          {{ $t('settings.installCapability') }}
        </button>
      </div>
      <div v-if="store.webView2SystemPrepStatus" class="settings-grid compact-grid">
        <div>
          <strong>{{ $t('settings.developerMode') }}</strong>
          <span>{{ store.webView2SystemPrepStatus.developerModeEnabled ? $t('settings.enabledState') : $t('settings.disabledState') }}</span>
        </div>
        <div>
          <strong>{{ $t('settings.capabilityLabel') }}</strong>
          <span>{{ store.webView2SystemPrepStatus.capabilityState || '—' }}</span>
        </div>
      </div>
      <p v-if="store.webView2CapabilityInstallResult?.message" class="status-line">
        {{ store.webView2CapabilityInstallResult.message }}
      </p>
      <p v-else-if="store.webView2CapabilityInstallResult?.error" class="error">
        {{ store.webView2CapabilityInstallResult.error }}
      </p>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.mainLogTitle') }}</h2>
        <span>{{ store.logLines.length }}</span>
      </div>
      <div v-if="store.logLines.length === 0" class="empty-line">
        {{ $t('settings.noLogLoaded') }}
      </div>
      <pre v-else class="log-viewer">{{ store.logLines.join('\n') }}</pre>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.smartSearchEventsTitle') }}</h2>
        <div class="panel-actions">
          <span>{{ debugEvents.length }}</span>
          <button class="btn btn-secondary compact" :disabled="debugEvents.length === 0" @click="store.clearSmartSearchDebug()">
            {{ $t('settings.clear') }}
          </button>
        </div>
      </div>
      <div v-if="debugEvents.length === 0" class="empty-line">
        {{ $t('settings.noDebugEvent') }}
      </div>
      <div v-else class="debug-list">
        <div v-for="(event, index) in debugEvents" :key="index" class="debug-row">
          <div class="debug-head">
            <strong>{{ String(event.event ?? '-') }}</strong>
            <span>{{ String(event.timestamp ?? '') }}</span>
          </div>
          <code v-if="event.query">{{ event.query }}</code>
          <p v-if="eventSummary(event)">{{ eventSummary(event) }}</p>
          <p v-if="event.intentRationale">{{ event.intentRationale }}</p>
          <p v-if="event.message">{{ event.message }}</p>
          <p v-if="event.error" class="error">{{ event.error }}</p>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-title">
        <h2>{{ $t('settings.sessionTitle') }}</h2>
      </div>
      <div class="actions-row">
        <button class="btn btn-primary" :disabled="store.settingsSaving" @click="saveAll">
          {{ store.settingsSaving ? $t('settings.saving') : $t('settings.saveSettings') }}
        </button>
        <button class="btn btn-secondary" @click="store.resetWorkflow()">
          {{ $t('settings.resetWorkflow') }}
        </button>
        <button class="btn btn-secondary" :disabled="!store.isAttached" @click="store.detach()">
          {{ $t('process.detach') }}
        </button>
      </div>
      <!-- UX-PIPE-5 : distinction visuelle explicite, plus seulement le texte
           -- sinon un échec de persistance disque n'affichait qu'un message
           neutre indiscernable d'un succès. -->
      <p v-if="store.settingsStatus" class="status-line" :class="{ error: store.settingsSaveError }">{{ store.settingsStatus }}</p>
    </section>
  </div>
</template>

<style scoped>
.settings-view {
  max-width: 980px;
  padding: 24px 32px;
}

.header,
.panel-title,
.setting-row,
.ping-line,
.actions-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.panel-title span,
.empty-line {
  color: var(--text-dim);
  font-size: 12px;
}

.panel-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.workspace-actions {
  flex-wrap: wrap;
  justify-content: flex-start;
  margin-top: 10px;
}

.workspace-export {
  margin-top: 12px;
}

.workspace-import {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-panel,
.bookmark-list,
.audit-panel {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.project-name-input {
  min-width: 180px;
}

.bookmark-create {
  display: grid;
  grid-template-columns: 1fr 1fr 120px 140px 1.5fr auto;
  gap: 8px;
  padding: 10px 0;
}

.bookmark-note-input {
  min-width: 0;
}

.project-row,
.bookmark-row,
.audit-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.project-row:first-of-type,
.bookmark-row:first-of-type,
.audit-row:first-of-type {
  border-top: none;
}

.project-row div,
.bookmark-row div,
.audit-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.project-row span,
.bookmark-row span,
.audit-row span {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 980px) {
  .bookmark-create {
    grid-template-columns: 1fr 1fr;
  }
}

.workspace-import-input {
  width: 100%;
  min-height: 120px;
  resize: vertical;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  padding: 10px;
}

.import-preview {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}

.import-preview span {
  border: 1px solid var(--border);
  border-radius: 999px;
  color: var(--text-dim);
  font-size: 11px;
  padding: 4px 8px;
}

.template-list {
  margin-top: 12px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.template-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  padding: 8px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}

.template-row:first-of-type {
  border-top: none;
}

.template-row div {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 3px;
}

.template-row span {
  overflow: hidden;
  color: var(--text-dim);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.template-detail {
  margin-top: 10px;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.template-field-row {
  display: grid;
  grid-template-columns: 58px minmax(70px, 0.5fr) minmax(90px, 1fr) minmax(90px, 1fr) minmax(140px, 1.4fr);
  gap: 8px;
  align-items: center;
  min-height: 28px;
  padding: 5px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  color: var(--text-dim);
  font-size: 12px;
}

.template-field-row:first-of-type {
  border-top: none;
}

.template-field-row code,
.template-field-row strong,
.template-field-row span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.workspace-export pre {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.header {
  margin-bottom: 18px;
}

.header h1 {
  color: var(--text-primary);
  font-size: 22px;
}

.header p {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 13px;
}

.panel {
  margin-bottom: 12px;
  padding: 14px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
}

.panel-title {
  margin-bottom: 12px;
}

.panel-title h2 {
  color: var(--text-primary);
  font-size: 15px;
}

.panel-title h3 {
  color: var(--text-primary);
  font-size: 13px;
}

.setting-row strong,
.runtime-cell strong {
  display: block;
  color: var(--text-primary);
  font-size: 14px;
}

.settings-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}

.settings-grid label,
.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.settings-grid label span,
.toggle-row span,
.hint,
.status-line {
  color: var(--text-dim);
  font-size: 12px;
}

.status-line.error {
  color: var(--error);
  font-weight: 600;
}

.input {
  min-width: 0;
  width: 100%;
  min-height: 34px;
  padding: 8px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 13px;
  outline: none;
  transition: border-color 0.15s, box-shadow 0.15s, background 0.15s;
}

.list-filter-input {
  margin: 8px 0;
}

.list-show-more {
  align-self: center;
  margin: 8px auto 0;
  display: block;
}

.input::placeholder {
  color: var(--text-dim);
}

.input:focus {
  border-color: rgba(122, 162, 247, 0.8);
  box-shadow: 0 0 0 2px rgba(122, 162, 247, 0.15);
}

.input:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.input[type="number"] {
  appearance: textfield;
  -moz-appearance: textfield;
}

.input[type="number"]::-webkit-outer-spin-button,
.input[type="number"]::-webkit-inner-spin-button {
  margin: 0;
  appearance: none;
  -webkit-appearance: none;
}

.select {
  cursor: pointer;
}

.select option {
  background: var(--bg-primary);
  color: var(--text-primary);
}

.settings-grid .wide {
  grid-column: span 2;
}

.model-path-row {
  display: flex;
  gap: 8px;
}

.model-path-row .input {
  flex: 1;
  min-width: 0;
}

.toggle-row {
  align-items: flex-start;
  justify-content: center;
}

.settings-grid .toggle-row,
.inline-setting .toggle-row {
  flex-direction: row;
  align-items: center;
  justify-content: flex-start;
  min-height: 34px;
}

.toggle-row input {
  width: 15px;
  height: 15px;
  margin: 0;
  accent-color: var(--accent);
}

.inline-setting {
  padding-top: 10px;
  border-top: 1px solid var(--border);
}

.short-input {
  max-width: 120px;
}

.hint,
.status-line {
  margin-top: 10px;
}

.setting-row span,
.runtime-cell span,
.path-row span {
  display: block;
  color: var(--text-dim);
  font-size: 12px;
}

.segmented {
  display: inline-flex;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.segmented button {
  min-width: 42px;
  padding: 6px 10px;
  border: none;
  border-radius: 4px;
  background: transparent;
  color: var(--text-dim);
  cursor: pointer;
}

.segmented button.active {
  background: var(--bg-accent);
  color: var(--accent);
}

.runtime-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
}

.model-status-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(120px, 1fr));
  gap: 8px;
  margin-top: 12px;
}

.runtime-cell {
  min-height: 72px;
  padding: 11px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.runtime-cell strong {
  overflow: hidden;
  margin-top: 8px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.status-pill {
  border: 1px solid var(--border);
  border-radius: 999px;
  font-size: 11px;
  padding: 4px 8px;
}

.status-pill.ok {
  border-color: rgba(158, 206, 106, 0.45);
  color: var(--success);
}

.status-pill.warn {
  border-color: rgba(224, 175, 104, 0.45);
  color: var(--warning);
}

.model-candidates {
  margin-top: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list {
  margin-top: 10px;
  color: var(--text-dim);
  font-size: 12px;
}

.embedded-agent-list > strong {
  color: var(--text-primary);
}

.model-candidates summary {
  cursor: pointer;
}

.candidate-columns {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 12px;
  margin-top: 8px;
}

.candidate-path {
  display: grid;
  grid-template-columns: 28px minmax(0, 1fr);
  gap: 6px;
  align-items: center;
  margin-top: 5px;
}

.candidate-path code {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.ok-text {
  color: var(--success);
}

.dim-text {
  color: var(--text-dim);
}

.ping-line {
  margin-top: 12px;
}

.path-row {
  display: grid;
  grid-template-columns: 150px minmax(0, 1fr);
  gap: 12px;
  align-items: center;
  padding: 8px 0;
  border-top: 1px solid var(--border);
}

.path-row:first-of-type {
  border-top: none;
}

.remembered-patterns {
  margin-top: 12px;
}

.remembered-pattern-row {
  display: grid;
  grid-template-columns: minmax(160px, 1fr) 80px 50px minmax(0, 1.5fr) auto;
  gap: 10px;
  align-items: center;
  padding: 6px 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-tertiary);
  margin-bottom: 4px;
  font-size: 12px;
}

.remembered-pattern-row code {
  overflow: hidden;
  color: var(--text-primary);
  font-family: 'Cascadia Code', monospace;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.remembered-pattern-label {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.remembered-pattern-label strong {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.debug-list {
  display: flex;
  max-height: 360px;
  flex-direction: column;
  gap: 6px;
  overflow-y: auto;
}

.kernel-learning {
  display: grid;
  gap: 10px;
  margin: 12px 0;
  padding: 10px;
  border: 1px solid color-mix(in srgb, var(--accent) 28%, var(--border));
  border-radius: 6px;
  background: color-mix(in srgb, var(--accent) 7%, var(--bg-secondary));
}

.kernel-learning-head,
.kernel-learning-step {
  display: flex;
  justify-content: space-between;
  gap: 10px;
}

.kernel-learning-head strong {
  color: var(--text-primary);
}

.kernel-learning-head span,
.kernel-learning-step span {
  color: var(--text-dim);
  font-size: 12px;
}

.kernel-learning-steps {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}

.kernel-learning-step {
  min-height: 72px;
  flex-direction: column;
  justify-content: flex-start;
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.kernel-learning-step strong {
  color: var(--text-primary);
  font-size: 12px;
}

.log-viewer {
  max-height: 320px;
  overflow: auto;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 11px;
  line-height: 1.45;
  white-space: pre-wrap;
}

.debug-row {
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: var(--bg-primary);
}

.debug-head {
  display: flex;
  justify-content: space-between;
  gap: 10px;
  margin-bottom: 5px;
}

.debug-head strong {
  color: var(--text-primary);
  font-size: 13px;
}

.debug-head span,
.debug-row p {
  color: var(--text-dim);
  font-size: 12px;
}

.debug-row p {
  margin-top: 5px;
}

.error {
  color: var(--error) !important;
}

code {
  overflow-wrap: anywhere;
  color: var(--text-secondary);
  font-family: 'Cascadia Code', monospace;
  font-size: 12px;
}

.btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.15s;
}

.compact {
  padding: 5px 9px;
  font-size: 12px;
}

.btn:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.btn-secondary {
  background: var(--bg-accent);
  color: var(--text-secondary);
}

.danger-action {
  color: var(--error);
}

.btn-primary {
  background: var(--accent);
  color: var(--bg-primary);
  font-weight: 600;
}

.btn-primary:hover:not(:disabled) {
  background: var(--accent-hover);
}

@media (max-width: 850px) {
  .runtime-grid,
  .model-status-grid,
  .candidate-columns,
  .path-row,
  .kernel-learning-steps,
  .settings-grid {
    grid-template-columns: 1fr;
  }

  .setting-row,
  .ping-line,
  .actions-row {
    align-items: stretch;
    flex-direction: column;
  }
}

.stealth-analysis {
  margin-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.stealth-risk-line {
  display: flex;
  align-items: center;
  gap: 10px;
}

.risk-badge {
  padding: 3px 10px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  text-transform: uppercase;
  border: 1px solid var(--border);
}

.risk-badge.risk-low {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 50%, var(--border));
}

.risk-badge.risk-medium {
  color: var(--warning);
  border-color: color-mix(in srgb, var(--warning) 50%, var(--border));
}

.risk-badge.risk-high {
  color: var(--error);
  border-color: color-mix(in srgb, var(--error) 50%, var(--border));
}

.stealth-threat-list,
.stealth-recommendation-list {
  margin: 0;
  padding-left: 18px;
  font-size: 13px;
  color: var(--text-secondary);
}

.stealth-threat-list li,
.stealth-recommendation-list li {
  margin-bottom: 4px;
}
</style>
